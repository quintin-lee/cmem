# cmem Core Quality Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Improve cmem's core quality through intelligent leak detection, fuzzing integration, comprehensive benchmarks, and cross-platform validation.

**Architecture:** Four-phase implementation: (1) Leak detection enhancement with severity classification and pattern analysis, (2) Complete fuzzing integration with CI and corpus management, (3) Performance benchmark expansion with multithreaded and real-workload tests, (4) Cross-platform validation with Windows/Android support and CI matrix.

**Tech Stack:** C11, libFuzzer, AddressSanitizer, GitHub Actions, CMake, lcov, Valgrind

---

## Phase 1: Intelligent Leak Detection

### Task 1.1: Add Leak Severity Classification

**Files:**
- Modify: `include/cmem.h` (add enum and function declarations)
- Modify: `src/cmem_diag.c` (implement severity logic)
- Test: `tests/test_leak_severity.c` (new test file)

- [ ] **Step 1: Add leak severity enum to cmem.h**

Add after line 229 in `include/cmem.h` (after `mp_allocation_info_t`):

```c
/**
 * @brief Leak severity classification.
 */
typedef enum {
    MP_LEAK_SEVERITY_CRITICAL, /**< > 1MB or frequent allocation pattern */
    MP_LEAK_SEVERITY_WARNING,  /**< Medium-sized or recurring leak */
    MP_LEAK_SEVERITY_INFO      /**< Small or occasional leak */
} mp_leak_severity_t;
```

- [ ] **Step 2: Add severity API declarations to cmem.h**

Add after line 1034 (after `mp_export_leak_report`):

```c
/**
 * @brief Gets the severity classification for a leak.
 *
 * @param info Allocation info from mp_get_allocation_info()
 * @return Severity level
 */
mp_leak_severity_t mp_get_leak_severity(const mp_allocation_info_t *info);

/**
 * @brief Leak pattern analysis result.
 */
typedef struct {
    const char *pattern_name;     /**< Pattern identifier */
    int confidence;               /**< 0-100 confidence score */
    const char *suggestion;       /**< Fix suggestion */
} mp_leak_pattern_t;

/**
 * @brief Analyzes leak pattern and returns analysis result.
 *
 * @param info Allocation info from mp_get_allocation_info()
 * @return Leak pattern analysis
 */
mp_leak_pattern_t mp_analyze_leak_pattern(const mp_allocation_info_t *info);
```

- [ ] **Step 3: Run test to verify compilation**

Run: `gcc -std=c11 -c -I./include src/cmem.c -o /tmp/test_compile.o`
Expected: No errors (will fail later when implementing, but header should compile)

- [ ] **Step 4: Create test file for leak severity**

Create `tests/test_leak_severity.c`:

```c
#include "cmem.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

static void test_leak_severity_critical(void) {
    printf("--- Test: Critical Leak Severity ---\n");
    
    memory_pool_t *pool = mp_create(1024 * 1024, MP_FLAG_DEBUG_CANARY);
    assert(pool != NULL);
    
    // Allocate 2MB (exceeds critical threshold)
    void *ptr = mp_alloc(pool, 2 * 1024 * 1024);
    assert(ptr != NULL);
    
    mp_allocation_info_t info;
    assert(mp_get_allocation_info(pool, ptr, &info));
    
    mp_leak_severity_t severity = mp_get_leak_severity(&info);
    assert(severity == MP_LEAK_SEVERITY_CRITICAL);
    
    mp_leak_pattern_t pattern = mp_analyze_leak_pattern(&info);
    assert(pattern.confidence > 0);
    
    mp_free(pool, ptr);
    mp_destroy(pool);
    printf("[PASS] test_leak_severity_critical\n\n");
}

static void test_leak_severity_warning(void) {
    printf("--- Test: Warning Leak Severity ---\n");
    
    memory_pool_t *pool = mp_create(1024 * 1024, MP_FLAG_DEBUG_CANARY);
    assert(pool != NULL);
    
    // Allocate 100KB (warning range)
    void *ptr = mp_alloc(pool, 100 * 1024);
    assert(ptr != NULL);
    
    mp_allocation_info_t info;
    assert(mp_get_allocation_info(pool, ptr, &info));
    
    mp_leak_severity_t severity = mp_get_leak_severity(&info);
    assert(severity == MP_LEAK_SEVERITY_WARNING);
    
    mp_free(pool, ptr);
    mp_destroy(pool);
    printf("[PASS] test_leak_severity_warning\n\n");
}

static void test_leak_severity_info(void) {
    printf("--- Test: Info Leak Severity ---\n");
    
    memory_pool_t *pool = mp_create(1024 * 1024, MP_FLAG_DEBUG_CANARY);
    assert(pool != NULL);
    
    // Allocate 1KB (info range)
    void *ptr = mp_alloc(pool, 1024);
    assert(ptr != NULL);
    
    mp_allocation_info_t info;
    assert(mp_get_allocation_info(pool, ptr, &info));
    
    mp_leak_severity_t severity = mp_get_leak_severity(&info);
    assert(severity == MP_LEAK_SEVERITY_INFO);
    
    mp_free(pool, ptr);
    mp_destroy(pool);
    printf("[PASS] test_leak_severity_info\n\n");
}

int main(void) {
    test_leak_severity_critical();
    test_leak_severity_warning();
    test_leak_severity_info();
    return 0;
}
```

