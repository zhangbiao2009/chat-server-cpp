/**
 * Chat Server - Using Boost.Asio and C++20 Coroutines
 */
#include <iostream>
#include <string>
#include <unordered_map>
#include <queue>
#include <memory>
#include <random>
#include <coroutine>
#include <functional>
#include <thread>
#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

using boost::asio::ip::tcp;
namespace asio = boost::asio;
using namespace asio::experimental::awaitable_operators;
using asio::awaitable;
using asio::co_spawn;
using asio::detached;
using asio::use_awaitable;

// Random nickname generator
std::string generate_random_nick(int length) {
    static const std::string chars = 
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dist(0, chars.size() - 1);

    std::string result(length, ' ');
    for (int i = 0; i < length; i++) {
        result[i] = chars[dist(gen)];
    }
    return result;
}

// Forward declaration of Session for use in ChatServer
class Session;

// Chat server class definition first
class ChatServer {
public:
    ChatServer(asio::io_context& io_context, short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
        
        std::cout << "Server started on port " << port << std::endl;
        
        // Start accepting connections
        co_spawn(io_context, listener(), detached);
    }

    // Broadcast a message to all clients except the sender
    void broadcast_message(std::shared_ptr<Session> sender, const std::string& message);

    // Remove a session
    void remove_session(std::shared_ptr<Session> session);

private:
    // Connection listener coroutine
    awaitable<void> listener();

    tcp::acceptor acceptor_;
    std::vector<std::shared_ptr<Session>> sessions_;
};

// Session represents a connected client
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, ChatServer& server)
        : socket_(std::move(socket)),
          server_(server),
          username_(generate_random_nick(4)),
          write_strand_(socket_.get_executor()) {
    }

    // Start the session
    void start() {
        // Send welcome message
        send_message("Welcome to the chat server!\r\n");
        
        // Start reading in a coroutine
        asio::co_spawn(
            socket_.get_executor(),
            [self = shared_from_this()]() -> awaitable<void> {
                return self->reader();
            },
            detached
        );
    }

    // Send a message to this client
    void send_message(std::string message) {
        auto self = shared_from_this();
        asio::dispatch(
            write_strand_,
            [self, message = std::move(message)]() mutable {
                bool write_in_progress = !self->write_queue_.empty();
                self->write_queue_.push(std::move(message));
                
                if (!write_in_progress) {
                    // Start the writer coroutine if not already running
                    asio::co_spawn(
                        self->write_strand_,
                        [self]() -> awaitable<void> {
                            return self->writer();
                        },
                        detached
                    );
                }
            }
        );
    }

    // Get the client's username
    const std::string& username() const {
        return username_;
    }

    // Change username
    void set_username(const std::string& name) {
        username_ = name;
    }

private:
    // Read coroutine
    awaitable<void> reader() {
        try {
            std::string read_buffer;
            
            for (;;) {
                // Read until we get a newline
                std::size_t n = co_await asio::async_read_until(
                    socket_,
                    asio::dynamic_buffer(read_buffer),
                    '\n',
                    use_awaitable
                );

                // Extract the line from the buffer
                std::string line(read_buffer.substr(0, n));
                read_buffer.erase(0, n);

                // Remove carriage return if present
                if (!line.empty() && line[line.size() - 2] == '\r') {
                    line.erase(line.size() - 2, 1);
                }
                if (!line.empty() && line.back() == '\n') {
                    line.pop_back();
                }
                
                // Process the line
                process_line(line);
            }
        } 
        catch (std::exception& e) {
            server_.remove_session(shared_from_this());
        }
    }

    // Write coroutine
    awaitable<void> writer() {
        try {
            while (!write_queue_.empty()) {
                // Write the message
                co_await asio::async_write(
                    socket_,
                    asio::buffer(write_queue_.front()),
                    use_awaitable
                );
                
                // Remove it from the queue
                write_queue_.pop();
            }
        } 
        catch (std::exception& e) {
            server_.remove_session(shared_from_this());
        }
    }

    // Process a received line
    void process_line(const std::string& line) {
        std::cout << "Received from " << username_ << ": " << line << std::endl;
        
        // Handle commands
        if (!line.empty() && line[0] == '/') {
            handle_command(line);
        } else {
            // Broadcast message
            server_.broadcast_message(shared_from_this(), line);
        }
    }
    
    // Handle client commands
    void handle_command(const std::string& command) {
        if (command.substr(0, 6) == "/nick ") {
            std::string new_name = command.substr(6);
            if (!new_name.empty()) {
                std::string old_name = username_;
                set_username(new_name);
                std::cout << "Client renamed from " << old_name << " to " << new_name << std::endl;
                send_message("Nickname changed to: " + new_name + "\r\n");
            }
        }
    }

    tcp::socket socket_;
    ChatServer& server_;
    std::string username_;
    asio::strand<asio::any_io_executor> write_strand_;
    std::queue<std::string> write_queue_;
};

// Now implement the ChatServer methods that depend on the full Session definition

// Implement the listener coroutine
awaitable<void> ChatServer::listener() {
    for (;;) {
        // Accept a new connection
        tcp::socket socket = co_await acceptor_.async_accept(use_awaitable);
        
        std::cout << "New connection from: " 
                  << socket.remote_endpoint().address().to_string() 
                  << ":" << socket.remote_endpoint().port() << std::endl;
        
        // Create a new session
        auto session = std::make_shared<Session>(std::move(socket), *this);
        
        // Add to the list of sessions
        sessions_.push_back(session);
        
        // Start session
        session->start();
    }
}

// Implement broadcast_message method
void ChatServer::broadcast_message(std::shared_ptr<Session> sender, const std::string& message) {
    std::string full_message = sender->username() + ": " + message + "\r\n";
    std::cout << "Broadcasting: " << full_message;
    
    for (auto& session : sessions_) {
        if (session != sender) {
            session->send_message(full_message);
        }
    }
}

// Implement remove_session method
void ChatServer::remove_session(std::shared_ptr<Session> session) {
    auto it = std::find(sessions_.begin(), sessions_.end(), session);
    if (it != sessions_.end()) {
        std::cout << "Client disconnected: " << session->username() << std::endl;
        sessions_.erase(it);
    }
}

int main() {
    try {
        asio::io_context io_context(1);
        
        // Create and start the server
        ChatServer server(io_context, 12345);
        
        // Run the I/O service with multiple threads for better performance
        const int thread_count = std::thread::hardware_concurrency();
        std::vector<std::thread> threads;
        
        // The first thread is the current one, start additional threads
        for (int i = 1; i < thread_count; ++i) {
            threads.emplace_back([&io_context]() {
                io_context.run();
            });
        }
        
        // Use the main thread to run the io_context as well
        io_context.run();
        
        // Join all threads
        for (auto& thread : threads) {
            thread.join();
        }
        
    } catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }
    
    return 0;
}