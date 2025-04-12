# Chat Server C++ Project

This project implements a simple chat server in C++. The server is designed to handle multiple clients using asynchronous I/O with the epoll mechanism. Below are the details on how to build and run the application.

## Project Structure

```
chat-server-cpp
├── main.cpp          # Implementation of the chat server
├── Dockerfile        # Docker configuration for building the application
├── CMakeLists.txt    # CMake configuration for building the project
└── README.md         # Documentation for the project
```

## Prerequisites

- Docker (for building and running the application in a container)
- CMake (if building locally)

## Building the Application

To build the application using Docker, run the following command in the project directory:

```bash
docker build -t chat-server-cpp .
```

This command will create a Docker image named `chat-server-cpp`.

## Running the Application

After building the Docker image, you can run the chat server with the following command:

```bash
docker run -p 12345:12345 chat-server-cpp
```

This will start the chat server and expose it on port 12345.

## Usage

Once the server is running, you can connect to it using a telnet client or any other TCP client. Simply connect to `localhost` on port `12345` to start chatting.

## Commands

- To change your nickname, send a command in the format: `/nick new_nickname`.

## Notes

- The server handles multiple clients and broadcasts messages to all connected clients except the sender.
- Ensure that the server is running before attempting to connect with clients.