- [ ] **Step 5: Implement severity classification in cmem_diag.c**

Add to `src/cmem_diag.c` after existing leak analysis functions:

```c
mp_leak_severity_t mp_get_leak_severity(const mp_allocation_info_t *info) {
    if (!info) {
        return MP_LEAK_SEVERITY_INFO;
    }
    
    // Critical: > 1MB or very frequent (small class with large count)
    if (info->requested_size > 1024 * 1024) {
        return MP_LEAK_SEVERITY_CRITICAL;
    }
    
    // Warning: > 100KB
    if (info->requested_size > 100 * 1024) {
        return MP_LEAK_SEVERITY_WARNING;
    }
    
    // Info: everything else
    return MP_LEAK_SEVERITY_INFO;
}

mp_leak_pattern_t mp_analyze_leak_pattern(const mp_allocation_info_t *info) {
    mp_leak_pattern_t result = {0};
    
    if (!info) {
        result.pattern_name = "unknown";
        result.confidence = 0;
        result.suggestion = "Provide valid allocation info";
        return result;
    }
    
    // Pattern 1: Large single allocation (likely buffer leak)
    if (info->requested_size > 64 * 1024) {
        result.pattern_name = "large_buffer_leak";
        result.confidence = 85;
        result.suggestion = "Check for missing free() on large allocations";
        return result;
    }
    
    // Pattern 2: Repeated small allocation (likely loop leak)
    if (info->requested_size < 256 && info->slab_class > 0) {
        result.pattern_name = "repeated_small_leak";
        result.confidence = 70;
        result.suggestion = "Check for allocations in loops without corresponding free()";
        return result;
    }
    
    // Pattern 3: String allocation (likely strdup leak)
    if (info->alloc_file && info->alloc_func) {
        result.pattern_name = "string_allocation_leak";
        result.confidence = 60;
        result.suggestion = "Check mp_strdup/mp_memdup calls for missing free()";
        return result;
    }
    
    // Default: generic leak
    result.pattern_name = "generic_leak";
    result.confidence = 50;
    result.suggestion = "Review allocation at " __FILE__ ":" "line";
    return result;
}
```

- [ ] **Step 6: Compile and run tests**

Run: `make test_leak_severity`
Expected: All 3 tests pass

- [ ] **Step 7: Integrate into main test suite**

Add to `tests/test_main.c` in the `main()` function:

```c
    // Include leak severity tests
    extern void test_leak_severity_critical(void);
    extern void test_leak_severity_warning(void);
    extern void test_leak_severity_info(void);
    test_leak_severity_critical();
    test_leak_severity_warning();
    test_leak_severity_info();
```

- [ ] **Step 8: Run full test suite**

Run: `make test`
Expected: All tests pass, including new ones

- [ ] **Step 9: Commit**

```bash
git add include/cmem.h src/cmem_diag.c tests/test_leak_severity.c tests/test_main.c
git commit -m "feat(diagnostics): ✨ add leak severity classification and pattern analysis"
```

