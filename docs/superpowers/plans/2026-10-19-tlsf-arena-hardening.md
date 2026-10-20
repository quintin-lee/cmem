# Per-Thread TLSF Arena Hardening — Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the confirmed defects in the per-thread TLSF arena (multi-arena child pools), prove correctness with targeted + randomized + multithreaded tests, measure performance, and close out the stale 2026-08-08 plan/spec.

**Architecture:** Adopt the shared-TLSF block invariants verbatim (distance-based size field, end sentinel, doubly-linked address-sorted free list, single insert/unlink helper). Make arena blocks self-describing (`MP_BLOCK_FLAG_TLSF_ARENA` in `header->flags`, `header->subpool` holds the `tlsf_arena_t *` descriptor). Route alloc/free/realloc by flag, never by the caller's pool handle. Descriptors are pool-owned (registry, released at `mp_destroy`). Cross-thread and post-retire frees go through a lock-free MPSC mailbox; the last freer (or the retiring thread) claims cleanup and releases the region.

**Tech Stack:** C11 (`_Atomic`, `__atomic` builtins via existing `CMEM_ATOMIC_*` macros where present), pthreads, ASan/UBSan/TSan, Makefile gates (`make test_all`, `make CONFIG=… test_all`, `make cmake-build && make cmake-test`, `make format-check`).

**Spec:** `docs/superpowers/specs/2026-10-19-tlsf-arena-hardening-design.md` (Approved).

**Execution notes for the implementer:**
- Repo root: `/data/home/quintin/workspace/source/c/cmem`. Work on `master`; no worktree.
- This plan file stays **untracked** per AGENTS.md (plans are not committed). Commit only code/test/doc changes; each chunk ends with one or more commits using the repo's conventional-commit + emoji style (e.g. `fix(arena): 🐛 …`, `test(arena): ✅ …`), no push, no amend.
- `make clean` before switching between Makefile and CMake builds (both collide on `build/` in older flows; Makefile uses `build-$(CONFIG)`).
- Existing suite must stay green after every task except where a step explicitly expects a red test.
- Anti-defect map (from spec): D1 FREE-on-live corruption, D2 dead coalescing, D3 missing sentinel, D4 drain/destroy never called (leak), D5 NULL-tpool crash on master/cross-thread free, D6 stats, D7 unsorted remainder, D11 unpaired `pool_unlock`, D12 shared-TLSF data race.

> **Pre-execution note (added 2026-10-19):** D12 (shared-TLSF data race, spec
> Section 3.6) was discovered while preparing this plan's verification and fixed
> **before Chunk 0**: coarse per-arena `tpool->lock` serialization in
> `src/cmem_tlsf.c` plus two residual `cmem_event.c` races (usable_size-derived
> block size in `mp_free`; ts-gated atomic histogram add). Regression test
> `test_shared_tlsf_threaded_race` in `tests/test_advanced.c` (red before:
> hang/segfault + 165 TSan races; green after, zero warnings). Committed as a
> separate `fix(tlsf):` commit before the Chunk 1 work. Chunks below execute on
> this fixed base.

---

## Baseline (captured during Chunk 0) and post-fix comparison

Fill the **before** cells from the Chunk 0 baseline run (`make bench`). Fill the
**after** cells in Chunk 5 Task 5.1 (`make bench` again). Every multi-arena cell
is written `before → after` so the post-fix comparison is meaningful. `regular`
is the control (arena off). `p50`/`p99` are sampled only in single-thread mode
(Task 0.1 Step 2), so they are `N/A` for 2/4/8 threads.

| size | threads | regular Mops/s | multi-arena Mops/s (before → after) | p50 ns, 1 th (before → after) | p99 ns, 1 th (before → after) |
|------|---------|----------------|--------------------------------------|-------------------------------|-------------------------------|
| 1 KiB   | 1 | 5.10 | 6.62 → 3.53 | 65.0 → 135.0 | 102.0 → 147.0 |
| 1 KiB   | 2 | 2.81 | 16.30 → 10.62 | N/A | N/A |
| 1 KiB   | 4 | 2.40 | 32.19 → 21.04 | N/A | N/A |
| 1 KiB   | 8 | 2.05 | 61.63 → 43.03 | N/A | N/A |
| 8 KiB   | 1 | 5.63 | 5.03 → 4.13 | 98.0 → 106.0 | 109.0 → 145.0 |
| 8 KiB   | 2 | 3.47 | 16.37 → 10.79 | N/A | N/A |
| 8 KiB   | 4 | 2.06 | 31.98 → 21.53 | N/A | N/A |
| 8 KiB   | 8 | 2.01 | 62.75 → 43.80 | N/A | N/A |
| 64 KiB  | 1 | 6.07 | 5.01 → 3.69 | 100.0 → 127.0 | 108.0 → 139.0 |
| 64 KiB  | 2 | 1.98 | 16.44 → 11.29 | N/A | N/A |
| 64 KiB  | 4 | 1.70 | 32.23 → 22.34 | N/A | N/A |
| 64 KiB  | 8 | 1.53 | 63.00 → 36.58 | N/A | N/A |
| 192 KiB | 1 | 4.21 | 5.57 → 3.84 | 66.0 → 127.0 | 112.0 → 134.0 |
| 192 KiB | 2 | 2.72 | 16.40 → 11.35 | N/A | N/A |
| 192 KiB | 4 | 1.74 | 32.22 → 21.14 | N/A | N/A |
| 192 KiB | 8 | 1.58 | 62.06 → 43.05 | N/A | N/A |
| 1 MiB (reference) | 1 | 4.27 | 4.96 → 4.14 | 82.0 → 113.0 | 124.0 → 145.0 |

`regular` is the same pool **without** `MP_FLAG_MULTI_ARENA` (arena off) and is
the A/B control; the fix does not touch that path, so a single column suffices.
The 1 MiB row is the shared-pool reference: 1 MiB can never be arena-resident
(arena = 256 KiB), so it always exercises the fallback.

**Documented spec deviation (§5):** the spec asks the benchmark to report the
shared-pool fallback rate. The current code exposes no counter for arena
fallbacks and adding one is out of scope for this hardening effort. The plan
therefore records the fallback rate as **not instrumentable** and states this
explicitly as a deviation. The 192 KiB / 1 MiB rows are the indirect proxy for
fallback behavior (larger sizes fall back more often).

**After run environment note:** the post-fix run was taken on a busier machine
than the baseline (load average ~3 on 32 cores; one concurrent `benchmark`
process was identified and eliminated between repeats). The untouched `regular`
control also reads ~20-25% below its baseline (e.g. 1 KiB/8 threads 2.05 -> 1.47,
8 KiB/1 thread 5.63 -> 4.16), so absolute before/after deltas understate the
arena path. Three post-fix repeats show single-thread multi-arena spread
3.53-4.69 Mops/s (1 KiB), i.e. run-to-run noise dominates the single-thread
delta; the multi-thread speedup ratio is preserved (1 KiB/8 threads: 30.1x
before vs 29.3x after relative to regular in the same run). Decision: **no
tuning needed** - no arena-eligible size/thread combination regresses beyond
run-to-run noise once the control is accounted for.

Keep raw `make bench` output in /tmp only (never commit it).

**Baseline base (deviation from this plan's original wording):** the baseline was
captured on the tree **after** the D12 fix (`238bc09`), not on `5831392`. The
benchmark's threaded arms are exactly the D12 repro condition (regular/shared
TLSF at ≥ 64 KiB with 2–8 threads, and the 1 MiB multi-arena fallback), so a
pre-D12 baseline hangs or segfaults and cannot produce a table. The arena
defects D1–D7 are still present at baseline, so every `before → after` delta
still measures the arena hardening. Raw run: `/tmp/bench_baseline_full.txt`
(`make clean && make bench`, exit 0, all 40 rows present).

---

## Chunk 0: Benchmark scenario + baseline

### Task 0.1: Add `bench_tlsf_arena` scenario to `benchmarks/bench_main.c`

**Files:**
- Modify: `benchmarks/bench_main.c` (declarations near lines 261-274, definition after `bench_thread_scaling` ~1135, call in `main()` ~280-301)

- [x] **Step 1: Add the scenario implementation**

Insert after the `bench_thread_scaling` definition. The thread function churns one live block at a time (interleaved alloc/free), so peak memory stays at `threads × alloc_size`; a hold-all pattern would demand up to `50000 × 192 KiB = 9.6 GiB` per thread and silently degrade to NULL-returning (failure-path) allocs, making every number meaningless. Keep bench knobs as named `#define`s (`TLSF_ARENA_*`) — `readability-magic-numbers` will flag literals like `50000`/`196608`. Note the free-handle pattern keeps the benchmark runnable BOTH on pre-fix HEAD (where master-handle free would crash, D5) and post-fix: free through the thread's bound arena child when one exists.

```c
/* ------------------------------------------------------------------ */
/* TLSF arena benchmark: multi-arena (arena on) vs regular (arena off) */
/* ------------------------------------------------------------------ */

#define TLSF_ARENA_SAMPLES 200000
#define TLSF_ARENA_ALLOCS_PER_THREAD 50000

typedef struct {
    memory_pool_t *pool;
    int            threads;
    int            allocs_per_thread;
    size_t         alloc_size;
    double        *samples;      /* per-op latency buffer (single-thread mode only) */
    int            sample_cap;   /* capacity of samples[]                            */
    int            sample_count; /* written sample count                             */
} tlsf_arena_arg_t;

static double bench_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void *tlsf_arena_thread_func(void *arg)
{
    tlsf_arena_arg_t *barg = (tlsf_arena_arg_t *)arg;
    int    sample = (barg->samples != NULL);
    int    i;

    /* Free through the thread's bound arena child when one exists; the
     * master handle is only a fallback (pre-fix HEAD would crash freeing an
     * arena block via the master handle, D5). */
    memory_pool_t *free_pool = mp_get_thread_arena(barg->pool);
    if (free_pool == NULL) {
        free_pool = barg->pool;
    }

    /* Interleaved alloc/free churn: exactly one live block per thread at a
     * time, so the peak footprint is threads * alloc_size — far below the
     * 128 MiB pool even for 192 KiB / 1 MiB sizes. (A hold-all pattern would
     * demand up to 50000 * 192 KiB = 9.6 GiB per thread, silently degrading
     * to NULL-returning failure-path allocs.) Same bounded pattern as
     * bench_scaling_thread_func. */
    for (i = 0; i < barg->allocs_per_thread; i++) {
        double t0 = sample ? bench_now_ns() : 0.0;
        void *ptr = mp_alloc(barg->pool, barg->alloc_size);
        if (sample && barg->sample_count < barg->sample_cap) {
            barg->samples[barg->sample_count++] = bench_now_ns() - t0;
        }
        bench_escape(ptr);
        if (ptr != NULL) {
            t0 = sample ? bench_now_ns() : 0.0;
            mp_free(free_pool, ptr);
            if (sample && barg->sample_count < barg->sample_cap) {
                barg->samples[barg->sample_count++] = bench_now_ns() - t0;
            }
        }
    }
    return NULL;
}
```

