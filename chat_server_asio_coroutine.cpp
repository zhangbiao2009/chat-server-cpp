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
    static const std::string chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> dist(0, chars.size() - 1);

    std::string result(length, ' ');
    for (char& c : result) {
        c = chars[dist(gen)];
    }
    return result;
}

// Forward declarations
class chat_room;
class chat_participant;

// Represents a single client connection
class chat_participant : public std::enable_shared_from_this<chat_participant> {
public:
    chat_participant(tcp::socket socket, chat_room& room) 
        : socket_(std::move(socket)), 
          room_(room), 
          username_(random_nickname()),
          id_(-1) {}

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

    int id() const { return id_; }
    void id(int new_id) { id_ = new_id; }

private:
    // Declarations only - implementations moved outside the class
    awaitable<void> reader();
    awaitable<void> writer();

    tcp::socket socket_;
    chat_room& room_;
    std::string username_;
    int id_;
    std::queue<std::string> write_queue_;
    std::string read_buffer_;
};

// Manages all connected clients
class chat_room {
public:
    chat_room() : next_id_(1) {}

    // Add a new participant to the room
    void join(std::shared_ptr<chat_participant> participant) {
        int id = next_id_++;
        participant->id(id);
        participants_[id] = participant;
        std::cout << "Client connected: ID " << id << std::endl;
    }

    // Remove a participant from the room
    void leave(std::shared_ptr<chat_participant> participant) {
        participants_.erase(participant->id());
    }

    // Broadcast a message to all participants except the sender
    void broadcast(std::shared_ptr<chat_participant> sender, const std::string& message) {
        std::string formatted = sender->username() + ": " + message + "\r\n";

        for (auto& [id, participant] : participants_) {
            if (participant != sender) {
                participant->deliver(formatted);
            }
        }
    }

private:
    std::unordered_map<int, std::shared_ptr<chat_participant>> participants_;
    int next_id_;
};

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
        try {
            for (;;) {
                // Accept new connections
                auto socket = co_await acceptor_.async_accept(use_awaitable);

                // Create and start new participant
                auto participant = std::make_shared<chat_participant>(std::move(socket), room_);
                room_.join(participant);
                participant->start();
            }
        } catch (std::exception& e) {
            std::cerr << "Listener exception: " << e.what() << std::endl;
        }
    }

    tcp::acceptor acceptor_;
    chat_room room_;
};

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

            // Extract the line and process
            std::string line(read_buffer_.substr(0, n));
            read_buffer_.erase(0, n);

            // Trim CR/LF
            if (!line.empty()) {
                if (line.back() == '\n') line.pop_back();
                if (!line.empty() && line.back() == '\r') line.pop_back();
            }

            // Handle the message
            if (!line.empty()) {
                if (line[0] == '/') {
                    // Command handling
                    if (line.substr(0, 6) == "/nick " && line.length() > 6) {
                        username_ = line.substr(6);
                        deliver("Nickname changed to: " + username_ + "\r\n");
                    }
                } else {
                    room_.broadcast(shared_from_this(), line);
                }
            }
        }
    } 
    catch (std::exception&) {
        room_.leave(shared_from_this());
    }
}

awaitable<void> chat_participant::writer() {
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
        room_.leave(shared_from_this());
    }
}

int main(int argc, char* argv[]) {
    try {
        unsigned short port = 12345;
        if (argc > 1)
            port = std::stoi(argv[1]);

        asio::io_context io_context;

        // Create and start the server
        chat_server server(io_context, port);

        // Run in a single thread as requested
        std::cout << "Running server in single-threaded mode" << std::endl;
        io_context.run();
    }
    catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}