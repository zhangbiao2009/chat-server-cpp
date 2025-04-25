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

        // Register for EPOLLIN once during initialization
        register_for_reading();
        std::cout << "[Client] FD " << fd_ << " registered for reading (EPOLLIN)" << std::endl;
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

    // Register for reading (EPOLLIN) events
    void register_for_reading() {
        epoll_event ev;
        ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
        ev.data.ptr = this;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd_, &ev);
    }

    // Add EPOLLOUT to the existing registration
    void enable_writing() {
        std::cout << "[Client] Enabling writing for FD " << fd_ << std::endl;
        epoll_event ev;
        ev.events = EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLERR; // Add EPOLLOUT
        ev.data.ptr = this;
        epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd_, &ev);
    }

    // Remove EPOLLOUT from the existing registration
    void disable_writing() {
        std::cout << "[Client] Disabling writing for FD " << fd_ << std::endl;
        epoll_event ev;
        ev.events = EPOLLIN | EPOLLHUP | EPOLLERR; // Back to just EPOLLIN
        ev.data.ptr = this;
        epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd_, &ev);
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

    // Add these accessor methods for the write queue
    const std::string& front_message() const { return write_queue_.front(); }
    void pop_message() { write_queue_.pop(); }
    void update_front_message(const std::string& new_data) { write_queue_.front() = new_data; }

    uint32_t event = 0;  // Store current epoll event

    void broadcast_message(const std::string& message) {
        broadcast_(fd_, message);
    }

    void change_username(const std::string& new_name) {
        username_ = new_name;
        send("Nickname changed to: " + new_name + "\r\n");
    }

private:
    int fd_;
    int epoll_fd_;
    bool closed_;
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
    bool is_write_awaitable;

    // Simplified constructor - just needs client and whether it's for write operations
    EpollAwaitable(Client* c, bool is_write = false) 
        : client(c), is_write_awaitable(is_write) {
        std::cout << "[EpollAwaitable] Created for client " << client->fd() 
                  << (is_write ? " (write)" : " (read)") << std::endl;
    }

    bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        std::cout << "[EpollAwaitable] Suspending " << (is_write_awaitable ? "write" : "read") 
                  << " coroutine for client " << client->fd() << std::endl;

        // Store the handle in the appropriate slot based on read vs write
        if (is_write_awaitable) {
            client->set_write_handle(h);
            client->enable_writing(); // Enable writing when write coroutine suspends
        } else {
            client->set_read_handle(h);
            // No need to register for reading, it's already done once in the constructor
        }
    }

    uint32_t await_resume() noexcept { 
        // Return the epoll event to the coroutine
        std::cout << "[EpollAwaitable] Resuming " << (is_write_awaitable ? "write" : "read")
                  << " coroutine for client " << client->fd() 
                  << " with events: " 
                  << (client->event & EPOLLIN ? "EPOLLIN " : "") 
                  << (client->event & EPOLLOUT ? "EPOLLOUT " : "")
                  << (client->event & EPOLLHUP ? "EPOLLHUP " : "") 
                  << (client->event & EPOLLERR ? "EPOLLERR " : "") << std::endl;
        return client->event; 
    }
};

/**
 * Read coroutine - Handles reading in a blocking-style
 */
Task handle_client_read(Client* client) {
    std::cout << "[Debug] Client " << client->fd() << ": Read coroutine started" << std::endl;

    // Send welcome message and log it
    std::cout << "[Debug] Sending welcome message to client " << client->fd() << std::endl;
    client->send("Welcome to the chat server!\r\n");

    // Local buffer in the coroutine - persists across co_await calls
    std::string read_buffer;

    while (!client->is_closed()) {
        // Wait for read events in a blocking style
        uint32_t events = co_await EpollAwaitable(client);  // using default false for is_write

        // Check for disconnect events
        if (events & (EPOLLHUP | EPOLLERR)) {
            std::cerr << "[Debug] Client " << client->fd() << ": Disconnected: HUP/ERR" << std::endl;
            co_return;
        }

        // If we have EPOLLIN event, read data until we get a line
        if (events & EPOLLIN) {
            // Read available data
            char buffer[1024];
            ssize_t bytes = ::read(client->fd(), buffer, sizeof(buffer) - 1);

            if (bytes <= 0) {
                if (bytes < 0 && (errno == EAGAIN || EWOULDBLOCK)) {
                    continue;  // No data available, not an error
                }

                std::cerr << "Client FD " << client->fd() << " read failed: "
                          << (bytes == 0 ? "EOF" : std::to_string(errno)) << '\n';
                std::cerr << "[Debug] Client " << client->fd() << ": Read failed, closing" << std::endl;
                co_return;
            }

            buffer[bytes] = '\0';
            read_buffer += buffer;

            // Process any complete lines
            bool processed;
            do {
                size_t pos = read_buffer.find('\n');
                if (pos == std::string::npos) {
                    processed = false; // No complete line available
                } else {
                    std::string line = read_buffer.substr(0, pos);
                    if (!line.empty() && line.back() == '\r') line.pop_back();

                    if (!line.empty()) {
                        if (line[0] == '/' && line.size() > 6 && line.substr(0, 5) == "/nick") {
                            client->change_username(line.substr(6));
                        } else {
                            client->broadcast_message(line);
                        }
                    }

                    read_buffer = read_buffer.substr(pos + 1);
                    processed = true;
                }
            } while (processed);
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
    std::cout << "[Debug] Client " << client->fd() << ": Has write data: " << (client->has_write_data() ? "yes" : "no") << std::endl;

    // Continue writing as long as we have data and client is connected
    while (!client->is_closed() && client->has_write_data()) {
        // Log the message we're trying to send
        std::cout << "[Debug] Client " << client->fd() << ": Waiting to write: \"" 
                  << client->front_message().substr(0, 20) << "...\"" << std::endl;

        // Wait for write events in a blocking style
        uint32_t events = co_await EpollAwaitable(client, true);  // true for is_write

        // Check for disconnect events
        if (events & (EPOLLHUP | EPOLLERR)) {
            std::cerr << "[Debug] Client " << client->fd() << ": Disconnected: HUP/ERR" << std::endl;
            co_return;
        }

        // If we have EPOLLOUT event, write data directly
        if (events & EPOLLOUT && client->has_write_data()) {
            const std::string& data = client->front_message();
            ssize_t bytes = ::send(client->fd(), data.data(), data.size(), 0);

            if (bytes <= 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "Write failed for FD " << client->fd() << ": errno " << errno << '\n';
                co_return;
            }

            if (bytes > 0) {
                if (static_cast<size_t>(bytes) == data.size()) {
                    client->pop_message();
                } else {
                    client->update_front_message(data.substr(bytes));
                }
            }
        }
    }

    // When we're done writing, disable the EPOLLOUT flag
    if (!client->is_closed()) {
        client->disable_writing();
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