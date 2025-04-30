FROM ubuntu:24.04

WORKDIR /app

# Install build dependencies and latest GCC
RUN apt-get update && \
    apt-get install -y build-essential gcc g++ clang cmake llvm wget \
    libboost-system-dev libboost-thread-dev libboost-dev

# (Optional) Install a newer Boost from source if you need Boost >= 1.76
# RUN wget https://boostorg.jfrog.io/artifactory/main/release/1.83.0/source/boost_1_83_0.tar.gz && \
#     tar xzf boost_1_83_0.tar.gz && \
#     cd boost_1_83_0 && \
#     ./bootstrap.sh && \
#     ./b2 install

COPY . .

RUN cmake . && make

ENV ASAN_OPTIONS=detect_leaks=1:halt_on_error=0:detect_stack_use_after_return=1

CMD ["./chat-server-cpp"]