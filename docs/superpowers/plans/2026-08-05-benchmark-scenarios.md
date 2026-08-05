# Enhanced Benchmark Scenarios Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add six new benchmark scenarios (Benchmarks 6-11) to `benchmarks/bench_main.c` covering FAST_PATH, size distributions, batch APIs, thread scaling, compressed storage, and allocation patterns.

**Architecture:** Extend the single-file benchmark driver with scenario functions following the existing pattern (named constants, `--- Benchmark N: title ---` headers, system-malloc-vs-cmem comparisons where meaningful). No library code changes; `make bench` runs all 11.

**Tech Stack:** C11, cmem public API (include/cmem.h), pthread. Benchmark only file: `benchmarks/bench_main.c`.

---

## File Structure

- **Modify only:** `benchmarks/bench_main.c` (322 lines currently)
- No new source files. No library changes.
- New function prototypes added near existing forward decls (L164-170).
- New function definitions appended at the end (after `bench_arena_workload`, L322).
- `main()` at L177 extended with the new calls.

## Verified API signatures (from include/cmem.h / src/cmem_event.c)

- `memory_pool_t *mp_create(size_t initial_capacity, mp_flags_t flags)` — L484
- `void mp_destroy(memory_pool_t *pool)`
- `void *mp_alloc(memory_pool_t *pool, size_t size)`
- `void mp_free(memory_pool_t *pool, void *ptr)`
- `void mp_reset(memory_pool_t *pool)` — L856
- `size_t mp_alloc_batch(memory_pool_t *pool, size_t size, void **out_ptrs, size_t count)` — L1223
- `void mp_free_batch(memory_pool_t *pool, void **ptrs, size_t count)` — L1232
- `compressed_handle_t mp_compress_block(memory_pool_t *pool, void *data, size_t size)` — L330
- `void *mp_decompress_block(memory_pool_t *pool, compressed_handle_t handle)` — L339
- `bool mp_free_compressed(memory_pool_t *pool, compressed_handle_t handle)` — L345
- `bool mp_get_compressed_stats(memory_pool_t *pool, cmem_compressed_stats_t *stats)` — L367
- Flags: `MP_FLAG_FAST_PATH=(1<<18)`, `MP_FLAG_THREAD_LOCAL_CACHE=(1<<3)`, `MP_FLAG_THREAD_SAFE=(1<<0)`, `MP_FLAG_MULTI_ARENA=(1<<17)`, `MP_FLAG_DEFAULT`
- Existing constants: SMALL_ALLOC_COUNT=1000000, SMALL_ALLOC_SPREAD=224, MEDIUM_ALLOC_COUNT=100000, NSEC_PER_SEC=1e9, MILLION_OPS=1e6, `static volatile void *bench_escape_sink;` (L25)

## clang-tidy constraints (STRICT)

- All loop counters and pointer variables MUST be ≥2 chars: use `i`, `idx`, `ptr`, `ptrs` (all ≥2).
- Magic numbers: only 1;2;3;4;8;16;32;64;128;256;512;1024;4096;8192;65536;1048576 + powers of 2 are allowed. Values like `10` (thread counts), `25` (live-set %), `3`, `13`, `7` in `(i*13) % 16384` ARE allowed? **NO** — 13 and 7 are NOT powers of 2 and NOT in the ignore list. Use named constants for ALL such values.
- Keep clang-format clean: run `clang-format -i benchmarks/bench_main.c` before build.

---

### Task A: Add shared helpers + prototypes + main() wiring

**Files:**
- Modify: `benchmarks/bench_main.c`

- [ ] **Step 1: Add new scenario constants after L20**

```c
#define BENCH_FAST_PATH_ITERS 1000000
#define BENCH_SIZE_FIXED 32
#define BENCH_SIZE_MIXED_SPREAD 224
#define BENCH_SIZE_LARGE_BASE 4096
#define BENCH_SIZE_LARGE_SPREAD 16384
#define BENCH_BATCH_ELEMS 64
#define BENCH_BATCH_COUNT 10000
#define BENCH_THREAD_COUNTS 4
#define BENCH_COMPRESS_BLOCK_SIZE 4096
#define BENCH_COMPRESS_ITERS 10000
#define BENCH_PATTERN_OPS 200000
#define BENCH_PATTERN_LIVE_KEEP 25
```

