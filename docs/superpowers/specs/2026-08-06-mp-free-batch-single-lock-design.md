# Spec: Single-Lock Batch Free (mp_free_batch) Optimization for cmem

## Overview

Refactor `mp_free_batch` in `src/cmem_event.c` so a batch of N freed pointers acquires the pool write lock at most once per batch instead of once per element. Slab-backed pointers are classified lock-free, their shared stats/active-list update is aggregated into one critical section, and the slot placement (TLS cache / per-CPU freelist / remote-free queue / slab page) happens entirely lock-free afterward. All pool configurations (thread-safe, fast path, poison-on-free, canary, subpool redirection, per-CPU freelist) are supported with observable behavior matching N consecutive `mp_free` calls, except for three explicitly documented deviations (Section 4).

The motivating data (benchmark 8c, THREAD_SAFE single pool): single-element free costs one `pool_wrlock` per element (`src/cmem_event.c:2839-2841`, `2861-2863`, `2873-2875`), so a 64-element batch pays 64 write locks per thread. Under 8 threads this write-lock storm is the dominant scalability killer (batch free-path speedup currently 0.92x, i.e. *slower* than per-element; 8T batch efficiency 0.10 vs single-element 0.19).

## Detailed Requirements

### 1. Rewritten `mp_free_batch` (`src/cmem_event.c:2746` → new body)

Control flow, in order:

1. **Validation**: `!pool || !ptrs || count == 0` returns immediately (current behavior).

2. **Classification pass** (lock-free, single loop over `ptrs[]`): for each non-NULL `ptrs[i]`:
   - `header = (mp_block_header_t *)((uint8_t *)ptrs[i] - sizeof(mp_block_header_t))`.
   - **Fallback to `mp_free(pool, ptrs[i])`** (then `ptrs[i] = NULL`) when any of:
     - `!(pool->flags & MP_FLAG_FAST_PATH) && header->magic != MP_MAGIC_HEAD` (corrupt header — `mp_free` handles the error path/logging at `src/cmem_event.c:2792-2807`);
     - `header->subpool != NULL` (ownership redirection — the element may belong to a different pool, `src/cmem_event.c:2809-2820`);
     - `header->alloc_type != ALLOC_TYPE_SLAB` (TLSF / OS / EMERGENCY elements keep their existing generic path `src/cmem_event.c:2885-2943`).
   - Otherwise (a same-pool SLAB element): record `hdrs[n++] = header` in a fixed 256-entry stack buffer, set `ptrs[i] = NULL`, and when the buffer fills (`n == 256`) flush it via the block routine below. After the loop, flush any remainder.

   The payload pointer for POISON and events is recovered as `ptr = (uint8_t *)header + sizeof(mp_block_header_t)` (fixed layout per `src/cmem_event.c:2789`), so only one array is needed. The stack footprint is 256 × 8 B = 2 KiB; re-entrant callbacks can recurse but remain well within default thread stacks.

