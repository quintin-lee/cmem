# Spec: Single-Lock Batch Allocation (mp_alloc_batch) Optimization for cmem

## Overview

Refactor `mp_alloc_batch` in `src/cmem_event.c` so a batch of N same-sized allocations acquires each lock at most once per batch instead of once per element. Slab-backed requests additionally pull up to N slots in a single critical section (new `slab_alloc_batch` primitive in `src/cmem_slab.c`), and the shared stats/active-list update is aggregated into one critical section. All pool configurations (cache-aligned, arena routing, memory limit, OOM fallback, emergency buffer, per-CPU freelist, fast path) are supported inside the batch fast path with observable behavior matching N consecutive `mp_alloc` calls, except for four explicitly documented deviations (Section 4).

## Detailed Requirements

### 1. New internal primitive `slab_alloc_batch` (`src/cmem_slab.c`, declared in `src/cmem_internal.h`)

```c
size_t slab_alloc_batch(memory_pool_t *pool, uint8_t class_idx,
                        mp_slab_slot_t **out_slots, size_t max_count);
```

- Acquires the slab class lock **once** (only when `MP_FLAG_THREAD_SAFE`), runs `remote_free_harvest` once, then pops up to `max_count` slots in a loop from the `partial_pages` `free_list`, reusing the page transition/create logic of `tls_cache_refill` (bulk-pop template), writing slots into `out_slots[]` instead of the TLS cache.
- Page lifecycle transitions (partial → full, empty → partial/empty reuse, `slab_create_page` when needed) are preserved exactly as in `tls_cache_refill`.
- Accumulates `stats.slab_allocated_bytes += n * slot_size`; unlocks; returns the number of slots actually produced (`0..max_count`).
- Prefix semantics: if the pool is exhausted partway, the prefix produced is returned.

### 2. Rewritten `mp_alloc_batch` (`src/cmem_event.c:2269` → new body)

Control flow, in order:

