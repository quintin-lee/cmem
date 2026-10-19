# mp_free_batch Single-Lock Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor `mp_free_batch` (src/cmem_event.c:2746) so a batch of N freed pointers acquires the pool write lock at most once per batch instead of once per element. Same-pool SLAB pointers are classified lock-free into a 256-entry stack buffer; their stats/active-list update is aggregated into a single critical section; slot placement (TLS cache → per-CPU → remote-free queue → `slab_free_nolock`) is entirely lock-free afterward. Corrupt, subpool-redirected, and non-SLAB pointers fall back to per-element `mp_free`, preserving observable behavior exactly (plus 3 documented deviations in the spec).

**Motivating data (benchmark 8c/8d):** `mp_free_batch` currently loops over `mp_free` → 64 `pool_wrlock` per batch per thread (SLAB stats path at src/cmem_event.c:2839-2841). Free-path speedup is 0.92x (slower than per-element); 8T batch efficiency is 0.10 vs 0.19 single-element.

**Target:** benchmark 8d speedup ≥ 1.0x; combined alloc+free batch at 8 threads reaches ≥ 0.5x efficiency.

**Architecture (per spec `docs/superpowers/specs/2026-08-06-mp-free-batch-single-lock-design.md`, approved):**

```
mp_free_batch(pool, ptrs, count):
  1. validate pool/ptrs/count
  2. classification loop (lock-free): for each ptr[i]:
       - NULL → skip
       - header = ptr[i] - sizeof(header)
       - fallback to mp_free(pool, ptr[i]) then ptrs[i]=NULL when:
           non-FAST_PATH magic corrupt | header->subpool | alloc_type != SLAB
       - else hdrs[n++] = header; ptrs[i] = NULL; flush block when n == 256
     flush remainder
  flush_slab_block(pool, hdrs, n):
    Phase A: POISON_ON_FREE → memset(ptr, MP_POISON_BYTE, requested_size) per element (lock-free)
    Phase B: aggregated stats:
        non-TS: loop mp_free_stats_update (lock-free)
        TS+FAST_PATH: loop 3× __atomic_fetch_sub/add (lock-free)
        TS non-FAST_PATH: ONE pool_lock → loop mp_free_stats_update → pool_unlock
    Phase C: if event_cb, loop trigger_event(MP_EVENT_FREE, ptr, requested_size) (after unlock)
    Phase D: slot placement (lock-free), per element:
        slot = header->raw_base; class_idx = header->slab_class
        counts[class] < TLS_CACHE_MAX_SLOTS → push tls_cache
        else percpu_push(...) success → done
        else TS → remote_free_push(pool, class_idx, slot)
        else → slab_free_nolock(pool, header)   // non-TS: no lock needed
```

**Ordering invariant (correctness-critical):** Phase B MUST precede Phase D — `slot->next = tls_cache.slots[...]` overwrites `header->next`, which `active_list_remove` (src/cmem_event.c:1900-1912) reads.

**Source of truth (verified line numbers):** `mp_free_batch` body src/cmem_event.c:2746-2757. `mp_free` 2783-2943 (SLAB fast path 2826-2852, per-CPU 2854-2869, remote-free 2872-2882, generic 2885-2943). `mp_free_stats_update` (static) 1921-1929. `active_list_remove` 1900-1912. `TLS_CACHE_MAX_SLOTS` 256 (src/cmem_internal.h:176). `tls_cache_validate_owner` src/cmem_slab.c:99. `remote_free_push` (lock-free CAS) src/cmem_slab.c:146-157. `percpu_push` (lock-free CAS) 1227. `slab_free_nolock` 413-474 (requires sc->lock when TS; non-TS lock-free). `pool_lock`/`pool_unlock` = `pthread_rwlock_wrlock` when MP_FLAG_THREAD_SAFE (src/cmem_internal.h:465-506).

**Style gates:** clang-format; clang-tidy (named constants for non-power-of-2 literals, ≥2-char identifiers, loop `i` exempt); Conventional Commits (scope `perf(batch)`); `make clean` before switching build systems; plans dir stays untracked.

---

## Chunk 1: Rewrite `mp_free_batch` in src/cmem_event.c

**Files:**
- Modify: `src/cmem_event.c` (mp_free_batch at 2746; helper inserted before it)

### Task 1.1: Add `flush_slab_block` helper + constant