- [x] **Step 2: Add the double comparator + percentile helper**

```c
static int cmp_double(const void *lhs, const void *rhs)
{
    double a = *(const double *)lhs;
    double b = *(const double *)rhs;
    return (a < b) ? -1 : (a > b) ? 1 : 0;
}

static double percentiles_ns(const double *sorted, int n, double pct)
{
    int idx = (int)(pct * (double)(n - 1));
    return sorted[idx];
}
```

`cmp_double` is the qsort comparator the percentile helper needs (without it the
samples cannot be ordered, so p50/p99 would be garbage). Single-thread latency
sampling is implemented in `tlsf_arena_thread_func` (Step 1): when `samples != NULL`
the thread timestamps every `mp_alloc`/`mp_free` with `bench_now_ns()`. The runner
(Step 3) allocates the buffer only for `threads == 1`, `qsort`s with `cmp_double`,
and reports p50/p99 via `percentiles_ns`. 50 000 alloc+free pairs = 100 000
samples, below `TLSF_ARENA_SAMPLES` (200 000).

- [x] **Step 3: Declare + call it**

```c
/* forward declaration near the other bench decls (line ~261-274) */
static void bench_tlsf_arena(void);

/* in main(), after bench_thread_scaling() call */
bench_tlsf_arena();
```

```c
static void bench_tlsf_arena(void)
{
    static const size_t sizes[] = { 1024, 8192, 65536, 196608, 1048576 };
    static const char  *labels[] = { "1KiB", "8KiB", "64KiB", "192KiB", "1MiB(ref)" };
    static const int    thread_counts[] = { 1, 2, 4, 8 };
    const int           n_sizes = (int)(sizeof(sizes) / sizeof(sizes[0]));
    const int           nthread_counts =
        (int)(sizeof(thread_counts) / sizeof(thread_counts[0]));
    int s;
    int t;
    int mode;

    printf("\n=== TLSF Arena Benchmark (multi-arena vs regular) ===\n");

    for (s = 0; s < n_sizes; s++) {
        for (mode = 0; mode < 2; mode++) {
            for (t = 0; t < nthread_counts; t++) {
                memory_pool_t *pool = mp_create(
                    128 * 1024 * 1024,
                    MP_FLAG_THREAD_SAFE | MP_FLAG_THREAD_LOCAL_CACHE |
                    (mode == 1 ? MP_FLAG_MULTI_ARENA : MP_FLAG_DEFAULT));
                if (pool == NULL) {
                    continue;
                }
                bench_warmup(pool);
                int nthreads = thread_counts[t];
                pthread_t threads[8];
                tlsf_arena_arg_t args[8];
                double *samples = NULL;
                int i;
                double t0;
                double elapsed;
                double mops;

                if (nthreads == 1) {
                    samples = (double *)malloc(TLSF_ARENA_SAMPLES * sizeof(double));
                }
                for (i = 0; i < nthreads; i++) {
                    args[i].pool = pool;
                    args[i].threads = nthreads;
                    args[i].allocs_per_thread = TLSF_ARENA_ALLOCS_PER_THREAD;
                    args[i].alloc_size = sizes[s];
                    args[i].samples = (i == 0) ? samples : NULL;
                    args[i].sample_cap = TLSF_ARENA_SAMPLES;
                    args[i].sample_count = 0;
                }
                t0 = get_time_sec();
                for (i = 0; i < nthreads; i++) {
                    pthread_create(&threads[i], NULL, tlsf_arena_thread_func, &args[i]);
                }
                for (i = 0; i < nthreads; i++) {
                    pthread_join(threads[i], NULL);
                }
                elapsed = get_time_sec() - t0;
                mops = ((double)nthreads * (double)TLSF_ARENA_ALLOCS_PER_THREAD) / elapsed / MILLION_OPS;
                printf("  %-9s %-12s threads=%d  %8.2f Mops/s",
                       labels[s], mode ? "multi-arena" : "regular ", nthreads, mops);
                if (samples != NULL && args[0].sample_count > 0) {
                    int nsamp = args[0].sample_count;
                    qsort(samples, (size_t)nsamp, sizeof(double), cmp_double);
                    printf("  p50=%.1f ns p99=%.1f ns",
                           percentiles_ns(samples, nsamp, 0.50),
                           percentiles_ns(samples, nsamp, 0.99));
                }
                printf("\n");
                free(samples);
                mp_destroy(pool);
            }
        }
    }
}
```

Expected output (the format the implementer must see; numbers are placeholders):

```
=== TLSF Arena Benchmark (multi-arena vs regular) ===
  1KiB      regular      threads=1      12.34 Mops/s  p50=112.0 ns p99=480.0 ns
  1KiB      multi-arena  threads=1      18.76 Mops/s  p50= 88.0 ns p99=260.0 ns
  1KiB      regular      threads=2      24.10 Mops/s
  1KiB      multi-arena  threads=2      30.55 Mops/s
  (8KiB, 64KiB, 192KiB rows for every thread count omitted here for brevity)
  1MiB(ref) regular      threads=1       3.10 Mops/s  p50=310.0 ns p99=900.0 ns
  1MiB(ref) multi-arena  threads=1       3.05 Mops/s  p50=315.0 ns p99=910.0 ns
```

Each row prints Mops/s; only `threads=1` rows also print `p50`/`p99`.

- [x] **Step 4: Build + smoke-run**

Run: `make clean && make bench 2>&1 | tail -40`
Expected: compiles; prints the new section; all cases run to completion. If ANY case crashes, that is the D5 red evidence — record it, then continue (fix comes in Chunk 1). If the pre-fix build crashes on multi-arena cases, reduce `allocs_per_thread` temporarily to demonstrate and record in the plan; keep the final committed value at 50000.

- [x] **Step 5: Capture baseline into the table above, then commit**

```bash
make bench 2>&1 | tee /tmp/tlsf_arena_baseline.txt
git add benchmarks/bench_main.c
git commit -m "bench(arena): 📊 add TLSF arena multi-arena vs regular scenario"
```

---

## Chunk 1: Descriptor core, invariants, flag routing

### Task 1.1: Constants + forward declaration + thread cache field swap

**Files:**
- Modify: `include/cmem.h` (constants ~1020-1027, `tlsf_cache_entry_t` ~1030, `thread_cache_t` ~1036-1053, `mp_block_header_t` ~1055-1071)

- [ ] **Step 1: Add the block flag + forward declaration**

Before `mp_block_header_t` (after the TLSF cache constants):

```c
/** Block belongs to a per-thread TLSF arena; subpool holds tlsf_arena_t*. */
#define MP_BLOCK_FLAG_TLSF_ARENA 0x0001u
```

Before `thread_cache_t`:

```c
struct tlsf_arena; /* internal: per-thread TLSF arena descriptor */
```

- [ ] **Step 2: Replace the four raw arena fields with a descriptor pointer**

In `thread_cache_t`, replace:

```c
    void  *tlsf_arena_raw_mem; /**< Raw memory from sys_mem_alloc (for munmap) */
    void  *tlsf_arena_free;    /**< Head of address-ordered free list           */
    size_t tlsf_arena_used;    /**< Bytes currently allocated from arena         */
    size_t tlsf_arena_total;   /**< Total arena capacity (raw_size - block_hdr)  */
```

with:

```c
    struct tlsf_arena *tlsf_arena; /**< This thread's TLSF arena (multi-arena child pools) */
```

- [ ] **Step 3: Build check**

Run: `make clean && make lib`
Expected: compiles with errors ONLY in `src/cmem_slab.c` / `src/cmem_event.c` referencing the removed fields (expected; fixed in later tasks). Do not commit yet.

### Task 1.2: Descriptor + registry in `src/cmem_internal.h`

**Files:**
- Modify: `src/cmem_internal.h` (atomic-order macros ~37-39/59-61/77-79; after `tlsf_block_t` ~265; `struct memory_pool` ~395-432; declaration block ~588-593)

- [ ] **Step 1: Define `CMEM_ORDER_ACQ_REL` in all three mapping blocks**

`src/cmem_internal.h` currently defines only `CMEM_ORDER_RELAXED`,
`CMEM_ORDER_ACQUIRE`, `CMEM_ORDER_RELEASE` (C++ `std::atomic` block ~37-39;
`_WIN32` `Interlocked` block ~59-61; C11 `<stdatomic.h>` block ~77-79). Add
`CMEM_ORDER_ACQ_REL` to each, immediately after `CMEM_ORDER_RELEASE`:

```c
/* __cplusplus (std::atomic) block, after CMEM_ORDER_RELEASE (~39): */
#define CMEM_ORDER_ACQ_REL std::memory_order_acq_rel
/* _WIN32 (Interlocked) block, after CMEM_ORDER_RELEASE (~61); orders are no-ops: */
#define CMEM_ORDER_ACQ_REL 0
/* C11 <stdatomic.h> block, after CMEM_ORDER_RELEASE (~79): */
#define CMEM_ORDER_ACQ_REL memory_order_acq_rel
```

Must land before Task 1.3, which uses `CMEM_ORDER_ACQ_REL` in
`tlsf_arena_free_block`. Without it, the calls fail to compile.

- [ ] **Step 2: Define the descriptor after `tlsf_block_t`**

