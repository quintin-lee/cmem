# Spec: Per-Thread TLSF Sub-Pool for Performance Optimization

## Overview

Add per-thread TLSF sub-pools to the existing multi-arena architecture so that TLSF allocations (512B–4MB) can be served lock-free from thread-local sub-pools for the common case, with graceful fallback to the shared pool under contention or when the sub-pool is exhausted. This targets the benchmark-8 TLSF batch speedup (currently 1.16×) and the single-thread cmem-vs-malloc gap (7.58 vs 61.80 Mops/sec).

## Problem Statement

Current per-thread benchmark data:

| Path | Single Mops | Batch Mops | Speedup |
|---|---|---|---|
| Slab 32B | 7.90 | 44.87 | 5.68× |
| TLSF 4KB | 10.10 | 12.06 | 1.16× |
| TLSF TS 4KB | 2.23 | 43.54 | 19.56× |
| Multi-arena 8T | — | — | 1.39× eff |

Root causes:
1. **TLSF batch holds `pool_lock` for the entire loop** — all N tlsf_alloc calls run sequentially under one write lock. Slab batch bypasses the lock entirely for TLS cache + per-CPU free-list.
2. **Single-thread cmem < malloc** — `mp_alloc_internal` holds `pool_lock` around every TLSF allocation (even single-thread pools use `pool_lock`/`pool_unlock` pair in the `MP_FLAG_THREAD_SAFE` path).
3. **Per-thread TLSF sub-pools are unused** — the multi-arena mechanism binds slab threads to arenas but TLSF still routes through the shared `tlsf_root`.

## Design

### 1. Sub-Pool Selection (TLS Cache Reuse)

Re-use `tls_cache.bound_arena`: when `mp_enable_multi_arena` is called on the root pool, each child arena gets its own `tlsf_root`. The existing `mp_get_thread_bound_arena()` already returns the bound child for the current thread. We extend it so TLSF allocations in `mp_alloc_internal` use the bound arena directly, bypassing the pool lock.

**Key invariant**: each child arena created by `mp_enable_multi_arena` is a full `memory_pool_t` with its own `tlsf_root`, slab classes, and lock. TLSF allocations from a bound arena never touch the master pool's `tlsf_root`.

### 2. Fast TLSF Path in `mp_alloc_internal`

**Before (current)**:
```c
if (size <= TLSF_MAX_SIZE) {
    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pool_lock(pool);
        ptr = tlsf_alloc(pool, size);
        pool_unlock(pool);
    } else {
        ptr = tlsf_alloc(pool, size);
    }
}
```

**After (proposed)**:
```c
if (size <= TLSF_MAX_SIZE) {
    /* Fast path: use bound arena's tlsf_root directly (no lock) */
    if (pool->num_arenas > 0 && tls_cache.bound_arena) {
        memory_pool_t *arena = tls_cache.bound_arena;
        ptr = tlsf_alloc(arena, size);
    } else if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pool_lock(pool);
        ptr = tlsf_alloc(pool, size);
        pool_unlock(pool);
    } else {
        ptr = tlsf_alloc(pool, size);
    }
}
```

**Rationale**: The bound arena's `tlsf_root` is private to that arena (created with `mp_create_child`). No other thread accesses it, so no lock is needed. This path is lock-free for the common multi-arena case.

### 3. TLSF Batch Optimization

**Before (current)**:
```c
pool_lock(pool);
while (produced < target && !breaker_tripped) {
    ptr = tlsf_alloc(pool, total_size);
    if (!ptr) break;
    out_ptrs[produced++] = ptr;
    if (batch_breaker_accrue(pool, size)) { ... break; }
}
pool_unlock(pool);
```

**After (proposed)**:
```c
/* Same lock-free fast path as single alloc when bound arena exists */
if (pool->num_arenas > 0 && tls_cache.bound_arena) {
    memory_pool_t *arena = tls_cache.bound_arena;
    while (produced < target && !breaker_tripped) {
        void *ptr = tlsf_alloc(arena, total_size);
        if (!ptr) break;
        out_ptrs[produced++] = ptr;
        if (batch_breaker_accrue(pool, size)) { ... }
    }
} else if (pool->flags & MP_FLAG_THREAD_SAFE) {
    pool_lock(pool);
    while (produced < target && !breaker_tripped) {
        void *ptr = tlsf_alloc(pool, total_size);
        if (!ptr) break;
        out_ptrs[produced++] = ptr;
        if (batch_breaker_accrue(pool, size)) { ... }
    }
    pool_unlock(pool);
} else {
    while (produced < target && !breaker_tripped) {
        void *ptr = tlsf_alloc(pool, total_size);
        if (!ptr) break;
        out_ptrs[produced++] = ptr;
        if (batch_breaker_accrue(pool, size)) { ... }
    }
}
```

