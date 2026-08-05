# Spec: Single-Lock Batch Allocation (mp_alloc_batch) Optimization for cmem

## Overview

Refactor `mp_alloc_batch` in `src/cmem_event.c` so a batch of N same-sized allocations acquires each lock at most once per batch instead of once per element. Slab-backed requests additionally pull up to N slots in a single critical section (new `slab_alloc_batch` primitive in `src/cmem_slab.c`), and the shared stats/active-list update is aggregated into one critical section. All pool configurations (cache-aligned, arena routing, memory limit, OOM fallback, emergency buffer, fast path) are supported inside the batch fast path with behavior identical to N consecutive `mp_alloc` calls.

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

### 2. Rewritten `mp_alloc_batch` (`src/cmem_event.c:2227`)

Control flow, in order:

1. **Validation**: `pool`, `out_ptrs`, `count > 0`, `size > 0` (returns 0 on invalid, matching current behavior).
2. **Arena routing** (once, not per element): if `num_arenas > 0`, resolve `mp_get_thread_bound_arena(pool)`; if non-NULL and different from `pool`, recurse into `mp_alloc_batch` on that arena.
3. **Memory limit** (once): if `max_memory_limit > 0`, read `active_bytes` under `pool_rdlock` and compute `k = min(count, (limit - active_bytes) / size)`:
   - `k == 0`: if `fallback_to_sys_alloc_on_oom`, trigger one `MP_EVENT_OOM` and continue as if unlimited (current per-element semantics: fallback ignores the limit); else use the single-use emergency buffer path (at most one element) and return 0 or 1.
   - `k > 0`: allocate exactly `k` elements via the paths below; the remaining elements are not attempted (prefix result).
4. **Cache-aligned** (`MP_FLAG_CACHE_ALIGNED`): internally request `size + 64 + header + canary` per element through the same batch paths, then align each returned payload to 64, relocate the header by copy (`*new_header = *orig_header`, preserving `raw_base = original slot`), and relink into the active list under the single stats critical section — replicating `mp_aligned_alloc` (`src/cmem_event.c:2561`) per element.
5. **Slab path** (`size <= SLAB_MAX_SIZE` and not `STATIC_BUFFER`):
   - Validate TLS cache owner once.
   - Phase A (lock-free): pop `min(need, tls_cache.counts[class_idx])` slots from the TLS cache.
   - Phase B: for the remainder, call `slab_alloc_batch` (one class lock per batch).
   - Phase C (lock-free): format the block header per element exactly as `mp_alloc_internal` does (`alloc_type=ALLOC_TYPE_SLAB`, `slab_class`, `requested_size`, `usable_size`, `raw_base`, `subpool`, `magic` unless `MP_FLAG_FAST_PATH`, canary byte, zero-fill per flags).
6. **TLSF path** (`size <= TLSF_MAX_SIZE` or `STATIC_BUFFER`): one `pool_lock`; inside it loop `tlsf_alloc(pool, size)` × need (it does not lock internally); `pool_unlock`.
7. **OS path** (`size > TLSF_MAX_SIZE`): loop `sys_mem_alloc` per element (no pool lock involved); format headers.
8. **Aggregated stats critical section** (one `pool_lock` when `MP_FLAG_THREAD_SAFE`): for all n elements — `active_list_add` unless `MP_FLAG_FAST_PATH`, `active_bytes += n*size`, peak update, `active_allocations += n`, `total_alloc_ops += n`, `size_histogram[bucket] += n`, watermark callback check once, per-element `trigger_event(MP_EVENT_ALLOC)` (preserves per-element event semantics); `pool_unlock`.
9. **Circuit breaker**: accumulate `thread_quota.alloc_bytes += n*size` once, apply the same trip check as `mp_alloc`.

### 3. Lock overhead per batch (THREAD_SAFE pool, N elements)

| Path | Before | After |
|---|---|---|
| Slab, TLS cache hit | N × pool_lock | 0 locks + 1 pool_lock |
| Slab, TLS cache miss | N × (class_lock + pool_lock) | 1 class_lock + 1 pool_lock |
| TLSF | 2N pool_lock | 1 pool_lock |
| OS | 0 (no locks) | 0 (no locks) |

### 4. Behavior preservation (identical to N × mp_alloc)

- Prefix semantics: returns count of actually allocated elements; stops at first failure.
- Per-element `MP_EVENT_ALLOC` events; `MP_EVENT_OOM` on limit breach.
- Canary / zero-fill / poison handling, `MP_FLAG_FAST_PATH` header minimization, emergency buffer, dirty check and circuit-breaker trip semantics unchanged.
- `mp_free_batch` is NOT in scope (remains a loop over `mp_free`).

## File Changes Summary

- **Modify `src/cmem_slab.c`**: add `slab_alloc_batch` (bulk-pop under one class lock).
- **Modify `src/cmem_internal.h`**: declare `slab_alloc_batch` (near line 547).
- **Modify `src/cmem_event.c`**: rewrite `mp_alloc_batch` (line 2227) per the control flow above.
- **Modify `tests/test_main.c`**: keep Test 9; add new tests (sequential test numbers, no collision with Test 38):
  - THREAD_SAFE pool, slab sizes (e.g. 8/64/512), full batch success + `active_allocations` accounting.
  - TLSF sizes (e.g. 1024, 4096, 1 MiB) and OS size (> 4 MiB) batches.
  - Partial failure / prefix truncation (exhaust pool mid-batch).
  - `MP_FLAG_CACHE_ALIGNED` batch alignment.
  - `max_memory_limit` + emergency buffer batch; OOM fallback batch.
  - `MP_FLAG_FAST_PATH` batch (no active list).
  - Arena routing (pool with `num_arenas > 0`).
- **Modify `benchmarks/bench_main.c`**: extend batch benchmark (line 581) to also cover THREAD_SAFE pool and TLSF-class sizes; report throughput vs. per-element loop baseline.

## Verification Plan

1. **Compilation & formatting**: `make format-check` clean; `make clean && make test` passes (unit + advanced + C++ PMR, ASan/UBSan); CMake build (`cmake -B build_cmake -G Ninja && cmake --build build_cmake && ctest --test-dir build_cmake`) passes; clang-tidy clean.
2. **Unit tests**: all existing tests pass unchanged; new batch tests (above) pass.
3. **Correctness under sanitizers**: ASan/UBSan clean on new batch paths (cache-aligned relocation, TLSF loop, prefix truncation).
4. **Performance**: `./build/benchmark` batch throughput improves over the per-element loop baseline for THREAD_SAFE slab/TLSF batches; no regression for OS path or non-batch paths.
5. **Fuzz/stress sanity**: `fuzz_alloc` (uses `mp_alloc_batch` with size 16) runs clean; 15-second stress smoke (`gcc -DSTRESS_DURATION_SEC=15 ... tests/stress_test.c src/*.c -pthread -o /tmp/stress_verify && /tmp/stress_verify`).