```c
/**
 * Per-thread TLSF arena descriptor (multi-arena child pools only).
 *
 * Lifetime is POOL-OWNED: descriptors live in `memory_pool.arena_registry`
 * and are released only by tlsf_arena_registry_release() (from mp_destroy).
 * `finished` means: all blocks returned, raw region already released.
 */
typedef struct tlsf_arena {
    uint8_t          *raw;             /**< Region start                          */
    size_t            size;            /**< Region size (TLSF_ARENA_DEFAULT_SIZE) */
    tlsf_block_t     *free_list;       /**< Doubly-linked, address-sorted         */
    tlsf_block_t     *sentinel;        /**< raw + size - sizeof(tlsf_block_t)     */
    _Atomic size_t    live_bytes;      /**< Sum of live block spans (incl header) */
    _Atomic(tlsf_block_t *) remote_free;   /**< MPSC mailbox head (next_free link) */
    memory_pool_t    *owner_pool;      /**< The child pool this arena serves      */
    unsigned long     owner_thread;    /**< Diagnostic only (pthread_self)        */
    _Atomic bool      retired;         /**< Owner detached; mailbox-only          */
    _Atomic bool      cleanup_claimed; /**< Exactly one cleaner                   */
    _Atomic bool      finished;        /**< Raw released; only descriptor remains */
    struct tlsf_arena *registry_next;  /**< Pool registry chain                   */
} tlsf_arena_t;
```

Requires `<stdatomic.h>` (verify it is included; if not, add at top of `cmem_internal.h` before first use).

- [ ] **Step 3: Add registry fields to `struct memory_pool`** (near `is_multi_arena_child` ~398):

```c
    pthread_mutex_t    arena_registry_lock; /**< Guards arena_registry          */
    tlsf_arena_t      *arena_registry;      /**< Per-thread arenas (pool-owned) */
```

- [ ] **Step 4: Replace the arena declaration block (~588-593) with:**

```c
/* Per-thread TLSF arena (multi-arena child pools) — src/cmem_slab.c */
extern bool tlsf_arena_init(memory_pool_t *pool);
extern tlsf_block_t *tlsf_arena_alloc(tlsf_arena_t *arena, size_t total_needed);
extern void tlsf_arena_free_block(tlsf_arena_t *arena, tlsf_block_t *block);
extern void tlsf_arena_reap(tlsf_arena_t *arena);
extern void tlsf_arena_retire(memory_pool_t *pool);
extern bool tlsf_arena_validate(tlsf_arena_t *arena);
extern bool tlsf_arena_validate_current(void); /* tests: validates tls_cache.tlsf_arena */
extern void tlsf_arena_registry_release(memory_pool_t *pool);
```

- [ ] **Step 5: Registry init at every pool construction site**

Any new pool field (the `arena_registry_lock` mutex and the `arena_registry`
list head) must be initialized/zeroed at **both** confirmed construction sites:

- `src/cmem.c:276` — `calloc(1, sizeof(memory_pool_t))` in `mp_create_child`
  (calloc zeroes `arena_registry`; still `pthread_mutex_init(&pool->arena_registry_lock, NULL);`
  right after the existing `pthread_mutex_init(&pool->lock, NULL)` ~291).
- `src/cmem_event.c:888` — `memset(pool, 0, sizeof(memory_pool_t))` in
  `mp_create_from_buffer` (zeroes the list head; add
  `pthread_mutex_init(&pool->arena_registry_lock, NULL);` after it, or the
  shared-attr variant mirroring the shared `pool->lock` at ~777 for shared pools).

Then run: `grep -n "sizeof(memory_pool_t)" src/*.c` — every construction site
must have a matching registry init; add init at any additional site found.

**Note:** the multi-arena descriptor registry is only ever used by multi-arena
child pools, but initializing it unconditionally at both sites is harmless and
keeps `grep sizeof(memory_pool_t)` clean.

### Task 1.3: Core rewrite in `src/cmem_slab.c`

**Files:**
- Modify: `src/cmem_slab.c` lines 77-257 (all `tlsf_arena_*` functions), `tls_cache_flush_pool` ~259-307

- [ ] **Step 1: Replace the whole arena block with the hardened implementation**

```c
/* ------------------------------------------------------------------ */
/* Per-thread TLSF arena (multi-arena child pools, owner-thread path) */
/*                                                                     */
/* Invariants (shared-TLSF compatible):                                */
/*  - size_and_flags & BLOCK_SIZE_MASK = distance from this block's    */
/*    start to the next physical block's start (own header included).  */
/*  - The final sizeof(tlsf_block_t) bytes hold a sentinel with        */
/*    size_and_flags == 0 (never FREE) and prev_physical maintained.   */
/*  - FREE  <=> block is on arena->free_list (mailboxed blocks keep    */
/*    FREE clear so they can neither be scanned nor coalesced).        */
/*  - Free list is doubly-linked (next_free/prev_free) and sorted.     */
/*  - live_bytes counts the spans of live blocks only.                 */
/* ------------------------------------------------------------------ */

static size_t arena_span(const tlsf_block_t *block)
{
    return block->size_and_flags & BLOCK_SIZE_MASK;
}

static void arena_unlink_free(tlsf_arena_t *arena, tlsf_block_t *block)
{
    if (block->prev_free != NULL) {
        block->prev_free->next_free = block->next_free;
    } else {
        arena->free_list = block->next_free;
    }
    if (block->next_free != NULL) {
        block->next_free->prev_free = block->prev_free;
    }
    block->next_free = NULL;
    block->prev_free = NULL;
}

/** Insert a FREE-stamped block (address order) and coalesce both directions. */
static void arena_insert_free(tlsf_arena_t *arena, tlsf_block_t *block)
{
    tlsf_block_t *prev = NULL;
    tlsf_block_t *curr = arena->free_list;

    while (curr != NULL && (uintptr_t)curr < (uintptr_t)block) {
        prev = curr;
        curr = curr->next_free;
    }

    /* Forward merge: curr starts exactly where block ends. */
    if (curr != NULL && (uint8_t *)block + arena_span(block) == (uint8_t *)curr) {
        tlsf_block_t *after = curr->next_free;
        size_t merged = arena_span(block) + arena_span(curr);
        arena_unlink_free(arena, curr);
        block->size_and_flags = merged | BLOCK_STATE_FREE;
        curr = after;
    }

    /* Backward merge: prev ends exactly where block begins. */
    if (prev != NULL && (uint8_t *)prev + arena_span(prev) == (uint8_t *)block) {
        tlsf_block_t *before = prev->prev_free;
        size_t merged = arena_span(prev) + arena_span(block);
        arena_unlink_free(arena, prev);
        prev->size_and_flags = merged | BLOCK_STATE_FREE;
        block = prev;
        prev = before;
    }

    block->next_free = curr;
    block->prev_free = prev;
    if (prev != NULL) {
        prev->next_free = block;
    } else {
        arena->free_list = block;
    }
    if (curr != NULL) {
        curr->prev_free = block;
    }

    /* Physical metadata for the successor of the merged block. */
    tlsf_block_t *after = (tlsf_block_t *)((uint8_t *)block + arena_span(block));
    if (after != arena->sentinel) {
        after->size_and_flags |= BLOCK_STATE_PREV_FREE;
    }
    after->prev_physical = block;
}

bool tlsf_arena_init(memory_pool_t *pool)
{
    void *raw = sys_mem_alloc(pool, TLSF_ARENA_DEFAULT_SIZE, 8);
    if (raw == NULL) {
        return false;
    }
    tlsf_arena_t *arena = (tlsf_arena_t *)calloc(1, sizeof(tlsf_arena_t));
    if (arena == NULL) {
        sys_mem_free(pool, raw, TLSF_ARENA_DEFAULT_SIZE);
        return false;
    }

    arena->raw = (uint8_t *)raw;
    arena->size = TLSF_ARENA_DEFAULT_SIZE;
    arena->sentinel = (tlsf_block_t *)(arena->raw + arena->size - sizeof(tlsf_block_t));
    arena->owner_pool = pool;
    arena->owner_thread = (unsigned long)pthread_self();

    tlsf_block_t *first = (tlsf_block_t *)arena->raw;
    first->size_and_flags = (arena->size - sizeof(tlsf_block_t)) | BLOCK_STATE_FREE;
    first->prev_physical = NULL;
    first->next_free = NULL;
    first->prev_free = NULL;
    arena->free_list = first;

    arena->sentinel->size_and_flags = 0;
    arena->sentinel->prev_physical = first;
    arena->sentinel->next_free = NULL;
    arena->sentinel->prev_free = NULL;

    pthread_mutex_lock(&pool->arena_registry_lock);
    arena->registry_next = pool->arena_registry;
    pool->arena_registry = arena;
    pthread_mutex_unlock(&pool->arena_registry_lock);

    CMEM_ATOMIC_FETCH_ADD(&pool->total_pool_size, arena->size, CMEM_ORDER_RELAXED);
    tls_cache.tlsf_arena = arena;
    return true;
}

void tlsf_arena_reap(tlsf_arena_t *arena)
{
    tlsf_block_t *block = CMEM_ATOMIC_EXCHANGE(&arena->remote_free, NULL, CMEM_ORDER_ACQUIRE);
    while (block != NULL) {
        tlsf_block_t *next = block->next_free; /* mailbox link */
        block->size_and_flags = arena_span(block) | BLOCK_STATE_FREE;
        /* prev_physical is intentionally preserved: it still points at the
         * block's true physical predecessor, which the validator walks. */
        arena_insert_free(arena, block);
        block = next;
    }
}

tlsf_block_t *tlsf_arena_alloc(tlsf_arena_t *arena, size_t total_needed)
{
    if (CMEM_ATOMIC_LOAD(&arena->remote_free, CMEM_ORDER_ACQUIRE) != NULL) {
        tlsf_arena_reap(arena);
    }

    tlsf_block_t *block = arena->free_list;
    while (block != NULL && arena_span(block) < total_needed) {
        block = block->next_free;
    }
    if (block == NULL) {
        return NULL;
    }

    size_t block_size = arena_span(block);
    size_t remaining = block_size - total_needed;
    tlsf_block_t *after = (tlsf_block_t *)((uint8_t *)block + block_size);

    arena_unlink_free(arena, block);

    if (remaining >= TLSF_MIN_BLOCK_SIZE + sizeof(tlsf_block_t)) {
        tlsf_block_t *remainder = (tlsf_block_t *)((uint8_t *)block + total_needed);
        block->size_and_flags = total_needed;                /* live: FREE cleared  */
        remainder->size_and_flags = remaining | BLOCK_STATE_FREE;
        remainder->prev_physical = block;
        arena_insert_free(arena, remainder);
    } else {
        block->size_and_flags = block_size;                  /* live: FREE cleared  */
        after->prev_physical = block;
        if (after != arena->sentinel) {
            after->size_and_flags &= ~BLOCK_STATE_PREV_FREE;
        }
    }

    CMEM_ATOMIC_FETCH_ADD(&arena->live_bytes, arena_span(block), CMEM_ORDER_RELAXED);
#ifndef NDEBUG
    assert(tlsf_arena_validate(arena));
#endif
    return block;
}

void tlsf_arena_free_block(tlsf_arena_t *arena, tlsf_block_t *block)
{
    size_t span = arena_span(block);
    bool   owner = (tls_cache.tlsf_arena == arena) &&
                   !CMEM_ATOMIC_LOAD(&arena->retired, CMEM_ORDER_ACQUIRE);

    if (owner) {
        block->size_and_flags = span | BLOCK_STATE_FREE;
        /* prev_physical is intentionally preserved (not NULLed): the block's
         * true physical predecessor is needed by the validator's chain walk. */
        arena_insert_free(arena, block);
    } else {
        /* Deferred free: link BEFORE publishing (CAS retry loop), so a
         * concurrent consumer that exchanges the mailbox can never observe
         * `block` with a stale `next_free`. Still done BEFORE the counter
         * decrement so a decrement-to-zero implies the push is visible. */
        tlsf_block_t *expected =
            CMEM_ATOMIC_LOAD(&arena->remote_free, CMEM_ORDER_ACQUIRE);
        for (;;) {
            block->next_free = expected;
            if (CMEM_ATOMIC_COMPARE_EXCHANGE(&arena->remote_free, &expected, block,
                                             CMEM_ORDER_ACQ_REL, CMEM_ORDER_ACQUIRE)) {
                break;
            }
        }
    }

    size_t prev_live = CMEM_ATOMIC_FETCH_SUB(&arena->live_bytes, span, CMEM_ORDER_ACQ_REL);
    if (prev_live == span && CMEM_ATOMIC_LOAD(&arena->retired, CMEM_ORDER_ACQUIRE)) {
        bool expected = false;
        if (CMEM_ATOMIC_COMPARE_EXCHANGE(
                &arena->cleanup_claimed, &expected, true, CMEM_ORDER_ACQ_REL, CMEM_ORDER_RELAXED)) {
            tlsf_arena_cleanup(arena); /* defined in Task 2.1 */
        }
    }
#ifndef NDEBUG
    /* Cleanup nulls arena->raw and marks `finished`; only validate while the
     * region is still live, otherwise the post-cleanup walk dereferences NULL. */
    if (!CMEM_ATOMIC_LOAD(&arena->finished, CMEM_ORDER_ACQUIRE)) {
        assert(tlsf_arena_validate(arena));
    }
#endif
}
```

