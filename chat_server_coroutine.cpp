/**
 * Improved Chat Server - Using epoll and C++20 coroutines
 * With clean, blocking-style handling of network I/O
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

// Debug logging control - comment out to disable verbose logs
#define DEBUG_LOGGING

#ifdef DEBUG_LOGGING
    #define LOG_DEBUG(msg) std::cout << "[Debug] " << msg << std::endl
    #define LOG_INFO(msg) std::cout << "[Info] " << msg << std::endl
#else
    #define LOG_DEBUG(msg)
    #define LOG_INFO(msg) std::cout << msg << std::endl
#endif

// Set socket to non-blocking mode
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Set socket send buffer size
void set_socket_send_buffer_size(int fd, int size) {
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0) {
        std::cerr << "Failed to set SO_SNDBUF to " << size << " bytes: " 
                  << strerror(errno) << std::endl;
        return;
    }

    // Get the actual buffer size allocated by the kernel
    int actual_size;
    socklen_t size_len = sizeof(actual_size);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &actual_size, &size_len) < 0) {
        std::cerr << "Failed to get SO_SNDBUF value: " << strerror(errno) << std::endl;
    } else {
        LOG_INFO("Socket " << fd << " send buffer size: requested=" << size 
                 << ", actual=" << actual_size << " bytes");
    }
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

void process_complete_lines(Client* client, std::string& buffer);

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

        // Set send buffer size to 4K for client socket
        set_socket_send_buffer_size(fd, 4096);

        // Register for EPOLLIN once during initialization
        update_epoll_registration();
        LOG_INFO("Client " << fd_ << " connected and registered for reading");
    }

    ~Client() { close(); }

    void close() {
        if (!closed_) {
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd_, nullptr);
            ::close(fd_);
            closed_ = true;

            // Clean up coroutines
            read_handle_ = write_handle_ = nullptr;
            read_task_.reset();
            write_task_.reset();
            LOG_INFO("Client " << fd_ << " closed");
        }
    }

    void send(const std::string& msg) {
        write_queue_.push(msg);

        // Try to write immediately - if it succeeds completely, we avoid creating a coroutine
        if (try_write()) {
            // All data written, no need to start write coroutine
            return;
        }

        // Start write coroutine if needed
        maybe_start_write_coroutine();
    }

    // Try to write all queued messages without waiting for EPOLLOUT
    // Returns true if all data was written, false if we need to wait for EPOLLOUT
    bool try_write() {
        if (write_queue_.empty()) return true;

        while (!write_queue_.empty()) {
            const std::string& data = write_queue_.front();
            ssize_t bytes = ::send(fd_, data.data(), data.size(), 0);

            LOG_DEBUG("Client " << fd_ << " immediate write: " << (bytes > 0 ? bytes : 0) 
                      << " of " << data.size() << " bytes");

            if (bytes <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Socket buffer is full, need to wait for EPOLLOUT
                    LOG_DEBUG("Client " << fd_ << " socket buffer full, will wait for EPOLLOUT");
                    return false;
                } else {
                    // Real error
                    std::cerr << "Client " << fd_ << " write error: " << strerror(errno) << std::endl;
                    return false;
                }
            }

            if (static_cast<size_t>(bytes) == data.size()) {
                // Full write successful
                write_queue_.pop();
            } else {
                // Partial write, update the buffer and return false to wait for EPOLLOUT
                write_queue_.front() = data.substr(bytes);
                return false;
            }
        }

        return true; // All data was written
    }

    // Update epoll registration based on current needs
    void update_epoll_registration(bool enable_write = false) {
        epoll_event ev;
        ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
        if (enable_write) ev.events |= EPOLLOUT;
        ev.data.ptr = this;

        // Try to modify first (most common case), fall back to add if needed
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd_, &ev) < 0 && errno == ENOENT) {
            epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd_, &ev);
        }

        LOG_DEBUG("Client " << fd_ << " epoll registration updated: " 
                  << (enable_write ? "read+write" : "read only"));
    }

    // Enable writing (add EPOLLOUT)
    void enable_writing() {
        LOG_DEBUG("Client " << fd_ << " enabling write events");
        update_epoll_registration(true);
    }

    // Disable writing (remove EPOLLOUT)
    void disable_writing() {
        LOG_DEBUG("Client " << fd_ << " disabling write events");
        update_epoll_registration(false);
    }

    // Set coroutine handle (consolidated method)
    void set_coroutine_handle(std::coroutine_handle<> handle, bool is_write) {
        if (is_write) {
            write_handle_ = handle;
        } else {
            read_handle_ = handle;
        }
    }

    // Start a coroutine task (consolidated method)
    void start_coroutine(Task&& task, bool is_write) {
        if (is_write) {
            write_task_ = std::make_unique<Task>(std::move(task));
            if (write_task_ && write_task_->handle) {
                write_task_->handle.resume();
            }
        } else {
            read_task_ = std::make_unique<Task>(std::move(task));
            if (read_task_ && read_task_->handle) {
                read_task_->handle.resume();
            }
        }
    }

    // Start write coroutine if needed
    void maybe_start_write_coroutine() {
        // Start the write coroutine if there's data and it's not running
        if (!write_queue_.empty() && (!write_task_ || !write_handle_ || write_handle_.done())) {
            LOG_DEBUG("Client " << fd_ << " creating new write coroutine");
            start_coroutine(handle_client_write(this), true);
        }
    }

    // Processing message commands
    void process_command(const std::string& line) {
        if (line[0] == '/' && line.size() > 6 && line.substr(0, 5) == "/nick") {
            change_username(line.substr(6));
        } else {
            broadcast_message(line);
        }
    }

    // Core functionality
    void broadcast_message(const std::string& message) {
        broadcast_(fd_, message);
    }

    void change_username(const std::string& new_name) {
        username_ = new_name;
        send("Nickname changed to: " + new_name + "\r\n");
    }

    // Accessors
    bool is_closed() const { return closed_; }
    int fd() const { return fd_; }
    int epoll_fd() const { return epoll_fd_; }
    const std::string& username() const { return username_; }
    std::coroutine_handle<> read_handle() const { return read_handle_; }
    std::coroutine_handle<> write_handle() const { return write_handle_; }
    bool has_write_data() const { return !write_queue_.empty(); }
    const std::string& front_message() const { return write_queue_.front(); }
    void pop_message() { write_queue_.pop(); }
    void update_front_message(const std::string& new_data) { write_queue_.front() = new_data; }

    // Store current epoll events
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
 * Epoll awaitable - for coroutine suspension with specific event types
 */