---

## Phase 2: Complete Fuzzing Integration

### Task 2.1: Add Fuzzing Make Targets and CI

**Files:**
- Modify: `Makefile` (add fuzz targets)
- Modify: `CMakeLists.txt` (add fuzz build)
- Modify: `.github/workflows/ci.yml` (add fuzz job)
- Create: `corpus/` directory

- [ ] **Step 1: Add fuzz targets to Makefile**

Add to `Makefile` after existing test targets:

```makefile
# Fuzzing targets
FUZZ_SRCS = tests/fuzz_alloc.c src/cmem.c
FUZZ_CFLAGS = -fsanitize=fuzzer -fsanitize=address -fsanitize=undefined \
              -fno-omit-frame-pointer -O1 -g

fuzz-build:
	$(CC) $(FUZZ_CFLAGS) -I./include $(FUZZ_SRCS) -o build/fuzz_alloc

fuzz-run: fuzz-build
	@echo "Running fuzzing (Ctrl+C to stop)..."
	@mkdir -p corpus
	./build/fuzz_alloc corpus -max_len=4096 -jobs=4

fuzz-ci: fuzz-build
	@mkdir -p corpus
	@echo "Running CI fuzzing (10 seconds)..."
	./build/fuzz_alloc corpus -max_len=4096 -timeout=10 -runs=1000

fuzz-clean:
	rm -f build/fuzz_alloc
```

- [ ] **Step 2: Add fuzz build to CMakeLists.txt**

Add to `CMakeLists.txt` after existing test targets:

```cmake
# Fuzzing target
add_executable(fuzz_alloc
    tests/fuzz_alloc.c
    src/cmem.c
)
target_compile_options(fuzz_alloc PRIVATE
    -fsanitize=fuzzer
    -fsanitize=address
    -fsanitize=undefined
    -fno-omit-frame-pointer
    -O1
    -g
)
target_link_options(fuzz_alloc PRIVATE
    -fsanitize=fuzzer
    -fsanitize=address
    -fsanitize=undefined
)
```

- [ ] **Step 3: Create corpus directory with initial seeds**

Create `corpus/README.md`:

```markdown
# Fuzzing Corpus

This directory contains seed inputs for libFuzzer.

## Structure
- `seed_*.bin`: Initial seed files
- `crash_*.bin`: Inputs that caused crashes (auto-generated)
- `timeout_*.bin`: Inputs that caused timeouts (auto-generated)

## Usage
```bash
# Initial fuzzing
./build/fuzz_alloc corpus

# CI mode (quick run)
./build/fuzz_alloc corpus -max_len=4096 -timeout=10 -runs=1000
```
```

Create initial seed file:

```bash
mkdir -p corpus
echo -n "test_seed" > corpus/seed_00001.bin
```

- [ ] **Step 4: Add fuzzing job to CI workflow**

Add to `.github/workflows/ci.yml` after the valgrind job:

```yaml
  fuzzing:
    name: Fuzzing
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      
      - name: Install dependencies
        run: sudo apt-get update && sudo apt-get install -y clang
      
      - name: Build fuzz target
        run: make fuzz-build
      
      - name: Run fuzzing (CI mode)
        run: make fuzz-ci
        timeout-minutes: 5
      
      - name: Upload corpus
        uses: actions/upload-artifact@v4
        with:
          name: fuzz-corpus
          path: corpus/
```

- [ ] **Step 5: Test fuzzing build**

Run: `make fuzz-build`
Expected: `build/fuzz_alloc` created successfully

- [ ] **Step 6: Run quick fuzzing test**

Run: `./build/fuzz_alloc corpus/ -max_len=64 -runs=100`
Expected: Fuzzer runs without crashes, reports coverage stats

- [ ] **Step 7: Commit**

```bash
git add Makefile CMakeLists.txt .github/workflows/ci.yml corpus/
git commit -m "feat(fuzzing): 🧪 add libFuzzer integration with CI support"
```

---

## Phase 3: Performance Benchmark Expansion

### Task 3.1: Add Multithreaded and Real-Workload Benchmarks

