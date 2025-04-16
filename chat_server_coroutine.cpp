/**
 * Chat Server - Implemented using epoll and C++20 coroutines
 * 
 * This implementation combines the efficiency of epoll with the
 * readability of coroutines to create a high-performance, easy-to-understand
 * chat server.
 */
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <queue>
#include <random>
#include <memory>
#include <algorithm>
#include <coroutine>
#include <exception>
#include <optional>
#include <functional>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <errno.h>

// Set socket to non-blocking mode
void setNonBlocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * Task<T> - A simple coroutine task with a result of type T
 */
template<typename T = void>
class Task {
public:
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    struct promise_type {
        std::optional<T> result;
        std::exception_ptr exception;

        Task get_return_object() {
            return Task(handle_type::from_promise(*this));
        }

        std::suspend_never initial_suspend() noexcept {
            return {};
        }

        auto final_suspend() noexcept {
            struct FinalAwaiter {
                bool await_ready() noexcept { return false; }
                void await_resume() noexcept {}
                std::coroutine_handle<> await_suspend(handle_type h) noexcept {
                    return h.done() ? std::noop_coroutine() : std::coroutine_handle<>{};
                }
            };
            return FinalAwaiter{};
        }

        template<typename U>
        void return_value(U&& value) {
            result = std::forward<U>(value);
        }

        void unhandled_exception() {
            exception = std::current_exception();
        }
    };

    // Default constructor - creates an empty task with null handle
    Task() : handle_(nullptr) {}

    Task(handle_type h) : handle_(h) {}

    Task(Task&& t) noexcept : handle_(t.handle_) {
        t.handle_ = nullptr;
    }

    // Add move assignment operator
    Task& operator=(Task&& t) noexcept {
        if (this != &t) {
            if (handle_) handle_.destroy();
            handle_ = t.handle_;
            t.handle_ = nullptr;
        }
        return *this;
    }

    ~Task() {
        if (handle_) handle_.destroy();
    }

    T result() const {
        if (handle_->exception)
            std::rethrow_exception(handle_->exception);
        if (!handle_->result.has_value())
            throw std::runtime_error("Task has no result");
        return *handle_->result;
    }

    bool done() const {
        return handle_ ? handle_.done() : true;
    }

private:
    handle_type handle_;
};

// Specialization for void return type
template<>
struct Task<void>::promise_type {
    std::exception_ptr exception;

    Task<void> get_return_object() {
        return Task<void>(handle_type::from_promise(*this));
    }

    std::suspend_never initial_suspend() noexcept { 
        return {}; 
    }

    auto final_suspend() noexcept {
        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            void await_resume() noexcept {}
            std::coroutine_handle<> await_suspend(handle_type h) noexcept {
                return h.done() ? std::noop_coroutine() : std::coroutine_handle<>{};
            }
        };
        return FinalAwaiter{};
    }

    void return_void() {}

    void unhandled_exception() {
        exception = std::current_exception();
    }
};

/**
 * Event Handler Interface - Base class for objects that can be resumed by epoll events
 */
class EventHandler {
public:
    virtual ~EventHandler() = default;
    virtual void resume() = 0;
    virtual int get_fd() const = 0;
};

/**
 * EpollContext - Manages epoll event registration
 */
class EpollContext {
public:
    explicit EpollContext(int epoll_fd) : epoll_fd_(epoll_fd) {}

    int epoll_fd() const { return epoll_fd_; }

    void register_fd_for_read(int fd, EventHandler* handler) {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.ptr = handler;

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            throw std::runtime_error("Failed to add fd to epoll: " + std::string(strerror(errno)));
        }
    }

    void register_fd_for_write(int fd, EventHandler* handler) {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        ev.data.ptr = handler;

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
            throw std::runtime_error("Failed to modify fd for write in epoll: " + std::string(strerror(errno)));
        }
    }

    void register_server_socket(int fd, EventHandler* handler) {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.ptr = handler;

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            throw std::runtime_error("Failed to add server socket to epoll: " + std::string(strerror(errno)));
        }
    }

    void set_read_only(int fd, EventHandler* handler) {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.ptr = handler;

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
            throw std::runtime_error("Failed to modify fd in epoll: " + std::string(strerror(errno)));
        }
    }

    void unregister_fd(int fd) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    }

    // Process events with direct handler access
    int process_events(int timeout_ms = -1) {
        const int MAX_EVENTS = 64;
        struct epoll_event events[MAX_EVENTS];

        int num_events = epoll_wait(epoll_fd_, events, MAX_EVENTS, timeout_ms);

        if (num_events < 0) {
            if (errno == EINTR) return 0; // Interrupted, not an error
            throw std::runtime_error("epoll_wait failed: " + std::string(strerror(errno)));
        }

        for (int i = 0; i < num_events; i++) {
            auto* handler = static_cast<EventHandler*>(events[i].data.ptr);
            if (handler) {
                handler->resume();
            }
        }

        return num_events;
    }