struct EpollAwaitable {
    Client* client;
    bool is_write_awaitable;

    // Simplified constructor - just needs client and whether it's for write operations
    EpollAwaitable(Client* c, bool is_write = false) 
        : client(c), is_write_awaitable(is_write) {
        LOG_DEBUG("EpollAwaitable created for client " << client->fd() 
                  << (is_write ? " (write)" : " (read)"));
    }

    bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        LOG_DEBUG("Suspending " << (is_write_awaitable ? "write" : "read") 
                 << " coroutine for client " << client->fd());

        // Store the handle in the appropriate slot based on read vs write
        client->set_coroutine_handle(h, is_write_awaitable);

        if (is_write_awaitable) {
            client->enable_writing(); // Enable writing when write coroutine suspends
        }
    }

    uint32_t await_resume() noexcept { 
        // Return the epoll event to the coroutine
        LOG_DEBUG("Resuming " << (is_write_awaitable ? "write" : "read")
                 << " coroutine for client " << client->fd() 
                 << " with events: " 
                 << (client->event & EPOLLIN ? "EPOLLIN " : "") 
                 << (client->event & EPOLLOUT ? "EPOLLOUT " : "")
                 << (client->event & EPOLLHUP ? "EPOLLHUP " : "") 
                 << (client->event & EPOLLERR ? "EPOLLERR " : ""));
        return client->event; 
    }
};

/**
 * Read coroutine - Handles reading in a blocking-style
 */
