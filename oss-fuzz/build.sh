#!/bin/bash -eu
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$CFLAGS -fno-omit-frame-pointer" \
    -DCMAKE_CXX_FLAGS="$CXX_FLAGS -fno-omit-frame-pointer" \
    -DCMEM_BUILD_TESTS=ON \
    -DCMEM_BUILD_BENCHMARKS=OFF
cmake --build build --parallel
cp build/fuzz_alloc $OUT/
cp -r corpus $OUT/fuzz_alloc_corpus
