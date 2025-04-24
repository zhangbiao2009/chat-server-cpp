FROM gcc:latest

WORKDIR /app

# Install build dependencies
RUN apt-get update && apt-get install -y cmake llvm clang

COPY . .

# Build with AddressSanitizer support
RUN cmake . && make

# Set environment variables for AddressSanitizer
ENV ASAN_OPTIONS=detect_leaks=1:halt_on_error=0:detect_stack_use_after_return=1

# Run the server
CMD ["./chat-server-cpp"]