### 4. Fast-Path `mp_alloc` (non-batch, non-TLSF)

For single-threaded non-TLSF allocations (slab path), the bottleneck is `remote_free_harvest_all(pool)` which acquires per-class locks. When the pool is NOT `MP_FLAG_THREAD_SAFE`, we can skip this call since there's no concurrent remote-free.

**Change in `mp_alloc_internal`** (around line 2055):
```c
if ((pool->flags & MP_FLAG_STATIC_BUFFER) == 0 && size <= SLAB_MAX_SIZE) {
    tls_cache_validate_owner(pool);
    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        remote_free_harvest_all(pool);  /* only needed for THREAD_SAFE */
    }
    ...
}
```

### 5. No Structural Changes to tlsf_pool_t

The sub-pool is the child `memory_pool_t` itself — each child has its own `tlsf_root`. No new fields needed in `tlsf_pool_t` or `memory_pool_t`. The existing `is_multi_arena_child` flag is sufficient to identify sub-pools.

### 6. Correctness Guarantees

- **Free path**: `mp_free` already follows `header->subpool` for TLSF (line 2918: `tlsf_pool_t *tpool = (tlsf_pool_t *)header->subpool; if (tpool->owner_pool) pool = tpool->owner_pool;`). Since `tlsf_alloc` stamps `header->subpool = tpool` (the child arena's tlsf pool), frees route back to the correct arena. No changes needed.
- **Stats**: Each child arena maintains its own `pool->stats`. The benchmark's `mp_get_stats(pool)` reads from the master pool, which aggregates child stats via `mp_dump_stats` only when explicitly queried. For single-pool benchmarks (no `MP_FLAG_MULTI_ARENA`), behavior is unchanged.
- **OOM / limits**: The memory limit check in `mp_alloc_internal` runs on the master pool before arena dispatch. Once dispatched to a child, the child has its own limit tracking (inherited from master at creation time).
- **Event callbacks**: Events fire from the child arena's context, but the `pool` pointer passed to the callback is the child — this matches existing multi-arena behavior for slab.

## Implementation Steps

1. **Modify `mp_alloc_internal`** (`src/cmem_event.c`): add bound-arena fast path for TLSF tier (lines 2113–2116).
2. **Modify `mp_alloc_batch` TLSF branch** (`src/cmem_event.c`): add bound-arena fast path for TLSF batch (lines 2598–2601).
3. **Skip `remote_free_harvest_all`** for non-THREAD_SAFE pools in `mp_alloc_internal` (line 2056).
4. **Add benchmark**: extend benchmark 9 to measure TLSF single-thread with/without multi-arena.
5. **Run full test suite** (`make test`, `make test_advanced`, `make test_cpp`).

## Expected Performance Impact

| Metric | Before | Target |
|---|---|---|
| TLSF single (32B benchmark = 4KB) | 10.10 Mops | 30+ Mops |
| TLSF batch (64 elems × 4KB) | 12.06 Mops | 40+ Mops |
| TLSF batch speedup | 1.16× | 3.5×+ |
| 8-thread efficiency (batch) | 0.27× | 0.6×+ |
| Slab single (32B) | 7.90 Mops | 10+ Mops |

## Risks & Mitigations

| Risk | Mitigation |
|---|---|
| Child arena TLSF exhaustion → fallback to master | Not needed: each child gets independent `tlsf_root`; if exhausted, tlsf_alloc returns NULL and the caller handles it |
| Stats inconsistency across arenas | Each arena has independent stats; benchmark reads from master pool which reports root arena stats only — matches existing behavior |
| `header->subpool` pointing to wrong arena on free | Already correct: tlsf_alloc stamps subpool = the tlsf_pool that served the allocation; tlsf_free uses that pointer directly |
| Windows compatibility | `tls_cache.bound_arena` is already used in `tls_cache_validate_owner` on Windows; no new platform-specific code |

## Out of Scope

- Per-thread slab sub-pools (already handled by multi-arena)
- NUMA-aware sub-pool placement (future)
- TLSF sub-pool auto-growth beyond initial child capacity