- [ ] **Step 1: Insert `CMEM_FREE_BATCH_CHUNK` constant + static helper before `mp_free_batch` (before line 2740 doc comment)**

```c
/* Fixed stack-buffer size for batching same-pool SLAB frees. */
#define CMEM_FREE_BATCH_CHUNK 256

/**
 * @brief Batch-process up to CMEM_FREE_BATCH_CHUNK same-pool SLAB headers.
 *
 * Aggregates the shared stats/active-list update into a single pool write lock
 * (thread-safe pools) or lock-free loop (fast-path / non-thread-safe), then
 * places every slot into the TLS cache, per-CPU freelist, remote-free queue or
 * its slab page — all lock-free. Phase order is critical: stats (which reads
 * header->next via active_list_remove) MUST run before slot placement (which
 * overwrites header->next).
 */
static void flush_slab_block(memory_pool_t *pool, mp_block_header_t **hdrs, size_t n)
{
    /* Phase A: poison */
    if (pool->flags & MP_FLAG_POISON_ON_FREE) {
        for (size_t i = 0; i < n; i++) {
            mp_block_header_t *header = hdrs[i];
            memset((uint8_t *)header + sizeof(mp_block_header_t), MP_POISON_BYTE,
                   header->requested_size);
        }
    }

    /* Phase B: aggregated stats */
    if (!(pool->flags & MP_FLAG_THREAD_SAFE)) {
        for (size_t i = 0; i < n; i++) {
            mp_free_stats_update(pool, hdrs[i]);
        }
    } else if (pool->flags & MP_FLAG_FAST_PATH) {
        for (size_t i = 0; i < n; i++) {
            mp_block_header_t *header = hdrs[i];
            __atomic_fetch_sub(&pool->stats.active_bytes, header->requested_size, __ATOMIC_RELAXED);
            __atomic_fetch_sub(&pool->stats.active_allocations, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&pool->stats.total_free_ops, 1, __ATOMIC_RELAXED);
        }
    } else {
        pool_lock(pool);
        for (size_t i = 0; i < n; i++) {
            mp_free_stats_update(pool, hdrs[i]);
        }
        pool_unlock(pool);
    }

    /* Phase C: events (after unlock; pool_lock is non-recursive) */
    if (pool->event_cb) {
        for (size_t i = 0; i < n; i++) {
            mp_block_header_t *header = hdrs[i];
            void *ptr = (uint8_t *)header + sizeof(mp_block_header_t);
            trigger_event(pool, MP_EVENT_FREE, ptr, header->requested_size);
        }
    }

    /* Phase D: slot placement (lock-free) */
    for (size_t i = 0; i < n; i++) {
        mp_block_header_t *header = hdrs[i];
        uint8_t class_idx = header->slab_class;
        mp_slab_slot_t *slot = (mp_slab_slot_t *)header->raw_base;
        if (tls_cache.counts[class_idx] < TLS_CACHE_MAX_SLOTS) {
            slot->next = tls_cache.slots[class_idx];
            tls_cache.slots[class_idx] = slot;
            tls_cache.counts[class_idx]++;
        } else if (pool->percpu_freelists &&
                   percpu_push(pool, percpu_cpu_index(), class_idx, slot)) {
            /* accepted onto per-CPU freelist */
        } else if (pool->flags & MP_FLAG_THREAD_SAFE) {
            remote_free_push(pool, class_idx, slot);
        } else {
            slab_free_nolock(pool, header);
        }
    }
}
```

Note: `mp_free_stats_update` is `static` in cmem_event.c (1921) — same translation unit, no declaration change needed. `percpu_cpu_index` may need declaring if not already visible in cmem_event.c; check and add extern to cmem_internal.h if missing (verify via compile).

### Task 1.2: Rewrite `mp_free_batch` body

- [ ] **Step 2: Replace body at 2746-2757**

