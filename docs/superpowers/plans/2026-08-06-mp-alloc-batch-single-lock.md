# mp_alloc_batch Single-Lock Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor `mp_alloc_batch` (src/cmem_event.c:2269) so a batch of N same-sized allocations acquires each lock at most once per batch, pulling N slab slots via a new `slab_alloc_batch` primitive, while preserving per-element observable behavior (prefix semantics, events, OOM/emergency/limit/breaker parity).

**Architecture:** Three tiers feed the batch: slab (TLS cache → per-CPU → one `slab_alloc_batch` class-lock bulk pop per 64-slot chunk), TLSF (one pool_lock loop over `tlsf_alloc`, which does not lock internally), OS (lock-free `sys_mem_alloc` loop). A single aggregated pool_lock handles all stats/active-list/alignment for the whole batch. Circuit-breaker accrual is folded into each production loop to prevent over-production (which would leak slots). OOM events, emergency element, and histogram/event exclusions replicate per-element semantics exactly. Full design: `docs/superpowers/specs/2026-08-05-mp-alloc-batch-single-lock-design.md` (approved, 4 documented deviations).

**Tech Stack:** C11, pthread (rwlock + mutex), TLSF/slab/OS allocator tiers, Makefile (build/, ASan+UBSan) + CMake (build_cmake/, Ninja + clang-tidy). Style gates: clang-format, strict clang-tidy (named constants for non-power-of-2 literals, ≥2-char identifiers), Conventional Commits, `make clean` before switching build systems.

**Source of truth (verified line numbers):** `mp_alloc_batch` body at src/cmem_event.c:2269-2284. Per-element slab tier 2054-2115, TLSF branch 2116-2123, OS branch 2124-2156, stats block 2159-2223, `mp_alloc` 2234-2259, `mp_aligned_alloc` 2616-2671, `tls_cache_refill` src/cmem_slab.c:517-587 (the bulk-pop template for `slab_alloc_batch`). `pool_lock`/`pool_unlock`/`pool_rdlock`/`pool_rdunlock` are no-ops unless `MP_FLAG_THREAD_SAFE` (src/cmem_internal.h:465-506).

---

## Chunk 1: `slab_alloc_batch` primitive

### Task 1.1: Declare in internal header

**Files:**
- Modify: `src/cmem_internal.h:553` (after `tls_cache_refill` extern)

- [ ] **Step 1: Add the extern declaration**

Insert immediately after the existing `extern void tls_cache_refill(memory_pool_t *pool, uint8_t class_idx);` line:

```c
extern size_t slab_alloc_batch(memory_pool_t *pool, uint8_t class_idx,
                               mp_slab_slot_t **out_slots, size_t max_count);
```

- [ ] **Step 2: Verify compile**

Run: `gcc -fsyntax-only -Iinclude -Isrc src/cmem_internal.h`
Expected: clean (may need `-pthread` if errno/pthread headers complain — add `-pthread` if so).

### Task 1.2: Implement in cmem_slab.c

**Files:**
- Modify: `src/cmem_slab.c` (insert after `tls_cache_refill`, i.e. after line 587, before the `percpu` doc comment at 589)

- [ ] **Step 1: Add the implementation**

```c
/**
 * @brief Bulk-allocate up to max_count slots for a size class.
 *
 * Pops slots directly from the slab tier under a single class mutex lock
 * acquisition and writes them into out_slots[0..n-1] in allocation order.
 * Page transitions (empty -> partial, partial -> full) are identical to
 * tls_cache_refill. On return the caller owns all produced slots and is
 * responsible for stamping each slot's block header.
 *
 * @param pool      Pool to draw slots from.
 * @param class_idx Size class to allocate from.
 * @param out_slots Receives up to max_count slot pointers (prefix).
 * @param max_count Maximum number of slots requested.
 * @return Number of slots actually produced.
 */
size_t slab_alloc_batch(memory_pool_t *pool, uint8_t class_idx,
                        mp_slab_slot_t **out_slots, size_t max_count)
{
    mp_slab_class_t *sc = &pool->slab_classes[class_idx];
    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_lock(&sc->lock);
    }

    remote_free_harvest(pool, class_idx);

    size_t produced = 0;
    while (produced < max_count) {
        mp_slab_page_t *page = sc->partial_pages;
        if (!page) {
            if (sc->empty_pages) {
                page = sc->empty_pages;
                sc->empty_pages = page->next;
                if (sc->empty_pages) {
                    sc->empty_pages->prev = NULL;
                }
                sc->empty_page_count--;

                page->next = sc->partial_pages;
                page->prev = NULL;
                if (sc->partial_pages) {
                    sc->partial_pages->prev = page;
                }
                sc->partial_pages = page;
            } else {
                page = slab_create_page(pool, class_idx);
                if (!page) {
                    break;
                }
                page->next = sc->partial_pages;
                page->prev = NULL;
                if (sc->partial_pages) {
                    sc->partial_pages->prev = page;
                }
                sc->partial_pages = page;
            }
        }

        mp_slab_slot_t *slot = page->free_list;
        page->free_list = slot->next;
        page->free_count--;

        if (page->free_count == 0) {
            sc->partial_pages = page->next;
            if (page->next) {
                page->next->prev = NULL;
            }

            page->next = sc->full_pages;
            page->prev = NULL;
            if (sc->full_pages) {
                sc->full_pages->prev = page;
            }
            sc->full_pages = page;
        }

        out_slots[produced] = slot;
        produced++;
    }

    pool->stats.slab_allocated_bytes += produced * sc->slot_size;

    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_unlock(&sc->lock);
    }
    return produced;
}
```