Notes:
- `CMEM_ATOMIC_EXCHANGE` and `CMEM_ATOMIC_COMPARE_EXCHANGE` **already exist** in `src/cmem_internal.h` (C++ `std::atomic` path ~33-35; `_WIN32` path ~53-57; C11 path ~73-75) — they are NOT in `include/cmem.h`. Use them; do not hand-roll `__atomic_*`.
- `CMEM_ATOMIC_COMPARE_EXCHANGE` takes **FIVE** arguments: `(obj, expected, desired, success_order, failure_order)`. Any 3-argument call is a compile error.
- `CMEM_ORDER_ACQ_REL` is added in Task 1.2 Step 1 (all three mapping blocks) and is required by `tlsf_arena_free_block`.
- `tlsf_arena_cleanup` is declared static in this file; Task 1.3 can add a forward declaration `static void tlsf_arena_cleanup(tlsf_arena_t *arena);` with a temporary body that frees raw + marks finished (Task 2.1 completes retire logic).
- Delete the old `tlsf_arena_alloc(arena_total_needed)` / `tlsf_arena_free` / `tlsf_arena_drain` / `tlsf_arena_destroy` implementations.

- [ ] **Step 2: Validator + test hook (same file)**

```c
bool tlsf_arena_validate(tlsf_arena_t *arena)
{
    if (arena == NULL || arena->raw == NULL) {
        return false;
    }
    /* An empty free list is legal: a final alloc may absorb a sub-floor
     * remainder whole, leaving the arena fully allocated. */

    /* Intentionally NOT checked here:
     *  - live (allocated) blocks: by design they are not required to be on any
     *    free list, so there is no structural list to cross-check them against;
     *  - live_bytes: it is maintained purely by atomic counters, is not
     *    structurally derivable from the block layout without a full sweep, and
     *    races with concurrent mailbox pushes — so it is verified by the
     *    lifecycle/randomized tests, not by this structural validator. */
    /* Physical chain: every block advances inside the region; sentinel last. */
    tlsf_block_t *prev = NULL;
    tlsf_block_t *block = (tlsf_block_t *)arena->raw;
    uint8_t      *end = arena->raw + arena->size - sizeof(tlsf_block_t);
    while ((uint8_t *)block < end) {
        size_t span = arena_span(block);
        if (span < TLSF_MIN_BLOCK_SIZE || (span & TLSF_ALIGN_MASK) != 0) {
            return false;
        }
        if ((uint8_t *)block + span > end) {
            return false;
        }
        if (block->prev_physical != prev) {
            return false;
        }
        /* PREV_FREE consistency: a block whose physical predecessor is FREE
         * must carry BLOCK_STATE_PREV_FREE; a block whose predecessor is not
         * free (allocated, or mailboxed/live-looking) must NOT. */
        bool prev_free = (prev != NULL) && ((prev->size_and_flags & BLOCK_STATE_FREE) != 0);
        bool prev_free_flag = (block->size_and_flags & BLOCK_STATE_PREV_FREE) != 0;
        if (prev_free != prev_free_flag) {
            return false;
        }
        prev = block;
        block = (tlsf_block_t *)((uint8_t *)block + span);
    }
    if ((uint8_t *)block != end || block->size_and_flags != 0) {
        return false;
    }
    if (block->prev_physical != prev) {
        return false;
    }

    /* Free list: sorted, doubly-linked, in-region, no adjacent pair. */
    tlsf_block_t *prev_free = NULL;
    for (block = arena->free_list; block != NULL; block = block->next_free) {
        if ((block->size_and_flags & BLOCK_STATE_FREE) == 0) {
            return false;
        }
        if (block->prev_free != prev_free) {
            return false;
        }
        if (prev_free != NULL) {
            if ((uintptr_t)prev_free >= (uintptr_t)block) {
                return false;
            }
            if ((uint8_t *)prev_free + arena_span(prev_free) == (uint8_t *)block) {
                return false; /* adjacent free blocks must have coalesced */
            }
        }
        if ((uint8_t *)block < arena->raw ||
            (uint8_t *)block + arena_span(block) > end) {
            return false;
        }
        prev_free = block;
    }
    return true;
}

bool tlsf_arena_validate_current(void)
{
    if (tls_cache.tlsf_arena == NULL) {
        return true;
    }
    return tlsf_arena_validate(tls_cache.tlsf_arena);
}
```

Add `#include <assert.h>` at the top of `src/cmem_slab.c` if not already present.

- [ ] **Step 3: Update `tls_cache_flush_pool` arena branch** (~259-307): replace the old arena-range check + `tlsf_arena_free(block, bsize)` with:

```c
        if (tls_cache.tlsf_arena != NULL &&
            (uint8_t *)blk >= tls_cache.tlsf_arena->raw &&
            (uint8_t *)blk < tls_cache.tlsf_arena->raw + tls_cache.tlsf_arena->size) {
            tlsf_arena_free_block(tls_cache.tlsf_arena, (tlsf_block_t *)blk);
        } else {
            tlsf_insert_free_block((tlsf_pool_t *)entry->tpool, (tlsf_block_t *)blk);
        }
```

- [ ] **Step 4: Compile check**

Run: `make clean && make lib 2>&1 | head -40`
Expected: errors only in `src/cmem_event.c` (old field references). Fix in Task 1.4.

### Task 1.4: Alloc integration in `src/cmem_event.c`

**Files:**
- Modify: `src/cmem_event.c` (~2240-2288)

- [ ] **Step 1: Replace the arena init/attempt block**

```c
            if (pool->is_multi_arena_child && tls_cache.tlsf_arena == NULL) {
                (void)tlsf_arena_init(pool);
            }
            if (tls_cache.tlsf_arena != NULL) {
                tlsf_block_t *arena_block = tlsf_arena_alloc(tls_cache.tlsf_arena, total_needed);
                if (arena_block != NULL) {
                    mp_block_header_t *header =
                        (mp_block_header_t *)((uint8_t *)arena_block + sizeof(tlsf_block_t));
                    header->magic = MP_MAGIC_HEAD;
                    header->alloc_type = ALLOC_TYPE_TLSF;
                    header->slab_class = 0;
                    header->flags = MP_BLOCK_FLAG_TLSF_ARENA;
                    header->requested_size = size;
                    header->usable_size =
                        total_needed - sizeof(tlsf_block_t) - sizeof(mp_block_header_t);
                    header->raw_base = arena_block;
                    header->subpool = (void *)tls_cache.tlsf_arena;
                    header->alloc_file = NULL;
                    header->alloc_line = 0;
                    header->alloc_func = NULL;
                    header->backtrace_depth = 0;
                    ptr = (void *)((uint8_t *)header + sizeof(mp_block_header_t));
                    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
                        uint8_t *canary = (uint8_t *)ptr + size;
                        *canary = MP_CANARY_BYTE;
                    }
                    if (pool->flags & MP_FLAG_ZERO_ON_ALLOC) {
                        memset(ptr, 0, size);
                    }
                    CMEM_ATOMIC_FETCH_ADD(&tls_cache.tlsf_arena->owner_pool->tlsf_allocated_bytes,
                                          header->usable_size, CMEM_ORDER_RELAXED);
                }
            }
```

