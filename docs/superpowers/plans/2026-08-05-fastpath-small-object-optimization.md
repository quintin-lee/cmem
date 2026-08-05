# Fast Path Small Object Allocation Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement inline fast path allocation and free functions (`mp_alloc_fast`, `mp_free_fast`), eliminate mutex locks on thread-local cache frees under `MP_FLAG_FAST_PATH`, use an $O(1)$ size-to-class lookup table, and streamline block header initialization.

**Architecture:** Expose `cmem_tls_cache` and `cmem_size_to_class` in `include/cmem.h`, update `src/cmem_event.c` to eliminate mutex locks on TLS cache frees, and update `benchmarks/bench_main.c` Benchmark 6 to evaluate `mp_alloc_fast`/`mp_free_fast`.

**Tech Stack:** C11, C++17 PMR, GCC/Clang, POSIX pthreads

---

### Task 1: Expose Inline Fast Path Types and Functions in `include/cmem.h` and Export Lookup Table

**Files:**
- Modify: `include/cmem.h:100-300`
- Modify: `src/cmem.c:10-50`
- Modify: `src/cmem_slab.c:10-60`
- Modify: `src/cmem_internal.h:20-60`

- [ ] **Step 1: Define `cmem_size_to_class` and inline `mp_alloc_fast` / `mp_free_fast` in `include/cmem.h`**

Add public definitions for fast-path structures and inline functions to `include/cmem.h`:

```c
typedef struct mp_slab_slot {
    struct mp_slab_slot *next;
} mp_slab_slot_t;

typedef struct {
    mp_slab_slot_t *slots[16];
    uint16_t counts[16];
    void *owner_pool;
} mp_tls_cache_t;

extern _Thread_local mp_tls_cache_t cmem_tls_cache;
extern const uint8_t cmem_size_to_class[513];

static inline void *mp_alloc_fast(memory_pool_t *pool, size_t size)
{
    if (__builtin_expect(pool != NULL && (pool->flags & MP_FLAG_FAST_PATH) != 0 && size > 0 && size <= 512, 1)) {
        uint8_t class_idx = cmem_size_to_class[size];
        if (__builtin_expect(cmem_tls_cache.counts[class_idx] > 0, 1)) {
            mp_slab_slot_t *slot = cmem_tls_cache.slots[class_idx];
            cmem_tls_cache.slots[class_idx] = slot->next;
            cmem_tls_cache.counts[class_idx]--;

            mp_block_header_t *header = (mp_block_header_t *)slot;
            header->alloc_type = ALLOC_TYPE_SLAB;
            header->slab_class = class_idx;
            header->flags = 0;
            header->requested_size = size;
            header->usable_size = 512;
            header->raw_base = slot;
            header->subpool = pool;

            return (void *)((uint8_t *)header + sizeof(mp_block_header_t));
        }
    }
    return mp_alloc(pool, size);
}

static inline void mp_free_fast(memory_pool_t *pool, void *ptr)
{
    if (__builtin_expect(ptr != NULL && pool != NULL && (pool->flags & MP_FLAG_FAST_PATH) != 0, 1)) {
        mp_block_header_t *header = (mp_block_header_t *)((uint8_t *)ptr - sizeof(mp_block_header_t));
        if (__builtin_expect(header->alloc_type == ALLOC_TYPE_SLAB, 1)) {
            uint8_t class_idx = header->slab_class;
            if (__builtin_expect(cmem_tls_cache.counts[class_idx] < TLS_CACHE_MAX_SLOTS, 1)) {
                mp_slab_slot_t *slot = (mp_slab_slot_t *)header->raw_base;
                slot->next = cmem_tls_cache.slots[class_idx];
                cmem_tls_cache.slots[class_idx] = slot;
                cmem_tls_cache.counts[class_idx]++;
                return;
            }
        }
    }
    mp_free(pool, ptr);
}
```

- [ ] **Step 2: Export `cmem_size_to_class` and `cmem_tls_cache` symbol linkage in `src/cmem_slab.c`**

Define the static lookup table mapping sizes `1..512` to slab class indices `0..15` in `src/cmem_slab.c` and CMakeLists.txt (updating format-check and target files if needed).

- [ ] **Step 3: Run build to verify compilation**

Run: `make bench && cmake -B build_cmake -G Ninja && cmake --build build_cmake`
Expected: Compiles cleanly with zero errors.

- [ ] **Step 4: Commit**