3. **Block routine** `flush_slab_block(pool, hdrs, n)` — phases, in order:

   - **Phase A — poison** (lock-free): if `pool->flags & MP_FLAG_POISON_ON_FREE`, `memset(ptr, MP_POISON_BYTE, header->requested_size)` per element (matches `src/cmem_event.c:2822-2824`, which runs before the stats update).

   - **Phase B — aggregated stats** (one lock, not one per element):
     - Not `MP_FLAG_THREAD_SAFE`: loop `mp_free_stats_update(pool, hdrs[i])` lock-free (matches `2831-2832`).
     - `MP_FLAG_THREAD_SAFE` + `MP_FLAG_FAST_PATH`: loop the three relaxed atomics per element — `__atomic_fetch_sub(&stats.active_bytes, requested_size)`, `__atomic_fetch_sub(&stats.active_allocations, 1)`, `__atomic_fetch_add(&stats.total_free_ops, 1)` — lock-free (matches `2833-2837`).
     - `MP_FLAG_THREAD_SAFE` without FAST_PATH: **one `pool_lock(pool)`**, inside it loop `mp_free_stats_update(pool, hdrs[i])` for all n elements (each performs `active_list_remove` + counter updates, `src/cmem_event.c:1921-1929`), then `pool_unlock(pool)`.

   - **Phase C — events** (after unlock): if `pool->event_cb`, loop `trigger_event(pool, MP_EVENT_FREE, ptr, header->requested_size)` per element in order (matches per-element placement — event fires after stats, before slot placement, `2843-2845`).

   - **Phase D — slot placement** (lock-free): per element in order, `slot = header->raw_base`; `class_idx = header->slab_class`:
     - if `tls_cache.counts[class_idx] < TLS_CACHE_MAX_SLOTS`: `slot->next = tls_cache.slots[class_idx]; tls_cache.slots[class_idx] = slot; tls_cache.counts[class_idx]++` (matches `2847-2850`);
     - else if `pool->percpu_freelists && percpu_push(pool, percpu_cpu_index(), class_idx, slot)`: done (matches `2854-2869`);
     - else if `MP_FLAG_THREAD_SAFE`: `remote_free_push(pool, class_idx, slot)` (lock-free Treiber push, `src/cmem_slab.c:146-157`; matches `2872-2881`);
     - else: `slab_free_nolock(pool, header)` — valid only for non-thread-safe pools, where the class lock is never taken (matches the generic fallthrough `2923-2925` under a no-op `pool_lock`).

4. **`tls_cache_validate_owner(pool)`** is called once before the classification pass when the pool is thread-safe and at least one SLAB element is present (matches `2827`, and guarantees the TLS cache belongs to this pool before Phase D writes to it).

### 2. Ordering invariant (correctness-critical)

**Phase B (stats / `active_list_remove`) MUST run before Phase D (slot placement).** `slot->next = tls_cache.slots[...]` overwrites `header->next`, which `active_list_remove` (`src/cmem_event.c:1900-1912`) needs. The existing per-element code already honors this order (stats at `2839` before push at `2847`); the batch preserves it block-wide.

### 3. Lock overhead per batch (THREAD_SAFE pool, N SLAB elements)

| Path | Before (N × `mp_free`) | After (batch) |
|---|---|---|
| Stats (non-FAST_PATH) | N × `pool_wrlock` | **1 × `pool_wrlock`** |
| Stats (FAST_PATH) | N × 3 atomics | N × 3 atomics (unchanged, already lock-free) |
| TLS cache push | 0 locks | 0 locks |
| Per-CPU push | 0 locks | 0 locks |
| Remote-free push | 0 locks | 0 locks |

Non-SLAB, corrupt, and redirected elements pay their existing `mp_free` cost (fallback) — unchanged.

### 4. Behavior preservation and documented deviations

Behavior identical to N × `mp_free` for: `ptrs[i]` nulling (including fallback elements), magic validation + error path, subpool redirection, POISON fill, per-element `MP_EVENT_FREE` events (after stats, before slot placement), FAST_PATH atomic accounting, `slab_allocated_bytes` semantics (only decremented by `slab_free_nolock` for direct page returns, never by cache/queue placement), canary check (only via the generic fallback path for non-SLAB elements — SLAB fast path never canary-checks, same as today), and `remote_free_push` header-restamp semantics.

**Documented deviations** (intentional, must be covered by tests):

1. **Deferred event observation**: a *re-entrant* `MP_EVENT_FREE` callback observes the fully-aggregated stats (all n elements already unlinked/decremented) instead of `i+1` after element i. Event *order* is preserved. (Symmetric to the alloc-side deviation already documented in the `mp_alloc_batch` spec, Section 4.3.)
2. **TLS cache fill order**: Phase D places all n slots before the next batch's Phase B runs; per-element interleaves stats→event→push per element. Since the cache is thread-local and stats touch only shared state, the swap is not observable except through a re-entrant callback (covered by deviation 1) or a subsequent `mp_alloc`'s TLS cache warmth (identical to per-element after the loop finishes).
3. **`slab_allocated_bytes` / event order under mixed batches**: non-SLAB fallback elements are processed inline during the classification pass, so their `MP_EVENT_FREE` fires *before* the SLAB block's events. Per-element `mp_free` ordering is preserved element-by-element (each element is still fully freed before the next is visited).