Adapt variable names/returns to the surrounding code exactly (the old block at 2240-2280 is the template). The old `pool->is_multi_arena_child && !tls_cache.tlsf_arena_raw_mem` gate becomes `tls_cache.tlsf_arena == NULL`; drop the `tlsf_arena_free` NULL check.

### Task 1.5: Free routing (D5) + TLSF guards in `src/cmem_event.c` / `src/cmem_tlsf.c`

**Files:**
- Modify: `src/cmem_event.c` (routing ~3099-3107; TLSF branch ~3231-3266)
- Modify: `src/cmem_tlsf.c` (`tlsf_free` ~564-570; `tlsf_try_inplace_expand` ~775-789)

- [ ] **Step 1: Flag-first routing in `mp_free` (~3099)**

```c
    if ((header->flags & MP_BLOCK_FLAG_TLSF_ARENA) != 0) {
        tlsf_arena_t *arena = (tlsf_arena_t *)header->subpool;
        CMEM_ATOMIC_FETCH_SUB(&arena->owner_pool->tlsf_allocated_bytes,
                              header->usable_size, CMEM_ORDER_RELAXED);
        /* route later in the TLSF dispatch; keep `pool` untouched for events */
    } else if (header->subpool != NULL) {
        tlsf_pool_t *tpool = (tlsf_pool_t *)header->subpool;
        if (tpool->owner_pool != NULL) {
            pool = tpool->owner_pool;
        } else {
            pool = (memory_pool_t *)header->subpool;
        }
    } else if (pool == NULL) {
        return;
    }
```

- [ ] **Step 2: Arena dispatch in the TLSF branch (~3231)**

```c
        if ((header->flags & MP_BLOCK_FLAG_TLSF_ARENA) != 0) {
            tlsf_arena_free_block((tlsf_arena_t *)header->subpool, block);
        } else {
            /* TLSF cache push: stash non-arena blocks to avoid locking. */
            int fl = tlsf_block_size_to_fl(block_size);
            int cache_idx = fl - TLSF_CACHE_MIN_FL;
            if (cache_idx >= 0 && cache_idx < TLSF_CACHE_SIZES &&
                tls_cache.tlsf_counts[(unsigned)cache_idx] < TLSF_CACHE_MAX_SLOTS) {
                tlsf_cache_entry_t *entry =
                    &tls_cache.tlsf_entries[(unsigned)cache_idx * TLSF_CACHE_MAX_SLOTS +
                                            tls_cache.tlsf_counts[(unsigned)cache_idx]];
                entry->block = block;
                entry->tpool = header->subpool;
                entry->next = tls_cache.tlsf_slots[(unsigned)cache_idx];
                tls_cache.tlsf_slots[(unsigned)cache_idx] = entry;
                tls_cache.tlsf_counts[(unsigned)cache_idx]++;
            } else {
                tlsf_free(pool, header);
            }
        }
```

Remove the old `is_arena_block` range test entirely (flag is authoritative).

- [ ] **Step 3: Defensive guards**

`cmem_tlsf.c` `tlsf_free` (~567), after casting tpool:

```c
    assert((header->flags & MP_BLOCK_FLAG_TLSF_ARENA) == 0); /* arena blocks never reach here */
```

`cmem_tlsf.c` `tlsf_try_inplace_expand` (~782):

```c
    if (tpool == NULL || (header->flags & MP_BLOCK_FLAG_TLSF_ARENA) != 0) {
        return false;
    }
```

### Task 1.6: Realloc routing + D11 + stats

**Files:**
- Modify: `src/cmem_event.c` (~3311-3421)

- [ ] **Step 1: Confirm D11 by reading `mp_realloc` 3286-3410 once more; the old arena block ends with `pool_unlock(pool)` at ~3401 with NO matching lock — it must be deleted as part of this rewrite.**

- [ ] **Step 2: Replace the arena in-place expand block** with (inside the `ALLOC_TYPE_TLSF` handling, after the cache flush loop):

```c
        if ((header->flags & MP_BLOCK_FLAG_TLSF_ARENA) != 0) {
            tlsf_arena_t *arena = (tlsf_arena_t *)header->subpool;
            bool is_owner = (tls_cache.tlsf_arena == arena) &&
                            !CMEM_ATOMIC_LOAD(&arena->retired, CMEM_ORDER_ACQUIRE);
            if (is_owner) {
                size_t old_usable = header->usable_size;
                size_t canary = (pool->flags & MP_FLAG_DEBUG_CANARY) ? 1u : 0u;
                if (CMEM_ATOMIC_LOAD(&arena->remote_free, CMEM_ORDER_ACQUIRE) != NULL) {
                    tlsf_arena_reap(arena);
                }
                tlsf_block_t *ablock = (tlsf_block_t *)header->raw_base;
                size_t acur = arena_span(ablock);
                size_t need = sizeof(tlsf_block_t) + sizeof(mp_block_header_t) + new_size + canary;
                need = (need + TLSF_ALIGN_MASK) & ~(size_t)TLSF_ALIGN_MASK;
                if (need < TLSF_MIN_BLOCK_SIZE) {
                    need = TLSF_MIN_BLOCK_SIZE;
                }
                if (need > acur) {
                    tlsf_block_t *anext = (tlsf_block_t *)((uint8_t *)ablock + acur);
                    bool growable = (anext != arena->sentinel) &&
                                    ((anext->size_and_flags & BLOCK_STATE_FREE) != 0) &&
                                    (acur + arena_span(anext) >= need);
                    if (growable) {
                        size_t aspan = arena_span(anext);
                        size_t total = acur + aspan;
                        tlsf_block_t *after = (tlsf_block_t *)((uint8_t *)anext + aspan);
                        arena_unlink_free(arena, anext);
                        after->prev_physical = ablock;
                        if (after != arena->sentinel) {
                            after->size_and_flags &= ~BLOCK_STATE_PREV_FREE;
                        }
                        size_t new_span;
                        if (total - need >= TLSF_MIN_BLOCK_SIZE + sizeof(tlsf_block_t)) {
                            tlsf_block_t *remainder = (tlsf_block_t *)((uint8_t *)ablock + need);
                            ablock->size_and_flags = need;
                            remainder->size_and_flags = (total - need) | BLOCK_STATE_FREE;
                            remainder->prev_physical = ablock;
                            arena_insert_free(arena, remainder);
                            new_span = need;
                        } else {
                            ablock->size_and_flags = total;
                            new_span = total; /* no split: the live span is the whole merged block */
                        }
                        /* live_bytes must track the REAL new span (`need` only when split);
                         * the no-split case owns `total`, else the last-freer detection
                         * (prev_live == span) never reaches zero and retire hangs. */
                        CMEM_ATOMIC_FETCH_ADD(&arena->live_bytes, new_span - acur, CMEM_ORDER_RELAXED);
                        acur = new_span;
                    }
                }
                if (need <= acur) {
                    header->requested_size = new_size;
                    header->usable_size =
                        acur - sizeof(tlsf_block_t) - sizeof(mp_block_header_t) - canary;
                    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
                        uint8_t *canary_ptr = (uint8_t *)ptr + new_size;
                        *canary_ptr = MP_CANARY_BYTE;
                    }
                    if (header->usable_size >= old_usable) {
                        CMEM_ATOMIC_FETCH_ADD(&arena->owner_pool->tlsf_allocated_bytes,
                                              header->usable_size - old_usable, CMEM_ORDER_RELAXED);
                    } else {
                        CMEM_ATOMIC_FETCH_SUB(&arena->owner_pool->tlsf_allocated_bytes,
                                              old_usable - header->usable_size, CMEM_ORDER_RELAXED);
                    }
                    trigger_event(pool, MP_EVENT_REALLOC, ptr, new_size);
                    return ptr;
                }
                /* cannot grow in place: fall through to alloc-copy-free below */
            }
            /* non-owner or non-growable: fall through to alloc-copy-free below */
        }
```

Then let the existing tail (pool_lock + `tlsf_try_inplace_expand` [now returns false for arena] + alloc-copy-free) handle the fallback. Ensure the old ad-hoc split/merge/linear-search code and the stray `pool_unlock` are deleted.

- [ ] **Step 3: Compile + existing-suite smoke**

Run: `make clean && make test_all 2>&1 | tail -20`
Expected: existing tests pass (arena still untested here; validator asserts must not fire).

- [ ] **Step 4: Commit**

```bash
git add include/cmem.h src/cmem_internal.h src/cmem_slab.c src/cmem_event.c src/cmem_tlsf.c src/cmem.c
git commit -m "fix(arena): 🐛 rebuild per-thread TLSF arena on shared-TLSF invariants, flag-based routing"
```

### Task 1.7: Targeted tests 1-5, 8 in `tests/test_advanced.c` (RED first where possible)

**Files:**
- Modify: `tests/test_advanced.c` (append after `test_multi_arena` ~1019; register in `main()` before the final printf ~1071)

- [ ] **Step 1: Single-thread basic test (master handle, D5)**

```c
#ifndef NDEBUG
extern bool tlsf_arena_validate_current(void);
#else
static bool tlsf_arena_validate_current(void) { return true; }
#endif

/* Fresh multi-arena pool; sizes are above the TLSF cache range (>8 KiB) so the
 * TLSF arena path (not the cache/slab path) is exercised. */
static void test_tlsf_arena_basic(void)
{
    memory_pool_t *pool = mp_create(16 * 1024 * 1024, MP_FLAG_MULTI_ARENA | MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    void *block = mp_alloc(pool, 32768);
    assert(block != NULL);
    /* The child arena descriptor is created lazily on first alloc, so the
     * count only becomes non-zero now. */
    assert(mp_get_arena_count(pool) > 0);
    memset(block, 0xAB, 32768);
    assert(tlsf_arena_validate_current());
    mp_free(pool, block);                   /* master handle: must NOT crash (D5) */
    assert(tlsf_arena_validate_current());
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("TLSF arena basic alloc/free with master handle");
}
```