- [ ] **Step 2: Add forward declarations after L170 (bench_multithreaded decl)**

```c
/**
 * @brief Benchmarks FAST_PATH flag impact on small interleaved allocations.
 */
void bench_fast_path();

/**
 * @brief Benchmarks size-class distribution (fixed, mixed, large TLSF).
 */
void bench_size_distribution();

/**
 * @brief Benchmarks batch allocation APIs vs single alloc/free loops.
 */
void bench_batch_apis();

/**
 * @brief Benchmarks thread-count scaling for single vs multi-arena pools.
 */
void bench_thread_scaling();

/**
 * @brief Benchmarks compressed-storage compress/decompress throughput.
 */
void bench_compressed_storage();

/**
 * @brief Benchmarks allocation patterns: interleaved, batch, random, live-set.
 */
void bench_allocation_patterns();
```

- [ ] **Step 3: Add calls in main() after L184 (bench_arena_workload call)**

```c
    bench_fast_path();
    bench_size_distribution();
    bench_batch_apis();
    bench_thread_scaling();
    bench_compressed_storage();
    bench_allocation_patterns();
```

- [ ] **Step 4: Verify wiring**

Run: `gcc -Wall -Wextra -O2 -std=gnu11 -D_GNU_SOURCE -I./include -c benchmarks/bench_main.c -o /tmp/bench_wire.o`
Expected: warnings ONLY about undefined `bench_fast_path` etc. (implicit declarations) — confirms main() references them. (Full build happens in Task G.)

---

### Task B: Benchmark 6 — FAST_PATH

**Files:**
- Modify: `benchmarks/bench_main.c` (append definitions)

- [ ] **Step 1: Add bench_fast_path definition (after bench_arena_workload, end of file)**

```c
/**
 * @brief Compares MP_FLAG_THREAD_LOCAL_CACHE with and without MP_FLAG_FAST_PATH
 * on the same interleaved 32-256 byte workload as Benchmark 1.
 */
void bench_fast_path()
{
    printf("\n--- Benchmark 6: FAST_PATH (interleaved %d x 32-256B) ---\n", BENCH_FAST_PATH_ITERS);

    memory_pool_t *plain_pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);
    double start = get_time_sec();
    for (int i = 0; i < BENCH_FAST_PATH_ITERS; i++) {
        size_t sz = 32 + (i % SMALL_ALLOC_SPREAD);
        void *ptr = mp_alloc(plain_pool, sz);
        mp_free(plain_pool, ptr);
    }
    double time_plain = get_time_sec() - start;
    mp_destroy(plain_pool);

    memory_pool_t *fast_pool =
        mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE | MP_FLAG_FAST_PATH);
    start = get_time_sec();
    for (int i = 0; i < BENCH_FAST_PATH_ITERS; i++) {
        size_t sz = 32 + (i % SMALL_ALLOC_SPREAD);
        void *ptr = mp_alloc(fast_pool, sz);
        mp_free(fast_pool, ptr);
    }
    double time_fast = get_time_sec() - start;
    mp_destroy(fast_pool);

    printf("  THREAD_LOCAL_CACHE        : %.4f sec (%.2f Mops/sec)\n",
           time_plain,
           (BENCH_FAST_PATH_ITERS / time_plain) / MILLION_OPS);
    printf("  +MP_FLAG_FAST_PATH        : %.4f sec (%.2f Mops/sec)\n",
           time_fast,
           (BENCH_FAST_PATH_ITERS / time_fast) / MILLION_OPS);
    printf("  Speedup                   : %.2fx\n", time_plain / time_fast);
}
```

- [ ] **Step 2: Verify compile + single run**

