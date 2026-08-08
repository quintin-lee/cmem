# Per-Thread TLSF Arena Design

**Date**: 2026-08-08
**Status**: Approved
**Owner**: quintin

## 1. Problem Statement

The current TLSF allocation path requires acquiring one or more `bucket_locks` per allocation/free operation. Under high thread contention (8+ threads), these locks become a bottleneck, limiting scalability beyond ~400 Mops/sec for medium-sized objects (512B-4MB).

The slab tier already addresses this via per-thread TLS cache (`tls_cache.slots[]`) and per-CPU freelist. The TLSF tier lacks an equivalent lock-free hot path.

## 2. Solution: Per-Thread TLSF Arena

Introduce a small TLSF arena (~256KB) per thread, maintained in `thread_cache_t`. Most TLSF alloc/free operations serve from this local arena without acquiring any locks. Only when the arena is exhausted does the operation fall back to the shared pool chain.

### 2.1 Architecture

```
                    ┌─────────────────────────────────────┐
                    │         thread_cache_t              │
                    │  ┌─────────────────────────────┐    │
  mp_alloc ──────► │  │  tlsf_cache.tlsf_slots[]     │    │  ← Hit: 0 locks
                    │  │  (existing cache, 8 slots)   │    │
                    │  └─────────────────────────────┘    │
                    │  ┌─────────────────────────────┐    │
                    │  │  tlsf_arena (256KB local)    │    │  ← Hit: 0 locks
                    │  │  free list scan + coalesce   │    │
                    │  └─────────────────────────────┘    │
                    │  ┌─────────────────────────────┐    │
                    │  │  shared pool chain           │    │  ← Miss: bucket locks
                    │  │  (existing TLSF path)        │    │
                    │  └─────────────────────────────┘    │
                    └─────────────────────────────────────┘
```

### 2.2 Data Structure Changes

**`thread_cache_t` (in `include/cmem.h`)**:

```c
typedef struct {
    memory_pool_t *owner_pool;
    mp_slab_slot_t *slots[CMEM_SLAB_CLASS_COUNT];
    uint16_t counts[CMEM_SLAB_CLASS_COUNT];
    memory_pool_t *bound_arena;

    /* Per-size TLSF free-block caches. */
    tlsf_cache_entry_t *tlsf_slots[TLSF_CACHE_SIZES];
    uint8_t tlsf_counts[TLSF_CACHE_SIZES];
    tlsf_cache_entry_t tlsf_entries[TLSF_CACHE_SIZES * TLSF_CACHE_MAX_SLOTS];

    /* Per-thread TLSF arena (lock-free hot path). */
    tlsf_pool_t *tlsf_arena;              /**< Thread-local TLSF arena (NULL = not initialized) */
    tlsf_block_t  *tlsf_arena_free;       /**< Arena free region list head (address-ordered) */
    size_t        tlsf_arena_used;        /**< Bytes currently allocated from arena */
    size_t        tlsf_arena_total;       /**< Total arena capacity in bytes */
} thread_cache_t;
```

**Constants**:
```c
#define TLSF_ARENA_DEFAULT_SIZE (256 * 1024)  /**< 256 KB per thread */
#define TLSF_ARENA_MIN_SIZE   (64 * 1024)     /**< Minimum arena size */
```

### 2.3 Arena Memory Layout

The arena is a contiguous region of memory managed as a simple free-list allocator:

```
┌─────────────────────────────────────────────────────────────┐
│  tlsf_arena_free                                            │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐     │
│  │ Block A (free│→ │ Block B (free│→ │ Block C (free│ → … │
│  │  128B)       │  │  256B)       │  │  512B)       │     │
│  └──────────────┘  └──────────────┘  └──────────────┘     │
│                                                               │
│  Arena memory: [raw_arena_start ... raw_arena_end]           │
│  (allocated via sys_mem_alloc from shared pool's OS path)    │
└─────────────────────────────────────────────────────────────┘
```

**Key properties**:
- Free list is singly-linked via `tlsf_block_t::next_free`
- Blocks are maintained in address order (enables O(1) coalescing with next neighbor)
- No `prev_free` maintenance needed (coalescing only checks next_free neighbor)
- No bitmap or complex indexing (linear scan is acceptable for 256KB)

## 3. Allocation Path

Modified `mp_alloc_internal` TLSF branch (after existing TLSF cache hit check):