- [ ] **Step 2: `test_tlsf_arena_realloc` — grow in place, shrink, payload preserved**

```c
static void test_tlsf_arena_realloc(void)
{
    memory_pool_t *pool = mp_create(16 * 1024 * 1024, MP_FLAG_MULTI_ARENA | MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    size_t size = 32768;
    uint8_t *p = (uint8_t *)mp_alloc(pool, size);
    assert(p != NULL);
    for (size_t i = 0; i < size; i++) {
        p[i] = (uint8_t)(i & 0xFF);
    }
    assert(tlsf_arena_validate_current());

    size_t bigger = 49152;                  /* grow (in place if a free successor) */
    uint8_t *q = (uint8_t *)mp_realloc(pool, p, bigger);
    assert(q != NULL);
    for (size_t i = 0; i < size; i++) {
        assert(q[i] == (uint8_t)(i & 0xFF));
    }
    assert(tlsf_arena_validate_current());

    size_t smaller = 20000;                 /* shrink */
    uint8_t *r = (uint8_t *)mp_realloc(pool, q, smaller);
    assert(r != NULL);
    for (size_t i = 0; i < smaller; i++) {
        assert(r[i] == (uint8_t)(i & 0xFF));
    }
    assert(tlsf_arena_validate_current());

    mp_free(pool, r);
    assert(tlsf_arena_validate_current());
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("TLSF arena realloc grow/shrink preserves payload");
}
```

- [ ] **Step 3: `test_tlsf_arena_coalesce` — forward/backward/both merges + full-capacity**

```c
static void test_tlsf_arena_coalesce(void)
{
    memory_pool_t *pool = mp_create(16 * 1024 * 1024, MP_FLAG_MULTI_ARENA | MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    enum { N = 4 };
    const size_t blk = 32768;
    void *v[N];
    for (int i = 0; i < N; i++) {
        v[i] = mp_alloc(pool, blk);
        assert(v[i] != NULL);
        memset(v[i], (unsigned char)i, blk);
    }
    assert(tlsf_arena_validate_current());

    /* Free in an order that forces forward (1->2), backward (0), then the
     * remaining pair merges both ways. */
    const int order[N] = { 1, 0, 3, 2 };
    for (int i = 0; i < N; i++) {
        mp_free(pool, v[order[i]]);
        assert(tlsf_arena_validate_current());
    }

    /* Full-capacity proof: 192 KiB only succeeds if all 4 blocks plus the
     * arena's always-free tail coalesced back into one block. */
    void *big = mp_alloc(pool, 196608);
    assert(big != NULL);
    mp_free(pool, big);
    assert(tlsf_arena_validate_current());
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("TLSF arena coalesce (fwd/back/both) + full-capacity merge");
}
```

- [ ] **Step 4: `test_tlsf_arena_master_free` — free via an alternate child handle**

```c
static void test_tlsf_arena_master_free(void)
{
    memory_pool_t *pool = mp_create(16 * 1024 * 1024, MP_FLAG_MULTI_ARENA | MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    void *block = mp_alloc(pool, 32768);
    assert(block != NULL);
    memset(block, 0x5A, 32768);
    assert(tlsf_arena_validate_current());

    /* Free through a different child handle of the same pool. */
    memory_pool_t *child = mp_get_thread_arena(pool);
    assert(child != NULL);
    mp_free(child, block);
    assert(tlsf_arena_validate_current());

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("TLSF arena free via alternate child handle");
}
```

- [ ] **Step 5: `test_tlsf_arena_cross_thread_free` — mailbox + reap (arena-resident sizes)**

```c
typedef struct {
    memory_pool_t *pool;
    void          *ptrs[4];
    int            count;
} arena_xfree_arg_t;

static void *arena_xfree_worker(void *arg)
{
    arena_xfree_arg_t *xa = (arena_xfree_arg_t *)arg;
    for (int i = 0; i < xa->count; i++) {
        mp_free(xa->pool, xa->ptrs[i]);
    }
    return NULL;
}

static void test_tlsf_arena_cross_thread_free(void)
{
    memory_pool_t *pool = mp_create(16 * 1024 * 1024, MP_FLAG_MULTI_ARENA | MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    arena_xfree_arg_t xa;
    xa.pool = pool;
    xa.count = 4;
    /* 4 x 32 KiB = 128 KiB: all arena-resident in the 256 KiB arena. */
    for (int i = 0; i < xa.count; i++) {
        xa.ptrs[i] = mp_alloc(pool, 32768);
        assert(xa.ptrs[i] != NULL);
        memset(xa.ptrs[i], 0xC3, 32768);
    }
    assert(tlsf_arena_validate_current());

    pthread_t worker;
    assert(pthread_create(&worker, NULL, arena_xfree_worker, &xa) == 0);
    assert(pthread_join(worker, NULL) == 0);

    /* Owner still alive: the next owner-side alloc reaps the mailbox. */
    void *again = mp_alloc(pool, 32768);
    assert(again != NULL);
    mp_free(pool, again);
    assert(tlsf_arena_validate_current());
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("TLSF arena cross-thread free via mailbox + reap");
}
```

- [ ] **Step 6: `test_tlsf_arena_exhaustion` — arena fills, fallback to shared pool still succeeds**

```c
static void test_tlsf_arena_exhaustion(void)
{
    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_MULTI_ARENA | MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    enum { MAXBLK = 64 };                   /* 64 x 32 KiB = 2 MiB >> 256 KiB arena */
    void *blocks[MAXBLK];
    int n = 0;

    /* ~7 blocks fit the 256 KiB arena; every further block must transparently
     * fall back to the shared pool and still succeed. */
    for (; n < MAXBLK; n++) {
        blocks[n] = mp_alloc(pool, 32768);
        assert(blocks[n] != NULL);
        memset(blocks[n], 0x7E, 32768);
    }
    assert(tlsf_arena_validate_current());

    for (int i = 0; i < n; i++) {
        mp_free(pool, blocks[i]);
    }
    assert(tlsf_arena_validate_current());
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("TLSF arena exhaustion falls back to shared pool cleanly");
}
```
- [ ] **Step 7: Register all in `main()`** (after `test_multi_arena();`):

```c
    test_tlsf_arena_basic();
    test_tlsf_arena_realloc();
    test_tlsf_arena_coalesce();
    test_tlsf_arena_master_free();
    test_tlsf_arena_cross_thread_free();
    test_tlsf_arena_exhaustion();
```

- [ ] **Step 8: Run RED on pre-fix code if you skipped ahead (optional but valuable evidence):** `git stash` the src changes, build, run — record crash/failure; then `git stash pop`. Otherwise run green:

Run: `make clean && make test_advanced 2>&1 | tail -30`
Expected: all new tests PASS, no assert/validator failures.

- [ ] **Step 9: Commit**

```bash
git add tests/test_advanced.c
git commit -m "test(arena): ✅ cover TLSF arena basic/realloc/coalesce/master-free/cross-thread/exhaustion"
```

---

## Chunk 2: Lifecycle — retire, cleanup, registry release

### Task 2.1: `tlsf_arena_cleanup` + `tlsf_arena_retire` + cleanup hook

**Files:**
- Modify: `src/cmem_slab.c` (replace the Task 1.3 stub; add retire)

- [ ] **Step 1: Finalize cleanup** (called by exactly one claimant):

```c
/**
 * Sole-mutator cleanup: every block is free by now (live_bytes == 0 and the
 * owner detached, or retirement observed zero). Reaps leftovers, then
 * releases the region to the OS.
 *
 * NOTE (spec deviation, deliberate): the region is NOT inserted into the
 * shared TLSF's buckets — memory cannot both join the shared free list and
 * be returned to the OS; the pool does not track foreign regions. The spec
 * text is corrected in Chunk 5.
 */
static void tlsf_arena_cleanup(tlsf_arena_t *arena)
{
    tlsf_arena_reap(arena);
#ifndef NDEBUG
    assert(arena->free_list != NULL && arena->free_list->next_free == NULL);
    assert(tlsf_arena_validate(arena));
#endif
    memory_pool_t *pool = arena->owner_pool;
    sys_mem_free(pool, arena->raw, arena->size);
    arena->raw = NULL;
    CMEM_ATOMIC_FETCH_SUB(&pool->total_pool_size, arena->size, CMEM_ORDER_RELAXED);
    CMEM_ATOMIC_STORE(&arena->finished, true, CMEM_ORDER_RELEASE);
}

void tlsf_arena_retire(memory_pool_t *pool)
{
    tlsf_arena_t *arena = tls_cache.tlsf_arena;
    if (arena == NULL || arena->owner_pool != pool) {
        return;
    }
    /* Detach first: this thread can no longer take the direct path. */
    tls_cache.tlsf_arena = NULL;
    CMEM_ATOMIC_STORE(&arena->retired, true, CMEM_ORDER_RELEASE);

    if (CMEM_ATOMIC_LOAD(&arena->live_bytes, CMEM_ORDER_ACQUIRE) == 0) {
        bool expected = false;
        if (CMEM_ATOMIC_COMPARE_EXCHANGE(
                &arena->cleanup_claimed, &expected, true, CMEM_ORDER_ACQ_REL, CMEM_ORDER_RELAXED)) {
            tlsf_arena_cleanup(arena);
        }
    }
}
```

- [ ] **Step 2: Wire retire triggers**

- `tls_cache_flush_pool` (~259): after flushing slots and clearing owner_pool, call `tlsf_arena_retire(pool)`.
- `tls_cache_validate_owner` (~324): pool switch path already calls `tls_cache_flush_pool(old)` — covered. Verify it does.
- `tls_cache_dtor` (~310): flush path — covered.

Run: `make clean && make test_all`
Expected: green.

### Task 2.2: Registry release in `mp_destroy` (+ shared variant)

**Files:**
- Modify: `src/cmem_event.c` (`mp_destroy` ~1440-1494; `mp_destroy_shared` ~791)

- [ ] **Step 1: Implement release in `src/cmem_slab.c`:**

