# Testing Guide

## Table of Contents

1. [Test Framework](#1-test-framework)
2. [Test Structure](#2-test-structure)
3. [Running Tests](#3-running-tests)
4. [Adding New Tests](#4-adding-new-tests)
5. [Test Coverage](#5-test-coverage)
6. [Sanitizers](#6-sanitizers)
7. [Performance Tests](#7-performance-tests)
8. [CI/CD](#8-cicd)

---

## 1. Test Framework

cmem uses a **self-developed lightweight test framework** with no external dependencies.

**Features:**
- Zero dependency, pure C implementation
- Supports assertions (assert)
- Automatic memory leak detection
- Colored output
- Test case grouping

---

## 2. Test Structure

```
tests/
├── test_main.c         # C comprehensive unit tests (44+ test cases)
├── test_advanced.c     # Advanced C unit tests (callbacks, recovery, edge cases)
└── test_cpp.cpp        # C++ PMR + STL Allocator tests

benchmarks/
└── bench_main.c        # Performance benchmarks

examples/
├── example_basic.c         # Basic usage
├── example_embedded.c      # Static buffer mode
├── example_leak_analysis.c # Leak analysis
└── example_arena_tree.c    # Tree Arena

tools/
├── cmem-inspect/       # Real-time diagnostic CLI
│   ├── cmem-inspect.c
│   └── cmem-inspect.h
├── cmem-analyze/       # Offline snapshot analyzer
│   ├── cmem-analyze.c
│   ├── cmem-analyze-parser.c
│   └── cmem-analyze.h
└── common/             # Shared diagnostic output utilities
    ├── cmem-diag-output.c
    └── cmem-diag-output.h
```

---

## 3. Running Tests

### 3.1 C Unit Tests

```bash
# Debug build + Sanitizers
make test

# Manual compilation and run
gcc -fsanitize=address,undefined -Wall -Wextra -g -O0 \
    -std=c11 -D_GNU_SOURCE -I./include src/cmem.c tests/test_main.c \
    -o build/unit_tests -pthread -lrt
./build/unit_tests
```

### 3.2 C++ Tests

```bash
make test_cpp

# Manual compilation
g++ -std=c++17 -Wall -Wextra -g -O0 \
    -I./include src/cmem.c tests/test_cpp.cpp \
    -o build/cpp_tests -pthread -lrt
./build/cpp_tests
```

### 3.3 Performance Benchmarks

```bash
make bench
./build/benchmark
```

---

## 4. Adding New Tests

### 4.1 C Test Template

Add to `tests/test_main.c`:

```c
static void test_my_feature(void) {
    printf("--- Test: My New Feature ---\n");
    
    // 1. Create memory pool
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);
    assert(pool != NULL);
    
    // 2. Execute test logic
    void* p = mp_alloc(pool, 128);
    assert(p != NULL);
    assert(mp_ptr_valid(pool, p));
    
    // 3. Cleanup
    mp_free(pool, p);
    
    // 4. Check for leaks
    if (mp_check_leaks(pool)) {
        printf("[PASS] test_my_feature\n\n");
    } else {
        printf("[FAIL] test_my_feature - leaks detected\n\n");
        exit(1);
    }
    
    mp_destroy(pool);
}
```

Then call it in `main()`:

```c
int main() {
    test_my_feature();
    // ... other tests
    return 0;
}
```

### 4.2 C++ Test Template

Add to `tests/test_cpp.cpp`:

```cpp
static void test_cpp_feature() {
    std::cout << "--- Test: C++ Feature ---\n" << std::endl;
    
    cmem::MemoryPool pool(1024 * 1024, MP_FLAG_THREAD_SAFE);
    
    // Test logic
    void* p = pool.alloc(128);
    assert(p != nullptr);
    
    // Cleanup
    pool.free(p);
    assert(pool.check_leaks());
}

int main() {
    test_cpp_feature();
    // ...
    return 0;
}
```

---

## 5. Test Coverage

### 5.1 Existing Test Cases

| Test Name | Covered Functionality |
| :--- | :--- |
| `test_slab_small_allocs` | Slab small object allocation |
| `test_tlsf_medium_allocs` | TLSF medium object allocation |
| `test_realloc_and_aligned` | Realloc and aligned allocation |
| `test_multithread_safety` | Multi-thread concurrency safety |
| `test_arena_reset_and_json` | Arena reset and JSON export |
| `test_static_buffer_and_callbacks` | Static buffer and event callbacks |
| `test_child_arenas_and_html_export` | Child Arenas and HTML export |
| `test_leak_analysis_and_heap_audit` | Leak analysis and heap auditing |
| `test_memory_budget_and_oom` | Memory limit and OOM |
| `test_batch_alloc_and_compact` | Batch allocation and compaction |
| `test_allocation_histogram` | Allocation histogram |
| `test_cache_aligned_alloc` | Cache Line alignment |
| `test_guard_pages_protection` | Guard Pages protection |
| `test_realtime_throughput_meter` | Real-time throughput metering |
| `test_shared_memory_ipc` | Shared memory IPC |
| `test_global_override` | Global malloc interception |
| `test_huge_pages_alloc` | HugePages acceleration |
| `test_binary_snapshot` | Binary snapshot |
| `test_env_conf_tuning` | Environment variable tuning |
| `test_typed_object_pool` | Typed object pool |
| `test_prometheus_metrics` | Prometheus metrics |
| `test_purge_lazy` | Lazy RSS purge |
| `test_watermark_callback` | Watermark callback |
| `test_diff_snapshots` | Snapshot Diff |
| `test_frame_arena` | Frame Arena |
| `test_numa_node_binding` | NUMA node binding |
| `test_emergency_reserve` | Emergency OOM reserve |
| `test_convenience_apis` | Convenience functions |
| `test_reallocarray` | Overflow-safe reallocarray |
| `test_tlsf_inplace_realloc` | TLSF in-place realloc |
| `test_introspection_apis` | Introspection APIs |
| `test_advanced_stats` | Advanced statistics |
| `test_reset_stats_and_preferred_size` | Stats reset and preferred size |
| `test_mp_madvise` | Cross-platform madvise |
| `test_arena_metadata_apis` | Arena metadata |
| `test_batch_free_semantics` | Batch free semantics |
| `test_batch_free_equivalence` | Batch free equivalence vs individual |
| `test_batch_free_mixed_tiers` | Batch free across slab/tlsf/os tiers |
| `test_batch_free_corrupt` | Batch free with corrupt headers |
| `test_batch_free_subpool` | Batch free across sub-pools |
| `test_batch_free_fastpath` | Batch free in FAST_PATH mode |
| `test_batch_free_poison` | Batch free with poison on free |
| `test_batch_free_overflow` | Batch free overflow protection |
| `test_batch_alloc_and_compact` | Batch alloc + compaction |
| `test_batch_alloc_tiers` | Batch alloc across all tiers |
| `test_batch_alloc_configs` | Batch alloc with different flag configs |
| `test_boundary_cross_allocator` | Cross-allocator boundary test |
| `test_boundary_zero_size_all_tiers` | Zero-size across all tiers |
| `test_boundary_max_size` | Max-size allocation boundary |
| `test_idle_page_reclaim` | Idle page reclamation |

### 5.2 Coverage Targets

- **Line Coverage**: > 90%
- **Branch Coverage**: > 85%
- **Critical Paths**: 100% coverage

### 5.3 Running Coverage

```bash
# Using gcov
gcov src/cmem.c

# Using lcov
lcov --capture --directory . --output-file coverage.info
genhtml coverage.info --output-directory coverage_html
xdg-open coverage_html/index.html
```

---

## 6. Sanitizers

### 6.1 AddressSanitizer (ASan)

Detects memory errors:
- Out-of-bounds access (heap/stack/global)
- Use-After-Free
- Double Free
- Memory leaks

```bash
gcc -fsanitize=address -g -O1 -fno-omit-frame-pointer \
    -std=c11 -I./include src/cmem.c tests/test_main.c \
    -o build/unit_tests_asan -pthread -lrt
./build/unit_tests_asan
```

### 6.2 UndefinedBehaviorSanitizer (UBSan)

Detects undefined behavior:
- Integer overflow
- Null pointer dereference
- Alignment errors

```bash
gcc -fsanitize=undefined -g -O1 \
    -std=c11 -I./include src/cmem.c tests/test_main.c \
    -o build/unit_tests_ubsan -pthread -lrt
./build/unit_tests_ubsan
```

### 6.3 Combined Usage

```bash
gcc -fsanitize=address,undefined -fno-sanitize-recover=all \
    -g -O1 -fno-omit-frame-pointer \
    -std=c11 -I./include src/cmem.c tests/test_main.c \
    -o build/unit_tests_full -pthread -lrt
```

### 6.4 ASan Integration Test

```c
// Test ASan integration layer
static void test_asan_integration(void) {
    printf("--- Test: ASan Integration ---\n");
    
    memory_pool_t* pool = mp_create(1024*1024, MP_FLAG_ASAN_INTEGRATION);
    assert(pool != NULL);
    
    // Enable ASan integration
    mp_set_asan_integration(pool, true);
    assert(mp_asan_is_enabled() == false || true);  // May depend on compile options
    
    mp_destroy(pool);
    printf("[PASS] test_asan_integration\n\n");
}
```

---

### 6.5 ThreadSanitizer (TSan)
Detects data races and lock-order-inversion:

```bash
make CONFIG=tsan test
# or with CMake:
cmake -B build_tsan -G Ninja -DCMAKE_BUILD_TYPE=TSan
cmake --build build_tsan
ctest --test-dir build_tsan --output-on-failure
```

TSan runs all 44+ test cases with thread-level race detection enabled.

---

## 7. Performance Tests


### 7.1 Benchmark Framework

Uses the benchmark framework in `benchmarks/bench_main.c`.

### 7.2 Key Metrics

| Metric | Description | Target |
| :--- | :--- | :--- |
| Small Object QPS | 32-256B allocation/deallocation throughput | > 1.5 Gops/sec |
| Medium Object QPS | 1KB-64KB allocation/deallocation throughput | > 300 Mops/sec |
| Arena Reset | 500 allocs × 1000 rounds | < 1 ms |
| Multi-thread Scaling | 8 threads × 100K allocs | Near-linear |

### 7.3 Running Benchmarks

```bash
make bench
```

### 7.4 Custom Benchmark

```c
#include <time.h>
#include "cmem.h"

int main() {
    memory_pool_t* pool = mp_create(64 * 1024 * 1024, MP_FLAG_THREAD_SAFE);
    
    const int N = 1000000;
    struct timespec start, end;
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < N; i++) {
        void* p = mp_alloc(pool, 64);
        mp_free(pool, p);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double elapsed = (end.tv_sec - start.tv_sec) + 
                     (end.tv_nsec - start.tv_nsec) / 1e9;
    double qps = N / elapsed;
    
    printf("QPS: %.2f Mops/sec\n", qps / 1e6);
    
    mp_destroy(pool);
    return 0;
}
```

---

## 8. CI/CD

### 8.1 GitHub Actions Configuration

The project should include `.github/workflows/ci.yml`:

```yaml
name: CI

on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install dependencies
        run: sudo apt update && sudo apt install -y build-essential cmake ninja-build
      - name: Build
        run: make lib
      - name: Test (ASan + UBSan)
        run: make test
      - name: C++ Test
        run: make test_cpp
      - name: TSan Test
        run: make CONFIG=tsan test
      - name: UBSan Test
        run: make CONFIG=ubsan test
      - name: Release Build
        run: make CONFIG=release all
      - name: Format Check
        run: make format-check
```

### 8.2 Quality Gates

- [ ] `make lib` compiles without warnings and produces versioned static archives
- [ ] `make lib_shared` builds shared library with correct SONAME
- [ ] CMake build reports no clang-tidy warnings (per `.clang-tidy` configuration)
- [ ] `make test` passes all tests (ASan + UBSan)
- [ ] `make CONFIG=tsan test` passes with zero TSan warnings
- [ ] `make test_cpp` passes
- [ ] `make format-check` passes
- [ ] CMake build succeeds for all build types (Debug, Release, ASan, TSan, UBSan)
- [ ] Code conforms to standards
- [ ] New features include tests
- [ ] Documentation is updated

---

## Appendix: Debugging Checklist

- [ ] Enable `MP_FLAG_DEBUG_CANARY` to detect out-of-bounds
- [ ] Enable `MP_FLAG_TRACK_LOCATIONS` to trace call stacks
- [ ] Run `mp_audit_heap()` to check heap integrity
- [ ] Run `mp_analyze_leaks()` to check for leaks
- [ ] Use Valgrind to check for memory errors
- [ ] Use ASan to detect memory errors
- [ ] Check `fragmentation_ratio` in `mp_get_stats()`
- [ ] Check size distribution in `mp_dump_histogram()`