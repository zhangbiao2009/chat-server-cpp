cmake_minimum_required(VERSION 3.16)

project(chat-server-cpp)

# Set C++20 standard for coroutine support
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(chat-server-cpp chat_server_coroutine.cpp)

# Link with pthread for threading support
find_package(Threads REQUIRED)
target_link_libraries(chat-server-cpp PRIVATE Threads::Threads)