```c
void mp_free_batch(memory_pool_t *pool, void **ptrs, size_t count)
{
    if (!pool || !ptrs || count == 0) {
        return;
    }

    mp_block_header_t *hdrs[CMEM_FREE_BATCH_CHUNK];
    size_t n = 0;
    bool seen_slab = false;

    tls_cache_validate_owner(pool);

    for (size_t i = 0; i < count; i++) {
        if (!ptrs[i]) {
            continue;
        }
        mp_block_header_t *header =
            (mp_block_header_t *)((uint8_t *)ptrs[i] - sizeof(mp_block_header_t));

        if ((pool->flags & MP_FLAG_FAST_PATH) || header->magic == MP_MAGIC_HEAD) {
            if (header->subpool || header->alloc_type != ALLOC_TYPE_SLAB) {
                mp_free(pool, ptrs[i]);
                ptrs[i] = NULL;
                continue;
            }
            hdrs[n++] = header;
            seen_slab = true;
            ptrs[i] = NULL;
            if (n == CMEM_FREE_BATCH_CHUNK) {
                flush_slab_block(pool, hdrs, n);
                n = 0;
            }
        } else {
            mp_free(pool, ptrs[i]);
            ptrs[i] = NULL;
        }
    }

    if (n > 0) {
        flush_slab_block(pool, hdrs, n);
    }
    (void)seen_slab; /* validate_owner already ran unconditionally */
}
```

Wait — refinement: `tls_cache_validate_owner` should only run when there is at least one SLAB element (spec §1.4). It has a `pthread_once` + `pthread_setspecific` cost plus a potential flush-on-owner-change. Per-element `mp_free` calls it only inside the SLAB branch. To preserve per-element cost on non-SLAB batches, gate it: run validate_owner lazily just before the first SLAB element is recorded:

```c
    bool owner_validated = false;
    ...
    if (header->subpool || header->alloc_type != ALLOC_TYPE_SLAB) {
        mp_free(pool, ptrs[i]); ptrs[i] = NULL; continue;
    }
    if (!owner_validated) {
        tls_cache_validate_owner(pool);
        owner_validated = true;
    }
    hdrs[n++] = header; ptrs[i] = NULL;
```

- [ ] **Step 3: Verify compile + format**

