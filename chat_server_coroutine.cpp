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

// Removed Session class

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

// Forward declarations for our coroutine handlers
struct Task handle_client_read(class Client* client);
struct Task handle_client_write(class Client* client);

/**
 * Client - Represents a connected client with separate read/write coroutines
 */
class Client {
public:
    Client(int fd, int epoll_fd, std::function<void(int, const std::string&)> broadcast)
        : fd_(fd), epoll_fd_(epoll_fd), closed_(false), 
          broadcast_(std::move(broadcast)),
          username_("user" + std::to_string(fd)) {
        set_nonblocking(fd);
    }

    ~Client() { close(); }

    void close() {
        if (!closed_) {
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd_, nullptr);
            ::close(fd_);
            closed_ = true;

            // Clean up coroutines
            if (read_handle_) {
                read_handle_ = nullptr;
            }
            if (write_handle_) {
                write_handle_ = nullptr;
            }

            read_task_.reset();
            write_task_.reset();
        }
    }

    void send(const std::string& msg) {
        write_queue_.push(msg);

        // Start write coroutine if needed and not already running
        maybe_start_write_coroutine();
    }

    bool perform_read() {
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

        return true;
    }

    // Process a complete line if available
    bool process_line() {
        size_t pos = read_buffer_.find('\n');
        if (pos == std::string::npos) {
            return false; // No complete line available
        }

        std::string line = read_buffer_.substr(0, pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (!line.empty()) {
            if (line[0] == '/' && line.size() > 6 && line.substr(0, 5) == "/nick") {
                username_ = line.substr(6);
                send("Nickname changed to: " + username_ + "\r\n");
            } else {
                broadcast_(fd_, line);
            }
        }

        read_buffer_ = read_buffer_.substr(pos + 1);
        return true;
    }

    bool perform_write() {
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

        return true;
    }

    // Set the read coroutine handle
    void set_read_handle(std::coroutine_handle<> handle) {
        read_handle_ = handle;
    }

    // Set the write coroutine handle
    void set_write_handle(std::coroutine_handle<> handle) {
        write_handle_ = handle;
    }

    void set_read_task(std::unique_ptr<Task> task) {
        read_task_ = std::move(task);
    }

    void set_write_task(std::unique_ptr<Task> task) {
        write_task_ = std::move(task);
    }

    void start_read_coroutine() {
        if (read_task_ && read_task_->handle) {
            read_task_->handle.resume();
        }
    }

    void start_write_coroutine() {
        if (write_task_ && write_task_->handle) {
            write_task_->handle.resume();
        }
    }

    void maybe_start_write_coroutine() {
        // Start the write coroutine if there's data and it's not running
        if (!write_queue_.empty() && (!write_task_ || !write_handle_ || write_handle_.done())) {
            // Create new write coroutine for this client
            std::cout << "[Debug] Creating new write coroutine for client " << fd_ << std::endl;
            write_task_ = std::make_unique<Task>(handle_client_write(this));
            start_write_coroutine();
        }
    }

    bool is_closed() const { return closed_; }
    int fd() const { return fd_; }
    int epoll_fd() const { return epoll_fd_; }
    const std::string& username() const { return username_; }
    std::coroutine_handle<> read_handle() const { return read_handle_; }
    std::coroutine_handle<> write_handle() const { return write_handle_; }
    bool has_write_data() const { return !write_queue_.empty(); }

    uint32_t event = 0;  // Store current epoll event

private:
    int fd_;
    int epoll_fd_;
    bool closed_;
    std::string read_buffer_;
    std::queue<std::string> write_queue_;
    std::function<void(int, const std::string&)> broadcast_;
    std::string username_;

    // Handles and tasks for read and write coroutines
    std::coroutine_handle<> read_handle_ = nullptr;
    std::coroutine_handle<> write_handle_ = nullptr;
    std::unique_ptr<Task> read_task_;
    std::unique_ptr<Task> write_task_;
};

/**
 * Epoll awaitable - for coroutine suspension with specific event types
 */
struct EpollAwaitable {
    Client* client;
    uint32_t interest_events;
    bool is_write_awaitable;

