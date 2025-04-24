/**
 * Minimalist Chat Server - Using epoll and C++20 coroutines
 */
#include <iostream>
#include <string>
#include <unordered_map>
#include <queue>
#include <memory>
#include <functional>
#include <coroutine>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <errno.h>

// Set socket to non-blocking mode
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * Simple session class - tracks user information
 */
struct Session {
    std::string username;
    Session(std::string name) : username(std::move(name)) {}
    ~Session() { std::cout << "Session for " << username << " destroyed\n"; }
};

/**
 * Simplified Task - Similar to the reference code
 */
struct Task {
    struct promise_type {
        Task get_return_object() {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() { return {}; }

        struct final_awaitable {
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<promise_type>) noexcept {}
            void await_resume() noexcept {}
        };

        final_awaitable final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() noexcept {}
    };

    std::coroutine_handle<promise_type> handle;

    Task(std::coroutine_handle<promise_type> h) : handle(h) {}
    Task() : handle(nullptr) {}
    ~Task() { if (handle) handle.destroy(); }

    // Move semantics
    Task(Task&& t) noexcept : handle(t.handle) { t.handle = nullptr; }
    Task& operator=(Task&& t) noexcept {
        if (this != &t) {
            if (handle) handle.destroy();
            handle = t.handle;
            t.handle = nullptr;
        }
        return *this;
    }

    // No copy
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
};

/**
 * Client - Represents a connected client (simplified)
 */
class Client {
public:
    Client(int fd, int epoll_fd, std::function<void(int, const std::string&)> broadcast)
        : fd_(fd), epoll_fd_(epoll_fd), closed_(false), 
          broadcast_(std::move(broadcast)),
          session_(std::make_unique<Session>("user" + std::to_string(fd))) {
        set_nonblocking(fd);
    }

    ~Client() { close(); }

    void close() {
        if (!closed_) {
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd_, nullptr);
            ::close(fd_);
            closed_ = true;

            // Clean up coroutine
            if (read_handle_) {
                //read_handle_.destroy();       // note: do not destroy here, coroutine is managed by the task
                read_handle_ = nullptr;
            }

            client_task_.reset();       // the coroutine will also be destroyed when the task is destroyed
            session_.reset();
        }
    }

    void send(const std::string& msg) {
        write_queue_.push(msg);
        register_epoll(true);
    }

    void register_epoll(bool include_write = false) {
        epoll_event ev;
        ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
        if (include_write && !write_queue_.empty()) ev.events |= EPOLLOUT;
        ev.data.ptr = this;

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd_, &ev) == -1) {
            if (errno == EINVAL || errno == ENOENT) {
                // Not yet added, add instead
                epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd_, &ev);
            } else {
                std::cerr << "epoll_ctl failed: " << strerror(errno) << ", fd_: "<< fd_<< std::endl;
            }
        }
    }

    bool write() {
        if (write_queue_.empty()) return true;

        const std::string& data = write_queue_.front();
        ssize_t bytes = ::send(fd_, data.data(), data.size(), 0);

        if (bytes <= 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            std::cerr << "Write failed for FD " << fd_ << ": errno " << errno << '\n';
            return false;
        }

        if (bytes > 0) {
            if (static_cast<size_t>(bytes) == data.size()) {
                write_queue_.pop();
            } else {
                write_queue_.front() = data.substr(bytes);
            }
        }

        register_epoll(!write_queue_.empty());
        return true;
    }

    bool read() {
        char buffer[1024];
        ssize_t bytes = ::read(fd_, buffer, sizeof(buffer) - 1);

        if (bytes <= 0) {
            if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return true;  // No data available, not an error
            }

            std::cerr << "Client FD " << fd_ << " read failed: "
                      << (bytes == 0 ? "EOF" : std::to_string(errno)) << '\n';
            return false;
        }

        buffer[bytes] = '\0';
        read_buffer_ += buffer;

        // Process complete lines
        size_t pos;
        while ((pos = read_buffer_.find('\n')) != std::string::npos) {
            std::string line = read_buffer_.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            if (!line.empty()) {
                if (line[0] == '/' && line.size() > 6 && line.substr(0, 5) == "/nick") {
                    session_->username = line.substr(6);
                    send("Nickname changed to: " + session_->username + "\r\n");
                } else {
                    broadcast_(fd_, line);
                }
            }

            read_buffer_ = read_buffer_.substr(pos + 1);
        }

        return true;
    }

    void set_coroutine_handle(std::coroutine_handle<> handle) {
        read_handle_ = handle;
    }

    void set_task(std::unique_ptr<Task> task) {
        client_task_ = std::move(task);
    }

    // Add a method to start the coroutine
    void start_coroutine() {
        if (client_task_ && client_task_->handle) {
            client_task_->handle.resume();
        }
    }

    bool is_closed() const { return closed_; }
    int fd() const { return fd_; }
    const std::string& username() const { return session_->username; }
    std::coroutine_handle<> read_handle() const { return read_handle_; }
    uint32_t event = 0;  // Store current epoll event