private:
    int epoll_fd_;
};

/**
 * ReadAwaiter - Awaitable for reading from a socket
 */
class ReadAwaiter : public EventHandler {
public:
    ReadAwaiter(int fd, EpollContext& epoll_ctx) 
        : fd_(fd), epoll_ctx_(epoll_ctx), ready_(false), handler_(nullptr) {}

    bool await_ready() const noexcept { return ready_; }

    void await_suspend(std::coroutine_handle<> h) {
        handler_ = h;
        epoll_ctx_.register_fd_for_read(fd_, this);
    }

    int await_resume() noexcept { 
        return fd_; 
    }

    // EventHandler interface
    void resume() override {
        if (handler_) {
            ready_ = true;
            auto h = handler_;
            handler_ = nullptr;
            h.resume();
            // Reset ready_ after resuming so next co_await will suspend properly
            ready_ = false;
        }
    }

    int get_fd() const override { return fd_; }

private:
    int fd_;
    EpollContext& epoll_ctx_;
    bool ready_;
    std::coroutine_handle<> handler_;
};

/**
 * WriteAwaiter - Awaitable for writing to a socket
 */
class WriteAwaiter : public EventHandler {
public:
    WriteAwaiter(int fd, EpollContext& epoll_ctx) 
        : fd_(fd), epoll_ctx_(epoll_ctx), ready_(true), handler_(nullptr) {}

    bool await_ready() const noexcept { return ready_; }

    void await_suspend(std::coroutine_handle<> h) {
        handler_ = h;
        epoll_ctx_.register_fd_for_write(fd_, this);
    }

    int await_resume() noexcept { 
        return fd_; 
    }

    // Set ready state to false - call this when a write operation fails with EAGAIN
    void set_not_ready() {
        ready_ = false;
    }

    // EventHandler interface
    void resume() override {
        if (handler_) {
            ready_ = true;
            auto h = handler_;
            handler_ = nullptr;
            h.resume();
        }
    }

    int get_fd() const override { return fd_; }

private:
    int fd_;
    EpollContext& epoll_ctx_;
    bool ready_;
    std::coroutine_handle<> handler_;
};

/**
 * AcceptAwaiter - Specialized awaiter for accept connections
 */
class AcceptAwaiter : public EventHandler {
public:
    AcceptAwaiter(int fd, EpollContext& epoll_ctx) 
        : fd_(fd), epoll_ctx_(epoll_ctx), ready_(false), handler_(nullptr) {}

    // Move constructor
    AcceptAwaiter(AcceptAwaiter&& other) noexcept
        : fd_(other.fd_), 
          epoll_ctx_(other.epoll_ctx_), 
          ready_(other.ready_), 
          handler_(other.handler_) {
        other.handler_ = nullptr;
    }

    // Move assignment operator
    AcceptAwaiter& operator=(AcceptAwaiter&& other) noexcept {
        if (this != &other) {
            fd_ = other.fd_;
            // No need to change epoll_ctx_ reference
            ready_ = other.ready_;
            handler_ = other.handler_;
            other.handler_ = nullptr;
        }
        return *this;
    }

    bool await_ready() const noexcept { return ready_; }

    void await_suspend(std::coroutine_handle<> h) {
        handler_ = h;
        // Server socket stays registered continuously
    }

    int await_resume() noexcept { 
        return fd_; 
    }

    // EventHandler interface
    void resume() override {
        if (handler_) {
            ready_ = true;
            auto h = handler_;
            handler_ = nullptr;
            h.resume();
        }
    }

    int get_fd() const override { return fd_; }

private:
    int fd_;
    EpollContext& epoll_ctx_;
    bool ready_;
    std::coroutine_handle<> handler_;
};

/**
 * Client - Represents a connected client using coroutines for I/O
 */
class Client {
public:
    Client(int fd, EpollContext& epoll_ctx) 
        : fd_(fd), epoll_ctx_(epoll_ctx), closed_(false),
          read_awaiter_(fd, epoll_ctx), write_awaiter_(fd, epoll_ctx) {
        nickname_ = generateRandomNick(4);
    }

    ~Client() {
        close();
    }

    void close() {
        if (!closed_) {
            epoll_ctx_.unregister_fd(fd_);
            ::close(fd_);
            closed_ = true;
        }
    }