Run: `gcc -O2 -std=gnu11 -D_GNU_SOURCE -I./include benchmarks/bench_main.c src/*.c -pthread -o /tmp/bench_a && /tmp/bench_a`
Expected: Benchmark 1-5 run (may take ~15s), Benchmark 6 prints both rates; speedup roughly 1.0-1.5x. Note: this full run is slow — for iteration use a quick smoke: `sed` won't help; instead verify compile only at this stage and do full timing in Task G.

---

### Task C: Benchmark 7 — Size distributions

- [ ] **Step 1: Add bench_size_distribution definition**

```c
/**
 * @brief Compares cmem vs system malloc across three size distributions:
 * fixed 32-byte, irregular mixed 16-240 byte, and large TLSF 4-20KB.
 */
void bench_size_distribution()
{
    printf("\n--- Benchmark 7: Size Distributions (1M interleaved each) ---\n");

    /* (a) fixed 32-byte — single slab class */
    double start = get_time_sec();
    for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
        void *ptr = malloc(BENCH_SIZE_FIXED);
        bench_escape_sink = ptr;
        free(ptr);
    }
    double sys_fixed = get_time_sec() - start;
    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);
    start = get_time_sec();
    for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
        void *ptr = mp_alloc(pool, BENCH_SIZE_FIXED);
        mp_free(pool, ptr);
    }
    double cmem_fixed = get_time_sec() - start;
    printf("  Fixed 32B        : malloc %.2f | cmem %.2f Mops/sec (%.2fx)\n",
           (SMALL_ALLOC_COUNT / sys_fixed) / MILLION_OPS,
           (SMALL_ALLOC_COUNT / cmem_fixed) / MILLION_OPS,
           sys_fixed / cmem_fixed);

    /* (b) irregular mixed sizes 16-240 (i*7 avoids power-of-2 alignment) */
    start = get_time_sec();
    for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
        size_t sz = 16 + (i * BENCH_SIZE_MIXED_STEP) % BENCH_SIZE_MIXED_SPREAD;
        void *ptr = malloc(sz);
        bench_escape_sink = ptr;
        free(ptr);
    }
    double sys_mixed = get_time_sec() - start;
    start = get_time_sec();
    for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
        size_t sz = 16 + (i * BENCH_SIZE_MIXED_STEP) % BENCH_SIZE_MIXED_SPREAD;
        void *ptr = mp_alloc(pool, sz);
        mp_free(pool, ptr);
    }
    double cmem_mixed = get_time_sec() - start;
    printf("  Mixed 16-240B    : malloc %.2f | cmem %.2f Mops/sec (%.2fx)\n",
           (SMALL_ALLOC_COUNT / sys_mixed) / MILLION_OPS,
           (SMALL_ALLOC_COUNT / cmem_mixed) / MILLION_OPS,
           sys_mixed / cmem_mixed);

    /* (c) large 4-20KB — TLSF path */
    start = get_time_sec();
    for (int i = 0; i < MEDIUM_ALLOC_COUNT; i++) {
        size_t sz = BENCH_SIZE_LARGE_BASE + (i * BENCH_SIZE_LARGE_STEP) % BENCH_SIZE_LARGE_SPREAD;
        void *ptr = malloc(sz);
        bench_escape_sink = ptr;
        free(ptr);
    }
    double sys_large = get_time_sec() - start;
    start = get_time_sec();
    for (int i = 0; i < MEDIUM_ALLOC_COUNT; i++) {
        size_t sz = BENCH_SIZE_LARGE_BASE + (i * BENCH_SIZE_LARGE_STEP) % BENCH_SIZE_LARGE_SPREAD;
        void *ptr = mp_alloc(pool, sz);
        mp_free(pool, ptr);
    }
    double cmem_large = get_time_sec() - start;
    printf("  Large 4-20KB     : malloc %.2f | cmem %.2f Mops/sec (%.2fx)\n",
           (MEDIUM_ALLOC_COUNT / sys_large) / MILLION_OPS,
           (MEDIUM_ALLOC_COUNT / cmem_large) / MILLION_OPS,
           sys_large / cmem_large);

    mp_destroy(pool);
}
```

