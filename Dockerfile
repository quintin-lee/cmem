FROM ubuntu:22.04

LABEL maintainer="cmem contributors"
LABEL description="Universal High-Performance Tiered Memory Manager build environment"

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    clang \
    cmake \
    ninja-build \
    git \
    python3 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

COPY . /build

RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build \
    && ctest --test-dir build --output-on-failure

# Default to an interactive shell
CMD ["/bin/bash"]