1. **Validation**: `pool`, `out_ptrs`, `count > 0`, `size > 0` (returns 0 on invalid, matching current behavior).
2. **Arena routing** (once, not per element): if `num_arenas > 0`, resolve `mp_get_thread_bound_arena(pool)`; if non-NULL and different from `pool`, recurse into `mp_alloc_batch` on that arena. All subsequent steps operate on the arena-resolved pool.
3. **Entry checks** (once, on the arena-resolved pool — mirroring per-element ordering where `mp_alloc_internal`'s checks run after routing): if `pool->dirty && !fallback_to_sys_alloc_on_oom`, return 0; if `circuit_breaker_enabled && pool->circuit_breaker_tripped`, return 0 (`src/cmem_event.c:1973-1977`).
4. **Memory limit, fully exceeded** (once). Let `total_size` = user `size` for non-aligned pools, or `size + 64 + header + canary` for `MP_FLAG_CACHE_ALIGNED` (matching per-element accounting). Under `pool_rdlock`, if `active_bytes + total_size > limit` (i.e., even one element does not fit):
   - If `fallback_to_sys_alloc_on_oom`: fire `MP_EVENT_OOM` **once per element** (i.e. `count` times — per-element fires it for every independently exceeding element at `src/cmem_event.c:1996`), then proceed to allocate **all `count`** elements via the normal paths (per-element semantics: fallback ignores the limit and continues).
   - Else: fire `MP_EVENT_OOM` once (with the internal request as the size argument), then attempt the single-use emergency buffer (if available and `internal_size + header + canary <= emergency_size`, where `internal_size` = `total_size`, the internal request — matching per-element which checks the internal request at `src/cmem_event.c:1998`): if it fits, fire `MP_EVENT_OOM` **once more** (per-element fires it again when the loop continues to the next element, which exceeds again and is rejected at `src/cmem_event.c:1996`), and return 1 (the emergency element); otherwise return 0.
5. **Memory limit, partially fitting** (`active_bytes + total_size <= limit` but `k = (limit - active_bytes) / total_size < count`):
   - Allocate `k` elements via the paths below (prefix).
   - If the paths produce fewer than `k` elements (say `j`), stop and return `j` immediately — **without** firing OOM and **without** touching the emergency buffer (matching per-element, where physical exhaustion returns NULL before any limit logic runs).
   - If exactly `k` were produced and `k < count`:
     - If `fallback_to_sys_alloc_on_oom` is set: fire `MP_EVENT_OOM` **once per exceeding element** (i.e. `count - k` times, per-element parity) and continue allocating the remaining `count - k` elements via the normal paths (per-element fallback-continue); the remainder also follows prefix semantics — a physical failure partway stops the batch, as per-element's loop-stops-on-NULL does.
     - Else: fire `MP_EVENT_OOM` once (the element that would exceed, `k`), then attempt the emergency buffer once (same conditions as step 4); if it fits, allocate the emergency element, fire `MP_EVENT_OOM` **once more** (per-element fires it again for the follow-up rejected element), and return `k + 1`; else return `k`.
   - In all cases the result is a contiguous prefix in `out_ptrs[0..n-1]` where `n` is the returned count.
6. **Cache-aligned** (`MP_FLAG_CACHE_ALIGNED`): internally request `total_size` per element through the same batch paths, then align each returned payload to 64, relocate the header by copy (`*new_header = *orig_header`, preserving `raw_base = original slot`), and relink into the active list under the single stats critical section — replicating `mp_aligned_alloc` (`src/cmem_event.c:2616`) per element. `total_size` is used for limit accounting, `active_bytes`, and the histogram bucket; user `size` is used for the circuit-breaker quota (matching per-element). The emergency element produced by steps 4/5 is also aligned to 64 and its header relocated/relinked before it is stored at `out_ptrs[k]` (per-element `mp_aligned_alloc` applies alignment to whatever `mp_alloc_internal` returns, including the emergency buffer).
7. **Slab path** (`size <= SLAB_MAX_SIZE` and not `STATIC_BUFFER`):
   - Call `tls_cache_validate_owner(pool)` **once** before Phase A (its flush-on-owner-change runs if the calling thread changed).
   - Phase A (lock-free): pop `min(need, tls_cache.counts[class_idx])` slots from the TLS cache.
   - Phase B (lock-free, per-CPU tier): if the pool uses a per-CPU freelist (`MP_FLAG_PERCPU_FREELIST`), pop from the per-CPU freelist until miss or `need` met, using the same pop/refill logic as the per-element tier (`percpu_pop`/`percpu_refill`, `src/cmem_event.c:2066-2073`).
   - Phase C: for the remainder, call `slab_alloc_batch` (one class lock per batch).
   - Phase D (lock-free): format the block header per element exactly as `mp_alloc_internal` does (`alloc_type=ALLOC_TYPE_SLAB`, `slab_class`, `requested_size`, `usable_size`, `raw_base`, `subpool`, `magic` unless `MP_FLAG_FAST_PATH`, canary byte, zero-fill per flags).
8. **TLSF path** (`size <= TLSF_MAX_SIZE` or `STATIC_BUFFER`): one `pool_lock`; inside it loop `tlsf_alloc(pool, total_size)` × need (it does not lock internally); `pool_unlock`.
9. **OS path** (`size > TLSF_MAX_SIZE`): loop `sys_mem_alloc` per element (no pool lock involved); format headers; update per element `os_allocated_bytes += size` (user size) and `total_pool_size += size + header + canary` (internal OS total) — the same fields and values as the per-element OS branch (`src/cmem_event.c:2154-2155`).
10. **Aggregated stats critical section** (one `pool_lock` when `MP_FLAG_THREAD_SAFE`): for all n elements — `active_list_add` unless `MP_FLAG_FAST_PATH`, `active_bytes += n*size` (or `n*total_size` for aligned), peak update, `active_allocations += n`, `total_alloc_ops += n`, `size_histogram[get_slab_class_index(total_size)] += n` (guarded by `bucket < CMEM_HISTOGRAM_BUCKETS` as per-element does at `src/cmem_event.c:2179`, and **excluding any emergency element** — per-element's emergency path never updates the histogram); `pool_unlock`.
11. **Post-unlock callbacks** (NEVER inside the lock — `pool_lock` is a non-recursive `pthread_rwlock_wrlock` and a re-entrant callback would self-deadlock): after the stats section unlocks, collect the `n` `(ptr, size)` pairs and fire per-element `trigger_event(MP_EVENT_ALLOC)` in order for the **non-emergency** elements (per-element's emergency path returns before the event block at `src/cmem_event.c:2045` and fires no event), then run `check_watermark_after_change` once — matching the current post-`pool_unlock` placement (`src/cmem_event.c:2217-2222`).
12. **Circuit breaker**: accumulate `thread_quota.alloc_bytes += size` (user size) per element *within* the batch; when the quota crosses the trip threshold, set `pool->circuit_breaker_tripped = true` under `pool_lock` and stop the batch, returning the current prefix count (matching per-element behavior, where the next call sees `circuit_breaker_tripped` and returns NULL).

### 3. Lock overhead per batch (THREAD_SAFE pool, N elements)

| Path | Before | After |
|---|---|---|
| Slab, TLS cache hit | N × pool_lock | 0 locks + 1 pool_lock^†^ |
| Slab, TLS cache miss | N × (class_lock + pool_lock) | 1 class_lock + 1 pool_lock |
| TLSF | 2N pool_lock | 2 pool_lock |
| OS | N × pool_lock (stats section) | 1 pool_lock (stats section) |

^†^ "0 locks" holds only when the TLS cache fully satisfies `need`; a partial cache hit still pays Phase C's one class lock (and Phase B is lock-free per-CPU).

### 4. Behavior preservation and documented deviations

Behavior identical to N × mp_alloc for: prefix semantics, per-element `MP_EVENT_ALLOC` events (fired after unlock, in order), `MP_EVENT_OOM` on limit breach, emergency buffer single-use, cache-aligned relocation, arena routing, canary/zero-fill/poison handling, `MP_FLAG_FAST_PATH` header minimization, dirty check, and circuit-breaker trip semantics.

**Documented deviations** (intentional, must be covered by tests):
1. **TLS cache warmth**: a batch drains the TLS cache (Phase A) and pops the remainder directly into `out_slots[]` (Phase C) without refilling the cache. A subsequent single `mp_alloc` may hit the class lock once more before the cache is warm again. No functional change — only a warm-cache heuristic difference. Under high batch traffic, `tls_cache_refill`'s per-element refill benefit is replaced by the direct bulk pop.
2. **`slab_allocated_bytes` accounting**: per-element counts refilled-but-cached slots (refill-time accounting); the batch counts only delivered slots (`n * slot_size`). This is a public `mp_stats_t` field (`cmem.h:211`); the batch value is the number of bytes actually handed out.
3. **Deferred event observation**: a *re-entrant* `MP_EVENT_ALLOC` callback observes `active_allocations == n` (batch) instead of `i+1` (per-element), because events are deferred until after the whole batch allocates. Event order is preserved.
4. **Watermark check frequency**: `check_watermark_after_change` runs once per batch (after the final element) instead of after every element; threshold-crossing detection is preserved (the check is threshold-based, not per-alloc).

`mp_free_batch` is NOT in scope (remains a loop over `mp_free`).

### 5. Concurrency semantics

- The limit check (step 4/5) is a snapshot read of `active_bytes` under `pool_rdlock`. Under concurrent THREAD_SAFE allocation, a batch may admit slightly more than the strict per-element re-check would. This matches the existing check-then-act race of per-element allocation (documented, not a regression).

## File Changes Summary

- **Modify `src/cmem_slab.c`**: add `slab_alloc_batch` (bulk-pop under one class lock).
- **Modify `src/cmem_internal.h`**: declare `slab_alloc_batch` (near line 547).
- **Modify `src/cmem_event.c`**: rewrite `mp_alloc_batch` (line 2269) per the control flow above.
- **Modify `tests/test_main.c`**: keep Test 9; add new tests with sequential numbers **starting at 40** (Tests 38 and 39 are taken):
  - THREAD_SAFE pool, slab sizes (8/64/512), full batch success + `active_allocations` accounting.
  - TLSF sizes (1024, 4096, 1 MiB) and OS size (> 4 MiB) batches + `os_allocated_bytes` accounting.
  - Partial failure / prefix truncation (exhaust pool mid-batch).
  - `MP_FLAG_CACHE_ALIGNED` batch alignment + accounting.
  - `max_memory_limit` partial-fit → `k` prefix; emergency-buffer single element (`k+1`); full-limit-exceeded → 1 or 0; OOM-fallback → full count.
  - `MP_FLAG_FAST_PATH` batch (no active list).
  - Arena routing (pool with `num_arenas > 0`).
  - Per-CPU freelist pool batch.
  - Circuit-breaker trip mid-batch → prefix truncation.
- **Modify `benchmarks/bench_main.c`**: extend batch benchmark (line 581) to also cover THREAD_SAFE pool and TLSF-class sizes; report throughput vs. per-element loop baseline.

## Verification Plan

1. **Compilation & formatting**: `make format-check` clean; `make clean && make test` passes (unit + advanced + C++ PMR, ASan/UBSan); CMake build (`cmake -B build_cmake -G Ninja && cmake --build build_cmake && ctest --test-dir build_cmake`) passes; clang-tidy clean.
2. **Unit tests**: all existing tests pass unchanged; new batch tests (above) pass.
3. **Correctness under sanitizers**: ASan/UBSan clean on new batch paths (cache-aligned relocation, TLSF loop, prefix truncation, emergency/limit paths).
4. **Performance**: `./build/benchmark` batch throughput improves over the per-element loop baseline for THREAD_SAFE slab/TLSF batches; no regression for OS path or non-batch paths.
5. **Fuzz/stress sanity**: `fuzz_alloc` (uses `mp_alloc_batch` with size 16) runs clean; 15-second stress smoke (`gcc -DSTRESS_DURATION_SEC=15 ... tests/stress_test.c src/*.c -pthread -o /tmp/stress_verify && /tmp/stress_verify`).