Notes: identical page logic to `tls_cache_refill` (verified 517-587); `remote_free_harvest` once per call; `slab_allocated_bytes` accumulates delivered slots only (spec deviation #2); the caller stamps headers, which overwrites the stale `slot->next` free-list linkage at offset 0 — no dangling pointers.

- [ ] **Step 2: Format check**

Run: `clang-format -i src/cmem_slab.c src/cmem_internal.h && make format-check`
Expected: no output, exit 0.

- [ ] **Step 3: Build**

Run: `make`
Expected: compiles clean (gcc, C11), builds `build/libcmem.a` + shared lib.

- [ ] **Step 4: Commit**

```bash
git add src/cmem_slab.c src/cmem_internal.h
git commit -m "feat(slab): add slab_alloc_batch bulk-slot primitive"
```

---

## Chunk 2: Characterization tests (must pass against the CURRENT loop)

These lock in the behavioral contract. They MUST pass against the existing loop-based `mp_alloc_batch` — this is the proof that the batch rewrite in Chunk 3 preserves observable behavior.

### Task 2.1: Add Test 40 — tier coverage

**Files:**
- Modify: `tests/test_main.c` (insert after `test_batch_alloc_and_compact`, i.e. after line 1292)

- [ ] **Step 1: Add test_batch_alloc_tiers**

```c
#define BATCH_OS_SIZE (5u * 1024u * 1024u) /* OS-tier element size for batch test */

/**
 * @brief Tests batch allocation across slab / TLSF / OS tiers.
 */
void test_batch_alloc_tiers()
{
    printf("\n--- Test 40: Batch Allocation Across Allocator Tiers ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_THREAD_SAFE);

    {
        void *ptrs[128];
        size_t count = mp_alloc_batch(pool, 8, ptrs, 128);
        assert(count == 128);
        for (size_t i = 0; i < count; i++) {
            assert(ptrs[i] != NULL);
        }
        mp_stats_t stats;
        mp_get_stats(pool, &stats);
        assert(stats.active_allocations == 128);
        mp_free_batch(pool, ptrs, count);
        mp_get_stats(pool, &stats);
        assert(stats.active_allocations == 0);
    }
    {
        void *ptrs[100];
        size_t count = mp_alloc_batch(pool, 64, ptrs, 100);
        assert(count == 100);
        mp_free_batch(pool, ptrs, count);
    }
    {
        void *ptrs[50];
        size_t count = mp_alloc_batch(pool, 512, ptrs, 50);
        assert(count == 50);
        mp_free_batch(pool, ptrs, count);
    }
    {
        void *ptrs[20];
        size_t count = mp_alloc_batch(pool, 1024, ptrs, 20);
        assert(count == 20);
        mp_free_batch(pool, ptrs, count);
    }
    {
        void *ptrs[10];
        size_t count = mp_alloc_batch(pool, 4096, ptrs, 10);
        assert(count == 10);
        mp_free_batch(pool, ptrs, count);
    }
    {
        void *ptrs[4];
        size_t count = mp_alloc_batch(pool, 1024 * 1024, ptrs, 4);
        assert(count == 4);
        mp_free_batch(pool, ptrs, count);
    }
    {
        void *ptrs[3];
        mp_stats_t before;
        mp_get_stats(pool, &before);
        size_t count = mp_alloc_batch(pool, BATCH_OS_SIZE, ptrs, 3);
        assert(count == 3);
        mp_stats_t after;
        mp_get_stats(pool, &after);
        assert(after.os_allocated_bytes == before.os_allocated_bytes + 3 * BATCH_OS_SIZE);
        assert(after.active_allocations == before.active_allocations + 3);
        mp_free_batch(pool, ptrs, count);
        mp_get_stats(pool, &after);
        assert(after.active_allocations == before.active_allocations);
    }

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_batch_alloc_tiers");
}
```

- [ ] **Step 2: Verify it passes against the current loop**

The OS-tier sub-case validates `os_allocated_bytes` accounting (5 MiB > TLSF_MAX_SIZE → OS path, per-element adds user size at src/cmem_event.c:2154). Expected to pass already.

### Task 2.2: Add Test 41 — configuration coverage

**Files:**
- Modify: `tests/test_main.c` (insert after `test_batch_alloc_tiers`)

- [ ] **Step 1: Add test_batch_alloc_configs**

```c
#define BATCH_LIMIT_PARTIAL 2500u /* limit 2500: fits 4 x 512, not 5 */
#define BATCH_LIMIT_SMALL 100u    /* limit 100: nothing fits */
#define BATCH_QUOTA 200u          /* thread quota: trips after 4 x 64 */
#define BATCH_EMERGENCY_RESERVE 4096u

/**
 * @brief Tests batch allocation with cache-alignment, memory limit,
 *        emergency reserve, OOM fallback, fast path, per-CPU freelists,
 *        multi-arena routing, and circuit breaker.
 */
void test_batch_alloc_configs()
{
    printf("\n--- Test 41: Batch Allocation Configurations ---\n");

    {
        /* (a) Cache-aligned payloads */
        memory_pool_t *pool = mp_create(0, MP_FLAG_CACHE_ALIGNED | MP_FLAG_THREAD_SAFE);
        void *ptrs[64];
        size_t count = mp_alloc_batch(pool, 32, ptrs, 64);
        assert(count == 64);
        for (size_t i = 0; i < count; i++) {
            assert(((uintptr_t)ptrs[i] & 63u) == 0);
            memset(ptrs[i], 0xAB, 32);
        }
        mp_free_batch(pool, ptrs, count);
        assert(mp_check_leaks(pool) == true);
        mp_destroy(pool);
    }
    {
        /* (b) Memory-limit partial fit + emergency buffer: k=4, +1 emergency */
        memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
        bool ok = mp_enable_emergency_reserve(pool, BATCH_EMERGENCY_RESERVE);
        assert(ok);
        mp_set_memory_limit(pool, BATCH_LIMIT_PARTIAL);
        void *ptrs[8];
        size_t count = mp_alloc_batch(pool, 512, ptrs, 8);
        assert(count == 5); /* 4 fit under the limit, 1 emergency element */
        mp_free_batch(pool, ptrs, count);
        assert(mp_check_leaks(pool) == true);
        mp_destroy(pool);
    }
    {
        /* (c) Limit fully exceeded, emergency too small: return 0 */
        memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
        bool ok = mp_enable_emergency_reserve(pool, 512);
        assert(ok);
        mp_set_memory_limit(pool, BATCH_LIMIT_SMALL);
        void *ptrs[4];
        size_t count = mp_alloc_batch(pool, 1024, ptrs, 4);
        assert(count == 0);
        mp_destroy(pool);
    }
    {
        /* (d) OOM fallback ignores the limit: full count */
        memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
        mp_set_memory_limit(pool, BATCH_LIMIT_SMALL);
        mp_set_fallback_on_oom(pool, true);
        void *ptrs[4];
        size_t count = mp_alloc_batch(pool, 1024, ptrs, 4);
        assert(count == 4);
        mp_free_batch(pool, ptrs, count);
        assert(mp_check_leaks(pool) == true);
        mp_destroy(pool);
    }
    {
        /* (e) Fast path */
        memory_pool_t *pool = mp_create(0, MP_FLAG_FAST_PATH);
        void *ptrs[32];
        size_t count = mp_alloc_batch(pool, 64, ptrs, 32);
        assert(count == 32);
        mp_free_batch(pool, ptrs, count);
        assert(mp_check_leaks(pool) == true);
        mp_destroy(pool);
    }
    {
        /* (f) Per-CPU freelist */
        memory_pool_t *pool = mp_create(0, MP_FLAG_PERCPU_FREELIST);
        void *ptrs[32];
        size_t count = mp_alloc_batch(pool, 64, ptrs, 32);
        assert(count == 32);
        mp_free_batch(pool, ptrs, count);
        assert(mp_check_leaks(pool) == true);
        mp_destroy(pool);
    }
    {
        /* (g) Multi-arena routing (recurses into the bound arena) */
        memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
        bool ok = mp_enable_multi_arena(pool, 2);
        assert(ok);
        mp_bind_thread_to_arena(pool, 0);
        void *ptrs[16];
        size_t count = mp_alloc_batch(pool, 64, ptrs, 16);
        assert(count == 16);
        mp_free_batch(pool, ptrs, count);
        assert(mp_check_leaks(pool) == true);
        mp_destroy(pool);
    }
    {
        /* (h) Circuit breaker trips mid-batch (4 x 64 = 256 >= 200) */
        memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
        mp_set_thread_quota(pool, BATCH_QUOTA);
        mp_set_circuit_breaker(pool, true);
        void *ptrs[10];
        size_t count = mp_alloc_batch(pool, 64, ptrs, 10);
        assert(count == 4);
        assert(mp_is_circuit_breaker_tripped(pool) == true);
        mp_free_batch(pool, ptrs, count);
        mp_reset_thread_quota(pool);
        assert(mp_check_leaks(pool) == true);
        mp_destroy(pool);
    }

    TEST_PASS("test_batch_alloc_configs");
}
```

- [ ] **Step 2: Register both tests in main()**

In `main()` (tests/test_main.c), insert after the existing `test_batch_alloc_and_compact();` line (1771):

```c
    test_batch_alloc_tiers();
    test_batch_alloc_configs();
```

- [ ] **Step 3: Run the full suite against the current loop implementation**

Run: `make clean && make test_all && make test_cpp`
Expected: ALL CMEM UNIT TESTS PASSED SUCCESSFULLY (unit + advanced + cpp, ASan/UBSan clean). Tests 40 and 41 must pass with the OLD loop-based `mp_alloc_batch` — this is the characterization baseline.

- [ ] **Step 4: Commit**

```bash
git add tests/test_main.c
git commit -m "test(batch): add tier and config characterization tests for mp_alloc_batch"
```

---

## Chunk 3: `mp_alloc_batch` rewrite (single-lock)

### Task 3.1: Add static helpers before mp_alloc_batch

**Files:**
- Modify: `src/cmem_event.c` (insert before the `mp_alloc_batch` doc comment at line 2261)

- [ ] **Step 1: Add chunk constants and the four static helpers**

```c
/* Batch allocation chunk constants (powers of two for slab/clang-tidy). */
enum {
    CMEM_BATCH_SLAB_CHUNK = 64, /**< Slots pulled per slab_alloc_batch call */
    CMEM_BATCH_ALIGN_MASK = 63  /**< Cache-line alignment mask (MP_FLAG_CACHE_ALIGNED) */
};

/**
 * @brief Stamp a slab slot's block header and return its payload pointer.
 */
static void *batch_format_slab(memory_pool_t *pool, uint8_t class_idx,
                               mp_slab_slot_t *slot, size_t req_size)
{
    mp_block_header_t *header = (mp_block_header_t *)slot;
    header->alloc_type = ALLOC_TYPE_SLAB;
    header->slab_class = class_idx;
    header->flags = 0;
    header->requested_size = req_size;
    header->usable_size = pool->slab_classes[class_idx].slot_size;
    header->raw_base = slot;
    header->subpool = pool;
    if (!(pool->flags & MP_FLAG_FAST_PATH)) {
        header->magic = MP_MAGIC_HEAD;
        header->alloc_file = NULL;
        header->alloc_line = 0;
        header->alloc_func = NULL;
        header->backtrace_depth = 0;
        header->prev = NULL;
        header->next = NULL;
    }

    void *payload = (void *)((uint8_t *)header + sizeof(mp_block_header_t));
    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
        uint8_t *canary = (uint8_t *)payload + req_size;
        *canary = MP_CANARY_BYTE;
    }
    if (pool->flags & MP_FLAG_ZERO_ON_ALLOC) {
        memset(payload, 0, req_size);
    }
    return payload;
}

/**
 * @brief Stamp the single-use emergency buffer and return its payload.
 */
static void *batch_format_emergency(memory_pool_t *pool, size_t req_size)
{
    pool->emergency_used = true;
    mp_block_header_t *header = (mp_block_header_t *)pool->emergency_buf;
    header->alloc_type = ALLOC_TYPE_EMERGENCY;
    header->slab_class = 0;
    header->flags = 0;
    header->requested_size = req_size;
    header->usable_size = pool->emergency_size - sizeof(mp_block_header_t);
    header->raw_base = pool->emergency_buf;
    header->subpool = NULL;
    if (!(pool->flags & MP_FLAG_FAST_PATH)) {
        header->magic = MP_MAGIC_HEAD;
        header->alloc_file = NULL;
        header->alloc_line = 0;
        header->alloc_func = NULL;
        header->backtrace_depth = 0;
        header->prev = NULL;
        header->next = NULL;
    }

    void *payload = (void *)((uint8_t *)header + sizeof(mp_block_header_t));
    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
        uint8_t *canary = (uint8_t *)payload + req_size;
        *canary = MP_CANARY_BYTE;
    }
    if (pool->flags & MP_FLAG_ZERO_ON_ALLOC) {
        memset(payload, 0, req_size);
    }
    return payload;
}

/**
 * @brief Stamp an OS-tier block and accumulate its accounting deltas.
 */
static void *batch_format_os(memory_pool_t *pool, void *raw_mem, size_t req_size,
                             size_t os_total, size_t *os_bytes, size_t *os_pool_total)
{
    mp_block_header_t *header = (mp_block_header_t *)raw_mem;
    header->alloc_type = ALLOC_TYPE_OS;
    header->slab_class = 0;
    header->flags = 0;
    header->requested_size = req_size;
    header->usable_size = req_size;
    header->raw_base = raw_mem;
    header->subpool = pool;
    if (!(pool->flags & MP_FLAG_FAST_PATH)) {
        header->magic = MP_MAGIC_HEAD;
        header->alloc_file = NULL;
        header->alloc_line = 0;
        header->alloc_func = NULL;
        header->backtrace_depth = 0;
    }

    void *payload = (void *)((uint8_t *)header + sizeof(mp_block_header_t));
    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
        uint8_t *canary = (uint8_t *)payload + req_size;
        *canary = MP_CANARY_BYTE;
    }
    if (pool->flags & MP_FLAG_ZERO_ON_ALLOC) {
        memset(payload, 0, req_size);
    }
    *os_bytes += req_size;
    *os_pool_total += os_total;
    return payload;
}

/**
 * @brief Accrue the calling thread's quota toward the circuit breaker.
 *
 * Lock-free; the caller must set pool->circuit_breaker_tripped under
 * pool_lock (or directly when already holding it, e.g. the TLSF loop).
 *
 * @return true when the quota crossed the trip threshold.
 */
static bool batch_breaker_accrue(memory_pool_t *pool, size_t user_size)
{
    if (!pool->circuit_breaker_enabled) {
        return false;
    }
    thread_quota.alloc_bytes += user_size;
    thread_quota.alloc_count++;
    return pool->thread_quota_bytes > 0 &&
           thread_quota.alloc_bytes >= pool->thread_quota_bytes;
}
```

Note: `thread_quota` is the module TLS variable (`extern MP_THREAD_LOCAL mp_thread_quota_t thread_quota`, src/cmem_internal.h:233) — NOT a pool field, matching per-element accrual at src/cmem_event.c:2250 and 2630.

- [ ] **Step 2: Verify helpers compile**

Run: `make`
Expected: compiles (the four static helpers are unused until Task 3.2, so gcc emits `-Wunused-function` warnings — expected, no `-Werror` in the Makefile; the build succeeds). If the tree's build dir is stale, run `make clean` first.

### Task 3.2: Replace mp_alloc_batch

**Files:**
- Modify: `src/cmem_event.c:2269-2284` (replace the entire body of `mp_alloc_batch`; keep the doc comment at 2261-2268)

- [ ] **Step 1: Replace the body**

Replace lines 2270-2283 (everything between `{` and `}`) with:

```c
    if (!pool || !out_ptrs || count == 0 || size == 0) {
        return 0;
    }

    /* Arena routing: delegate to the bound arena, exactly like mp_alloc. */
    if (pool->num_arenas > 0) {
        memory_pool_t *arena = mp_get_thread_bound_arena(pool);
        if (arena && arena != pool) {
            return mp_alloc_batch(arena, size, out_ptrs, count);
        }
    }

    /* Entry checks on the resolved pool (mirror mp_alloc_internal). */
    if (mp_is_pool_dirty(pool) && !pool->fallback_to_sys_alloc_on_oom) {
        return 0;
    }
    if (pool->circuit_breaker_enabled && pool->circuit_breaker_tripped) {
        return 0;
    }

    const bool aligned = (pool->flags & MP_FLAG_CACHE_ALIGNED) != 0;
    const size_t canary_len = (pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0;
    const size_t header_sz = sizeof(mp_block_header_t);
    const size_t total_size = aligned ? size + 64 + header_sz + canary_len : size;
    /* Per-element parity: the limit, active_bytes, histogram bucket and event
     * sizes charge the internal request for aligned pools but the user size
     * for non-aligned pools (mp_aligned_alloc passes total_size into
     * mp_alloc_internal at src/cmem_event.c:2622-2624; plain mp_alloc passes
     * user size). The circuit breaker always uses the user size. */
    const size_t limit_charge = aligned ? total_size : size;

    /* Memory-limit decision: single snapshot, per-element limit semantics.
     * NOTE: the limit lives on mp_stats_t (pool->stats.max_memory_limit),
     * exactly as per-element reads it at src/cmem_event.c:1981. */
    size_t target = count;
    size_t oom_fire_count = 0;           /* fallback: one OOM per exceeding element */
    bool full_exceed_oom = false;        /* full-exceed, no emergency: 1 OOM, return 0 */
    bool limit_breach_attempted = false; /* partial-fit: element k would exceed -> 1 OOM */
    bool emergency_only = false;
    bool do_emergency = false;

    if (pool->stats.max_memory_limit > 0) {
        size_t active = pool->stats.active_bytes;
        if (pool->flags & MP_FLAG_THREAD_SAFE) {
            pool_rdlock(pool);
            active = pool->stats.active_bytes;
            pool_rdunlock(pool);
        }
        if (active + limit_charge > pool->stats.max_memory_limit) {
            /* Even one element does not fit. */
            if (pool->fallback_to_sys_alloc_on_oom) {
                oom_fire_count = count;
            } else if (pool->emergency_buf && !pool->emergency_used &&
                       total_size + header_sz + canary_len <= pool->emergency_size) {
                emergency_only = true;
            } else {
                full_exceed_oom = true;
            }
        } else {
            size_t k = (pool->stats.max_memory_limit - active) / limit_charge;
            if (k < count) {
                if (pool->fallback_to_sys_alloc_on_oom) {
                    oom_fire_count = count - k;
                    target = count;
                } else if (pool->emergency_buf && !pool->emergency_used &&
                           total_size + header_sz + canary_len <= pool->emergency_size) {
                    target = k;
                    do_emergency = true;
                    limit_breach_attempted = true;
                } else {
                    target = k;
                    limit_breach_attempted = true;
                }
            }
        }
    }

    /* OOM events, in per-element order:
     * - Fallback: one MP_EVENT_OOM per independently-exceeding element
     *   (per-element fires at src/cmem_event.c:1996 for each).
     * - Full-exceed without emergency: one OOM, then return 0 (per-element
     *   fires once at 1996 and returns NULL at 2047).
     * - Emergency-only: OOM#1 for the limit-rejected element that consumes
     *   the emergency buffer (OOM#2 for the follow-up rejected element fires
     *   in the emergency block below, unless the breaker trips first). */
    for (size_t i = 0; i < oom_fire_count; i++) {
        trigger_event(pool, MP_EVENT_OOM, NULL, limit_charge);
    }
    if (full_exceed_oom) {
        trigger_event(pool, MP_EVENT_OOM, NULL, limit_charge);
        return 0;
    }
    if (emergency_only) {
        trigger_event(pool, MP_EVENT_OOM, NULL, limit_charge);
    }

    size_t n = 0;
    bool emergency_produced = false;
    size_t emergency_index = 0;
    bool breaker_tripped = false;

    if (!emergency_only && (pool->flags & MP_FLAG_STATIC_BUFFER) == 0 &&
        total_size <= SLAB_MAX_SIZE) {
        tls_cache_validate_owner(pool);
        uint8_t class_idx = get_slab_class_index(pool, total_size);

        /* Phase A: drain the thread-local cache (lock-free). */
        size_t avail = tls_cache.counts[class_idx];
        size_t need = target - n;
        if (avail > need) {
            avail = need;
        }
        for (size_t i = 0; i < avail; i++) {
            mp_slab_slot_t *slot = tls_cache.slots[class_idx];
            tls_cache.slots[class_idx] = slot->next; /* read before stamping */
            tls_cache.counts[class_idx]--;
            out_ptrs[n] = batch_format_slab(pool, class_idx, slot, total_size);
            n++;
            if (batch_breaker_accrue(pool, size)) {
                pool_lock(pool);
                pool->circuit_breaker_tripped = true;
                pool_unlock(pool);
                breaker_tripped = true;
                break;
            }
        }

        /* Phase B: per-CPU freelist (lock-free). */
        if (!breaker_tripped && n < target && pool->percpu_freelists) {
            int cpu = percpu_cpu_index();
            while (n < target) {
                mp_slab_slot_t *slot = percpu_pop(pool, cpu, class_idx);
                if (!slot) {
                    percpu_refill(pool, cpu, class_idx);
                    slot = percpu_pop(pool, cpu, class_idx);
                    if (!slot) {
                        break;
                    }
                }
                out_ptrs[n] = batch_format_slab(pool, class_idx, slot, total_size);
                n++;
                if (batch_breaker_accrue(pool, size)) {
                    pool_lock(pool);
                    pool->circuit_breaker_tripped = true;
                    pool_unlock(pool);
                    breaker_tripped = true;
                    break;
                }
            }
        }

        /* Phase C: bulk pop from the slab tier (one class lock per chunk).
         * Cap each chunk by the remaining circuit-breaker budget so the
         * breaker can never trip with already-popped-but-unstamped slots
         * (slab_alloc_batch removes slots from the page free lists; the
         * popped-but-unstamped remainder would be permanently lost). cap is
         * ceil((threshold - accrued) / user_size) and INCLUDES the crossing
         * element, matching per-element semantics. */
        if (!breaker_tripped && n < target) {
            mp_slab_slot_t *slots[CMEM_BATCH_SLAB_CHUNK];
            while (n < target) {
                size_t want = target - n;
                if (want > CMEM_BATCH_SLAB_CHUNK) {
                    want = CMEM_BATCH_SLAB_CHUNK;
                }
                if (pool->circuit_breaker_enabled && pool->thread_quota_bytes > 0) {
                    size_t cap = (pool->thread_quota_bytes > thread_quota.alloc_bytes)
                                     ? (pool->thread_quota_bytes - thread_quota.alloc_bytes +
                                        size - 1) /
                                           size
                                     : 0;
                    if (cap == 0) {
                        /* Already over quota (thread crossed on another pool):
                         * still produce the single crossing element and trip
                         * THIS pool, as per-element would (its entry check runs
                         * on this pool's tripped flag, which is still false). */
                        want = 1;
                    } else if (want > cap) {
                        want = cap;
                    }
                }
                size_t got = slab_alloc_batch(pool, class_idx, slots, want);
                if (got == 0) {
                    break;
                }
                for (size_t i = 0; i < got; i++) {
                    out_ptrs[n] = batch_format_slab(pool, class_idx, slots[i], total_size);
                    n++;
                    if (batch_breaker_accrue(pool, size)) {
                        pool_lock(pool);
                        pool->circuit_breaker_tripped = true;
                        pool_unlock(pool);
                        breaker_tripped = true;
                        break;
                    }
                }
                if (breaker_tripped) {
                    break;
                }
            }
        }
    } else if (!emergency_only &&
               (total_size <= TLSF_MAX_SIZE || (pool->flags & MP_FLAG_STATIC_BUFFER))) {
        /* TLSF: one pool lock for the whole loop (tlsf_alloc does not lock). */
        pool_lock(pool);
        while (n < target && !breaker_tripped) {
            void *ptr = tlsf_alloc(pool, total_size);
            if (!ptr) {
                break;
            }
            out_ptrs[n] = ptr;
            n++;
            if (batch_breaker_accrue(pool, size)) {
                pool->circuit_breaker_tripped = true; /* lock already held */
                breaker_tripped = true;
            }
        }
        pool_unlock(pool);
    } else if (!emergency_only) {
        /* OS tier: lock-free loop, accounting accumulated after. */
        size_t os_total = total_size + header_sz + canary_len;
        size_t os_bytes = 0;
        size_t os_pool_total = 0;
        while (n < target) {
            void *raw_mem = sys_mem_alloc(pool, os_total, 8);
            if (!raw_mem) {
                break;
            }
            out_ptrs[n] =
                batch_format_os(pool, raw_mem, total_size, os_total, &os_bytes, &os_pool_total);
            n++;
            if (batch_breaker_accrue(pool, size)) {
                pool_lock(pool);
                pool->circuit_breaker_tripped = true;
                pool_unlock(pool);
                breaker_tripped = true;
            }
        }
        pool->stats.os_allocated_bytes += os_bytes;
        pool->stats.total_pool_size += os_pool_total;
    }

    /* Partial-fit OOM#1: element k would have been attempted and rejected by
     * the limit. Fire it only when all k elements were actually produced AND
     * the circuit breaker has not tripped — per-element: a tripped breaker
     * rejects the next element at the entry check (src/cmem_event.c:1977)
     * BEFORE the limit check, so no OOM fires; physical exhaustion before k
     * (j < k) also fires none. */
    if (limit_breach_attempted && n == target && !breaker_tripped) {
        trigger_event(pool, MP_EVENT_OOM, NULL, limit_charge);
    }

    /* Emergency element: only when the limit path requested it, all target
     * elements were produced (prefix semantics), and the breaker has not
     * tripped. OOM#2 fires for the follow-up rejected element unless the
     * breaker trips on the emergency accrual (per-element: the breaker entry
     * check precedes the limit check at 1977, so a tripped breaker suppresses
     * the follow-up OOM). */
    if (!breaker_tripped && (emergency_only || (do_emergency && n == target))) {
        out_ptrs[n] = batch_format_emergency(pool, total_size);
        emergency_index = n;
        n++;
        emergency_produced = true;
        if (batch_breaker_accrue(pool, size)) {
            pool_lock(pool);
            pool->circuit_breaker_tripped = true;
            pool_unlock(pool);
        } else {
            trigger_event(pool, MP_EVENT_OOM, NULL, limit_charge);
        }
    }

    /* Aggregated stats + active-list + cache-alignment relink, one lock. */
    if (n > 0) {
        pool_lock(pool);
        for (size_t i = 0; i < n; i++) {
            mp_block_header_t *orig_h =
                (mp_block_header_t *)((uint8_t *)out_ptrs[i] - header_sz);
            if (!(pool->flags & MP_FLAG_FAST_PATH)) {
                active_list_add(pool, orig_h);
            }
            if (aligned) {
                uintptr_t raw_addr = (uintptr_t)out_ptrs[i];
                uintptr_t aligned_addr = (raw_addr + header_sz + CMEM_BATCH_ALIGN_MASK) &
                                         ~(uintptr_t)CMEM_BATCH_ALIGN_MASK;
                mp_block_header_t *new_h =
                    (mp_block_header_t *)(aligned_addr - header_sz);
                if (new_h != orig_h) {
                    *new_h = *orig_h;
                    new_h->requested_size = size;
                    if (orig_h->prev) {
                        orig_h->prev->next = new_h;
                    } else {
                        pool->active_head = new_h;
                    }
                    if (orig_h->next) {
                        orig_h->next->prev = new_h;
                    }
                } else {
                    new_h->requested_size = size;
                }
                if (pool->flags & MP_FLAG_DEBUG_CANARY) {
                    uint8_t *canary = (uint8_t *)aligned_addr + size;
                    *canary = MP_CANARY_BYTE;
                }
                out_ptrs[i] = (void *)aligned_addr;
            }
        }
        pool->stats.active_bytes += n * limit_charge;
        if (pool->stats.active_bytes > pool->stats.peak_bytes) {
            pool->stats.peak_bytes = pool->stats.active_bytes;
        }
        pool->stats.active_allocations += n;
        pool->stats.total_alloc_ops += n;
        size_t hist_count = n - (emergency_produced ? 1 : 0);
        if (hist_count > 0) {
            int bucket = get_slab_class_index(pool, limit_charge);
            if (bucket < CMEM_HISTOGRAM_BUCKETS) {
                pool->stats.size_histogram[bucket] += hist_count;
            }
        }
        bool call_watermark = pool->watermark_cb != NULL;
        pool_unlock(pool);

        /* Post-unlock callbacks (never inside the lock: non-recursive rwlock). */
        if (pool->event_cb) {
            for (size_t i = 0; i < n; i++) {
                if (emergency_produced && i == emergency_index) {
                    continue;
                }
                trigger_event(pool, MP_EVENT_ALLOC, out_ptrs[i], limit_charge);
            }
        }
        if (call_watermark) {
            check_watermark_after_change(pool);
        }
    }

    return n;
```

- [ ] **Step 2: Format**

Run: `clang-format -i src/cmem_event.c && make format-check`
Expected: no output, exit 0.

### Task 3.3: Build and test

- [ ] **Step 1: Run the full suite**

Run: `make clean && make test_all && make test_cpp`
Expected: ALL CMEM UNIT TESTS PASSED SUCCESSFULLY (ASan/UBSan clean). Test 9 (existing batch test), Tests 40 and 41 (characterization) must pass — proving behavior preservation vs the loop baseline. (`make test` alone only builds/runs `unit_tests`; `test_all` adds `test_advanced`, `test_cpp` covers the C++ PMR wrapper.)

- [ ] **Step 2: Verify the lock reduction empirically (optional, quick)**

Run: `./build/benchmark | grep -A5 "Benchmark 8"`
Expected: "Batch speedup" no worse than the pre-change run (batch path now does strictly fewer lock ops). If the machine is noisy, re-run the suite; do not gate on absolute numbers.

- [ ] **Step 3: Commit**

```bash
git add src/cmem_event.c
git commit -m "perf(batch): single-lock mp_alloc_batch via slab_alloc_batch bulk pop"
```

---

## Chunk 4: Benchmark extension and full verification

### Task 4.1: Extend the batch benchmark

**Files:**
- Modify: `benchmarks/bench_main.c` (extend `bench_batch_apis`, lines 557-605)

- [ ] **Step 1: Add TLSF-size and THREAD_SAFE batch cases**

Append before the final `mp_destroy(pool); free(ptrs);` (lines 603-604) in `bench_batch_apis`:

```c
    /* TLSF-class size (4 KB) on the same pool */
    start = get_time_sec();
    for (int batch_idx = 0; batch_idx < BENCH_BATCH_COUNT; batch_idx++) {
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            ptrs[i] = mp_alloc(pool, BENCH_SIZE_TLSF);
            bench_escape(ptrs[i]);
        }
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            mp_free(pool, ptrs[i]);
        }
    }
    double time_tlsf_single = get_time_sec() - start;

    start = get_time_sec();
    for (int batch_idx = 0; batch_idx < BENCH_BATCH_COUNT; batch_idx++) {
        size_t got = mp_alloc_batch(pool, BENCH_SIZE_TLSF, ptrs, BENCH_BATCH_ELEMS);
        if (got != BENCH_BATCH_ELEMS) {
            fprintf(stderr, "mp_alloc_batch(4KB) returned %zu (expected %d)\n", got,
                    BENCH_BATCH_ELEMS);
        }
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            bench_escape(ptrs[i]);
        }
        mp_free_batch(pool, ptrs, BENCH_BATCH_ELEMS);
    }
    double time_tlsf_batch = get_time_sec() - start;
    printf("  TLSF 4KB single : %.4f sec (%.2f Mops/sec)\n",
           time_tlsf_single, (total_ops / time_tlsf_single) / MILLION_OPS);
    printf("  TLSF 4KB batch  : %.4f sec (%.2f Mops/sec)\n",
           time_tlsf_batch, (total_ops / time_tlsf_batch) / MILLION_OPS);
    printf("  TLSF 4KB speedup: %.2fx\n", time_tlsf_single / time_tlsf_batch);

    /* THREAD_SAFE pool (lock-bound): batch should amortize pool_lock */
    memory_pool_t *ts_pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_SAFE);
    bench_warmup(ts_pool);
    start = get_time_sec();
    for (int batch_idx = 0; batch_idx < BENCH_BATCH_COUNT; batch_idx++) {
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            ptrs[i] = mp_alloc(ts_pool, BENCH_SIZE_FIXED);
            bench_escape(ptrs[i]);
        }
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            mp_free(ts_pool, ptrs[i]);
        }
    }
    double time_ts_single = get_time_sec() - start;

    start = get_time_sec();
    for (int batch_idx = 0; batch_idx < BENCH_BATCH_COUNT; batch_idx++) {
        size_t got = mp_alloc_batch(ts_pool, BENCH_SIZE_FIXED, ptrs, BENCH_BATCH_ELEMS);
        if (got != BENCH_BATCH_ELEMS) {
            fprintf(stderr, "mp_alloc_batch(TS) returned %zu (expected %d)\n", got,
                    BENCH_BATCH_ELEMS);
        }
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            bench_escape(ptrs[i]);
        }
        mp_free_batch(ts_pool, ptrs, BENCH_BATCH_ELEMS);
    }
    double time_ts_batch = get_time_sec() - start;
    printf("  TS single loops : %.4f sec (%.2f Mops/sec)\n",
           time_ts_single, (total_ops / time_ts_single) / MILLION_OPS);
    printf("  TS batch APIs   : %.4f sec (%.2f Mops/sec)\n",
           time_ts_batch, (total_ops / time_ts_batch) / MILLION_OPS);
    printf("  TS batch speedup: %.2fx\n", time_ts_single / time_ts_batch);

    mp_destroy(ts_pool);
```

- [ ] **Step 2: Add the TLSF benchmark size constant**

Near `BENCH_SIZE_FIXED` (benchmarks/bench_main.c:23):

```c
#define BENCH_SIZE_TLSF 4096 /* TLSF-class element size for batch benchmark */
```

- [ ] **Step 3: Build and run the benchmark**

Run: `make benchmark && ./build/benchmark`
Expected: Benchmark 8 prints the new TLSF 4KB and TS single/batch rows with a speedup ≥ 1.0x (batch amortizes lock overhead).

### Task 4.2: Full verification matrix

- [ ] **Step 1: CMake build + ctest (clang, clang-tidy gate)**

```bash
make clean
cmake -B build_cmake -G Ninja
cmake --build build_cmake
ctest --test-dir build_cmake
```

Expected: build clean (clang + strict clang-tidy — no magic-number or identifier-length violations), all ctest tests pass.

- [ ] **Step 2: Fuzz smoke**

Run: `make fuzz-build` then run the fuzzer briefly (`./build/fuzz_alloc` with a timeout via `timeout 20` — see the fuzz target name in the Makefile; use the same invocation CI uses, e.g. `-runs=100000`).
Expected: no crashes/leaks (libFuzzer exits clean or reports no errors).

- [ ] **Step 3: Stress smoke (15s, not the 24h default)**

```bash
gcc -DSTRESS_DURATION_SEC=15 tests/stress_test.c src/*.c -pthread -o /tmp/stress_verify && /tmp/stress_verify
```

Expected: passes (no leak report, no assertion failure).

- [ ] **Step 4: Commit benchmark changes**

```bash
git add benchmarks/bench_main.c
git commit -m "bench(batch): add TLSF and thread-safe single-vs-batch cases"
```

- [ ] **Step 5: Clean up**

Run: `make clean` to leave the tree in a consistent state (or rebuild per the user's workflow preference).

---

## Verification checklist (final gate)

- [ ] `make format-check` clean
- [ ] `make clean && make test_all && make test_cpp` — all unit + advanced + cpp tests pass under ASan/UBSan
- [ ] CMake + ctest clean (clang, strict clang-tidy)
- [ ] Fuzz smoke + 15s stress pass
- [ ] `git status` shows only intended files; plans directory left untracked (not committed, per AGENTS.md)