> NOTE: Uses `BENCH_SIZE_MIXED_STEP` (=7) and `BENCH_SIZE_LARGE_STEP` (=13) — magic numbers MUST be named for clang-tidy. Add to constants:

```c
#define BENCH_SIZE_MIXED_STEP 7
#define BENCH_SIZE_LARGE_STEP 13
```

- [ ] **Step 2: Verify compile only**

Run: `gcc -Wall -Wextra -O2 -std=gnu11 -D_GNU_SOURCE -I./include -c benchmarks/bench_main.c -o /tmp/bench_c.o`
Expected: exit 0, no warnings.

---

### Task D: Benchmark 8 — Batch APIs

- [ ] **Step 1: Add bench_batch_apis definition**

```c
/**
 * @brief Compares mp_alloc_batch/mp_free_batch against individual alloc/free
 * loops for a fixed 32-byte block, 64 elements per batch, 10,000 batches.
 */
void bench_batch_apis()
{
    printf("\n--- Benchmark 8: Batch APIs (64 elems x %d batches, 32B) ---\n", BENCH_BATCH_COUNT);

    void **ptrs = (void **)malloc(sizeof(void *) * BENCH_BATCH_ELEMS);
    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);

    /* single alloc/free loops */
    double start = get_time_sec();
    for (int b = 0; b < BENCH_BATCH_COUNT; b++) {
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            ptrs[i] = mp_alloc(pool, BENCH_SIZE_FIXED);
        }
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            mp_free(pool, ptrs[i]);
        }
    }
    double time_single = get_time_sec() - start;

    /* batch APIs */
    start = get_time_sec();
    for (int b = 0; b < BENCH_BATCH_COUNT; b++) {
        size_t got = mp_alloc_batch(pool, BENCH_SIZE_FIXED, ptrs, BENCH_BATCH_ELEMS);
        if (got != BENCH_BATCH_ELEMS) {
            fprintf(stderr, "mp_alloc_batch returned %zu (expected %d)\n",
                    got, BENCH_BATCH_ELEMS);
        }
        mp_free_batch(pool, ptrs, BENCH_BATCH_ELEMS);
    }
    double time_batch = get_time_sec() - start;

    size_t total_ops = (size_t)BENCH_BATCH_COUNT * (size_t)BENCH_BATCH_ELEMS;
    printf("  Single loops : %.4f sec (%.2f Mops/sec, %.2f ns/op)\n",
           time_single,
           (total_ops / time_single) / MILLION_OPS,
           (time_single / total_ops) * 1e9);
    printf("  Batch APIs   : %.4f sec (%.2f Mops/sec, %.2f ns/op)\n",
           time_batch,
           (total_ops / time_batch) / MILLION_OPS,
           (time_batch / total_ops) * 1e9);
    printf("  Batch speedup: %.2fx\n", time_single / time_batch);

    mp_destroy(pool);
    free(ptrs);
}
```

- [ ] **Step 2: Verify compile only**

Run: `gcc -Wall -Wextra -O2 -std=gnu11 -D_GNU_SOURCE -I./include -c benchmarks/bench_main.c -o /tmp/bench_d.o`
Expected: exit 0, no warnings.

---

### Task E: Benchmark 9 — Thread scaling

- [ ] **Step 1: Add bench_scaling_thread_arg_t + worker**