**Files:**
- Modify: `benchmarks/bench_main.c` (add new benchmarks)
- Modify: `docs/en/performance.md` (update docs)
- Modify: `docs/zh/performance.md` (update docs)

- [ ] **Step 1: Add multithreaded benchmark function**

Add to `benchmarks/bench_main.c`:

```c
#define _GNU_SOURCE
#include <pthread.h>

typedef struct {
    memory_pool_t *pool;
    int thread_id;
    int alloc_count;
    double elapsed;
} bench_thread_arg_t;

static void *bench_thread_func(void *arg) {
    bench_thread_arg_t *ta = (bench_thread_arg_t *)arg;
    void **ptrs = malloc(sizeof(void *) * ta->alloc_count);
    if (!ptrs) return NULL;
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < ta->alloc_count; i++) {
        size_t sz = 32 + (i % 256);
        ptrs[i] = mp_alloc(ta->pool, sz);
    }
    
    for (int i = 0; i < ta->alloc_count; i++) {
        mp_free(ta->pool, ptrs[i]);
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    ta->elapsed = (end.tv_sec - start.tv_sec) + 
                  (end.tv_nsec - start.tv_nsec) / 1e9;
    
    free(ptrs);
    return NULL;
}

void bench_multithreaded(int thread_count, int allocs_per_thread) {
    printf("\n--- Benchmark 4: Multithreaded Allocations (%d threads x %d ops) ---\n",
           thread_count, allocs_per_thread);
    
    memory_pool_t *pool = mp_create(64 * 1024 * 1024, 
                                    MP_FLAG_THREAD_SAFE | MP_FLAG_THREAD_LOCAL_CACHE);
    
    bench_thread_arg_t *args = malloc(sizeof(bench_thread_arg_t) * thread_count);
    pthread_t *threads = malloc(sizeof(pthread_t) * thread_count);
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < thread_count; i++) {
        args[i].pool = pool;
        args[i].thread_id = i;
        args[i].alloc_count = allocs_per_thread;
        pthread_create(&threads[i], NULL, bench_thread_func, &args[i]);
    }
    
    for (int i = 0; i < thread_count; i++) {
        pthread_join(threads[i], NULL);
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    double total_time = (end.tv_sec - start.tv_sec) + 
                        (end.tv_nsec - start.tv_nsec) / 1e9;
    
    printf("  Total Time: %.4f sec\n", total_time);
    printf("  Throughput: %.2f Mops/sec\n", 
           (thread_count * allocs_per_thread) / total_time / 1e6);
    
    for (int i = 0; i < thread_count; i++) {
        printf("  Thread %d: %.4f sec\n", i, args[i].elapsed);
    }
    
    mp_destroy(pool);
    free(args);
    free(threads);
}
```

- [ ] **Step 2: Add arena workload benchmark**

Add to `benchmarks/bench_main.c`:

```c
void bench_arena_workload(int rounds, int allocs_per_round) {
    printf("\n--- Benchmark 5: Arena Workload (Game/Render style) ---\n");
    
    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_DEFAULT);
    void **ptrs = malloc(sizeof(void *) * allocs_per_round);
    
    // Method 1: Individual free
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < allocs_per_round; i++) {
            ptrs[i] = mp_alloc(pool, 64 + (i * 8));
        }
        for (int i = 0; i < allocs_per_round; i++) {
            mp_free(pool, ptrs[i]);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double time_individual = (end.tv_sec - start.tv_sec) + 
                             (end.tv_nsec - start.tv_nsec) / 1e9;
    
    // Method 2: Arena reset
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < allocs_per_round; i++) {
            mp_alloc(pool, 64 + (i * 8));
        }
        mp_reset(pool);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double time_reset = (end.tv_sec - start.tv_sec) + 
                        (end.tv_nsec - start.tv_nsec) / 1e9;
    
    printf("  Individual Free:  %.4f sec\n", time_individual);
    printf("  Arena Reset:      %.4f sec\n", time_reset);
    printf("  Speedup:          %.2fx\n", time_individual / time_reset);
    
    mp_destroy(pool);
    free(ptrs);
}
```

- [ ] **Step 3: Update main() to call new benchmarks**

Modify `benchmarks/bench_main.c` main function:

```c
int main() {
    printf("================ CMEM PERFORMANCE BENCHMARK ================\n");
    bench_small_allocs();
    bench_medium_allocs();
    bench_arena_reset();
    bench_multithreaded(4, 100000);
    bench_arena_workload(1000, 500);
    return 0;
}
```

- [ ] **Step 4: Build and run benchmarks**

Run: `make bench`
Expected: All 5 benchmarks complete successfully

- [ ] **Step 5: Commit**

```bash
git add benchmarks/bench_main.c
git commit -m "feat(benchmarks): ⚡️ add multithreaded and arena workload benchmarks"
```

---

## Phase 4: Cross-Platform Validation

### Task 4.1: Add Windows Support and CI Matrix

**Files:**
- Modify: `src/cmem_sys.c` (add Windows support)
- Modify: `.github/workflows/ci.yml` (add Windows/Android jobs)
- Modify: `include/cmem.h` (add platform macros)

- [ ] **Step 1: Add Windows platform detection macros**

Add to `include/cmem.h` after existing platform checks:

```c
/* Platform detection */
#if defined(_WIN32) || defined(_WIN64)
#define CMEM_PLATFORM_WINDOWS 1
#elif defined(__APPLE__) && defined(__MACH__)
#define CMEM_PLATFORM_MACOS 1
#elif defined(__linux__)
#define CMEM_PLATFORM_LINUX 1
#elif defined(__ANDROID__)
#define CMEM_PLATFORM_ANDROID 1
#elif defined(__FreeBSD__)
#define CMEM_PLATFORM_FREEBSD 1
#endif
```

- [ ] **Step 2: Add Windows-specific syscalls to cmem_sys.c**

Add to `src/cmem_sys.c`:

```c
#if defined(CMEM_PLATFORM_WINDOWS)
#include <windows.h>
#include <sys/stat.h>

static void *cmem_win_alloc(size_t size) {
    void *ptr = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return ptr;
}

static void cmem_win_free(void *ptr, size_t size) {
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
}

static int cmem_win_madvise(void *addr, size_t length, int advice) {
    (void)addr;
    (void)length;
    (void)advice;
    return 0;  // No-op on Windows
}

mp_sys_allocator_t cmem_get_win_allocator(void) {
    mp_sys_allocator_t alloc = {
        .sys_alloc = cmem_win_alloc,
        .sys_free = cmem_win_free,
        .user_data = NULL
    };
    return alloc;
}
#endif
```

- [ ] **Step 3: Add Windows/Android jobs to CI**

Add to `.github/workflows/ci.yml`:

```yaml
  windows-build:
    name: Windows Build
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      
      - name: Configure CMake
        run: cmake -B build -G "MinGW Makefiles"
        
      - name: Build
        run: cmake --build build
        
      - name: Test
        run: ctest --test-dir build --output-on-failure

  android-build:
    name: Android NDK Build
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      
      - name: Set up Android NDK
        uses: nttld/setup-ndk@v1
        with:
          ndk-version: r25b
          
      - name: Build for Android
        run: |
          cmake -B build_android \
            -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
            -DANDROID_ABI=arm64-v8a \
            -DANDROID_PLATFORM=android-21
          cmake --build build_android
```

- [ ] **Step 4: Test compilation on current platform**

Run: `make clean && make lib`
Expected: Library builds successfully

- [ ] **Step 5: Commit**

```bash
git add include/cmem.h src/cmem_sys.c .github/workflows/ci.yml
git commit -m "feat(platform): 🐧 add Windows support and cross-platform CI matrix"
```

---

## Verification

- [ ] Run full test suite: `make test`
- [ ] Run fuzzing: `make fuzz-ci`
- [ ] Run benchmarks: `make bench`
- [ ] Check coverage: `make coverage`
- [ ] Verify all docs updated

---

## Summary

This plan implements four core quality improvements:

1. **Leak Detection** - Severity classification and pattern analysis
2. **Fuzzing** - Complete libFuzzer integration with CI
3. **Benchmarks** - Multithreaded and real-workload tests
4. **Cross-Platform** - Windows support and CI matrix

**Estimated effort:** 2-3 weeks for full implementation
**Risk level:** Low (each phase is independent and testable)