```c
void tlsf_arena_registry_release(memory_pool_t *pool)
{
    pthread_mutex_lock(&pool->arena_registry_lock);
    tlsf_arena_t *arena = pool->arena_registry;
    pool->arena_registry = NULL;
    pthread_mutex_unlock(&pool->arena_registry_lock);

    while (arena != NULL) {
        tlsf_arena_t *next = arena->registry_next;
        if (!CMEM_ATOMIC_LOAD(&arena->finished, CMEM_ORDER_ACQUIRE)) {
            /* Pool destroyed with blocks still outstanding: force-release. */
            if (arena->raw != NULL) {
                sys_mem_free(pool, arena->raw, arena->size);
                CMEM_ATOMIC_FETCH_SUB(&pool->total_pool_size, arena->size, CMEM_ORDER_RELAXED);
            }
        }
        free(arena);
        arena = next;
    }
    pthread_mutex_destroy(&pool->arena_registry_lock);
}
```

- [ ] **Step 2: Call it — pin the exact teardown ordering**

`mp_destroy` (`src/cmem_event.c:1440`): call `tlsf_arena_registry_release(pool)`
**last**, after:
1. `tls_cache_flush_pool(pool)` at the top (line ~1446) — retires this thread's
   arena (releases its raw region if `live_bytes == 0`);
2. the child-recursion loop (`mp_destroy(child)`, ~1455-1460) — each child's
   `tls_cache_flush_pool` retires that child's arena;
3. the slab/compressed/per-CPU teardown.

Place the call immediately **before** the final `free(pool)` / pool-struct
teardown, so no thread can hold a `tlsf_arena_t*` when descriptors are freed.

`mp_destroy_shared` (`src/cmem_event.c:791`): same order, pinned as
1. `tls_cache_flush_pool(pool)` (already at line ~796);
2. `tlsf_arena_registry_release(pool)` — **before** destroying `pool->lock`/`pool->rwlock`;
3. `pthread_rwlock_destroy` / `pthread_mutex_destroy` (~801-802);
4. `munmap` of the pool range (~806).
The registry mutex lives inside `pool`, so it MUST be released and destroyed
before the `munmap`; releasing it after the locks are destroyed is also invalid
(`registry_release` itself locks `arena_registry_lock`, and the arena raw regions
were allocated via `sys_mem_alloc`, i.e. outside the shm mapping).

- [ ] **Step 2b: Verify before committing**

Run: `make clean && make test_all 2>&1 | tail -20`
Expected: green (all existing + Task 1.7/2.3 tests pass; no registry/cleanup
asserts fire). Then `make CONFIG=asan test_all 2>&1 | tail -20` → no ASan reports
(the registry release frees raw regions the sanitizer is tracking).

- [ ] **Step 3: Commit**

```bash
git add src/cmem_slab.c src/cmem_event.c
git commit -m "feat(arena): ✨ retire/drain lifecycle for per-thread TLSF arenas"
```

### Task 2.3: Thread-exit + pool-switch tests

**Files:**
- Modify: `tests/test_advanced.c` (append; register in main)

- [ ] **Step 1: `test_tlsf_arena_thread_exit`** — worker thread allocs 4×32 KiB (arena-resident in the 256 KiB arena), stores pointers into a shared array, exits (its arena retires with live blocks). Main thread then frees all four via `mp_free(master, ptr)`; the last free triggers cleanup; assert no crash, validator current clean, and pool accounting stable via `mp_check_leaks`. **Do NOT use 256 KiB blocks:** a 256 KiB block cannot fit the 256 KiB arena (cap ~192 KiB), so it would allocate from the shared pool and never exercise retire-with-live-blocks.

- [ ] **Step 2: `test_tlsf_arena_pool_switch`** — one thread allocates from pool A (multi-arena, gets arena), then allocates from pool B (another multi-arena pool) — its cache switches, arena A retires; free A's block afterwards (via A's master) and run allocations to force reap/cleanup; no crash, both pools leak-clean at destroy.

- [ ] **Step 3: Run + commit**

```bash
make clean && make test_advanced 2>&1 | tail -20
git add tests/test_advanced.c
git commit -m "test(arena): ✅ thread-exit and pool-switch lifecycle coverage"
```

---

## Chunk 3: Randomized differential + multithreaded stress

### Task 3.1: `test_tlsf_arena_randomized` in `tests/test_advanced.c`

**Files:**
- Modify: `tests/test_advanced.c` (append the randomized test; register it in `main()`)

- [ ] **Step 1: Implement a slot-table + interval-model differential test:**

```c
#define RAND_SLOTS 256
/* Model: sorted array of live [start, end) intervals + payload tags.
   Every op: alloc / free(random live) / realloc(random live, new size),
   sizes in [16 KiB, 192 KiB]; 20000 ops; deterministic seed (12345).
   Every 256 ops: tlsf_arena_validate_current() and interval-table invariants.
   Payload verify on EVERY op (spec §4): alloc writes the tag; free and
   realloc check the block's preserved prefix before touching it; realloc
   re-tags the new tail.
   End: free all; assert(mp_check_leaks(pool) == true); alloc a full-capacity block. */
```

Follow `test_main.c`-style helpers for a xorshift PRNG (do not use libc rand). Keep the model independent of allocator internals.

- [ ] **Step 2: Run** `make test_advanced` — expect PASS (validator catches structural drift).
- [ ] **Step 3: Commit** `test(arena): ✅ randomized differential test vs interval model`.

### Task 3.2: Multi-arena TLSF phase in `tests/stress_test.c`

**Files:**
- Modify: `tests/stress_test.c` (add `stress_arena_worker`; wire a bounded phase in `main()`)

**Thread count:** exactly **4** worker threads for this phase (spec §4 allows 4–8 bound threads; 4 keeps the quick `-DSTRESS_DURATION_SEC=15` run bounded).