`clang-format -i src/cmem_event.c && make format-check` then `make clean && make test` — all existing tests must pass (batch-free equivalence is already exercised by existing batch tests' cleanup, e.g. `mp_free_batch` in test_batch_alloc_and_compact Test 38/39 and bench cleanup).

---

## Chunk 2: Characterization tests (tests/test_main.c, Tests 42+)

**Files:**
- Modify: `tests/test_main.c` (insert after Test 41 `test_batch_alloc_configs`; register in `main()` after `test_batch_alloc_configs();`)

### Task 2.1: Test 42 — THREAD_SAFE batch-free semantics

- [ ] **Step 1: Add `test_batch_free_semantics` (Test 42)**

THREAD_SAFE pool (no FAST_PATH): batch-alloc 512×32B via `mp_alloc_batch`, then `mp_free_batch`. Assert: all `ptrs[i] == NULL` after; `stats.active_allocations == 0`; `stats.total_free_ops` incremented by 512; `mp_check_leaks` clean.

### Task 2.2: Test 43 — batch vs per-element equivalence

- [ ] **Step 2: Add `test_batch_free_equivalence` (Test 43)**

Two identical THREAD_SAFE pools: pool A batch-free 256×64B + 8×1KB (mixed SLAB classes), pool B per-element `mp_free` same pattern. Assert identical final `stats` (active_bytes, active_allocations, total_free_ops, os_allocated_bytes) and both `mp_check_leaks` clean.

### Task 2.3: Test 44 — mixed-tier batch (fallback routing)

- [ ] **Step 3: Add `test_batch_free_mixed_tiers` (Test 44)**

THREAD_SAFE pool: one batch call freeing [SLAB 32B × 8, TLSF 4KB × 4, OS 6MB × 1] in one `ptrs[]` array. Assert all nulled; per-element stats identical to separate frees (active_allocations → 0, os_allocated_bytes accounting exact); leaks clean. Also cover an interleaved order (non-SLAB first, middle, last).

### Task 2.4: Test 45 — corrupt header fallback

- [ ] **Step 4: Add `test_batch_free_corrupt` (Test 45)**

THREAD_SAFE pool: batch-alloc 16×32B; corrupt `header->magic` of element 7 (e.g. `((mp_block_header_t *)(ptrs[7] - sizeof(mp_block_header_t)))->magic = 0xDEADBEEF`); `mp_free_batch`. Assert pool marked dirty (`pool->dirty == true`), all 16 pointers nulled, remaining 15 elements still freed (active_allocations → 0 — note corrupt element was never accounted, so expected active before free = 15).

### Task 2.5: Test 46 — subpool redirection in batch

- [ ] **Step 5: Add `test_batch_free_subpool` (Test 46)**

Pool A with a subpool (verify existing test setup for subpools — e.g. `mp_attach_subpool` or `mp_alloc` on a subpool'd pool); batch includes an element whose `header->subpool` is set (pointing to pool A's owner or another pool); assert it is freed into its owner pool and all pointers nulled, both pools' stats consistent.

### Task 2.6: Test 47 — FAST_PATH batch accounting

- [ ] **Step 6: Add `test_batch_free_fastpath` (Test 47)**

`MP_FLAG_FAST_PATH` pool: batch-alloc 128×16B (or whatever FAST_PATH supports), batch-free. Assert active_allocations == 0 via the atomic accounting (read after batch; small delay not needed — single thread), total_free_ops == 128, no active-list involvement. (Existing FAST_PATH pool construction flags from Test 34+ — reuse pattern.)

### Task 2.7: Test 48 — POISON_ON_FREE batch

- [ ] **Step 7: Add `test_batch_free_poison` (Test 48)**

POISON_ON_FREE pool: batch-alloc 64×64B, fill payload with 0xAB, batch-free, then batch-alloc 64×64B again (same slots reused) and assert every payload byte == MP_POISON_BYTE before overwrite... (simpler: after batch-free, memcmp the freed region is unsafe; instead verify via a second alloc where we inspect before writing). Use the existing poison test pattern if one exists (search `MP_POISON_BYTE` in tests).

### Task 2.8: Test 49 — TLS cache overflow (>256 same-class slots)

- [ ] **Step 8: Add `test_batch_free_overflow` (Test 49)**

THREAD_SAFE pool with PERCPU_FREELIST? Not required — overflow path is per-CPU fallback then remote-free. Batch-alloc 600×32B, batch-free all 600. Assert all nulled, active_allocations == 0, leaks clean. Exercises the >256 overflow into per-CPU/remote-free placement. (600 > 256 forces multiple flush blocks too.)

- [ ] **Step 9: Register all 8 tests in `main()`** after `test_batch_alloc_configs();` — sequential numbers 42-49, `RUN_TEST(...)` style consistent with existing registration.

---

## Chunk 3: Benchmark extension (benchmarks/bench_main.c, benchmark 8d)

**Files:**
- Modify: `benchmarks/bench_main.c` (`bench_batch_free_path`)

### Task 3.1: Thread-scaled free-path benchmark

- [ ] **Step 1: Extend `bench_batch_free_path`** to run 1/2/4/8 threads on a THREAD_SAFE pool, reporting `free_batch Mops | free_single Mops | speedup` per thread count, reusing `bench_batch_run`/`bench_batch_thread_func` (bench_batch_elems = 64; allocation phase outside timing, free phase timed). Keep the existing single-thread 32B case. Named constants for any non-power-of-2 literals (e.g. free ops count).

- [ ] **Step 2: Verify**: `clang-format -i benchmarks/bench_main.c && make format-check`, `make bench` → 8d shows speedup ≥ 1.0x, then `make clean && make test_all`.

---

## Chunk 4: Full verification matrix + commits

- [ ] **Step 1:** `make clean && make test_all` (unit + advanced + C++ PMR, ASan/UBSan) — all pass.
- [ ] **Step 2:** `make clean` → `cmake -B build_cmake -G Ninja && cmake --build build_cmake` (clang-tidy gate, 0 warnings) → `ctest --test-dir build_cmake` (3/3).
- [ ] **Step 3:** `make fuzz-build` (or CMake fuzz target) + run `fuzz_alloc` ~20s smoke.
- [ ] **Step 4:** 15s stress: `gcc -D_GNU_SOURCE -DSTRESS_DURATION_SEC=15 -I./include tests/stress_test.c src/*.c -pthread -o /tmp/stress_verify && /tmp/stress_verify`.
- [ ] **Step 5:** `make bench` → record 8d speedup and 8c 8T efficiency (gate: 8d ≥ 1.0x; 8c batch 8T eff toward ≥ 0.5x).
- [ ] **Step 6:** Commits (Conventional, NO push/amend):
  1. `perf(batch): single-lock mp_free_batch via aggregated stats and lock-free slot placement` (src/cmem_event.c)
  2. `test(batch): add batch-free semantics, equivalence, fallback and overflow tests` (tests/test_main.c)
  3. `bench(batch): scale free-path benchmark across 1/2/4/8 threads` (benchmarks/bench_main.c)
- [ ] **Step 7:** `make clean`; final summary with measured numbers vs the ≥ 0.5x target.