    // Coroutine that processes client messages
    Task<> process() {
        const size_t BUFFER_SIZE = 1024;
        char buffer[BUFFER_SIZE];

        try {
            while (true) {
                // Wait for data to be available to read
                co_await read_awaiter_;

                ssize_t bytesRead = read(fd_, buffer, BUFFER_SIZE - 1);
                if (bytesRead <= 0) {
                    if (bytesRead < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        continue; // No data available, loop back and wait
                    }
                    break; // Connection closed or error
                }

                buffer[bytesRead] = '\0';
                receive_buffer_ += buffer;

                // Process complete lines
                process_lines();
            }
        } catch (const std::exception& e) {
            std::cerr << "Error processing client " << fd_ << ": " << e.what() << std::endl;
        }

        // Notify of disconnection
        if (on_disconnect) {
            on_disconnect(fd_);
        }
        co_return;
    }

    // Send a message to this client
    Task<bool> send_message(const std::string& message) {
        outgoing_queue_.push(message);

        // If this is the first message, start the send coroutine
        if (outgoing_queue_.size() == 1 && current_send_buffer_.empty()) {
            send_task_ = send_loop();
        }

        co_return true;
    }

    int fd() const { return fd_; }
    const std::string& nickname() const { return nickname_; }
    void set_nickname(const std::string& nickname) { nickname_ = nickname; }

    // Set callbacks for messaging and disconnection
    using DisconnectCallback = std::function<void(int)>;
    using MessageCallback = std::function<void(int, const std::string&)>;

    void set_disconnect_callback(DisconnectCallback callback) {
        on_disconnect = std::move(callback);
    }

    void set_message_callback(MessageCallback callback) {
        on_message = std::move(callback);
    }

private:
    void process_lines() {
        size_t pos;
        while ((pos = receive_buffer_.find('\n')) != std::string::npos) {
            std::string line = receive_buffer_.substr(0, pos);
            // Remove possible \r
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            receive_buffer_ = receive_buffer_.substr(pos + 1);

            // Check for commands
            if (!line.empty() && line[0] == '/') {
                process_command(line);
            } else if (!line.empty() && on_message) {
                // Normal message
                on_message(fd_, line);
            }
        }
    }

    void process_command(const std::string& command) {
        size_t spacePos = command.find(' ');
        if (spacePos == std::string::npos) return;

        std::string cmd = command.substr(1, spacePos - 1);
        std::string args = command.substr(spacePos + 1);

        if (cmd == "nick" && !args.empty()) {
            std::string old_nick = nickname_;
            nickname_ = args;
            std::cout << "Client renamed from " << old_nick << " to " << args << std::endl;
        }
    }

    // Coroutine that handles sending messages
    Task<> send_loop() {
        try {
            while (!outgoing_queue_.empty() || !current_send_buffer_.empty()) {
                // Get next message if needed
                if (current_send_buffer_.empty() && !outgoing_queue_.empty()) {
                    current_send_buffer_ = outgoing_queue_.front();
                    outgoing_queue_.pop();
                }

                // Wait until socket is writable
                co_await write_awaiter_;

                // Try to send data
                ssize_t sent = send(fd_, current_send_buffer_.c_str(), current_send_buffer_.size(), 0);
                if (sent < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // Socket buffer full, mark as not ready and wait for epoll
                        write_awaiter_.set_not_ready();
                        continue; // Try again after resuming
                    }
                    // Other error
                    throw std::runtime_error("Socket send error: " + std::string(strerror(errno)));
                }

                // Handle partial sends
                if (static_cast<size_t>(sent) < current_send_buffer_.size()) {
                    current_send_buffer_ = current_send_buffer_.substr(sent);
                    // After partial write, we'll likely hit EAGAIN soon
                    // Be optimistic but prepared to wait
                } else {
                    current_send_buffer_.clear();
                }

                // Restore EPOLLIN-only if no more data to send
                if (current_send_buffer_.empty() && outgoing_queue_.empty()) {
                    epoll_ctx_.set_read_only(fd_, &read_awaiter_);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error sending to client " << fd_ << ": " << e.what() << std::endl;
            if (on_disconnect) {
                on_disconnect(fd_);
            }
        }

        co_return;
    }

    std::string generateRandomNick(size_t length) {
        static const std::string chars = 
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> dist(0, chars.size() - 1);

        std::string result(length, ' ');
        for (size_t i = 0; i < length; i++) {
            result[i] = chars[dist(gen)];
        }
        return result;
    }

    int fd_;
    EpollContext& epoll_ctx_;
    bool closed_;
    std::string nickname_;
    std::string receive_buffer_;
    std::queue<std::string> outgoing_queue_;
    std::string current_send_buffer_;

    DisconnectCallback on_disconnect;
    MessageCallback on_message;

    // Dedicated awaiters for this client
    ReadAwaiter read_awaiter_;
    WriteAwaiter write_awaiter_;

    Task<> send_task_;
};

/**
 * ChatServer - Main server class
 */
class ChatServer {
public:
    ChatServer(int port) 
        : port_(port), running_(false), epoll_ctx_(create_epoll()), 
          accept_awaiter_(-1, epoll_ctx_) {
    }