private:
    int fd_;
    int epoll_fd_;
    bool closed_;
    std::string read_buffer_;
    std::queue<std::string> write_queue_;
    std::function<void(int, const std::string&)> broadcast_;
    std::unique_ptr<Session> session_;
    std::coroutine_handle<> read_handle_ = nullptr;
    std::unique_ptr<Task> client_task_;
};

/**
 * Epoll awaitable - for coroutine suspension
 */
struct EpollAwaitable {
    Client* client;

    EpollAwaitable(Client* c) : client(c) {}
    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) noexcept {
        client->set_coroutine_handle(h);
        client->register_epoll(true);
    }
    uint32_t await_resume() noexcept { 
        // Return the epoll event to the coroutine
        return client->event; 
    }
};

/**
 * Client handling coroutine - Now handles all event processing like the reference code
 */
Task handle_client(Client* client) {
    std::cout << "[Debug] Client " << client->fd() << ": Coroutine started" << std::endl;
    client->send("Welcome to the chat server!\r\n");

    while (!client->is_closed()) {
        std::cout << "[Debug] Client " << client->fd() << ": Awaiting events..." << std::endl;
        // Wait for epoll events
        uint32_t events = co_await EpollAwaitable(client);
        std::cout << "[Debug] Client " << client->fd() << ": Got events: " 
                  << (events & EPOLLIN ? "EPOLLIN " : "")
                  << (events & EPOLLOUT ? "EPOLLOUT " : "") 
                  << (events & EPOLLHUP ? "EPOLLHUP " : "")
                  << (events & EPOLLERR ? "EPOLLERR " : "") << std::endl;

        // Handle the events directly in the coroutine
        if (events & (EPOLLHUP | EPOLLERR)) {
            std::cerr << "[Debug] Client " << client->fd() << ": Disconnected: HUP/ERR" << std::endl;
            co_return;
        }

        // Handle write if needed
        if (events & EPOLLOUT) {
            std::cout << "[Debug] Client " << client->fd() << ": Handling write" << std::endl;
            if (!client->write()) {
                std::cerr << "[Debug] Client " << client->fd() << ": Write failed, closing" << std::endl;
                co_return;
            }
        }

        // Handle read if needed
        if (events & EPOLLIN) {
            std::cout << "[Debug] Client " << client->fd() << ": Handling read" << std::endl;
            if (!client->read()) {
                std::cerr << "[Debug] Client " << client->fd() << ": Read failed, closing" << std::endl;
                co_return;
            }
        }

        // Re-register for epoll events if still open
        if (!client->is_closed()) {
            std::cout << "[Debug] Client " << client->fd() << ": Re-registering with epoll" << std::endl;
            client->register_epoll(true);
        }
    }

    std::cout << "[Debug] Client " << client->fd() << ": Coroutine completed" << std::endl;
    co_return;
}