```bash
git add include/cmem.h src/cmem.c src/cmem_slab.c src/cmem_internal.h CMakeLists.txt
git commit -m "feat(alloc): expose inline mp_alloc_fast and mp_free_fast in cmem.h"
```

---

### Task 2: Remove Mutex Lock Acquisitions on TLS Cache Frees under `MP_FLAG_FAST_PATH`

**Files:**
- Modify: `src/cmem_event.c:2310-2335`

- [ ] **Step 1: Eliminate mutex locking on TLS Cache hits in `mp_free` for FAST_PATH pools**

In `src/cmem_event.c`, update the TLS Cache free branch:

```c
        if (tls_cache.counts[class_idx] < TLS_CACHE_MAX_SLOTS) {
            if (!(pool->flags & MP_FLAG_FAST_PATH)) {
                if (!(pool->flags & MP_FLAG_THREAD_SAFE)) {
                    mp_free_stats_update(pool, header);
                } else {
                    pool_lock(pool);
                    mp_free_stats_update(pool, header);
                    pool_unlock(pool);
                }
                if (pool->event_cb) {
                    trigger_event(pool, MP_EVENT_FREE, ptr, header->requested_size);
                }
            }

            mp_slab_slot_t *slot = (mp_slab_slot_t *)header->raw_base;
            slot->next = tls_cache.slots[class_idx];
            tls_cache.slots[class_idx] = slot;
            tls_cache.counts[class_idx]++;
            return;
        }
```

- [ ] **Step 2: Run test suite and benchmarks**

Run: `make format-check && make bench && ctest --test-dir build_cmake`
Expected: Format check clean, tests 100% passing, benchmark runs cleanly.

- [ ] **Step 3: Commit**

```bash
git add src/cmem_event.c
git commit -m "perf(alloc): eliminate mutex locking on TLS cache free under MP_FLAG_FAST_PATH"
```

---

### Task 3: Update Benchmark 6 and Evaluate Accelerated Small Object Allocation Throughput

**Files:**
- Modify: `benchmarks/bench_main.c:415-460`

- [ ] **Step 1: Update Benchmark 6 in `benchmarks/bench_main.c` to test `mp_alloc_fast` / `mp_free_fast`**

Update `bench_fast_path()` to use `mp_alloc_fast` and `mp_free_fast`:

```c
void bench_fast_path()
{
    printf("\n--- Benchmark 6: FAST_PATH (interleaved %d x 32-256B) ---\n", BENCH_FAST_PATH_ITERS);

    memory_pool_t *plain_pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);
    bench_warmup(plain_pool);

    double plain_times[BENCH_MEDIAN_RUNS];
    for (int run = 0; run < BENCH_MEDIAN_RUNS; run++) {
        double start = get_time_sec();
        for (int i = 0; i < BENCH_FAST_PATH_ITERS; i++) {
            size_t sz = 32 + (i % SMALL_ALLOC_SPREAD);
            void *ptr = mp_alloc(plain_pool, sz);
            bench_escape(ptr);
            mp_free(plain_pool, ptr);
        }
        plain_times[run] = get_time_sec() - start;
    }
    double time_plain = get_median_time(plain_times, BENCH_MEDIAN_RUNS);
    mp_destroy(plain_pool);

    memory_pool_t *fast_pool =
        mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE | MP_FLAG_FAST_PATH);
    bench_warmup(fast_pool);

    double fast_times[BENCH_MEDIAN_RUNS];
    for (int run = 0; run < BENCH_MEDIAN_RUNS; run++) {
        double start = get_time_sec();
        for (int i = 0; i < BENCH_FAST_PATH_ITERS; i++) {
            size_t sz = 32 + (i % SMALL_ALLOC_SPREAD);
            void *ptr = mp_alloc_fast(fast_pool, sz);
            bench_escape(ptr);
            mp_free_fast(fast_pool, ptr);
        }
        fast_times[run] = get_time_sec() - start;
    }
    double time_fast = get_median_time(fast_times, BENCH_MEDIAN_RUNS);
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

- [ ] **Step 2: Run formatting check, full test suite, and benchmarks**

Run: `clang-format -i benchmarks/bench_main.c && make format-check && make bench && ctest --test-dir build_cmake`
Expected: 100% tests pass, `+MP_FLAG_FAST_PATH` speedup shows significant improvement.

- [ ] **Step 3: Commit**

```bash
git add benchmarks/bench_main.c
git commit -m "test(bench): evaluate mp_alloc_fast and mp_free_fast in benchmark 6"
```