```c
/* Existing: TLSF cache hit */
if (cache_idx >= 0 && cache_idx < TLSF_CACHE_SIZES &&
    tls_cache.tlsf_counts[cache_idx] > 0) {
    /* ... existing cache logic ... */
}

/* NEW: Per-thread arena hit */
else if (tls_cache.tlsf_arena != NULL) {
    size_t block = tlsf_arena_find_block(tls_cache.tlsf_arena,
                                          tls_cache.tlsf_arena_free,
                                          total_needed);
    if (block) {
        /* Stamp mp_block_header_t, update tlsf_arena_used, return */
        return payload_ptr;
    }
}

/* Arena not initialized: create on first use */
if (tls_cache.tlsf_arena == NULL) {
    tlsf_arena_init(pool);
    /* Retry allocation */
    goto retry_arena;
}
```

**`tlsf_arena_find_block` algorithm**:
```
1. Scan tlsf_arena_free list for first block with size >= needed
2. If found:
   a. Remove from free list
   b. If remainder >= TLSF_MIN_BLOCK_SIZE + sizeof(tlsf_block_t):
      - Split: create new free block from remainder
      - Insert remainder into free list (maintain address order)
   c. Update tlsf_arena_used += block_size
   d. Return block pointer
3. If not found: return NULL (fall through to shared pool)
```

**Time complexity**: O(n) where n = number of free blocks in arena. With 256KB arena and minimum block size 32B, worst case is ~8192 blocks. In practice, n is much smaller due to splits and allocations.

## 4. Free Path

Modified `mp_free` TLSF branch (after existing TLSF cache hit check):

```c
/* Existing: TLSF cache hit */
if (cache_idx >= 0 && cache_idx < TLSF_CACHE_SIZES &&
    tls_cache.tlsf_counts[cache_idx] < TLSF_CACHE_MAX_SLOTS) {
    /* ... existing cache logic ... */
}

/* NEW: Per-thread arena free */
else if (block_is_in_arena(header, tls_cache.tlsf_arena)) {
    tlsf_arena_free(pool, tls_cache.tlsf_arena,
                    tls_cache.tlsf_arena_free, header);
    return;
}
```

**`tlsf_arena_free` algorithm**:
```
1. Mark block as FREE in size_and_flags
2. Update tlsf_arena_used -= block_size
3. Insert block into address-ordered free list:
   a. Scan free list to find correct insertion point (address > block address)
   b. Insert before the found position (or append at end)
4. Try coalescing:
   a. Check NEXT neighbor in free list:
      - If next block is physically adjacent (next_addr == current_addr + current_size):
        * Merge: current->size += sizeof(tlsf_block_t) + next->size
        * Remove next from free list
        * Repeat step 4a with new next neighbor (chain coalescing)
      - If NOT adjacent: stop
   b. Check PREV neighbor in free list:
      - If prev block is physically adjacent (prev_addr + prev_size == current_addr):
        * Merge: prev->size += sizeof(tlsf_block_t) + current->size
        * Remove current from free list (it's now part of prev)
        * Return (merged block is now in prev's position)
      - If NOT adjacent: stop
5. Return
```

**Invariants**:
- Free list is always sorted by block address (ascending)
- No two adjacent free blocks exist in the list (guaranteed by coalescing)
- `tlsf_arena_used` is accurate (sum of all allocated block sizes)

**Arena full detection**:
- After free, if `tlsf_arena_used == 0` and `tlsf_arena_free` has only one block of full size, the arena is effectively empty. This is a normal state, not an error.

## 5. Drain & Cleanup Strategy

### 5.1 Pool Switch Drain

Triggered in `tls_cache_validate_owner` when `owner_pool` changes:

```c
void tls_cache_validate_owner(memory_pool_t *pool) {
    if (tls_cache.owner_pool == pool) return;

    /* Drain per-thread TLSF arena before switching pool */
    if (tls_cache.tlsf_arena != NULL) {
        tlsf_arena_drain(tls_cache.owner_pool);
        tlsf_arena_destroy();
    }

    /* Existing flush logic for tls_cache.tlsf_slots[] */
    if (tls_cache.owner_pool != NULL) {
        tls_cache_flush_pool(tls_cache.owner_pool);
    }

    tls_cache.owner_pool = pool;
    /* ... existing bound_arena logic ... */
}
```

**`tlsf_arena_drain` algorithm**:
```
1. For each block in tlsf_arena_free list:
   a. Mark block as FREE
   b. Call tlsf_insert_free_block(shared_tpool, block)
      (this may trigger coalescing with existing shared pool blocks)
2. Clear free list: tlsf_arena_free = NULL
3. Reset used counter: tlsf_arena_used = 0
```

### 5.2 Thread Exit Drain

Triggered in `tls_cache_dtor` (pthread key destructor):

```c
static void tls_cache_dtor(void *arg) {
    if (tls_cache.tlsf_arena != NULL) {
        tlsf_arena_drain(tls_cache.owner_pool);
        tlsf_arena_destroy();
    }
    if (tls_cache.owner_pool) {
        tls_cache_flush_pool(tls_cache.owner_pool);
    }
}
```