Task handle_client_read(Client* client) {
    LOG_INFO("Client " << client->fd() << " read coroutine started");

    // Send welcome message
    client->send("Welcome to the chat server!\r\n");
    LOG_DEBUG("Welcome message sent to client " << client->fd());

    // Local buffer that persists across co_await calls
    std::string read_buffer;

    while (!client->is_closed()) {
        LOG_DEBUG("Client " << client->fd() << " waiting for read events");

        // Wait for read events in a blocking style
        uint32_t events = co_await EpollAwaitable(client);

        // Check for disconnect events
        if (events & (EPOLLHUP | EPOLLERR)) {
            LOG_INFO("Client " << client->fd() << " disconnected (HUP/ERR)");
            co_return;
        }

        // Handle EPOLLIN events
        if (events & EPOLLIN) {
            // Read in a loop until EAGAIN/EWOULDBLOCK to drain the socket buffer
            while (true) {
                char buffer[1024];
                ssize_t bytes = ::read(client->fd(), buffer, sizeof(buffer) - 1);

                if (bytes > 0) {
                    // Data received
                    buffer[bytes] = '\0';
                    read_buffer += buffer;
                    LOG_DEBUG("Client " << client->fd() << " read " << bytes << " bytes");

                    // Process any complete lines after each read
                    process_complete_lines(client, read_buffer);
                } 
                else if (bytes == 0) {
                    // EOF - client closed connection
                    LOG_INFO("Client " << client->fd() << " disconnected (EOF)");
                    // Process any complete lines before returning
                    process_complete_lines(client, read_buffer);
                    co_return;
                }
                else {
                    // Error or would block
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // No more data available right now, break out of read loop
                        LOG_DEBUG("Client " << client->fd() << " EAGAIN/EWOULDBLOCK");
                        break;
                    } else {
                        // Real error
                        LOG_INFO("Client " << client->fd() << " read error: " << strerror(errno));
                        co_return;
                    }
                }
            }
        }
    }

    LOG_INFO("Client " << client->fd() << " read coroutine completed");
    co_return;
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
 * Write coroutine - Handles writing in a blocking-style
 * This coroutine is created on-demand when there's data to write
 */
Task handle_client_write(Client* client) {
    LOG_INFO("Client " << client->fd() << " write coroutine started");

    // First, try to write immediately without waiting
    if (client->try_write()) {
        LOG_DEBUG("Client " << client->fd() << " immediate write successful, completing");
        co_return; // No need to wait for EPOLLOUT if everything was written
    }

    // Continue writing as long as we have data and client is connected
    while (!client->is_closed() && client->has_write_data()) {
        LOG_DEBUG("Client " << client->fd() << " waiting for write events");

        // Wait for write events in a blocking style
        uint32_t events = co_await EpollAwaitable(client, true);

        // Check for disconnect events
        if (events & (EPOLLHUP | EPOLLERR)) {
            LOG_INFO("Client " << client->fd() << " disconnected during write (HUP/ERR)");
            co_return;
        }

        // If we have EPOLLOUT event, try to write again
        if (events & EPOLLOUT) {
            // If try_write successfully writes everything, we can exit the loop
            if (client->try_write()) {
                LOG_DEBUG("Client " << client->fd() << " all data written");
                break;
            }
        }
    }

    // When we're done writing, disable the EPOLLOUT flag
    if (!client->is_closed()) {
        client->disable_writing();
    }

    LOG_INFO("Client " << client->fd() << " write coroutine completed");
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

        // Set send buffer size to 4K
        set_socket_send_buffer_size(server_fd_, 4096);

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
                std::cerr << "Accept error: " << strerror(errno) << std::endl;
                break;
            }

            // Create client and start its coroutine
            auto client = std::make_shared<Client>(client_fd, epoll_fd_, 
                [this](int sender_fd, const std::string& msg) {
                    broadcast_message(sender_fd, msg);
                });

            // Create and start the read coroutine
            client->start_coroutine(handle_client_read(client.get()), false);

            // Add the client to our map
            clients_[client_fd] = client;
            LOG_INFO("New client connected: " << client_fd);
        }
    }

    void broadcast_message(int sender_fd, const std::string& message) {
        auto it = clients_.find(sender_fd);
        if (it == clients_.end()) {
            LOG_INFO("broadcast_message exit because sender_fd: " << sender_fd << " not found");
            return;
        }

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