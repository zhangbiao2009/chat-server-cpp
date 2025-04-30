cmake_minimum_required(VERSION 3.16)

project(chat-server-cpp)

# Set C++20 standard for coroutine support
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add Address Sanitizer for memory error detection
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -g")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")

# Find Boost
find_package(Boost REQUIRED)
include_directories(${Boost_INCLUDE_DIRS})

# Add executables
add_executable(chat-server-cpp chat_server_asio_coroutine.cpp)

# Link with pthread and boost
find_package(Threads REQUIRED)
target_link_libraries(chat-server-cpp PRIVATE Threads::Threads ${Boost_LIBRARIES})