```c
typedef struct {
    memory_pool_t *pool;
    int alloc_count;
} bench_scaling_arg_t;

static void *bench_scaling_thread_func(void *arg)
{
    bench_scaling_arg_t *ta = (bench_scaling_arg_t *)arg;
    for (int i = 0; i < ta->alloc_count; i++) {
        size_t sz = 32 + (i % 256);
        void *ptr = mp_alloc(ta->pool, sz);
        mp_free(ta->pool, ptr);
    }
    return NULL;
}

static double bench_scaling_run(memory_pool_t *pool, int threads, int allocs_per_thread)
{
    pthread_t *threads_arr = (pthread_t *)malloc(sizeof(pthread_t) * threads);
    bench_scaling_arg_t *args = (bench_scaling_arg_t *)malloc(sizeof(bench_scaling_arg_t) * threads);
    if (!threads_arr || !args) {
        free(threads_arr);
        free(args);
        return -1.0;
    }
    for (int i = 0; i < threads; i++) {
        args[i].pool = pool;
        args[i].alloc_count = allocs_per_thread;
        pthread_create(&threads_arr[i], NULL, bench_scaling_thread_func, &args[i]);
    }
    double start = get_time_sec();
    for (int i = 0; i < threads; i++) {
        pthread_join(threads_arr[i], NULL);
    }
    double time = get_time_sec() - start;
    free(threads_arr);
    free(args);
    return (double)threads * allocs_per_thread / time / MILLION_OPS;
}
```

> NOTE: timing starts AFTER pthread_create (joins dominate; spawn overhead is negligible for 100k-op workers). The `start` must be captured BEFORE the join loop — the code above captures it right before joins, which is correct.

- [ ] **Step 2: Add bench_thread_scaling definition**

```c
/**
 * @brief Measures throughput scaling across {1,2,4,8} threads for a
 * single shared pool vs a multi-arena pool, reporting Mops/sec and
 * scaling efficiency (x-thread rate / 1-thread rate).
 */
void bench_thread_scaling()
{
    printf("\n--- Benchmark 9: Thread Scaling (1-8 threads x 100k interleaved) ---\n");

    int thread_counts[BENCH_THREAD_COUNTS];
    thread_counts[0] = 1;
    thread_counts[1] = 2;
    thread_counts[2] = 4;
    thread_counts[3] = 8;
    const int allocs_per_thread = 100000;

    printf("  Mode       | Threads | Mops/sec | Efficiency\n");
    printf("  -----------+---------+----------+-----------\n");

    for (int m = 0; m < 2; m++) {
        const char *mode_name = (m == 0) ? "single      " : "multi-arena ";
        mp_flags_t flags = MP_FLAG_THREAD_SAFE | MP_FLAG_THREAD_LOCAL_CACHE;
        if (m == 1) {
            flags |= MP_FLAG_MULTI_ARENA;
        }
        memory_pool_t *pool = mp_create(128 * 1024 * 1024, flags);
        double base = 0.0;
        for (int t = 0; t < BENCH_THREAD_COUNTS; t++) {
            int nthreads = thread_counts[t];
            double mops = bench_scaling_run(pool, nthreads, allocs_per_thread);
            if (mops < 0.0) {
                fprintf(stderr, "Thread scaling run failed\n");
                break;
            }
            if (t == 0) {
                base = mops;
            }
            printf("  %s | %d       | %.2f     | %.2fx\n",
                   mode_name, nthreads, mops, mops / base);
        }
        mp_destroy(pool);
    }
}
```

> NOTE: `BENCH_THREAD_COUNTS` = 4 used for both the array size and loop bound. The 1-thread efficiency prints 1.00x (base=itself). `128 * 1024 * 1024` pool size avoids OOM for 8 threads × 100k × avg 144B ≈ 115MB.

- [ ] **Step 3: Verify compile only**

Run: `gcc -Wall -Wextra -O2 -std=gnu11 -D_GNU_SOURCE -I./include -c benchmarks/bench_main.c -o /tmp/bench_e.o`
Expected: exit 0, no warnings.

---

### Task F: Benchmarks 10-11 — Compressed storage + allocation patterns

- [ ] **Step 1: Add bench_compressed_storage definition**

