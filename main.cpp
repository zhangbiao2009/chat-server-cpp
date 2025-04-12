#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <random>
#include <algorithm>
#include <mutex>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>

class Client {
public:
    int fd;
    std::string nick;
    std::string buffer;

    Client(int fd) : fd(fd) {
        nick = generateRandomNick(4);
    }

    ~Client() {
        close(fd);
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

class ClientManager {
private:
    std::unordered_map<int, Client*> clients;
    std::mutex mutex;

public:
    ~ClientManager() {
        for (auto& pair : clients) {
            delete pair.second;
        }
    }

    void addClient(Client* client) {
        std::lock_guard<std::mutex> lock(mutex);
        clients[client->fd] = client;
        std::cout << "Client added: " << client->nick << std::endl;
    }

    void removeClient(int fd) {
        std::lock_guard<std::mutex> lock(mutex);
        if (clients.find(fd) != clients.end()) {
            std::string nick = clients[fd]->nick;
            delete clients[fd];
            clients.erase(fd);
            std::cout << "Client removed: " << nick << std::endl;
        }
    }

    void sendMessage(int senderFd, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex);
        std::string senderNick;
        
        if (clients.find(senderFd) != clients.end()) {
            senderNick = clients[senderFd]->nick;
        } else {
            return;
        }
        
        std::string fullMessage = senderNick + ": " + message;
        
        for (auto& pair : clients) {
            if (pair.first != senderFd) {
                send(pair.first, fullMessage.c_str(), fullMessage.size(), 0);
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
            std::lock_guard<std::mutex> lock(mutex);
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
        std::lock_guard<std::mutex> lock(mutex);
        auto it = clients.find(fd);
        return it != clients.end() ? it->second : nullptr;
    }
};

// Set socket to non-blocking mode
void setNonBlocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
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
            if (events[i].data.fd == serverFd) {
                // New client connection
                struct sockaddr_in clientAddr;
                socklen_t clientAddrLen = sizeof(clientAddr);
                int clientFd = accept(serverFd, (struct sockaddr*)&clientAddr, &clientAddrLen);
                
                if (clientFd < 0) {
                    std::cerr << "Accept failed" << std::endl;
                    continue;
                }
                
                setNonBlocking(clientFd);
                
                // Add client socket to epoll
                ev.events = EPOLLIN | EPOLLET;  // Edge triggered
                ev.data.fd = clientFd;
                if (epoll_ctl(epollFd, EPOLL_CTL_ADD, clientFd, &ev) < 0) {
                    std::cerr << "Failed to add client socket to epoll" << std::endl;
                    close(clientFd);
                    continue;
                }
                
                Client* client = new Client(clientFd);
                clientManager.addClient(client);
            }
            else {
                // Client socket is ready for reading
                int clientFd = events[i].data.fd;
                
                if (events[i].events & EPOLLIN) {
                    ssize_t bytesRead = read(clientFd, buffer, MAX_BUFFER_SIZE - 1);
                    
                    if (bytesRead <= 0) {
                        // Connection closed or error
                        if (bytesRead < 0) {
                            std::cerr << "Read error" << std::endl;
                        }
                        epoll_ctl(epollFd, EPOLL_CTL_DEL, clientFd, nullptr);
                        clientManager.removeClient(clientFd);
                    }
                    else {
                        buffer[bytesRead] = '\0';
                        
                        Client* client = clientManager.getClient(clientFd);
                        if (client) {
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
                                
                                if (!line.empty() && line[0] == '/') {
                                    clientManager.handleCommand(clientFd, line);
                                } else {
                                    clientManager.sendMessage(clientFd, line);
                                }
                            }
                        }
                    }
                }
                
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    // Error or hang up
                    epoll_ctl(epollFd, EPOLL_CTL_DEL, clientFd, nullptr);
                    clientManager.removeClient(clientFd);
                }
            }
        }
    }
    
    close(epollFd);
    close(serverFd);
    
    return 0;
}