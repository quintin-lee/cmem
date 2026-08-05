# Benchmark Methodology Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor `benchmarks/bench_main.c` to fix measurement bias, ensure symmetric memory barriers and memory touch-writes, add a warm-up phase, calculate 5-run median statistics, correct Benchmark 11 (Live Set) timing windows, and use a realistic JSON-like compression payload.

**Architecture:** Update `benchmarks/bench_main.c` helper utilities (`bench_escape`, `bench_warmup`, `get_median_time`) and refactor each benchmark function to use a 5-run median timing loop with symmetric memory barriers and memory touch-writes.

**Tech Stack:** C11, GCC/Clang, POSIX threads, `clock_gettime(CLOCK_MONOTONIC)`

---

### Task 1: Add Helper Functions (`bench_warmup`, `get_median_time`) and `bench_escape` Touch-Write

**Files:**
- Modify: `benchmarks/bench_main.c:40-70`

- [ ] **Step 1: Write helper functions for warm-up and median timing in `bench_main.c`**

Add `bench_warmup()` and `get_median_time()` to `benchmarks/bench_main.c`:

```c
#define BENCH_MEDIAN_RUNS 5

static inline void bench_escape(void *ptr)
{
    if (ptr) {
        ((volatile char *)ptr)[0] = 1;
    }
    __asm__ __volatile__("" : : "r"(ptr) : "memory");
}

static void bench_warmup(memory_pool_t *pool)
{
    for (int i = 0; i < 10000; i++) {
        void *sys_p = malloc(64);
        bench_escape(sys_p);
        free(sys_p);

        if (pool) {
            void *mp_p = mp_alloc(pool, 64);
            bench_escape(mp_p);
            mp_free(pool, mp_p);
        }
    }
}

static int double_cmp(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static double get_median_time(double times[], int count)
{
    qsort(times, count, sizeof(double), double_cmp);
    return times[count / 2];
}
```

- [ ] **Step 2: Run build to verify compilation**

Run: `make bench`
Expected: Passes build without errors or warnings.

- [ ] **Step 3: Commit**

```bash
git add benchmarks/bench_main.c
git commit -m "feat(bench): add warmup and median calculation helpers"
```

---

### Task 2: Refactor Micro-Benchmarks 1-6 with Median Sampling and Touch Writes

**Files:**
- Modify: `benchmarks/bench_main.c:70-420`

- [ ] **Step 1: Update `bench_small_allocs()`, `bench_medium_allocs()`, `bench_arena_reset()`, `bench_fast_path()`**

Refactor `bench_small_allocs`, `bench_medium_allocs`, `bench_arena_reset`, `bench_fast_path` to include:
1. `bench_warmup()` invocation before timings.
2. 5-run loop collecting times into `times[BENCH_MEDIAN_RUNS]` and selecting the median.
3. Symmetric `bench_escape(ptr)` and touch-writes in both `malloc` and `cmem` loops.

- [ ] **Step 2: Run build and execute benchmarks**

Run: `make bench`
Expected: Output shows valid non-zero median times for system malloc and cmem across Benchmarks 1-6.

- [ ] **Step 3: Commit**

```bash
git add benchmarks/bench_main.c
git commit -m "refactor(bench): apply median sampling and symmetric barriers to benchmarks 1-6"
```

---

### Task 3: Refactor Benchmarks 7-11 (Size Distributions, Batch APIs, Thread Scaling, Compression, Patterns)

**Files:**
- Modify: `benchmarks/bench_main.c:420-825`

- [ ] **Step 1: Update Benchmark 7, 8, 9, 10, 11**

1. Apply `bench_escape(ptr)` symmetrically in Benchmark 7 and Benchmark 11 loops.
2. In Benchmark 10 (`bench_compressed_storage`), generate a 4KB structured JSON-like text payload (simulating repeated keys, strings, and numbers):
   ```c
   for (int i = 0; i < BENCH_COMPRESS_BLOCK_SIZE; i++) {
       static const char pattern[] = "{\"id\":12345,\"name\":\"cmem_tiered_memory_pool\",\"status\":\"active\",\"tags\":[\"high_performance\",\"c11\"]}\n";
       bytes[i] = pattern[i % (sizeof(pattern) - 1)];
   }
   ```
3. In Benchmark 11 (d) Live Set, align the timing window to include the allocation loop and free 75% loop:
   ```c
   start = get_time_sec();
   for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
       ptrs[i] = malloc(32 + (i % SMALL_ALLOC_SPREAD));
       bench_escape(ptrs[i]);
   }
   for (int i = 0; i < BENCH_PATTERN_OPS - (int)live_count; i++) {
       free(ptrs[i]);
   }
   double sys_d = get_time_sec() - start;
   ```
   Do the symmetric timing window for `cmem`.

- [ ] **Step 2: Run formatting check, build, and test suite**

Run: `make format-check && make bench && cmake -B build_cmake -G Ninja && cmake --build build_cmake && ctest --test-dir build_cmake`
Expected: All benchmarks compile cleanly, format-check passes, ctest passes 100%.

- [ ] **Step 3: Commit**

```bash
git add benchmarks/bench_main.c
git commit -m "refactor(bench): optimize benchmarks 7-11 with aligned timing and realistic compression payload"
```