    // Constructor with specific interest events (default to read events)
    EpollAwaitable(Client* c, uint32_t events = EPOLLIN, bool is_write = false) 
        : client(c), interest_events(events), is_write_awaitable(is_write) {}

    bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        // Store the handle in the appropriate slot based on read vs write
        if (is_write_awaitable) {
            client->set_write_handle(h);
        } else {
            client->set_read_handle(h);
        }

        // Register for the specific events we're interested in
        epoll_event ev;
        ev.events = interest_events | EPOLLHUP | EPOLLERR;
        ev.data.ptr = client;

        int epoll_fd = client->epoll_fd();
        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd(), &ev) == -1) {
            if (errno == EINVAL || errno == ENOENT) {
                // Not yet added, add instead
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client->fd(), &ev);
            }
        }
    }

    uint32_t await_resume() noexcept { 
        // Return the epoll event to the coroutine
        return client->event; 
    }
};

/**
 * Read coroutine - Handles reading in a blocking-style
 */
Task handle_client_read(Client* client) {
    std::cout << "[Debug] Client " << client->fd() << ": Read coroutine started" << std::endl;
    client->send("Welcome to the chat server!\r\n");

    while (!client->is_closed()) {
        // Wait for read events in a blocking style
        uint32_t events = co_await EpollAwaitable(client, EPOLLIN, false);

        // Check for disconnect events
        if (events & (EPOLLHUP | EPOLLERR)) {
            std::cerr << "[Debug] Client " << client->fd() << ": Disconnected: HUP/ERR" << std::endl;
            co_return;
        }

        // If we have EPOLLIN event, read data until we get a line
        if (events & EPOLLIN) {
            // Read available data
            if (!client->perform_read()) {
                std::cerr << "[Debug] Client " << client->fd() << ": Read failed, closing" << std::endl;
                co_return;
            }

            // Process any complete lines
            while (client->process_line()) {
                // Each complete line is now processed and responses are queued
            }
        }
    }

    std::cout << "[Debug] Client " << client->fd() << ": Read coroutine completed" << std::endl;
    co_return;
}

/**
 * Write coroutine - Handles writing in a blocking-style
 * This coroutine is created on-demand when there's data to write
 */
Task handle_client_write(Client* client) {
    std::cout << "[Debug] Client " << client->fd() << ": Write coroutine started" << std::endl;

    // Continue writing as long as we have data and client is connected
    while (!client->is_closed() && client->has_write_data()) {
        // Wait for write events in a blocking style
        uint32_t events = co_await EpollAwaitable(client, EPOLLOUT, true);

        // Check for disconnect events
        if (events & (EPOLLHUP | EPOLLERR)) {
            std::cerr << "[Debug] Client " << client->fd() << ": Disconnected: HUP/ERR" << std::endl;
            co_return;
        }

        // If we have EPOLLOUT event, write data
        if (events & EPOLLOUT) {
            if (!client->perform_write()) {
                std::cerr << "[Debug] Client " << client->fd() << ": Write failed, closing" << std::endl;
                co_return;
            }
        }
    }

    std::cout << "[Debug] Client " << client->fd() << ": Write coroutine completed" << std::endl;
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

                    // Resume read coroutine if active
                    if (client->read_handle() && !client->read_handle().done()) {
                        std::cout << "[Debug] Resuming read coroutine for client " << client->fd() << std::endl;
                        client->read_handle().resume();
                    }

                    // Resume write coroutine if active
                    if (client->write_handle() && !client->write_handle().done()) {
                        std::cout << "[Debug] Resuming write coroutine for client " << client->fd() << std::endl;
                        client->write_handle().resume();
                    }

                    // Clean up if read coroutine is completed (main indicator client is done)
                    if (client->read_handle() && client->read_handle().done()) {
                        std::cout << "[Debug] Erasing client " << client->fd() << std::endl;
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
            auto task_ptr = std::make_unique<Task>(handle_client_read(client.get()));
            client->set_read_task(std::move(task_ptr));

            // Add the client to our map
            clients_[client_fd] = client;
            std::cout << "New client connected: " << client_fd << std::endl;

            // Start the coroutine - it's suspended initially
            client->start_read_coroutine();

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