    ~ChatServer() {
        stop();
    }

    bool start() {
        // Create server socket
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            std::cerr << "Failed to create socket" << std::endl;
            return false;
        }

        // Set socket options
        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setNonBlocking(server_fd_);

        // Bind
        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port_);

        if (bind(server_fd_, (struct sockaddr *)&address, sizeof(address)) < 0) {
            std::cerr << "Bind failed" << std::endl;
            close(server_fd_);
            return false;
        }

        // Listen
        if (listen(server_fd_, SOMAXCONN) < 0) {
            std::cerr << "Listen failed" << std::endl;
            close(server_fd_);
            return false;
        }

        // Create new awaiter for the server socket now that we have a valid fd
        accept_awaiter_ = AcceptAwaiter(server_fd_, epoll_ctx_);

        // Register server socket with epoll
        epoll_ctx_.register_server_socket(server_fd_, &accept_awaiter_);

        // Start the accept coroutine
        accept_task_ = accept_connections();

        std::cout << "Server started on port " << port_ << std::endl;
        running_ = true;
        return true;
    }

    void run() {
        if (!running_) {
            std::cerr << "Server not started" << std::endl;
            return;
        }

        // Main event loop - just processes epoll events
        // Events now directly trigger the respective coroutines
        try {
            while (running_) {
                epoll_ctx_.process_events();
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in event loop: " << e.what() << std::endl;
        }
    }

    void stop() {
        running_ = false;

        // Close all client connections
        for (const auto& [fd, client] : clients_) {
            client->close();
        }
        clients_.clear();

        // Close server socket
        if (server_fd_ >= 0) {
            close(server_fd_);
            server_fd_ = -1;
        }
    }

private:
    // Create epoll instance
    EpollContext create_epoll() {
        int epoll_fd = epoll_create1(0);
        if (epoll_fd < 0) {
            throw std::runtime_error("Failed to create epoll instance");
        }
        return EpollContext(epoll_fd);
    }

    // Coroutine that accepts new connections
    Task<> accept_connections() {
        try {
            while (running_) {
                // Wait for connection
                co_await accept_awaiter_;

                // Accept the connection
                struct sockaddr_in client_addr;
                socklen_t client_addr_len = sizeof(client_addr);
                int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_addr_len);

                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        continue; // No pending connections
                    }
                    throw std::runtime_error("Accept failed: " + std::string(strerror(errno)));
                }

                // Set non-blocking
                setNonBlocking(client_fd);

                // Create client and start processing
                auto client = std::make_shared<Client>(client_fd, epoll_ctx_);

                // Set callbacks
                client->set_disconnect_callback([this](int fd) {
                    remove_client(fd);
                });

                client->set_message_callback([this](int sender_fd, const std::string& message) {
                    broadcast_message(sender_fd, message);
                });

                // Store client and start processing
                clients_[client_fd] = client;
                std::cout << "New client connected: " << client->nickname() << " (fd: " << client_fd << ")" << std::endl;

                // Start client processing coroutine
                client->process();
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in accept loop: " << e.what() << std::endl;
        }

        co_return;
    }

    // Remove a client
    void remove_client(int fd) {
        auto it = clients_.find(fd);
        if (it != clients_.end()) {
            std::cout << "Client disconnected: " << it->second->nickname() << " (fd: " << fd << ")" << std::endl;
            clients_.erase(it);
        }
    }

    // Broadcast a message to all clients except sender
    void broadcast_message(int sender_fd, const std::string& message) {
        auto sender_it = clients_.find(sender_fd);
        if (sender_it == clients_.end()) return;

        std::string full_message = sender_it->second->nickname() + ": " + message + "\r\n";

        for (const auto& [fd, client] : clients_) {
            if (fd != sender_fd) {
                client->send_message(full_message);
            }
        }
    }

    int port_;
    int server_fd_ = -1;
    bool running_;
    EpollContext epoll_ctx_;
    std::unordered_map<int, std::shared_ptr<Client>> clients_;
    AcceptAwaiter accept_awaiter_;
    Task<> accept_task_;
};

/**
 * Main function
 */
int main() {
    const int PORT = 12345;

    try {
        ChatServer server(PORT);

        if (!server.start()) {
            return 1;
        }

        server.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}