`mp_alloc_batch` is unchanged (already single-lock). `mp_free` is unchanged.

### 5. Concurrency semantics

- The aggregated stats section holds `pool_wrlock` for the duration of n unlinks — strictly shorter total lock contention than n separate acquisitions.
- All slot placement paths (TLS cache, per-CPU CAS, remote-free CAS) are already lock-free and are unchanged.
- FAST_PATH pools do not maintain the active list; the atomic accounting loop preserves the existing semantics exactly.
- Non-thread-safe pools take no locks anywhere (the current behavior; `pool_lock` is a no-op there) — the batch is a pure refactor for those.

## File Changes Summary

- **Modify `src/cmem_event.c`**: rewrite `mp_free_batch` (line 2746) per the control flow above; add the `flush_slab_block` helper and a `CMEM_FREE_BATCH_CHUNK` constant (256).
- **Modify `tests/test_main.c`**: add tests with sequential numbers **starting at 42** (Tests 40/41 taken by batch-alloc):
  - THREAD_SAFE pool: batch-free N slab allocations, verify `ptrs[]` nulled, `active_allocations` → 0, `total_free_ops += N`, `mp_check_leaks` clean.
  - Equivalence: interleave batch-free and per-element `mp_free` on identical pools; assert identical final stats.
  - Mixed batch: TLSF + OS + SLAB pointers in one call — non-SLAB routed through fallback, all nulled, stats exact.
  - Corrupt header element in batch → error path invoked (magic check), remaining elements still freed.
  - Subpool-redirected element in batch → freed into its owner pool.
  - FAST_PATH pool batch (atomic accounting, no active list).
  - POISON_ON_FREE batch (poison bytes observed on freed payloads before reuse).
  - Batch that overflows the TLS cache (free > 256 same-class slots) → per-CPU / remote-free placement, stats still exact.
- **Modify `benchmarks/bench_main.c`**: extend `bench_batch_free_path` (benchmark 8d) to also report the batch-vs-single speedup at multiple thread counts (1/2/4/8) on a THREAD_SAFE pool, reusing `bench_batch_run`.

## Verification Plan

1. **Compilation & formatting**: `make format-check` clean; `make clean && make test` passes (unit + advanced + C++ PMR, ASan/UBSan); CMake build (`cmake -B build_cmake -G Ninja && cmake --build build_cmake && ctest --test-dir build_cmake`) passes; clang-tidy clean (no magic numbers; `CMEM_FREE_BATCH_CHUNK` = 256 is a power of two and exempt, but name it anyway for clarity).
2. **Unit tests**: all existing tests pass unchanged; new batch-free tests (above) pass.
3. **Correctness under sanitizers**: ASan/UBSan clean on the aggregated stats section and mixed fallback paths.
4. **Performance gate (the quantifiable goal from the analysis)**: benchmark 8d `mp_free_batch` speedup becomes ≥ 1.0x at 1 thread and improves with thread count; THREAD_SAFE 8-thread batch efficiency (`benchmark 8c` batch column) improves from 0.10 toward ≥ 0.5x — the `mp_alloc_batch` + `mp_free_batch` pair together must reach the grilling-agreed target of **8T efficiency ≥ 0.5x** vs the single-thread batch baseline.
5. **Fuzz/stress sanity**: `fuzz_alloc` runs clean; 15-second stress smoke (`gcc -D_GNU_SOURCE -DSTRESS_DURATION_SEC=15 -I./include tests/stress_test.c src/*.c -pthread -o /tmp/stress_verify && /tmp/stress_verify`).
