/**
 * Minimal Chat Server - Using epoll and C++20 coroutines with blocking-style I/O
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

// Debug logging macros - comment out to disable verbose logs
#define DEBUG_LOGGING

#ifdef DEBUG_LOGGING
    #define LOG_DEBUG(msg) std::cout << "[Debug] " << msg << std::endl
    #define LOG_INFO(msg) std::cout << "[Info] " << msg << std::endl
#else
    #define LOG_DEBUG(msg)
    #define LOG_INFO(msg) std::cout << msg << std::endl
#endif

// Helper functions
inline void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct Task {
    struct promise_type {
        Task get_return_object() { return {std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
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

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
};

// Forward declarations
struct Task handle_client_read(class Client* client);
struct Task handle_client_write(class Client* client);
void process_complete_lines(class Client* client, std::string& buffer);

/**
 * Client - Represents a connected client with separate read/write coroutines
 */
class Client {
public:
    Client(int fd, int epoll_fd, std::function<void(int, const std::string&)> broadcast)
        : fd_(fd), epoll_fd_(epoll_fd), closed_(false), 
          broadcast_(std::move(broadcast)),
          username_("user" + std::to_string(fd)) {

        // Initialize socket
        set_nonblocking(fd);

        // Register for EPOLLIN events
        epoll_event ev{EPOLLIN | EPOLLHUP | EPOLLERR, {.ptr = this}};
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd_, &ev);

        LOG_INFO("Client " << fd_ << " connected");
    }

    ~Client() { close(); }

    void close() {
        if (!closed_) {
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd_, nullptr);
            ::close(fd_);
            closed_ = true;
            read_handle_ = write_handle_ = nullptr;
            read_task_.reset();
            write_task_.reset();
            LOG_INFO("Client " << fd_ << " closed");
        }
    }

    // Send a message - try immediate write first, fall back to coroutine
    void send(const std::string& msg) {
        if (closed_) return;

        write_queue_.push(msg);

        // Try immediate write and create coroutine if needed
        while (!write_queue_.empty()) {
            const std::string& data = write_queue_.front();
            ssize_t bytes = ::send(fd_, data.data(), data.size(), 0);

            if (bytes <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Would block - start write coroutine if not already running
                    if (!write_task_ || !write_handle_ || write_handle_.done()) {
                        start_coroutine(handle_client_write(this), true);
                    }
                    return;
                } else {
                    // Real error
                    close();
                    return;
                }
            }

            if (static_cast<size_t>(bytes) == data.size()) {
                write_queue_.pop();
            } else {
                write_queue_.front() = data.substr(bytes);
                // Need to wait for more space in socket buffer
                if (!write_task_ || !write_handle_ || write_handle_.done()) {
                    start_coroutine(handle_client_write(this), true);
                }
                return;
            }
        }
    }

    // Update epoll registration
    void update_epoll(bool enable_write) {
        if (closed_) return;

        epoll_event ev;
        ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
        if (enable_write) ev.events |= EPOLLOUT;
        ev.data.ptr = this;

        epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd_, &ev);
        LOG_DEBUG("Client " << fd_ << " epoll: " << (enable_write ? "+EPOLLOUT" : "-EPOLLOUT"));
    }

    // Process a command or message from the client
    void process_command(const std::string& line) {
        if (line.empty()) return;

        if (line[0] == '/' && line.size() > 6 && line.substr(0, 5) == "/nick") {
            // Handle nickname change
            username_ = line.substr(6);
            send("Nickname changed to: " + username_ + "\r\n");
        } else {
            // Broadcast message to all other clients
            broadcast_(fd_, line);
        }
    }

    // Coroutine management - consolidated methods
    void set_coroutine_handle(std::coroutine_handle<> handle, bool is_write) {
        if (is_write) {
            write_handle_ = handle;
            update_epoll(true);
        } else {
            read_handle_ = handle;
        }
    }

    void start_coroutine(Task&& task, bool is_write) {
        auto& task_ptr = is_write ? write_task_ : read_task_;
        task_ptr = std::make_unique<Task>(std::move(task));
        if (task_ptr && task_ptr->handle) {
            task_ptr->handle.resume();
        }
    }

    // Accessors
    bool is_closed() const { return closed_; }
    int fd() const { return fd_; }
    int epoll_fd() const { return epoll_fd_; }
    const std::string& username() const { return username_; }
    std::coroutine_handle<> read_handle() const { return read_handle_; }
    std::coroutine_handle<> write_handle() const { return write_handle_; }

    // Write state accessors
    bool has_write_data() const { return !write_queue_.empty(); }
    const std::string& front_message() const { return write_queue_.front(); }
    void pop_message() { write_queue_.pop(); }
    void update_front_message(const std::string& new_data) { write_queue_.front() = new_data; }

    // Current epoll event
    uint32_t event = 0;

private:
    int fd_;
    int epoll_fd_;
    bool closed_;
    std::queue<std::string> write_queue_;
    std::function<void(int, const std::string&)> broadcast_;
    std::string username_;

    // Coroutine management
    std::coroutine_handle<> read_handle_ = nullptr;
    std::coroutine_handle<> write_handle_ = nullptr;
    std::unique_ptr<Task> read_task_;
    std::unique_ptr<Task> write_task_;
};

/**
 * Simplified EpollAwaitable
 */
struct EpollAwaitable {
    Client* client;
    bool is_write;

    EpollAwaitable(Client* c, bool write = false) : client(c), is_write(write) {}

    bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        client->set_coroutine_handle(h, is_write);
        if (is_write) client->update_epoll(true);
    }

    uint32_t await_resume() noexcept { return client->event; }
};

/**
 * Read coroutine - Handles reading in a blocking-style
 */
Task handle_client_read(Client* client) {
    client->send("Welcome to the chat server!\r\n");
    std::string buffer;

    while (!client->is_closed()) {
        // Wait for read events in a blocking style
        uint32_t events = co_await EpollAwaitable(client);

        // Handle disconnection
        if (events & (EPOLLHUP | EPOLLERR)) {
            process_complete_lines(client, buffer);
            break;
        }

        // Handle read events
        if (events & EPOLLIN) {
            while (true) { // Read until buffer is empty
                char tmp[1024];
                ssize_t bytes = ::read(client->fd(), tmp, sizeof(tmp) - 1);

                if (bytes > 0) {
                    tmp[bytes] = '\0';
                    buffer += tmp;
                    process_complete_lines(client, buffer);
                } 
                else if (bytes == 0 || (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    process_complete_lines(client, buffer);
                    co_return; // Connection closed or error
                }
                else {
                    break; // EAGAIN/EWOULDBLOCK - no more data available
                }
            }
        }
    }
}

/**
 * Write coroutine - Created on-demand when data can't be written immediately
 */
Task handle_client_write(Client* client) {
    while (!client->is_closed() && client->has_write_data()) {
        // Wait for write events
        uint32_t events = co_await EpollAwaitable(client, true);

        if (events & (EPOLLHUP | EPOLLERR)) break;

        if (events & EPOLLOUT) {
            const std::string& data = client->front_message();
            ssize_t bytes = ::send(client->fd(), data.data(), data.size(), 0);

            // Handle write result
            if (bytes <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) break; // Real error
            } else if (static_cast<size_t>(bytes) == data.size()) {
                client->pop_message();
            } else {
                client->update_front_message(data.substr(bytes));
            }
        }
    }

    // Disable write events when done
    if (!client->is_closed()) client->update_epoll(false);
}

// Helper function to process complete lines in a buffer
void process_complete_lines(Client* client, std::string& buffer) {
    size_t pos;
    while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (!line.empty()) {
            LOG_DEBUG("Client " << client->fd() << " processing line: " << line);
            client->process_command(line);
        }

        buffer = buffer.substr(pos + 1);
    }
}

/**
 * Chat Server
 */
class ChatServer {
public:
    ChatServer(int port) : port_(port), running_(false) {}

    ~ChatServer() { stop(); }

    bool start() {
        // Create epoll instance and server socket
        if ((epoll_fd_ = epoll_create1(0)) < 0 ||
            (server_fd_ = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            stop();
            return false;
        }

        // Set socket options
        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        set_nonblocking(server_fd_);

        // Bind and listen
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_addr.s_addr = INADDR_ANY,
            .sin_port = htons(port_)
        };

        if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
            listen(server_fd_, SOMAXCONN) < 0) {
            stop();
            return false;
        }

        // Register server socket with epoll
        struct epoll_event ev{
            .events = EPOLLIN,
            .data = {.fd = server_fd_}
        };
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &ev);

        running_ = true;
        LOG_INFO("Server started on port " << port_);
        return true;
    }

    void run() {
        if (!running_) return;

        const int MAX_EVENTS = 64;
        struct epoll_event events[MAX_EVENTS];

        while (running_) {
            int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, 100);

            if (n < 0) {
                if (errno == EINTR) continue;
                std::cerr << "epoll_wait error: " << strerror(errno) << std::endl;
                break;
            }

            for (int i = 0; i < n; i++) {
                // Handle server socket (new connections)
                if (events[i].data.fd == server_fd_) {
                    accept_connections();
                    continue;
                }

                // Handle client events
                Client* client = static_cast<Client*>(events[i].data.ptr);
                client->event = events[i].events;

                // Resume read coroutine if active
                auto read_handle = client->read_handle();
                if (read_handle && !read_handle.done()) {
                    LOG_DEBUG("Resuming read for client " << client->fd());
                    read_handle.resume();
                }

                // Resume write coroutine if active
                auto write_handle = client->write_handle();
                if (write_handle && !write_handle.done()) {
                    LOG_DEBUG("Resuming write for client " << client->fd());
                    write_handle.resume();
                }

                // Clean up completed clients
                if (read_handle && read_handle.done()) {
                    LOG_DEBUG("Removing client " << client->fd());
                    clients_.erase(client->fd());
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
                LOG_DEBUG("Accept error: " << strerror(errno));
                break;
            }

            // Create client with broadcast callback and start read coroutine
            auto client = std::make_shared<Client>(client_fd, epoll_fd_,
                [this](int sender_fd, const std::string& msg) {
                    broadcast_message(sender_fd, msg);
                });

            client->start_coroutine(handle_client_read(client.get()), false);
            clients_[client_fd] = client;
            LOG_INFO("Client connected: " << client_fd);
        }
    }

    void broadcast_message(int sender_fd, const std::string& message) {
        auto it = clients_.find(sender_fd);
        if (it == clients_.end()) return;

        std::string full_message = it->second->username() + ": " + message + "\r\n";

        // Send message to all other clients
        for (auto& [fd, client] : clients_) {
            if (fd != sender_fd) client->send(full_message);
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
    if (!server.start()) {
        std::cerr << "Failed to start server" << std::endl;
        return 1;
    }
    server.run();
    return 0;
}