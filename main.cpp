#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <queue>
#include <random>
#include <algorithm>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <errno.h>

// Constants for trySendData return values
const int SEND_RESULT_ERROR = -1;    // Error occurred, connection should be closed
const int SEND_RESULT_PENDING = 0;   // More data pending, socket buffer full
const int SEND_RESULT_COMPLETE = 1;  // All data sent successfully

// Global epoll file descriptor for use in functions
int g_epollFd = -1;

// Forward declarations
class Client;
class ClientManager;
void handleClientDisconnection(int clientFd, int epollFd, ClientManager& clientManager);
void updateClientEpollEvents(int clientFd, uint32_t events);

// Client class definition
class Client {
public:
    int fd;
    std::string nick;
    std::string buffer;
    std::queue<std::string> outgoingQueue;  // Queue for outgoing messages
    std::string currentSendBuffer;         // Current message being sent

    Client(int fd) : fd(fd) {
        nick = generateRandomNick(4);
    }

    ~Client() {
        close(fd);
    }

    // Add a message to the outgoing queue
    void queueMessage(const std::string& message) {
        outgoingQueue.push(message);
    }

    // Check if client has pending data to send
    bool hasDataToSend() const {
        return !currentSendBuffer.empty() || !outgoingQueue.empty();
    }

    // Try to send data from queue, returns status code indicating result
    int trySendData() {
        // If currentSendBuffer is empty but queue isn't, move a message to currentSendBuffer
        if (currentSendBuffer.empty() && !outgoingQueue.empty()) {
            currentSendBuffer = outgoingQueue.front();
            outgoingQueue.pop();
        }

        if (currentSendBuffer.empty()) {
            return SEND_RESULT_COMPLETE; // No data to send, all done
        }

        ssize_t sent = send(fd, currentSendBuffer.c_str(), currentSendBuffer.size(), 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return SEND_RESULT_PENDING; // Socket buffer full, try again later
            }
            // Other error with the TCP connection
            std::cerr << "Socket error on fd " << fd << ": " << strerror(errno) << std::endl;
            return SEND_RESULT_ERROR; // Signal that connection should be closed
        }

        // If we sent part of the message, keep the rest for later
        if (static_cast<size_t>(sent) < currentSendBuffer.size()) {
            currentSendBuffer = currentSendBuffer.substr(sent);
            return SEND_RESULT_PENDING; // More data pending
        }

        // Message completely sent
        currentSendBuffer.clear();

        // Check if there's more in the queue
        return outgoingQueue.empty() ? SEND_RESULT_COMPLETE : SEND_RESULT_PENDING;
    }

private:
    std::string generateRandomNick(int length) {
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
};

// ClientManager class definition
class ClientManager {
private:
    std::unordered_map<int, Client*> clients;

public:
    ~ClientManager() {
        for (auto& pair : clients) {
            delete pair.second;
        }
    }

    void addClient(Client* client) {
        clients[client->fd] = client;
        std::cout << "Client added: " << client->nick << std::endl;
    }

    void removeClient(int fd) {
        if (clients.find(fd) != clients.end()) {
            std::string nick = clients[fd]->nick;
            delete clients[fd];
            clients.erase(fd);
            std::cout << "Client removed: " << nick << std::endl;
        }
    }

    void sendMessage(int senderFd, const std::string& message) {
        std::string senderNick;

        if (clients.find(senderFd) != clients.end()) {
            senderNick = clients[senderFd]->nick;
        } else {
            return;
        }

        std::string fullMessage = senderNick + ": " + message + "\r\n";

        for (auto& pair : clients) {
            if (pair.first != senderFd) {
                Client* client = pair.second;

                // Queue the message instead of sending immediately
                client->queueMessage(fullMessage);

                // If this is the first message in the queue, we need to register for EPOLLOUT
                if (client->outgoingQueue.size() == 1 && client->currentSendBuffer.empty()) {
                    updateClientEpollEvents(client->fd, EPOLLIN | EPOLLOUT | EPOLLET);
                }
            }
        }
    }

    bool handleCommand(int fd, const std::string& command) {
        if (command.empty() || command[0] != '/') {
            return false;
        }

        size_t spacePos = command.find(' ');
        if (spacePos == std::string::npos) {
            return true;  // Just a command without arguments
        }

        std::string cmd = command.substr(0, spacePos);
        std::string args = command.substr(spacePos + 1);

        if (cmd == "/nick") {
            if (clients.find(fd) != clients.end()) {
                std::string oldNick = clients[fd]->nick;
                clients[fd]->nick = args;
                std::cout << "Client renamed from " << oldNick << " to " << args << std::endl;
            }
            return true;
        }

        return true;
    }

    Client* getClient(int fd) {
        auto it = clients.find(fd);
        return it != clients.end() ? it->second : nullptr;
    }
};

// Set socket to non-blocking mode
void setNonBlocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
}

// Update epoll events for a client socket
void updateClientEpollEvents(int clientFd, uint32_t events) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = clientFd;

    // Modify existing events
    if (epoll_ctl(g_epollFd, EPOLL_CTL_MOD, clientFd, &ev) < 0) {
        std::cerr << "Failed to modify epoll events for client: " << strerror(errno) << std::endl;
    }
}