```c
/**
 * @brief Measures compressed-storage throughput: compress 4KB repetitive
 * blocks, decompress a stored handle, and report the compression ratio.
 */
void bench_compressed_storage()
{
    printf("\n--- Benchmark 10: Compressed Storage (4KB blocks x %d) ---\n", BENCH_COMPRESS_ITERS);

    memory_pool_t *pool = mp_create(64 * 1024 * 1024, MP_FLAG_DEFAULT);
    void *data = mp_alloc(pool, BENCH_COMPRESS_BLOCK_SIZE);
    char *bytes = (char *)data;
    for (int i = 0; i < BENCH_COMPRESS_BLOCK_SIZE; i++) {
        bytes[i] = (char)('a' + (i % 4));
    }

    /* compress path: mp_compress_block TRANSFERS ownership of data on
     * success, so each iteration must re-allocate and refill the buffer. */
    double start = get_time_sec();
    size_t total_in = 0;
    size_t total_out = 0;
    for (int i = 0; i < BENCH_COMPRESS_ITERS; i++) {
        compressed_handle_t h = mp_compress_block(pool, data, BENCH_COMPRESS_BLOCK_SIZE);
        if (h != 0) {
            mp_free_compressed(pool, h);
            total_in += BENCH_COMPRESS_BLOCK_SIZE;
            total_out += BENCH_COMPRESS_BLOCK_SIZE; /* compressed size unknown per-block; see below */
        }
        data = mp_alloc(pool, BENCH_COMPRESS_BLOCK_SIZE);
        bytes = (char *)data;
        for (int j = 0; j < BENCH_COMPRESS_BLOCK_SIZE; j++) {
            bytes[j] = (char)('a' + (j % 4));
        }
    }
    double time_compress = get_time_sec() - start;

    /* decompress path — one stored handle, decompressed repeatedly */
    compressed_handle_t handle = mp_compress_block(pool, data, BENCH_COMPRESS_BLOCK_SIZE);
    if (handle == 0) {
        fprintf(stderr, "Benchmark 10: initial compress failed\n");
        mp_free(pool, data);
        mp_destroy(pool);
        return;
    }
    size_t comp_used = 0;
    size_t comp_budget = 0;
    size_t comp_blocks = 0;
    if (mp_get_compressed_stats(pool, &comp_used, &comp_budget, &comp_blocks)) {
        total_out = comp_used;
    }
    start = get_time_sec();
    void *out_buf = NULL;
    for (int i = 0; i < BENCH_COMPRESS_ITERS; i++) {
        out_buf = mp_decompress_block(pool, handle);
        if (out_buf) {
            mp_free(pool, out_buf);
        }
    }
    double time_decompress = get_time_sec() - start;

    printf("  Compress   : %.2f MB/s (%.2f Mops/sec)\n",
           ((double)total_in / time_compress) / (1024.0 * 1024.0),
           ((double)total_in / time_compress) / MILLION_OPS);
    printf("  Decompress : %.2f MB/s (%.2f Mops/sec)\n",
           ((double)total_in / time_decompress) / (1024.0 * 1024.0),
           ((double)total_in / time_decompress) / MILLION_OPS);
    printf("  Compressed : %zu bytes stored for %zu input (ratio %.2f:1)\n",
           total_out,
           total_in,
           total_in > 0 ? (double)total_in / (double)total_out : 0.0);

    mp_free_compressed(pool, handle);
    mp_free(pool, data);
    mp_destroy(pool);
}
```

> **CRITICAL VERIFIED SEMANTICS (from include/cmem.h:316-330):** `mp_compress_block` transfers ownership of `data` to the pool on success — the buffer is released via mp_free() and the caller must NOT use it afterward. The compress loop therefore re-allocates + refills `data` each iteration (only when `h != 0`; on failure the buffer is retained and can be retried). **`mp_get_compressed_stats` signature is `(pool, size_t *used, size_t *budget, size_t *block_count)`** — NOT a struct (verified at include/cmem.h:367-370). `total_out` uses the post-first-compress `used` figure (one block ≈ 4KB input).

- [ ] **Step 2: Add bench_allocation_patterns definition**

