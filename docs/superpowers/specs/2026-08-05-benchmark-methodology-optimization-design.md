# Spec: Benchmark Methodology Optimization for cmem

## Overview

This specification details the comprehensive architectural and methodology fixes for `benchmarks/bench_main.c` in the `cmem` codebase. The optimizations eliminate measurement bias, align timing metrics with actual operation counts, add warm-up loops and touch writes, implement multi-run median statistics, and replace synthetic compression inputs with realistic payloads.

---

## Detailed Requirements

### 1. Symmetric Escape Barrier & Memory Touch Write
- Define a uniform helper inline function `bench_escape(void *ptr)` using `__asm__ __volatile__("" : : "r"(ptr) : "memory")`.
- Call `bench_escape(ptr)` symmetrically in **both** system `malloc` and `cmem` loops (`mp_alloc`/`mp_free`).
- Touch-write 1 byte to newly allocated memory pointers (`((char *)ptr)[0] = (char)i`) before calling `bench_escape(ptr)` to simulate L1 Data Cache fill and real workload memory access.

### 2. Warm-Up Phase
- Add a static helper `bench_warmup()` that executes 10,000 allocation/free cycles for both system `malloc` and `cmem` before timing loops start.
- This stabilizes thread caches, CPU frequency states, and OS heap page mapping prior to measurement.

### 3. Multi-Run Sampling & Median Calculation
- Wrap key micro-benchmarks in a 5-run loop.
- Collect total elapsed time for each run into an array of 5 double values.
- Sort the array and select the median value (`times[2]`) as the reported benchmark time.
- Calculate throughput and speedup ratios using the median time.

### 4. Benchmark 11 (Live Set 25%) Timing Alignment
- Modify Benchmark 11 (d) Live Set timing window to measure the full lifecycle: allocating 100% of objects, and freeing the 75% temporary objects.
- Ensure the timing window encompasses both allocation and deallocation loops so the operation count (`BENCH_PATTERN_OPS`) matches the actual work timed.

### 5. Realistic Compression Payload
- In `bench_compressed_storage()`, generate a 4KB structured JSON-like text payload (simulating repeated keys, strings, and numbers) rather than `i % 4`.
- Achieve realistic compression ratios (~3:1 to 10:1) to accurately measure LZ4 compression/decompression throughput under realistic workload conditions.

---

## File Changes Summary

- **File**: `benchmarks/bench_main.c`
- **Changes**:
  - Add `bench_warmup()`, `bench_escape()`, and `get_median_time()` helpers.
  - Update all benchmark functions (1 through 11) to apply symmetric escape barriers and memory touch writes.
  - Update Benchmark 11 (d) timing window.
  - Update Benchmark 10 payload generator.

---

## Verification Plan

1. **Compilation & Formatting**:
   - `make format-check` must pass cleanly without clang-format errors.
   - `make bench` must compile without warnings or errors.
2. **Execution & Metrics**:
   - Run `./build/benchmark`.
   - Verify non-zero, realistic measurements across all benchmarks.
   - Verify Live Set 25% throughput is reasonable and consistent.
   - Verify LZ4 compression ratio is in the realistic range (~3:1 to 10:1).
3. **Regression Tests**:
   - `cmake -B build_cmake -G Ninja && cmake --build build_cmake && ctest --test-dir build_cmake` passes 100%.