// Handle client data ready for sending
void handleClientWrite(int clientFd, ClientManager& clientManager) {
    Client* client = clientManager.getClient(clientFd);
    if (!client) {
        return;
    }

    // Try to send queued data
    int result = client->trySendData();

    if (result == SEND_RESULT_COMPLETE) {
        // All data sent, stop monitoring for EPOLLOUT
        updateClientEpollEvents(clientFd, EPOLLIN | EPOLLET);
    } else if (result == SEND_RESULT_ERROR) {
        // Error occurred, close the connection
        handleClientDisconnection(clientFd, g_epollFd, clientManager);
    }
    // If result == SEND_RESULT_PENDING, keep monitoring for EPOLLOUT
}

// Handle new client connection
void handleNewConnection(int serverFd, int epollFd, ClientManager& clientManager) {
    struct sockaddr_in clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);
    int clientFd = accept(serverFd, (struct sockaddr*)&clientAddr, &clientAddrLen);

    if (clientFd < 0) {
        std::cerr << "Accept failed" << std::endl;
        return;
    }

    setNonBlocking(clientFd);

    // Add client socket to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;  // Edge triggered
    ev.data.fd = clientFd;
    if (epoll_ctl(epollFd, EPOLL_CTL_ADD, clientFd, &ev) < 0) {
        std::cerr << "Failed to add client socket to epoll" << std::endl;
        close(clientFd);
        return;
    }

    Client* client = new Client(clientFd);
    clientManager.addClient(client);
}

// Process a single line from client
void processClientLine(int clientFd, const std::string& line, ClientManager& clientManager) {
    if (!line.empty() && line[0] == '/') {
        clientManager.handleCommand(clientFd, line);
    } else {
        clientManager.sendMessage(clientFd, line);
    }
}

// Handle client disconnection
void handleClientDisconnection(int clientFd, int epollFd, ClientManager& clientManager) {
    epoll_ctl(epollFd, EPOLL_CTL_DEL, clientFd, nullptr);
    clientManager.removeClient(clientFd);
}

// Handle client data received
void handleClientData(int clientFd, int epollFd, char* buffer, size_t bufferSize, ClientManager& clientManager) {
    ssize_t bytesRead = read(clientFd, buffer, bufferSize - 1);

    if (bytesRead <= 0) {
        // Connection closed or error
        if (bytesRead < 0) {
            std::cerr << "Read error" << std::endl;
        }
        handleClientDisconnection(clientFd, epollFd, clientManager);
        return;
    }

    buffer[bytesRead] = '\0';

    Client* client = clientManager.getClient(clientFd);
    if (!client) {
        return;
    }

    // Append to client buffer and process lines
    client->buffer += buffer;

    // Process complete lines
    size_t pos;
    while ((pos = client->buffer.find('\n')) != std::string::npos) {
        std::string line = client->buffer.substr(0, pos);
        // Remove possible \r
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        client->buffer = client->buffer.substr(pos + 1);
        std::cout << "Received: " << line << std::endl;

        processClientLine(clientFd, line, clientManager);
    }
}

int main() {
    const int PORT = 12345;
    const int MAX_EVENTS = 64;
    const int MAX_BUFFER_SIZE = 1024;

    // Create socket
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return 1;
    }

    // Set socket options
    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Set to non-blocking mode
    setNonBlocking(serverFd);

    // Bind
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(serverFd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        std::cerr << "Bind failed" << std::endl;
        close(serverFd);
        return 1;
    }

    // Listen
    if (listen(serverFd, SOMAXCONN) < 0) {
        std::cerr << "Listen failed" << std::endl;
        close(serverFd);
        return 1;
    }

    std::cout << "Server is listening on 127.0.0.1:" << PORT << std::endl;

    // Create epoll instance
    int epollFd = epoll_create1(0);
    if (epollFd < 0) {
        std::cerr << "Failed to create epoll instance" << std::endl;
        close(serverFd);
        return 1;
    }

    g_epollFd = epollFd; // Set global epoll file descriptor

    // Add server socket to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = serverFd;
    if (epoll_ctl(epollFd, EPOLL_CTL_ADD, serverFd, &ev) < 0) {
        std::cerr << "Failed to add server socket to epoll" << std::endl;
        close(serverFd);
        close(epollFd);
        return 1;
    }

    struct epoll_event events[MAX_EVENTS];
    char buffer[MAX_BUFFER_SIZE];

    ClientManager clientManager;

    while (true) {
        int numEvents = epoll_wait(epollFd, events, MAX_EVENTS, -1);

        for (int i = 0; i < numEvents; i++) {
            const int currentFd = events[i].data.fd;
            const uint32_t currentEvents = events[i].events;

            if (currentFd == serverFd) {
                // New client connection
                handleNewConnection(serverFd, epollFd, clientManager);
            }
            else {
                // Client socket events
                if (currentEvents & EPOLLIN) {
                    handleClientData(currentFd, epollFd, buffer, MAX_BUFFER_SIZE, clientManager);
                }

                if (currentEvents & EPOLLOUT) {
                    handleClientWrite(currentFd, clientManager);
                }

                if (currentEvents & (EPOLLERR | EPOLLHUP)) {
                    // Error or hang up
                    handleClientDisconnection(currentFd, epollFd, clientManager);
                }
            }
        }
    }

    close(epollFd);
    close(serverFd);

    return 0;
}