- [ ] **Step 1: Add `stress_arena_worker`** — random alloc/free/**realloc** with sizes 512 B–256 KiB (sizes above the ~192 KiB arena-resident ceiling intentionally exercise the shared-pool fallback path), a per-thread slot array (e.g. 64 slots). Each iteration runs one op chosen by a xorshift PRNG:
  - **alloc**: pick a size, `mp_alloc`, store it in a free slot with a payload tag;
  - **free**: pick a live slot, **verify its payload prefix**, `mp_free`;
  - **realloc**: pick a live slot, **verify the preserved prefix**, `mp_realloc` to a new size, then fill the new tail.
  ~10% of frees are **delegated to a neighbor thread** via per-thread mail slots (indices 0..3): the freer pushes the pointer into the neighbor's mail slot instead of freeing directly; the neighbor frees it on its next iteration (exercising the mailbox / cross-thread path). Pool: `MP_FLAG_MULTI_ARENA | MP_FLAG_THREAD_SAFE | MP_FLAG_THREAD_LOCAL_CACHE`.
  Workers exit WITH live blocks left in their slots; the main thread frees the leftovers after `pthread_join`.
  **Payload verification is mandatory** on every free/realloc (a corrupted prefix indicates D1-style corruption and must fail the run).

- [ ] **Step 2: Wire a bounded phase** in `main()`: the arena phase runs for `phase_sec = STRESS_DURATION_SEC / 10`, floored at 5 s (`if (phase_sec < 5) phase_sec = 5;`), so the default 24 h run gets a ~2.4 h arena phase and a `-DSTRESS_DURATION_SEC=15` build still runs a meaningful ~5 s phase. There is **no separate env override** and no other duration statement — the quick run is a compile-time redefine (`-DSTRESS_DURATION_SEC=15`), exactly as AGENTS.md prescribes. Launch 4 `stress_arena_worker` threads, join them, then free the leftover slots from the main thread. **Placement:** the arena phase is a distinct phase with its **own dedicated multi-arena pool** and its own `phase_sec` timer — it does not replace, extend, or share a duration budget with the existing 8-thread loop in `main()`.

- [ ] **Step 3: Verify quick build (compile-time define form):**

```bash
gcc -DSTRESS_DURATION_SEC=15 -I include tests/stress_test.c src/*.c -pthread -o /tmp/stress_verify && /tmp/stress_verify
```
Expected: completes, no crash, no payload-mismatch assertions, report lines present.

- [ ] **Step 4: Commit** `test(arena): ✅ multithreaded arena stress phase with delegated frees`.

---

## Chunk 4: Full gates

### Task 4.1: Sanitizer + build matrix

- [ ] **Step 1:** `make clean && make test_all` → PASS.
- [ ] **Step 2:** `make CONFIG=asan test_all` → PASS, no ASan reports.
- [ ] **Step 3:** `make CONFIG=ubsan test_all` → PASS.
- [ ] **Step 4:** `make CONFIG=tsan test_all` → PASS, zero TSan warnings. (If TSan flags the mailbox atomics: verify ordering per spec §3.3 before changing anything; fix code, not the test.)
- [ ] **Step 5:** `make test_cpp` → PASS. (There is **no** `make cpp` target; the C++ test target is `make test_cpp`.)
- [ ] **Step 6:** `make cmake-build && make cmake-test` → PASS. (`cmake-build` = `cmake -B build_cmake -G Ninja -DCMAKE_BUILD_TYPE=Debug` + `cmake --build build_cmake`; `cmake-test` = `ctest --test-dir build_cmake --output-on-failure`. This uses the dedicated `build_cmake/` dir and avoids the raw `cmake ..` collision on `build/`.)
- [ ] **Step 7:** `make format-check` → clean; run `clang-format -i` on changed files if needed, re-verify, include in a `style(arena): 🎨` commit if formatting changed.

- [ ] **Step 8: Commit any fallout fixes** with focused messages.

---

## Chunk 5: Benchmark re-run, tuning decision, docs close-out

### Task 5.1: Re-run benchmark + decide tuning

- [x] **Step 1:** `make clean && make bench 2>&1 | tee /tmp/tlsf_arena_after.txt`. Fill the **after** half of every `before → after` cell in the restructured Baseline table at the top of this plan (multi-arena Mops/s, and p50/p99 for the single-thread rows). Expected output is the same format as Chunk 0 Task 0.1 Step 3, e.g.:

```
=== TLSF Arena Benchmark (multi-arena vs regular) ===
  1KiB      regular      threads=1      12.34 Mops/s  p50=112.0 ns p99=480.0 ns
  1KiB      multi-arena  threads=1      18.76 Mops/s  p50= 88.0 ns p99=260.0 ns
  1KiB      regular      threads=2      24.10 Mops/s
  1KiB      multi-arena  threads=2      30.55 Mops/s
  (8KiB, 64KiB, 192KiB rows for every thread count omitted here for brevity)
  1MiB(ref) regular      threads=1       3.10 Mops/s  p50=310.0 ns p99=900.0 ns
  1MiB(ref) multi-arena  threads=1       3.05 Mops/s  p50=315.0 ns p99=910.0 ns
```

- [x] **Step 2:** Compare multi-arena vs regular per size/threads and the multi-arena `before → after`. Tune ONLY if a clear regression vs pre-fix exists — "clear regression" means: for some arena-eligible size (≤ 192 KiB) at some thread count, the post-fix multi-arena number is worse than the pre-fix multi-arena number beyond run-to-run noise (repeat the run once to confirm). Candidate knobs (in order): first-fit unchanged (default), next-fit cursor, best-fit for large classes. Any tuning gets its own commit + before/after numbers in the message. Document "no tuning needed" otherwise.
- [x] **Step 3 (skipped, nothing to stage):** Commit bench/plan notes: `bench(arena): 📊 post-fix TLSF arena results` (bench code only if changed).

### Task 5.2: Docs close-out

- [x] **Step 1:** Correct the **whole** stale drain story in `docs/superpowers/specs/2026-10-19-tlsf-arena-hardening-design.md`, not just §3.3's headline:
  - §3.3 retire step (spec ~line 156-159): replace "drain into `owner_pool->tlsf_root` via `tlsf_insert_free_block` (thread-safe bucket locks) …" with the implemented behavior (**release the region to the OS via `sys_mem_free`**; memory cannot both join the shared free list and be returned to the OS, and the pool does not track foreign regions).
  - §3.3 correctness-argument bullet (spec ~lines 182-185): rewrite the "the drain is safe because … the shared TLSF can never walk out of the region" bullet — drop the shared-TLSF-walk rationale and state instead that with `live_bytes == 0` and correct coalescing the arena is exactly one free block `[raw, sentinel)`, which is then unmapped wholesale.
  - Descriptor comment (spec ~line 60): fix `memory_pool_t *owner_pool; /* drain target for shared TLSF */` → `/* pool this arena serves (registry owner) */`.
  - `grep -rn "drain\|tlsf_insert_free_block\|fallback rate" docs/superpowers/specs/2026-10-19-tlsf-arena-hardening-design.md` and update every residual reference — including the §3.3 "Documented consequences", any DoD wording that implies draining, and the §5 sentence claiming the benchmark reports the shared-pool fallback rate (spec ~L295; rewrite to "not instrumented", per Step 4 deviation #2).
  - Commit `docs(spec): 📝 correct arena drain semantics in hardening spec`.
- [x] **Step 2:** Reconcile the OLD PLAN `docs/superpowers/plans/2026-08-08-per-thread-tlsf-arena.md`: tick a box only if the corresponding behavior exists on post-fix HEAD (source/test inspection; the spec's count of 37 top-level unchecked boxes matches the file), add a top note "superseded by 2026-10-19-tlsf-arena-hardening.md for the hardening work". Commit `docs(plan): 📝 reconcile per-thread TLSF arena plan status`.
- [x] **Step 2b (DoD #5 — old SPEC banner):** add a "**Superseded by** [2026-10-19-tlsf-arena-hardening-design.md](2026-10-19-tlsf-arena-hardening-design.md)" banner at the top of the OLD SPEC `docs/superpowers/specs/2026-08-08-per-thread-tlsf-arena-design.md`. The banner is already present (added in commit `1d13c96`) — **verify** it and re-add it only if missing; DoD #5 requires both directions (the new spec's `Supersedes:` line plus the old spec's back-pointer). Include it in the same `docs(spec): 📝 …` commit.
- [x] **Step 3 (local-only):** AGENTS.md: add one line under Tests/conventions: "Per-thread TLSF arenas (multi-arena child pools): blocks carry MP_BLOCK_FLAG_TLSF_ARENA; free/realloc must route by flag (never by pool handle); descriptors are pool-owned via arena_registry; cross-thread frees go through the mailbox." Commit `docs: 📝 note TLSF arena invariants in AGENTS.md`.
- [x] **Step 4: Record the documented spec deviations** (in the plan's Self-Review Notes and in the corrected spec):
  1. **OS release instead of TLSF drain** — the region is returned to the OS, not inserted into the shared TLSF (§3.3).
  2. **Shared-pool fallback rate not instrumented** — spec §5 asks the benchmark to report it; no counter exists and adding one is out of scope (Chunk 0 Task 0.1 note).

---

## Self-Review Notes (author)

  - Spec coverage: D1 (Task 1.3 alloc FREE clear), D2 (arena_insert_free), D3 (sentinel, init + validator), D4 (Chunk 2 retire + registry release), D5 (Task 1.5 routing; realloc Task 1.6), D6 (stats add/sub; live_bytes), D7 (realloc via helper, Task 1.6), D11 (Task 1.6 remove stray unlock), D12 (pre-execution fix, spec Section 3.6 + regression test in `tests/test_advanced.c`), D8 (validator), D9 (Chunks 0/5), D10 (Chunks 1/2/3 tests).
- **Resolved verification items (previously open, now confirmed against source):**
  - `CMEM_ATOMIC_*` macros live in `src/cmem_internal.h` (NOT `include/cmem.h`): C++ `std::atomic` ~28-39, `_WIN32` ~45-61, C11 `<stdatomic.h>` ~67-79. `CMEM_ATOMIC_EXCHANGE` and `CMEM_ATOMIC_COMPARE_EXCHANGE` both exist, plus `CMEM_ATOMIC_INIT`/`FETCH_ADD`/`FETCH_SUB`/`LOAD`/`STORE`.
  - `CMEM_ATOMIC_COMPARE_EXCHANGE(obj, expected, desired, succ, fail)` takes **five** args; every call site in this plan now uses five.
  - `CMEM_ORDER_*` currently defines only RELAXED/ACQUIRE/RELEASE; the plan adds `CMEM_ORDER_ACQ_REL` in all three mapping blocks (Task 1.2 Step 1) before any use.
  - `mp_check_leaks` returns `bool`; tests use `assert(mp_check_leaks(pool) == true)` (matching existing `tests/test_advanced.c`).
  - Pool construction sites: `src/cmem.c:276` (calloc) and `src/cmem_event.c:888` (memset); the registry mutex/list head is initialized/zeroed at both (Task 1.2 Step 5).
  - D11 confirmed: `src/cmem_event.c:3401` is a stray unpaired `pool_unlock` (matching `pool_lock` at ~3406); removed in the Task 1.6 rewrite.
  - `sys_mem_alloc(pool, size, alignment)` / `sys_mem_free(pool, ptr, size)` (`src/cmem_internal.h:627-628`) — used with those exact arities.
  - Constants (`src/cmem_internal.h:183-194`): `TLSF_MIN_BLOCK_SIZE 32`, `TLSF_ALIGN_MASK 7u`, `BLOCK_STATE_FREE 0x1`, `BLOCK_STATE_PREV_FREE 0x2`, `BLOCK_SIZE_MASK`; `TLSF_ARENA_DEFAULT_SIZE = 262144` (`include/cmem.h:1026`). Arena = 256 KiB, so only blocks ≤ ~192 KiB are arena-resident.
  - `tlsf_block_t = { size_and_flags, prev_physical, next_free, prev_free }` (`src/cmem_internal.h:260-265`).
  - `pool_lock`/`pool_unlock` are `static inline` rwlock wrappers (`src/cmem_internal.h:522`/`:528`), no-ops when the pool is not `MP_FLAG_THREAD_SAFE`.
- Validator additions: PREV_FREE consistency (free physical predecessor ⇒ `PREV_FREE` set; non-free predecessor ⇒ cleared); live blocks and `live_bytes` are intentionally not structurally checked (documented inline in the validator).
- Documented spec deviations (Task 5.2 Step 4, recorded in spec §3.7):
  1. **OS release instead of TLSF drain** — the retired region is unmapped with
     `sys_mem_free`; it cannot both join the shared free list and return to the
     OS, and the pool does not track foreign regions.
  2. **Shared-pool fallback rate not instrumented** — the benchmark reports
     throughput/latency only; fallback is covered functionally, not measured.
  3. **D13** — pre-existing `mp_enable_multi_arena` recursive write-lock
     corruption; discovered by the Task 1.7 hang, fixed in `2625b80`
     (children created outside the pool lock, then published under it).
  4. **D14** — pre-existing TLSF cache class/span mismatch (a cached 1024-byte
     block could be re-stamped for a 1600-byte request, overlapping its live
     neighbor); discovered by the Task 3.2 stress SIGSEGV, fixed in `c945f47`
     (exact-span hits only; mismatched blocks return to the shared list and the
     normal allocation path splits them).
  5. **Helper exposure (Task 1.6)** — `tlsf_arena_span`,
     `tlsf_arena_unlink_free`, `tlsf_arena_insert_free` are cross-TU (declared
     in `src/cmem_internal.h`) so `mp_realloc` can reuse the list discipline;
     the design had them static.
  6. **Registry mutex ownership (Task 2.2)** —
     `tlsf_arena_registry_release` frees descriptors but does not destroy
     `arena_registry_lock`; `mp_destroy_shared` remains the single destroyer to
     avoid a double destroy on the shared path.
- Extra finding: `mp_free` must re-point `pool` to `arena->owner_pool` before
  booking stats for arena blocks; otherwise the master handle underflows
  `active_allocations`/`active_bytes` (fixed in `2625b80`).
- Task 5.2 Step 2 note: the old plan had 38 unchecked boxes (not 37); the
  reconciliation ticked all whose behavior exists on HEAD and left the
  superseded artifacts open with inline notes.
- Task 5.2 Step 3 note: `AGENTS.md` is listed in `.gitignore` in this working
  tree, so the invariant note was applied locally and cannot be committed
  without `-f` (deliberately not forced).
- Known risk accepted: `tlsf_arena_validate` runs structural checks in debug (not full live_bytes equality) to stay safe under concurrent mailbox pushes.
