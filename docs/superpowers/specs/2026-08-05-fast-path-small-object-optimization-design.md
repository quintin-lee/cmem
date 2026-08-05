# Spec: Fast Path Small Object Allocation Optimization for cmem

## Overview

This specification defines the high-performance inline fast path optimization for small object allocations ($\le 512$ bytes) in `cmem`. By exposing inline allocation and free primitives (`mp_alloc_fast`, `mp_free_fast`) in `include/cmem.h`, bypassing global lock acquisitions on thread-local cache frees, using an $O(1)$ size-to-class lookup array, and minimizing block header initialization overhead under `MP_FLAG_FAST_PATH`, small allocation throughput is significantly accelerated.

---

## Detailed Requirements

### 1. Header-Level Inline Fast-Path Primitives
- In `include/cmem.h`, expose the layout for thread-local slab cache structures (`cmem_tls_cache`) and header layouts required for inline fast paths.
- Define `static inline void *mp_alloc_fast(memory_pool_t *pool, size_t size)`:
  - Check if `pool` is non-NULL, has `MP_FLAG_FAST_PATH` set, and `size > 0 && size <= 512`.
  - Use `cmem_size_to_class[size]` for $O(1)$ direct slab class indexing.
  - Check if `cmem_tls_cache.counts[class_idx] > 0`.
  - Pop slot from `cmem_tls_cache`, initialize essential block header fields (`alloc_type`, `slab_class`, `raw_base`, `subpool`), and return usable payload pointer.
  - Fall back to `mp_alloc(pool, size)` on cache miss or non-fast-path requests.
- Define `static inline void mp_free_fast(memory_pool_t *pool, void *ptr)`:
  - Check if `ptr` is non-NULL and `pool` has `MP_FLAG_FAST_PATH` set.
  - Extract `mp_block_header_t` from `(uint8_t *)ptr - sizeof(mp_block_header_t)`.
  - If `alloc_type == ALLOC_TYPE_SLAB` and `cmem_tls_cache.counts[class_idx] < TLS_CACHE_MAX_SLOTS`:
    - Push slot back to `cmem_tls_cache` without acquiring any global pool mutex locks.
    - Return immediately.
  - Fall back to `mp_free(pool, ptr)` on cache overflow or non-fast-path frees.

### 2. Lock-Free TLS Cache Freeing in C Engine (`src/cmem_event.c`)
- In `mp_free()` within `src/cmem_event.c`, remove mutex lock acquisitions (`pool_lock(pool)`) when releasing a slab slot back into the thread-local cache under `MP_FLAG_FAST_PATH`.
- Maintain atomic counter updates (`__atomic_fetch_sub`) for stats if needed, avoiding blocking mutex synchronization on thread-local frees.

### 3. Direct Size-to-Class Lookup Array (`cmem_size_to_class`)
- Replace branch-heavy `get_slab_class_index(pool, size)` calculations for small sizes ($\le 512$B) with a static 513-element lookup array (`cmem_size_to_class[513]`).
- Array is pre-computed at library initialization (or static constant array) mapping exact byte counts `1..512` to their respective slab class index.

### 4. Minimal Block Header Initialization under `MP_FLAG_FAST_PATH`
- When `MP_FLAG_FAST_PATH` is enabled, skip writing `magic`, `alloc_file`, `alloc_line`, `alloc_func`, `prev`, `next`, and backtrace metadata in `mp_block_header_t`.
- Only populate fields strictly necessary for deallocation and pool attribution: `alloc_type`, `slab_class`, `raw_base`, `subpool`.

---

## File Changes Summary

- **Modify `include/cmem.h`**:
  - Expose `mp_slab_slot_t`, `mp_block_header_t` (if needed), `cmem_tls_cache`, `cmem_size_to_class`.
  - Add `mp_alloc_fast` and `mp_free_fast` inline helper implementations.
- **Modify `src/cmem.c` / `src/cmem_slab.c` / `src/cmem_event.c`**:
  - Export/initialize `cmem_size_to_class` lookup table.
  - Update `mp_free()` to eliminate mutex locking on TLS cache hits under `MP_FLAG_FAST_PATH`.
  - Align `mp_alloc_internal` with minimal header initialization when fast-path is active.
- **Modify `benchmarks/bench_main.c`**:
  - Update Benchmark 6 (`bench_fast_path`) to evaluate `mp_alloc_fast` / `mp_free_fast` performance.

---

## Verification Plan

1. **Compilation & Formatting**:
   - `make format-check` passes with zero warnings/errors.
   - Both Makefile (`make bench`) and CMake (`cmake -B build_cmake -G Ninja && cmake --build build_cmake`) build cleanly.
2. **Unit Tests & Integration**:
   - `ctest --test-dir build_cmake` passes 100% (C unit tests, advanced unit tests, C++ PMR tests).
3. **Performance Benchmarking**:
   - Run `./build/benchmark`.
   - Verify small allocation throughput (`mp_alloc_fast` / `MP_FLAG_FAST_PATH`) achieves significant speedup compared to baseline.
