/**
 * Simplified Chat Server - Boost.Asio with C++20 Coroutines
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

namespace asio = boost::asio;
using asio::ip::tcp;
using asio::awaitable;
using asio::co_spawn;
using asio::detached;
using asio::use_awaitable;

// Generate a random nickname (4 alphanumeric chars by default)
std::string random_nickname(size_t length = 4) {
    const std::string chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dist(0, chars.size() - 1);

    std::string result(length, ' ');
    for (size_t i = 0; i < length; i++) {
        result[i] = chars[dist(gen)];
    }
    return result;
}

// Forward declaration
class chat_room;

// Represents a single client connection
class chat_participant : public std::enable_shared_from_this<chat_participant> {
public:
    chat_participant(tcp::socket socket, chat_room& room) 
        : socket_(std::move(socket)), 
          room_(room), 
          username_(random_nickname()) {}

    // Start processing this connection
    void start() {
        deliver("Welcome to the chat server!\r\n");
        co_spawn(socket_.get_executor(), reader(), detached);
    }

    // Send a message to this client
    void deliver(std::string message) {
        bool write_in_progress = !write_queue_.empty();
        write_queue_.push(std::move(message));

        if (!write_in_progress) {
            co_spawn(socket_.get_executor(), writer(), detached);
        }
    }

    const std::string& username() const { return username_; }
    void username(const std::string& new_name) { username_ = new_name; }

private:
    // Read messages from client
    awaitable<void> reader();

    // Write messages to client
    awaitable<void> writer() {
        try {
            while (!write_queue_.empty()) {
                co_await asio::async_write(
                    socket_,
                    asio::buffer(write_queue_.front()),
                    use_awaitable
                );
                write_queue_.pop();
            }
        } 
        catch (std::exception&) {
            stop();
        }
        co_return;
    }

    // Process a line received from client
    void process_line(const std::string& line);

    // Handle a command (starting with /)
    void handle_command(const std::string& command) {
        if (command.substr(0, 6) == "/nick " && command.length() > 6) {
            std::string new_name = command.substr(6);
            std::string old_name = username_;
            username_ = new_name;
            std::cout << "Client renamed from " << old_name << " to " << new_name << std::endl;
            deliver("Nickname changed to: " + new_name + "\r\n");
        }
    }

    // Stop and remove this client
    void stop();

    tcp::socket socket_;
    chat_room& room_;
    std::string username_;
    std::queue<std::string> write_queue_;
    std::string read_buffer_;
};

// Manages all connected clients
class chat_room {
public:
    // Add a new participant to the room
    void join(std::shared_ptr<chat_participant> participant) {
        participants_.push_back(participant);
        std::cout << "Client connected: " << participant->username() << std::endl;
    }

    // Remove a participant from the room
    void leave(std::shared_ptr<chat_participant> participant) {
        auto it = std::find(participants_.begin(), participants_.end(), participant);
        if (it != participants_.end()) {
            std::cout << "Client disconnected: " << participant->username() << std::endl;
            participants_.erase(it);
        }
    }

    // Broadcast a message to all participants except the sender
    void broadcast(std::shared_ptr<chat_participant> sender, const std::string& message) {
        std::string formatted = sender->username() + ": " + message + "\r\n";
        std::cout << "Broadcasting: " << formatted;

        for (auto& participant : participants_) {
            if (participant != sender) {
                participant->deliver(formatted);
            }
        }
    }

private:
    std::vector<std::shared_ptr<chat_participant>> participants_;
};

// Implementation of participant methods that need the complete chat_room definition
void chat_participant::stop() {
    room_.leave(shared_from_this());
}

awaitable<void> chat_participant::reader() {
    try {
        for (;;) {
            // Read until newline
            std::size_t n = co_await asio::async_read_until(
                socket_,
                asio::dynamic_buffer(read_buffer_),
                '\n',
                use_awaitable
            );

            // Extract and process the line
            std::string line(read_buffer_.substr(0, n));
            read_buffer_.erase(0, n);

            // Remove CR/LF
            if (!line.empty() && line[line.size() - 2] == '\r')
                line.erase(line.size() - 2, 1);
            if (!line.empty() && line.back() == '\n')
                line.pop_back();

            process_line(line);
        }
    } 
    catch (std::exception&) {
        stop();
    }
    co_return;
}

void chat_participant::process_line(const std::string& line) {
    std::cout << "Received from " << username_ << ": " << line << std::endl;

    if (!line.empty() && line[0] == '/') {
        handle_command(line);
    } else {
        room_.broadcast(shared_from_this(), line);
    }
}

// Chat server accepts new connections and creates participants
class chat_server {
public:
    chat_server(asio::io_context& io_context, uint16_t port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
        std::cout << "Server listening on port " << port << std::endl;
        co_spawn(io_context, listener(), detached);
    }

private:
    awaitable<void> listener() {
        for (;;) {
            // Wait for a new connection
            tcp::socket socket = co_await acceptor_.async_accept(use_awaitable);

            // Create and start a new participant
            auto participant = std::make_shared<chat_participant>(std::move(socket), room_);
            room_.join(participant);
            participant->start();
        }
    }

    tcp::acceptor acceptor_;
    chat_room room_;
};

int main(int argc, char* argv[]) {
    try {
        unsigned short port = 12345;
        if (argc > 1)
            port = std::stoi(argv[1]);

        asio::io_context io_context;

        // Create and start the server
        chat_server server(io_context, port);

        // Run with multiple threads for better performance
        const int thread_count = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
        std::vector<std::thread> threads;

        // Create additional threads (current thread will be used too)
        for (int i = 1; i < thread_count; ++i) {
            threads.emplace_back([&io_context]() { io_context.run(); });
        }

        // Run in the current thread too
        io_context.run();

        // Join threads (should never get here under normal operation)
        for (auto& thread : threads)
            thread.join();
    }
    catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}