/**
 * Chat Server
 */
class ChatServer {
public:
    ChatServer(int port) : port_(port), running_(false) {}

    ~ChatServer() { stop(); }

    bool start() {
        // Create epoll instance
        epoll_fd_ = epoll_create1(0);
        if (epoll_fd_ < 0) return false;

        // Create server socket
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            close(epoll_fd_);
            return false;
        }

        // Set reuse and non-blocking
        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        set_nonblocking(server_fd_);

        // Bind
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
            listen(server_fd_, SOMAXCONN) < 0) {
            close(server_fd_);
            close(epoll_fd_);
            return false;
        }

        // Register server socket with epoll
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = server_fd_;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &ev);

        running_ = true;
        std::cout << "Server started on port " << port_ << std::endl;
        return true;
    }

    void run() {
        if (!running_) return;

        const int MAX_EVENTS = 64;
        struct epoll_event events[MAX_EVENTS];

        while (running_) {
            int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, 100);

            if (n < 0 && errno != EINTR) {
                std::cerr << "epoll_wait error" << std::endl;
                break;
            }

            for (int i = 0; i < n; i++) {
                if (events[i].data.fd == server_fd_) {
                    accept_connections();
                } 
                else if (events[i].data.ptr != nullptr) {
                    Client* client = static_cast<Client*>(events[i].data.ptr);

                    // Store the event in the client for the coroutine to access
                    client->event = events[i].events;

                    // Just resume the coroutine - it will handle the events
                    if (client->read_handle() && !client->read_handle().done()) {
                        std::cout<< "[Debug] Resuming coroutine for client " << client->fd() << std::endl;
                        client->read_handle().resume();
                    }

                    // Clean up or completed clients
                    if ((client->read_handle() && client->read_handle().done())) {
                        std::cout<< "[Debug] erasing client " << client->fd() << std::endl;
                        clients_.erase(client->fd());
                    }
                }
            }
        }
    }

    void stop() {
        running_ = false;
        clients_.clear();
        if (server_fd_ >= 0) close(server_fd_);
        if (epoll_fd_ >= 0) close(epoll_fd_);
        server_fd_ = epoll_fd_ = -1;
    }

private:
    void accept_connections() {
        while (running_) {
            struct sockaddr_in addr;
            socklen_t addr_len = sizeof(addr);
            int client_fd = accept(server_fd_, (struct sockaddr*)&addr, &addr_len);

            if (client_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                std::cerr << "Accept error" << std::endl;
                break;
            }

            // Create client and start its coroutine
            auto client = std::make_shared<Client>(client_fd, epoll_fd_, 
                [this](int sender_fd, const std::string& msg) {
                    broadcast_message(sender_fd, msg);
                });

            // Create and store task in the client
            auto task_ptr = std::make_unique<Task>(handle_client(client.get()));
            client->set_task(std::move(task_ptr));

            // Add the client to our map
            clients_[client_fd] = client;
            std::cout << "New client connected: " << client_fd << std::endl;

            // Start the coroutine - it's suspended initially
            client->start_coroutine();

            // Initial registration with epoll (done in the coroutine now)
        }
    }

    void broadcast_message(int sender_fd, const std::string& message) {
        auto it = clients_.find(sender_fd);
        if (it == clients_.end()) return;

        std::string full_message = it->second->username() + ": " + message + "\r\n";

        for (auto& [fd, client] : clients_) {
            if (fd != sender_fd) {
                client->send(full_message);
            }
        }
    }

    int port_;
    int server_fd_ = -1;
    int epoll_fd_ = -1;
    bool running_ = false;
    std::unordered_map<int, std::shared_ptr<Client>> clients_;
};

// Main function
int main() {
    ChatServer server(12345);
    if (!server.start()) return 1;
    server.run();
    return 0;
}