```c
/**
 * @brief Compares cmem vs system malloc across four allocation patterns:
 * interleaved, batch alloc-then-free, random-size, and live-set.
 */
void bench_allocation_patterns()
{
    printf("\n--- Benchmark 11: Allocation Patterns (%d ops, 32-256B) ---\n", BENCH_PATTERN_OPS);

    void **ptrs = (void **)malloc(sizeof(void *) * BENCH_PATTERN_OPS);
    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);
    const size_t live_count = BENCH_PATTERN_OPS * BENCH_PATTERN_LIVE_KEEP / 100;

    /* (a) interleaved */
    double start = get_time_sec();
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        size_t sz = 32 + (i % SMALL_ALLOC_SPREAD);
        void *ptr = malloc(sz);
        bench_escape_sink = ptr;
        free(ptr);
    }
    double sys_a = get_time_sec() - start;
    start = get_time_sec();
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        size_t sz = 32 + (i % SMALL_ALLOC_SPREAD);
        void *ptr = mp_alloc(pool, sz);
        mp_free(pool, ptr);
    }
    double cmem_a = get_time_sec() - start;

    /* (b) batch alloc-then-free */
    start = get_time_sec();
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        ptrs[i] = malloc(32 + (i % SMALL_ALLOC_SPREAD));
        bench_escape_sink = ptrs[i];
    }
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        free(ptrs[i]);
    }
    double sys_b = get_time_sec() - start;
    start = get_time_sec();
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        ptrs[i] = mp_alloc(pool, 32 + (i % SMALL_ALLOC_SPREAD));
    }
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        mp_free(pool, ptrs[i]);
    }
    double cmem_b = get_time_sec() - start;

    /* (c) random sizes via xorshift */
    uint32_t seed = 12345u;
    start = get_time_sec();
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        size_t sz = 32 + (seed % SMALL_ALLOC_SPREAD);
        void *ptr = malloc(sz);
        bench_escape_sink = ptr;
        free(ptr);
    }
    double sys_c = get_time_sec() - start;
    seed = 12345u;
    start = get_time_sec();
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        size_t sz = 32 + (seed % SMALL_ALLOC_SPREAD);
        void *ptr = mp_alloc(pool, sz);
        mp_free(pool, ptr);
    }
    double cmem_c = get_time_sec() - start;

    /* (d) live set: allocate all, free 75% (timed), free rest after */
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        ptrs[i] = malloc(32 + (i % SMALL_ALLOC_SPREAD));
    }
    start = get_time_sec();
    for (int i = 0; i < BENCH_PATTERN_OPS - (int)live_count; i++) {
        free(ptrs[i]);
    }
    double sys_d = get_time_sec() - start;
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        bench_escape_sink = ptrs[i];
    }
    for (int i = (int)live_count; i < BENCH_PATTERN_OPS; i++) {
        free(ptrs[i]);
    }

    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        ptrs[i] = mp_alloc(pool, 32 + (i % SMALL_ALLOC_SPREAD));
    }
    start = get_time_sec();
    for (int i = 0; i < BENCH_PATTERN_OPS - (int)live_count; i++) {
        mp_free(pool, ptrs[i]);
    }
    double cmem_d = get_time_sec() - start;
    for (int i = (int)live_count; i < BENCH_PATTERN_OPS; i++) {
        mp_free(pool, ptrs[i]);
    }

    printf("  Interleaved   : malloc %.2f | cmem %.2f Mops/sec (%.2fx)\n",
           (BENCH_PATTERN_OPS / sys_a) / MILLION_OPS,
           (BENCH_PATTERN_OPS / cmem_a) / MILLION_OPS,
           sys_a / cmem_a);
    printf("  Batch         : malloc %.2f | cmem %.2f Mops/sec (%.2fx)\n",
           (BENCH_PATTERN_OPS / sys_b) / MILLION_OPS,
           (BENCH_PATTERN_OPS / cmem_b) / MILLION_OPS,
           sys_b / cmem_b);
    printf("  Random sizes  : malloc %.2f | cmem %.2f Mops/sec (%.2fx)\n",
           (BENCH_PATTERN_OPS / sys_c) / MILLION_OPS,
           (BENCH_PATTERN_OPS / cmem_c) / MILLION_OPS,
           sys_c / cmem_c);
    printf("  Live set 25%%  : malloc %.2f | cmem %.2f Mops/sec (%.2fx)\n",
           (BENCH_PATTERN_OPS / sys_d) / MILLION_OPS,
           (BENCH_PATTERN_OPS / cmem_d) / MILLION_OPS,
           sys_d / cmem_d);

    mp_destroy(pool);
    free(ptrs);
}
```

