FROM gcr.io/oss-fuzz-base/base-builder
REQUIRES_COMPILER "clang"
RUN apt-get update && apt-get install -y cmake ninja-build
COPY . /src/cmem
WORKDIR /src/cmem
ENV COVERAGE_FLAGS="-fsanitize=coverage,trace-pc -fno-omit-frame-pointer"
RUN mkdir -p build && cd build \
    && cmake .. -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="$COVERAGE_FLAGS" \
        -DCMAKE_CXX_FLAGS="$COVERAGE_FLAGS" \
        -DCMEM_BUILD_TESTS=ON \
        -DCMEM_BUILD_BENCHMARKS=OFF \
        -DCMAKE_FUZZING_BUILD_MODE_UNSET_AS_TSAN=ON \
    && cmake --build . --parallel
COPY oss-fuzz/*.sh /src/
COPY oss-fuzz/fuzz_targets/ /src/fuzz_targets/