### 5.3 Arena Memory Management

**Arena allocation**:
- On first use, allocate arena memory via `sys_mem_alloc` (the existing OS-level allocator):
  ```c
  tls_cache.tlsf_arena_raw_mem = sys_mem_alloc(pool, TLSF_ARENA_DEFAULT_SIZE,
                                                TLSF_ALIGN_MASK + 1);
  if (!tls_cache.tlsf_arena_raw_mem) return NULL;
  tls_cache.tlsf_arena = (tlsf_pool_t *)tls_cache.tlsf_arena_raw_mem;
  tls_cache.tlsf_arena_free = (tlsf_block_t *)
      ((uint8_t *)tls_cache.tlsf_arena + sizeof(tlsf_pool_t));
  tls_cache.tlsf_arena_free->size_and_flags =
      TLSF_ARENA_DEFAULT_SIZE - sizeof(tlsf_pool_t) - sizeof(tlsf_block_t) | BLOCK_STATE_FREE;
  tls_cache.tlsf_arena_free->prev_physical = NULL;
  tls_cache.tlsf_arena_free->next_free = NULL;
  tls_cache.tlsf_arena_used = 0;
  tls_cache.tlsf_arena_total = TLSF_ARENA_DEFAULT_SIZE - sizeof(tlsf_pool_t);
  ```
- Arena memory is NOT part of any `tlsf_pool` — it's a standalone region managed directly by the thread cache.

**Arena deallocation**:
- Called during drain (pool switch or thread exit)
- Use `sys_mem_free(pool, tls_cache.tlsf_arena_raw_mem, TLSF_ARENA_DEFAULT_SIZE)` to return to OS

## 6. Integration with Existing Code

### 6.1 Files to Modify

| File | Change | Lines (approx) |
|------|--------|----------------|
| `include/cmem.h` | Add arena fields to `thread_cache_t` | +5 |
| `include/cmem.h` | Add `TLSF_ARENA_DEFAULT_SIZE`, `TLSF_ARENA_MIN_SIZE` constants | +2 |
| `src/cmem_slab.c` | Add `tlsf_arena_find_block()`, `tlsf_arena_free()`, `tlsf_arena_drain()`, `tlsf_arena_destroy()`, `tlsf_arena_init()` | +120 |
| `src/cmem_event.c` | Modify `mp_alloc_internal` TLSF branch | +15 |
| `src/cmem_event.c` | Modify `mp_free` TLSF branch | +10 |
| `src/cmem_event.c` | Modify `tls_cache_validate_owner` (in slab.c, called from event.c) | +8 |
| `src/cmem_event.c` | Modify `tls_cache_dtor` | +5 |
| `src/cmem_sys.c` | Add `sys_mem_alloc()`, `sys_mem_free()` wrappers for arena memory | +20 |

### 6.2 Files NOT Modified

- `src/cmem_tlsf.c` — Shared pool path unchanged
- `src/cmem.c` — No changes
- `src/cmem_compress.c` — No changes
- `src/cmem_diag.c` — No changes
- `tests/` — Add new benchmark, not new unit tests (behavior is identical)

### 6.3 Public API Impact

**None.** All changes are internal to `thread_cache_t` (a private structure). The public API (`mp_alloc`, `mp_free`, etc.) remains unchanged.

## 7. Expected Performance Impact

| Metric | Current | Expected | Notes |
|--------|---------|----------|-------|
| TLSF alloc QPS (1 thread) | ~400 Mops/sec | ~450 Mops/sec | Small improvement (cache hit still dominates) |
| TLSF alloc QPS (8 threads) | ~200 Mops/sec | ~600+ Mops/sec | Major improvement (eliminated lock contention) |
| TLSF alloc P99 latency | ~2μs | ~0.5μs | Eliminated lock wait tail latency |
| Memory overhead | 0 | ~2MB (16 threads × 256KB) | Acceptable for high-concurrency workloads |

## 8. Risk Assessment

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| Arena fragmentation | Medium | Linear scan with best-fit search; coalescing on free |
| Drain correctness | Low | Reuse existing `tlsf_insert_free_block` for shared pool insertion |
| Thread exit cleanup | Low | Existing `pthread_key` destructor pattern; add arena drain |
| Multi-pool correctness | Medium | Arena is pool-affiliated; drain on pool switch |
| Windows compatibility | Low | Same pattern as existing `tls_cache` (guard with `#ifndef _WIN32`) |

## 9. Out of Scope

- Per-thread arena for Slab tier (already handled by `tls_cache.slots[]`)
- Arena compaction (would require complex region management)
- Cross-thread block stealing (complexity outweighs benefit)
- Dynamic arena sizing based on workload (static 256KB is sufficient)

## 10. Implementation Plan

See corresponding implementation plan document.