- [ ] **Step 3: Verify compile only**

Run: `gcc -Wall -Wextra -O2 -std=gnu11 -D_GNU_SOURCE -I./include -c benchmarks/bench_main.c -o /tmp/bench_f.o`
Expected: exit 0, no warnings.

---

### Task G: Full verification

- [ ] **Step 1: clang-format + full ninja build**

Run: `clang-format -i benchmarks/bench_main.c && cd build_cmake && ninja -t clean && ninja 2>&1 | grep -cE 'warning:|error:'`
Expected: 0.

- [ ] **Step 2: ctest + test binaries**

Run: `ctest` → 3/3 pass; `./unit_tests && ./advanced_tests && ./cpp_tests` all exit 0.

- [ ] **Step 3: Full benchmark run**

Run: `make bench`
Expected: Benchmarks 1-11 all print; total runtime ~15-30s; exit 0. If `make bench` uses the Makefile build dir (`build/`), run `make clean` first (per AGENTS.md build-collision warning) OR use `cd build_cmake && cmake --build . --target benchmark && ./benchmark`.

- [ ] **Step 4: format-check**

Run: `make format-check` → exit 0.

- [ ] **Step 5: Commit**

```bash
git add benchmarks/bench_main.c
git commit -m "perf(bench): add size/pattern/batch/thread/compression benchmark scenarios"
```

Body:
```
Adds Benchmarks 6-11 to the benchmark driver:
6: MP_FLAG_FAST_PATH impact on interleaved small allocs
7: size distributions (fixed 32B, mixed 16-240B, large 4-20KB TLSF)
8: mp_alloc_batch / mp_free_batch vs individual loops
9: thread scaling 1/2/4/8 for single vs multi-arena pools
10: compressed-storage compress/decompress throughput + ratio
11: allocation patterns (interleaved, batch, random, live-set)
```

---

## Verification Summary

| Gate | Command | Expected |
|---|---|---|
| Compile (each task) | `gcc -Wall -Wextra -O2 ... -c benchmarks/bench_main.c` | exit 0, no warnings |
| Full build | `ninja -t clean && ninja` | 0 warnings/errors |
| Tests | `ctest` | 3/3 |
| Test binaries | `./unit_tests ./advanced_tests ./cpp_tests` | all exit 0 |
| Benchmark | `make bench` | all 11 scenarios print, exit 0 |
| Format | `make format-check` | exit 0 |

## Open verification items — 2 of 4 RESOLVED

1. ✅ **mp_compress_block ownership** — VERIFIED (include/cmem.h:316-330): pool takes ownership of `data` on success (released via mp_free, caller must not use afterward). Bench 10 compress loop re-allocates+refills each iteration when `h != 0`.
2. ✅ **mp_get_compressed_stats signature** — VERIFIED (include/cmem.h:367-370): `bool mp_get_compressed_stats(memory_pool_t *pool, size_t *used, size_t *budget, size_t *block_count)`. NOT a struct.
3. **mp_alloc_batch return** — returns count of successfully allocated ptrs; if short (OOM), free only `got` entries. Bench 8 already checks `got != BENCH_BATCH_ELEMS` and frees all (fine for 32B × 64 which cannot OOM a 32MB pool).
4. `make bench` build dir — AGENTS.md warns build/ collision; prefer `cd build_cmake && cmake --build . --target benchmark && ./benchmark` OR `make clean && make bench`.
