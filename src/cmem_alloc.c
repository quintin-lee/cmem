/**
 * @file cmem_alloc.c
 * @brief Core alloc/free/realloc hot paths, batch operations, and allocation introspection for cmem.
 */
#include "cmem.h"
#include "cmem_internal.h"
#include <stdarg.h>

static void mp_free_stats_update(memory_pool_t *pool, mp_block_header_t *header)
{
    if (!(pool->flags & MP_FLAG_FAST_PATH)) {
        active_list_remove(pool, header);
    }
    /* Use plain writes for non-thread-safe pools to avoid atomic overhead. */
    const bool ts = (pool->flags & MP_FLAG_THREAD_SAFE) != 0;
    if (ts) {
        CMEM_ATOMIC_FETCH_SUB(&pool->active_bytes, header->requested_size, CMEM_ORDER_RELAXED);
        CMEM_ATOMIC_FETCH_SUB(&pool->active_allocations, 1, CMEM_ORDER_RELAXED);
        CMEM_ATOMIC_FETCH_ADD(&pool->total_free_ops, 1, CMEM_ORDER_RELAXED);
    } else {
        pool->active_bytes -= header->requested_size;
        pool->active_allocations--;
        pool->total_free_ops++;
    }
}

/**
 * @brief Lock-free stats update for the free path.
 *
 * Updates active_bytes / active_allocations / total_free_ops atomically
 * without touching the active-list (which requires the pool lock).
 * Callers must invoke active_list_remove under the pool lock themselves
 * when MP_FLAG_THREAD_SAFE is set and the pool is not FAST_PATH.
 * @param pool   The memory pool.
 * @param header Block header being freed.
 */
static void mp_free_stats_no_lock(memory_pool_t *pool, mp_block_header_t *header)
{
    /* Use plain writes for non-thread-safe pools to avoid atomic overhead. */
    const bool ts = (pool->flags & MP_FLAG_THREAD_SAFE) != 0;
    if (ts) {
        CMEM_ATOMIC_FETCH_SUB(&pool->active_bytes, header->requested_size, CMEM_ORDER_RELAXED);
        CMEM_ATOMIC_FETCH_SUB(&pool->active_allocations, 1, CMEM_ORDER_RELAXED);
        CMEM_ATOMIC_FETCH_ADD(&pool->total_free_ops, 1, CMEM_ORDER_RELAXED);
    } else {
        pool->active_bytes -= header->requested_size;
        pool->active_allocations--;
        pool->total_free_ops++;
    }
}

/**
 * @brief Allocate with caller source-location attribution.
 * Records file/line/function and, when tracking is on, a backtrace before
 * delegating to the size-class dispatcher.
 * @return Allocated pointer, or NULL/OOM via fallback.
 */
void *mp_alloc_loc(memory_pool_t *pool, size_t size, const char *file, int line, const char *func)
{
    void *ptr = mp_alloc(pool, size);
    if (ptr) {
        mp_block_header_t *header =
            (mp_block_header_t *)((uint8_t *)ptr - sizeof(mp_block_header_t));
        header->alloc_file = file;
        header->alloc_line = line;
        header->alloc_func = func;
        if (pool->flags & MP_FLAG_TRACK_LOCATIONS) {
#ifdef CMEM_HAS_EXECINFO
            header->backtrace_depth = backtrace(header->backtrace_addrs, MAX_BACKTRACE_FRAMES);
#else
            header->backtrace_depth = 0;
#endif
        }
    }
    return ptr;
}

/**
 * @brief The core allocation dispatcher.
 * Applies dirty/circuit-breaker/percpu-freelist/thread-local-cache fast paths,
 * enforces the memory budget with the emergency reserve, then hands to slab
 * size <=512, TLSF 512..4MB, or the OS backend for larger requests, updating
 * stats/watermarks and firing events.
 * @param pool The memory pool.
 * @param size Requested bytes.
 * @return Allocated pointer or NULL.
 */
void *mp_alloc_internal(memory_pool_t *pool, size_t size)
{
    if (!pool || size == 0) {
        return NULL;
    }

    if (mp_is_pool_dirty(pool) && !pool->fallback_to_sys_alloc_on_oom) {
        return NULL;
    }

    if (pool->circuit_breaker_enabled && pool->circuit_breaker_tripped) {
        return NULL;
    }

    if (pool->stats.max_memory_limit > 0) {
        bool exceed = false;
        if (pool->flags & MP_FLAG_THREAD_SAFE) {
            pool_rdlock(pool);
            if (CMEM_ATOMIC_LOAD(&pool->active_bytes, CMEM_ORDER_RELAXED) + size >
                pool->stats.max_memory_limit) {
                exceed = true;
            }
            pool_rdunlock(pool);
        } else {
            if (CMEM_ATOMIC_LOAD(&pool->active_bytes, CMEM_ORDER_RELAXED) + size >
                pool->stats.max_memory_limit) {
                exceed = true;
            }
        }

        if (exceed) {
            trigger_event(pool, MP_EVENT_OOM, NULL, size);
            if (!pool->fallback_to_sys_alloc_on_oom) {
                if (pool->emergency_buf && !pool->emergency_used &&
                    size + sizeof(mp_block_header_t) +
                            ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0) <=
                        pool->emergency_size) {
                    pool->emergency_used = true;
                    mp_block_header_t *header = (mp_block_header_t *)pool->emergency_buf;
                    header->alloc_type = ALLOC_TYPE_EMERGENCY;
                    header->slab_class = 0;
                    header->flags = MP_BLOCK_FLAG_COUNTED;
                    header->requested_size = size;
                    header->usable_size = pool->emergency_size - sizeof(mp_block_header_t);
                    header->raw_base = pool->emergency_buf;
                    header->subpool = NULL;
                    header->magic = MP_MAGIC_HEAD;
                    if (!(pool->flags & MP_FLAG_FAST_PATH)) {
                        header->alloc_file = NULL;
                        header->alloc_line = 0;
                        header->alloc_func = NULL;
                        header->backtrace_depth = 0;
                        header->prev = NULL;
                        header->next = NULL;
                    }

                    void *emerg_ptr = (void *)((uint8_t *)header + sizeof(mp_block_header_t));
                    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
                        uint8_t *canary = (uint8_t *)emerg_ptr + size;
                        *canary = MP_CANARY_BYTE;
                    }
                    if (pool->flags & MP_FLAG_ZERO_ON_ALLOC) {
                        memset(emerg_ptr, 0, size);
                    }

                    if (!(pool->flags & MP_FLAG_FAST_PATH)) {
                        if (pool->flags & MP_FLAG_THREAD_SAFE) {
                            pool_lock(pool);
                        }
                        active_list_add(pool, header);
                        if (pool->flags & MP_FLAG_THREAD_SAFE) {
                            pool_unlock(pool);
                        }
                    }
                    /* Stats update for emergency buffer — same atomic/plain optimization. */
                    const bool ets = (pool->flags & MP_FLAG_THREAD_SAFE) != 0;
                    size_t e_cur =
                        ets ? CMEM_ATOMIC_FETCH_ADD(&pool->active_bytes, size, CMEM_ORDER_RELAXED)
                            : (pool->active_bytes += size);
                    size_t e_peak = ets ? CMEM_ATOMIC_LOAD(&pool->peak_bytes, CMEM_ORDER_RELAXED)
                                        : pool->peak_bytes;
                    while (e_cur + size > e_peak) {
                        if (ets) {
                            if (CMEM_ATOMIC_COMPARE_EXCHANGE(&pool->peak_bytes,
                                                             &e_peak,
                                                             e_cur + size,
                                                             CMEM_ORDER_RELAXED,
                                                             CMEM_ORDER_RELAXED)) {
                                break;
                            }
                        } else {
                            pool->peak_bytes = e_cur + size;
                            break;
                        }
                    }
                    if (ets) {
                        CMEM_ATOMIC_FETCH_ADD(&pool->active_allocations, 1, CMEM_ORDER_RELAXED);
                        CMEM_ATOMIC_FETCH_ADD(&pool->total_alloc_ops, 1, CMEM_ORDER_RELAXED);
                    } else {
                        pool->active_allocations++;
                        pool->total_alloc_ops++;
                    }
                    return emerg_ptr;
                }
                return NULL;
            }
        }
    }

    void *ptr = NULL;

    if ((pool->flags & MP_FLAG_STATIC_BUFFER) == 0 && size <= SLAB_MAX_SIZE) {
        tls_cache_validate_owner(pool);
        /* Remote-free queues are per-thread-safe-pool only.
         * Skip harvest when no cross-thread frees are pending — the
         * atomic load is cheaper than the function call. */
        if ((pool->flags & MP_FLAG_THREAD_SAFE) &&
            CMEM_ATOMIC_LOAD(&pool->remote_free_pending, CMEM_ORDER_RELAXED) != 0) {
            remote_free_harvest_all(pool);
        }
        uint8_t class_idx = get_slab_class_index(pool, size);
        mp_slab_slot_t *slot = NULL;

        /* Tier 1: Thread-Local Cache (fastest, lock-free, thread-local) */
        if (tls_cache.counts[class_idx] > 0) {
            slot = tls_cache.slots[class_idx];
            tls_cache.slots[class_idx] = slot->next;
            tls_cache.counts[class_idx]--;
        }
        /* Tier 2: Per-CPU Cache (lock-free per-CPU freelist) */
        else if (pool->percpu_freelists) {
            int cpu = percpu_cpu_index();
            slot = percpu_pop(pool, cpu, class_idx);
            if (!slot) {
                percpu_refill(pool, cpu, class_idx);
                slot = percpu_pop(pool, cpu, class_idx);
            }
        }
        /* Tier 3: Combined TLS-refill + slab alloc under a single lock */
        if (!slot) {
            slot = slab_alloc_with_tls_refill(pool, class_idx);
        }

        if (slot) {
            mp_block_header_t *header = (mp_block_header_t *)slot;
            header->alloc_type = ALLOC_TYPE_SLAB;
            header->slab_class = class_idx;
            header->flags = MP_BLOCK_FLAG_COUNTED;
            header->requested_size = size;
            header->usable_size = pool->slab_classes[class_idx].slot_size;
            header->raw_base = slot;
            header->subpool = pool;
            header->magic = MP_MAGIC_HEAD;
            if (!(pool->flags & MP_FLAG_FAST_PATH)) {
                header->alloc_file = NULL;
                header->alloc_line = 0;
                header->alloc_func = NULL;
                header->backtrace_depth = 0;
                header->prev = NULL;
                header->next = NULL;
            }

            ptr = (void *)((uint8_t *)header + sizeof(mp_block_header_t));

            if (pool->flags & MP_FLAG_DEBUG_CANARY) {
                uint8_t *canary = (uint8_t *)ptr + size;
                *canary = MP_CANARY_BYTE;
            }
            if (pool->flags & MP_FLAG_ZERO_ON_ALLOC) {
                memset(ptr, 0, size);
            }
        }
    } else if (size <= TLSF_MAX_SIZE || (pool->flags & MP_FLAG_STATIC_BUFFER)) {
        /* Multi-arena child pools have a thread-private tlsf_root; route
         * through the bound arena for a lock-free allocation path. */
        tls_cache_validate_owner(pool);

        /* TLSF cache hit: serve from per-thread free-block cache (lock-free). */
        size_t total_needed = sizeof(tlsf_block_t) + sizeof(mp_block_header_t) + size +
                              ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0);
        total_needed = (total_needed + TLSF_ALIGN_MASK) & ~(size_t)TLSF_ALIGN_MASK;
        if (total_needed < TLSF_MIN_BLOCK_SIZE) {
            total_needed = TLSF_MIN_BLOCK_SIZE;
        }
        int fl = tlsf_block_size_to_fl(total_needed);
        int cache_idx = fl - TLSF_CACHE_MIN_FL;
        if (cache_idx >= 0 && cache_idx < TLSF_CACHE_SIZES &&
            tls_cache.tlsf_counts[(unsigned)cache_idx] > 0) {
            tlsf_cache_entry_t *entry = tls_cache.tlsf_slots[(unsigned)cache_idx];
            tls_cache.tlsf_slots[(unsigned)cache_idx] = entry->next;
            tls_cache.tlsf_counts[(unsigned)cache_idx]--;

            tlsf_block_t *block = (tlsf_block_t *)entry->block;
            tlsf_pool_t *tpool = (tlsf_pool_t *)entry->tpool;
            if ((block->size_and_flags & BLOCK_SIZE_MASK) != total_needed) {
                /* Cache buckets are floor(log2) size classes, so the cached
                 * span need not match this request. Re-stamping it would
                 * rewrite the physical chain and overlap the neighbour;
                 * return it to the shared list and let the normal path split
                 * it correctly. */
                block->size_and_flags |= BLOCK_STATE_FREE;
                tlsf_insert_free_block(tpool, block);
            } else {
                mp_block_header_t *header =
                    (mp_block_header_t *)((uint8_t *)block + sizeof(tlsf_block_t));
                block->size_and_flags =
                    total_needed | (block->size_and_flags & BLOCK_STATE_PREV_FREE);
                header->magic = MP_MAGIC_HEAD;
                header->alloc_type = ALLOC_TYPE_TLSF;
                header->slab_class = 0;
                header->flags = MP_BLOCK_FLAG_COUNTED;
                header->requested_size = size;
                header->usable_size =
                    total_needed - sizeof(tlsf_block_t) - sizeof(mp_block_header_t);
                header->raw_base = block;
                header->subpool = tpool;
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
                CMEM_ATOMIC_FETCH_ADD(&tpool->owner_pool->tlsf_allocated_bytes,
                                      header->usable_size,
                                      CMEM_ORDER_RELAXED);
            }
        }
        if (!ptr) {
            /* Cache miss, or a cached block too small for this request (it
             * was returned to the shared list above): try the per-thread TLSF
             * arena (lock-free hot path) — only for multi-arena child pools
             * where the arena provides a dedicated per-thread free list that
             * bypasses shared pool locking. */
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
                    header->flags = MP_BLOCK_FLAG_TLSF_ARENA | MP_BLOCK_FLAG_COUNTED;
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
                                          header->usable_size,
                                          CMEM_ORDER_RELAXED);
                }
            }
            if (!ptr) {
                /* Cache/arena miss: fall through to normal tlsf_alloc path. */
                if (tls_cache.bound_arena) {
                    ptr = tlsf_alloc(tls_cache.bound_arena, size);
                } else {
                    ptr = tlsf_alloc(pool, size);
                }
            }
        }

    } else {
        size_t total_sz =
            size + sizeof(mp_block_header_t) + ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0);
        void *raw_mem = sys_mem_alloc(pool, total_sz, 8);
        if (raw_mem) {
            mp_block_header_t *header = (mp_block_header_t *)raw_mem;
            header->alloc_type = ALLOC_TYPE_OS;
            header->slab_class = 0;
            header->flags = MP_BLOCK_FLAG_COUNTED;
            header->requested_size = size;
            header->usable_size = size;
            header->raw_base = raw_mem;
            header->subpool = pool;
            header->magic = MP_MAGIC_HEAD;
            if (!(pool->flags & MP_FLAG_FAST_PATH)) {
                header->alloc_file = NULL;
                header->alloc_line = 0;
                header->alloc_func = NULL;
                header->backtrace_depth = 0;
            }

            ptr = (void *)((uint8_t *)header + sizeof(mp_block_header_t));

            if (pool->flags & MP_FLAG_DEBUG_CANARY) {
                uint8_t *canary = (uint8_t *)ptr + size;
                *canary = MP_CANARY_BYTE;
            }
            if (pool->flags & MP_FLAG_ZERO_ON_ALLOC) {
                memset(ptr, 0, size);
            }
            CMEM_ATOMIC_FETCH_ADD(&pool->os_allocated_bytes, size, CMEM_ORDER_RELAXED);
            CMEM_ATOMIC_FETCH_ADD(&pool->total_pool_size, total_sz, CMEM_ORDER_RELAXED);
        }
    }

    if (ptr) {
        mp_block_header_t *header =
            (mp_block_header_t *)((uint8_t *)ptr - sizeof(mp_block_header_t));
        /* Every allocation leaving this path counts toward pool stats; mark
         * the header even for fallback builders (cmem_tlsf, arena) whose
         * own format sites do not set the bit. */
        header->flags |= MP_BLOCK_FLAG_COUNTED;

        bool call_watermark = false;
        bool call_event = false;

        if (!(pool->flags & MP_FLAG_FAST_PATH)) {
            /* active_list_add requires the pool lock (list mutation).
             * Stats are updated atomically outside the lock to avoid
             * contending the pool_lock on the hot alloc/free path. */
            if (pool->flags & MP_FLAG_THREAD_SAFE) {
                pool_lock(pool);
            }
            active_list_add(pool, header);
            if (pool->flags & MP_FLAG_THREAD_SAFE) {
                pool_unlock(pool);
            }
        }
        /* Stats updates — use plain writes for non-thread-safe pools to avoid
         * atomic op overhead on the hot path. Thread-safe pools keep atomics. */
        const bool ts = (pool->flags & MP_FLAG_THREAD_SAFE) != 0;
        size_t cur_active =
            ts ? CMEM_ATOMIC_FETCH_ADD(&pool->active_bytes, size, CMEM_ORDER_RELAXED)
               : (pool->active_bytes += size);
        size_t cur_peak =
            ts ? CMEM_ATOMIC_LOAD(&pool->peak_bytes, CMEM_ORDER_RELAXED) : pool->peak_bytes;
        while (cur_active + size > cur_peak) {
            if (ts) {
                if (CMEM_ATOMIC_COMPARE_EXCHANGE(&pool->peak_bytes,
                                                 &cur_peak,
                                                 cur_active + size,
                                                 CMEM_ORDER_RELAXED,
                                                 CMEM_ORDER_RELAXED)) {
                    break;
                }
            } else {
                pool->peak_bytes = cur_active + size;
                break;
            }
        }
        if (ts) {
            CMEM_ATOMIC_FETCH_ADD(&pool->active_allocations, 1, CMEM_ORDER_RELAXED);
            CMEM_ATOMIC_FETCH_ADD(&pool->total_alloc_ops, 1, CMEM_ORDER_RELAXED);
        } else {
            pool->active_allocations++;
            pool->total_alloc_ops++;
        }

        int bucket = get_slab_class_index(pool, size);
        if (bucket < CMEM_HISTOGRAM_BUCKETS) {
            if (ts) {
                CMEM_ATOMIC_FETCH_ADD(
                    (_Atomic size_t *)&pool->stats.size_histogram[bucket], 1, CMEM_ORDER_RELAXED);
            } else {
                pool->stats.size_histogram[bucket]++;
            }
        }
        if (pool->watermark_cb) {
            call_watermark = true;
        }
        if (pool->event_cb) {
            call_event = true;
        }

        if (call_watermark) {
            check_watermark_after_change(pool);
        }
        if (call_event) {
            trigger_event(pool, MP_EVENT_ALLOC, ptr, size);
        }
    }

    return ptr;
}

/**
 * @brief Cache-aligned allocation entry point with circuit-breaker accounting.
 * @param pool A memory pool; NULL routes to the C library malloc.
 * @param size Requested bytes.
 * @return Pointer to usable storage, or NULL on failure.
 */
void *mp_alloc(memory_pool_t *pool, size_t size)
{
    if (!pool || size == 0) {
        return NULL;
    }
    if (pool->num_arenas > 0) {
        memory_pool_t *arena = mp_get_thread_bound_arena(pool);
        if (arena && arena != pool) {
            return mp_alloc(arena, size);
        }
    }
    if (pool->flags & MP_FLAG_CACHE_ALIGNED) {
        return mp_aligned_alloc(pool, 64, size);
    }
    void *ptr = mp_alloc_internal(pool, size);
    if (ptr && pool->circuit_breaker_enabled) {
        thread_quota.alloc_bytes += size;
        thread_quota.alloc_count++;
        if (pool->thread_quota_bytes > 0 && thread_quota.alloc_bytes >= pool->thread_quota_bytes) {
            pool_lock(pool);
            pool->circuit_breaker_tripped = true;
            pool_unlock(pool);
        }
    }
    return ptr;
}

/* Batch allocation chunk constants (powers of two for slab/clang-tidy). */
enum {
    CMEM_BATCH_SLAB_CHUNK = 64, /**< Slots pulled per slab_alloc_batch call */
    CMEM_BATCH_ALIGN_MASK = 63  /**< Cache-line alignment mask (MP_FLAG_CACHE_ALIGNED) */
};

/**
 * @brief Stamp a slab slot's block header and return its payload pointer.
 */
static void *
batch_format_slab(memory_pool_t *pool, uint8_t class_idx, mp_slab_slot_t *slot, size_t req_size)
{
    mp_block_header_t *header = (mp_block_header_t *)slot;
    header->alloc_type = ALLOC_TYPE_SLAB;
    header->slab_class = class_idx;
    header->flags = MP_BLOCK_FLAG_COUNTED;
    header->requested_size = req_size;
    header->usable_size = pool->slab_classes[class_idx].slot_size;
    header->raw_base = slot;
    header->subpool = pool;
    header->magic = MP_MAGIC_HEAD;
    if (!(pool->flags & MP_FLAG_FAST_PATH)) {
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
    header->flags = MP_BLOCK_FLAG_COUNTED;
    header->requested_size = req_size;
    header->usable_size = pool->emergency_size - sizeof(mp_block_header_t);
    header->raw_base = pool->emergency_buf;
    header->subpool = NULL;
    header->magic = MP_MAGIC_HEAD;
    if (!(pool->flags & MP_FLAG_FAST_PATH)) {
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
static void *batch_format_os(memory_pool_t *pool,
                             void *raw_mem,
                             size_t req_size,
                             size_t os_total,
                             size_t *os_bytes,
                             size_t *os_pool_total)
{
    mp_block_header_t *header = (mp_block_header_t *)raw_mem;
    header->alloc_type = ALLOC_TYPE_OS;
    header->slab_class = 0;
    header->flags = MP_BLOCK_FLAG_COUNTED;
    header->requested_size = req_size;
    header->usable_size = req_size;
    header->raw_base = raw_mem;
    header->subpool = pool;
    header->magic = MP_MAGIC_HEAD;
    if (!(pool->flags & MP_FLAG_FAST_PATH)) {
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
    return pool->thread_quota_bytes > 0 && thread_quota.alloc_bytes >= pool->thread_quota_bytes;
}

/**
 * @brief Allocate a batch of same-sized entries into a caller array.
 * @param pool The memory pool.
 * @param size Bytes per element.
 * @param out_ptrs Receives up to count pointers.
 * @param count Maximum number of allocations requested.
 * @return Number of pointers actually filled, or 0 on failure.
 */
size_t mp_alloc_batch(memory_pool_t *pool, size_t size, void **out_ptrs, size_t count)
{
    if (!pool || !out_ptrs || count == 0 || size == 0) {
        return 0;
    }

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
     * mp_alloc_internal; plain mp_alloc passes user size). The circuit
     * breaker always uses the user size. */
    const size_t limit_charge = aligned ? total_size : size;

    /* Memory-limit decision: single snapshot, per-element limit semantics.
     * The limit lives on mp_stats_t (pool->stats.max_memory_limit), exactly
     * as the per-element path reads it. */
    size_t target = count;
    size_t oom_fire_count = 0;           /* fallback: one OOM per exceeding element */
    bool full_exceed_oom = false;        /* full-exceed, no emergency: 1 OOM, return 0 */
    bool limit_breach_attempted = false; /* partial-fit: element max_fit would exceed -> 1 OOM */
    bool emergency_only = false;
    bool do_emergency = false;

    if (pool->stats.max_memory_limit > 0) {
        size_t active = CMEM_ATOMIC_LOAD(&pool->active_bytes, CMEM_ORDER_RELAXED);
        if (pool->flags & MP_FLAG_THREAD_SAFE) {
            pool_rdlock(pool);
            active = CMEM_ATOMIC_LOAD(&pool->active_bytes, CMEM_ORDER_RELAXED);
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
            size_t max_fit = (pool->stats.max_memory_limit - active) / limit_charge;
            if (max_fit < count) {
                if (pool->fallback_to_sys_alloc_on_oom) {
                    oom_fire_count = count - max_fit;
                    target = count;
                } else if (pool->emergency_buf && !pool->emergency_used &&
                           total_size + header_sz + canary_len <= pool->emergency_size) {
                    target = max_fit;
                    do_emergency = true;
                    limit_breach_attempted = true;
                } else {
                    target = max_fit;
                    limit_breach_attempted = true;
                }
            }
        }
    }

    /* OOM events, in per-element order:
     * - Fallback: one MP_EVENT_OOM per independently-exceeding element.
     * - Full-exceed without emergency: one OOM, then return 0.
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

    size_t produced = 0;
    bool emergency_produced = false;
    size_t emergency_index = 0;
    bool breaker_tripped = false;

    if (!emergency_only && (pool->flags & MP_FLAG_STATIC_BUFFER) == 0 &&
        total_size <= SLAB_MAX_SIZE) {
        tls_cache_validate_owner(pool);
        uint8_t class_idx = get_slab_class_index(pool, total_size);

        /* Phase A: drain the thread-local cache (lock-free). */
        size_t avail = tls_cache.counts[class_idx];
        size_t need = target - produced;
        if (avail > need) {
            avail = need;
        }
        for (size_t i = 0; i < avail; i++) {
            mp_slab_slot_t *slot = tls_cache.slots[class_idx];
            tls_cache.slots[class_idx] = slot->next; /* read before stamping */
            tls_cache.counts[class_idx]--;
            out_ptrs[produced] = batch_format_slab(pool, class_idx, slot, total_size);
            produced++;
            if (batch_breaker_accrue(pool, size)) {
                pool_lock(pool);
                pool->circuit_breaker_tripped = true;
                pool_unlock(pool);
                breaker_tripped = true;
                break;
            }
        }

        /* Phase B: per-CPU freelist (lock-free). */
        if (!breaker_tripped && produced < target && pool->percpu_freelists) {
            int cpu = percpu_cpu_index();
            while (produced < target) {
                mp_slab_slot_t *slot = percpu_pop(pool, cpu, class_idx);
                if (!slot) {
                    percpu_refill(pool, cpu, class_idx);
                    slot = percpu_pop(pool, cpu, class_idx);
                    if (!slot) {
                        break;
                    }
                }
                out_ptrs[produced] = batch_format_slab(pool, class_idx, slot, total_size);
                produced++;
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
        if (!breaker_tripped && produced < target) {
            mp_slab_slot_t *slots[CMEM_BATCH_SLAB_CHUNK];
            while (produced < target) {
                size_t want = target - produced;
                if (want > CMEM_BATCH_SLAB_CHUNK) {
                    want = CMEM_BATCH_SLAB_CHUNK;
                }
                if (pool->circuit_breaker_enabled && pool->thread_quota_bytes > 0) {
                    size_t cap =
                        (pool->thread_quota_bytes > thread_quota.alloc_bytes)
                            ? (pool->thread_quota_bytes - thread_quota.alloc_bytes + size - 1) /
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
                    out_ptrs[produced] = batch_format_slab(pool, class_idx, slots[i], total_size);
                    produced++;
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
        /* Multi-arena child pools have a thread-private tlsf_root; route
         * through the bound arena for a lock-free allocation path. */
        tls_cache_validate_owner(pool);
        memory_pool_t *tlsf_arena = tls_cache.bound_arena ? tls_cache.bound_arena : pool;
        while (produced < target && !breaker_tripped) {
            void *ptr = tlsf_alloc(tlsf_arena, total_size);
            if (!ptr) {
                break;
            }
            out_ptrs[produced] = ptr;
            produced++;
            if (batch_breaker_accrue(pool, size)) {
                pool_lock(pool);
                pool->circuit_breaker_tripped = true;
                pool_unlock(pool);
                breaker_tripped = true;
            }
        }
    } else if (!emergency_only) {
        /* OS tier: lock-free loop, accounting accumulated after. */
        size_t os_total = total_size + header_sz + canary_len;
        size_t os_bytes = 0;
        size_t os_pool_total = 0;
        while (produced < target) {
            void *raw_mem = sys_mem_alloc(pool, os_total, 8);
            if (!raw_mem) {
                break;
            }
            out_ptrs[produced] =
                batch_format_os(pool, raw_mem, total_size, os_total, &os_bytes, &os_pool_total);
            produced++;
            if (batch_breaker_accrue(pool, size)) {
                pool_lock(pool);
                pool->circuit_breaker_tripped = true;
                pool_unlock(pool);
                breaker_tripped = true;
            }
        }
        CMEM_ATOMIC_FETCH_ADD(&pool->os_allocated_bytes, os_bytes, CMEM_ORDER_RELAXED);
        CMEM_ATOMIC_FETCH_ADD(&pool->total_pool_size, os_pool_total, CMEM_ORDER_RELAXED);
    }

    /* Partial-fit OOM#1: element max_fit would have been attempted and rejected by
     * the limit. Fire it only when all max_fit elements were actually produced AND
     * the circuit breaker has not tripped — per-element: a tripped breaker
     * rejects the next element at the entry check BEFORE the limit check, so
     * no OOM fires; physical exhaustion before max_fit (j < max_fit) also fires none. */
    if (limit_breach_attempted && produced == target && !breaker_tripped) {
        trigger_event(pool, MP_EVENT_OOM, NULL, limit_charge);
    }

    /* Emergency element: only when the limit path requested it, all target
     * elements were produced (prefix semantics), and the breaker has not
     * tripped. OOM#2 fires for the follow-up rejected element unless the
     * breaker trips on the emergency accrual (per-element: the breaker entry
     * check precedes the limit check, so a tripped breaker suppresses the
     * follow-up OOM). */
    if (!breaker_tripped && (emergency_only || (do_emergency && produced == target))) {
        out_ptrs[produced] = batch_format_emergency(pool, total_size);
        emergency_index = produced;
        produced++;
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
    if (produced > 0) {
        pool_lock(pool);
        for (size_t i = 0; i < produced; i++) {
            mp_block_header_t *orig_h = (mp_block_header_t *)((uint8_t *)out_ptrs[i] - header_sz);
            /* The aggregation below counts every produced element toward
             * pool stats; mark the headers (covers the raw tlsf_alloc tier
             * that has no dedicated batch formatter). */
            orig_h->flags |= MP_BLOCK_FLAG_COUNTED;
            if (!(pool->flags & MP_FLAG_FAST_PATH)) {
                active_list_add(pool, orig_h);
            }
            if (aligned) {
                uintptr_t raw_addr = (uintptr_t)out_ptrs[i];
                uintptr_t aligned_addr = (raw_addr + header_sz + CMEM_BATCH_ALIGN_MASK) &
                                         ~(uintptr_t)CMEM_BATCH_ALIGN_MASK;
                mp_block_header_t *new_h = (mp_block_header_t *)(aligned_addr - header_sz);
                if (new_h != orig_h) {
                    *new_h = *orig_h;
                    new_h->requested_size = size;
                    if (orig_h->prev) {
                        orig_h->prev->next = new_h;
                    } else {
                        if (pool->active_head == orig_h) {
                            pool->active_head = new_h;
                        }
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
        CMEM_ATOMIC_FETCH_ADD(&pool->active_bytes, produced * limit_charge, CMEM_ORDER_RELAXED);
        if (CMEM_ATOMIC_LOAD(&pool->active_bytes, CMEM_ORDER_RELAXED) >
            CMEM_ATOMIC_LOAD(&pool->peak_bytes, CMEM_ORDER_RELAXED)) {
            CMEM_ATOMIC_STORE(&pool->peak_bytes,
                              CMEM_ATOMIC_LOAD(&pool->active_bytes, CMEM_ORDER_RELAXED),
                              CMEM_ORDER_RELAXED);
        }
        CMEM_ATOMIC_FETCH_ADD(&pool->active_allocations, produced, CMEM_ORDER_RELAXED);
        CMEM_ATOMIC_FETCH_ADD(&pool->total_alloc_ops, produced, CMEM_ORDER_RELAXED);
        size_t hist_count = produced - (emergency_produced ? 1 : 0);
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
            for (size_t i = 0; i < produced; i++) {
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

    return produced;
}

/* Stack-buffer capacity for one batch of same-pool SLAB frees. */
#define CMEM_FREE_BATCH_CHUNK 256

/**
 * @brief Batch-process up to CMEM_FREE_BATCH_CHUNK same-pool SLAB headers.
 *
 * Aggregates the shared stats/active-list update into a single pool write
 * lock (thread-safe, non-fast-path pools) or a lock-free loop (fast-path and
 * non-thread-safe pools), then places every slot into the TLS cache, the
 * per-CPU freelist, the remote-free queue or its slab page — all lock-free.
 * Phase order is correctness-critical: the stats update reads header->next
 * via active_list_remove, so it MUST run before slot placement, which
 * overwrites header->next with the intrusive free-list link.
 */
static void flush_slab_block(memory_pool_t *pool, mp_block_header_t **hdrs, size_t block_count)
{
    /* Re-validate the TLS cache owner only when it changed: fallback mp_free
     * calls during the classification pass may have switched it to another
     * pool. The owner check is a plain read; validate is the expensive path. */
    if (tls_cache.owner_pool != pool) {
        tls_cache_validate_owner(pool);
    }

    /* Phase A: poison fill (lock-free, before the stats update like mp_free). */
    if (pool->flags & MP_FLAG_POISON_ON_FREE) {
        for (size_t idx = 0; idx < block_count; idx++) {
            mp_block_header_t *header = hdrs[idx];
            void *payload = (void *)((uint8_t *)header + sizeof(mp_block_header_t));
            memset(payload, MP_POISON_BYTE, header->requested_size);
        }
    }

    /* Phase B: aggregated stats. */
    if (!(pool->flags & MP_FLAG_THREAD_SAFE)) {
        for (size_t idx = 0; idx < block_count; idx++) {
            if (hdrs[idx]->flags & MP_BLOCK_FLAG_COUNTED) {
                mp_free_stats_update(pool, hdrs[idx]);
            }
        }
    } else if (pool->flags & MP_FLAG_FAST_PATH) {
        for (size_t idx = 0; idx < block_count; idx++) {
            mp_block_header_t *header = hdrs[idx];
            if (header->flags & MP_BLOCK_FLAG_COUNTED) {
                CMEM_ATOMIC_FETCH_SUB(
                    &pool->active_bytes, header->requested_size, CMEM_ORDER_RELAXED);
                CMEM_ATOMIC_FETCH_SUB(&pool->active_allocations, 1, CMEM_ORDER_RELAXED);
            }
            CMEM_ATOMIC_FETCH_ADD(&pool->total_free_ops, 1, CMEM_ORDER_RELAXED);
        }
    } else {
        pool_lock(pool);
        for (size_t idx = 0; idx < block_count; idx++) {
            if (hdrs[idx]->flags & MP_BLOCK_FLAG_COUNTED) {
                mp_free_stats_update(pool, hdrs[idx]);
            }
        }
        pool_unlock(pool);
    }

    /* Phase C: events after unlock (pool_lock is a non-recursive wrlock). */
    if (pool->event_cb) {
        for (size_t idx = 0; idx < block_count; idx++) {
            mp_block_header_t *header = hdrs[idx];
            void *payload = (void *)((uint8_t *)header + sizeof(mp_block_header_t));
            trigger_event(pool, MP_EVENT_FREE, payload, header->requested_size);
        }
    }

    /* Phase D: slot placement — TLS cache, per-CPU, remote-free queue, or page. */
    for (size_t idx = 0; idx < block_count; idx++) {
        mp_block_header_t *header = hdrs[idx];
        uint8_t class_idx = header->slab_class;
        mp_slab_slot_t *slot = (mp_slab_slot_t *)header->raw_base;
        if (tls_cache.counts[class_idx] < TLS_CACHE_MAX_SLOTS) {
            slot->next = tls_cache.slots[class_idx];
            tls_cache.slots[class_idx] = slot;
            tls_cache.counts[class_idx]++;
        } else if (pool->percpu_freelists &&
                   percpu_push(pool, percpu_cpu_index(), class_idx, slot)) {
            /* Accepted onto the per-CPU freelist. */
        } else if (pool->flags & MP_FLAG_THREAD_SAFE) {
            remote_free_push(pool, class_idx, slot);
        } else {
            /* Non-thread-safe: slab_free takes no lock, direct page return. */
            slab_free(pool, header);
        }
    }
}

/**
 * @brief Free an array of pointers from a batch allocation.
 * @param pool The memory pool.
 * @param ptrs Array of pointers to free.
 * @param count Number of entries.
 */
void mp_free_batch(memory_pool_t *pool, void **ptrs, size_t count)
{
    if (!pool || !ptrs || count == 0) {
        return;
    }

    mp_block_header_t *hdrs[CMEM_FREE_BATCH_CHUNK];
    size_t block_count = 0;

    for (size_t idx = 0; idx < count; idx++) {
        if (!ptrs[idx]) {
            continue;
        }

        mp_block_header_t *header =
            (mp_block_header_t *)((uint8_t *)ptrs[idx] - sizeof(mp_block_header_t));

        if (header->magic != MP_MAGIC_HEAD) {
            /* Corrupt header: let mp_free run its error/recovery path. */
            mp_free(pool, ptrs[idx]);
            ptrs[idx] = NULL;
            continue;
        }

        if ((header->subpool && header->subpool != pool) || header->alloc_type != ALLOC_TYPE_SLAB) {
            /* Redirected (subpool != pool) or non-slab element: fall back to per-element free. */
            mp_free(pool, ptrs[idx]);
            ptrs[idx] = NULL;
            continue;
        }

        hdrs[block_count++] = header;
        ptrs[idx] = NULL;

        if (block_count == CMEM_FREE_BATCH_CHUNK) {
            flush_slab_block(pool, hdrs, block_count);
            block_count = 0;
        }
    }

    if (block_count > 0) {
        flush_slab_block(pool, hdrs, block_count);
    }
}

/**
 * @brief Zero-initialized allocation.
 * @param pool A memory pool (NULL -> libc calloc).
 * @param num Element count.
 * @param size Element size.
 * @return Zeroed pointer, or NULL.
 */
void *mp_calloc(memory_pool_t *pool, size_t num, size_t size)
{
    size_t total_size = num * size;
    void *ptr = mp_alloc(pool, total_size);
    if (ptr && !(pool->flags & MP_FLAG_ZERO_ON_ALLOC)) {
        memset(ptr, 0, total_size);
    }
    return ptr;
}

/**
 * @brief Verify the debug canary of an allocations still holds on free.
 * Reports corruption through the event callback, marks the pool dirty, and
 * invokes the error recovery callback exactly like the old slow-path check.
 * @param pool Owning pool (canary flag must be checked by the caller).
 * @param ptr Payload pointer of the allocation being freed.
 */
static void mp_verify_free_canary(memory_pool_t *pool, void *ptr)
{
    mp_block_header_t *header = (mp_block_header_t *)((uint8_t *)ptr - sizeof(mp_block_header_t));
    uint8_t *canary = (uint8_t *)ptr + header->requested_size;
    if (*canary != MP_CANARY_BYTE) {
        (void)fprintf(stderr, "[MEMORY_POOL BUG] Buffer overflow detected at pointer %p!\n", ptr);
        trigger_event(pool, MP_EVENT_CANARY_CORRUPTION, ptr, header->requested_size);
        mp_mark_pool_dirty(pool);
        if (pool->error_recovery_cb) {
            pool->error_recovery_cb(pool,
                                    true,
                                    CMEM_ATOMIC_LOAD(&pool->active_bytes, CMEM_ORDER_RELAXED),
                                    pool->stats.max_memory_limit,
                                    pool->error_recovery_user_data);
        }
    }
}

/**
 * @brief Release a pointer back to its owning tier.
 * Validates magic, optionally poisons, handles TLS/percpu caches and canary
 * check, unlinks from the active list, and routes to slab/tlsf/OS-free.
 * @param pool The memory pool.
 * @param ptr Allocation to release.
 */
void mp_free(memory_pool_t *pool, void *ptr)
{
    if (!ptr) {
        return;
    }

    mp_block_header_t *header = (mp_block_header_t *)((uint8_t *)ptr - sizeof(mp_block_header_t));

    /* Magic is stamped on every allocation path (fast lane included), so
     * corruption detection stays enabled on FAST_PATH pools. */
    if (header->magic != MP_MAGIC_HEAD) {
        if (pool) {
            (void)fprintf(
                stderr, "[MEMORY_POOL ERROR] Corrupt header or invalid free on pointer %p!\n", ptr);
            trigger_event(pool, MP_EVENT_DOUBLE_FREE, ptr, 0);
            mp_mark_pool_dirty(pool);
            if (pool->error_recovery_cb) {
                pool->error_recovery_cb(pool,
                                        true,
                                        CMEM_ATOMIC_LOAD(&pool->active_bytes, CMEM_ORDER_RELAXED),
                                        pool->stats.max_memory_limit,
                                        pool->error_recovery_user_data);
            }
        }
        return;
    }

    if ((header->flags & MP_BLOCK_FLAG_TLSF_ARENA) != 0) {
        tlsf_arena_t *arena = (tlsf_arena_t *)header->subpool;
        CMEM_ATOMIC_FETCH_SUB(
            &arena->owner_pool->tlsf_allocated_bytes, header->usable_size, CMEM_ORDER_RELAXED);
        /* The arena's stats and active list belong to its owning child pool,
         * so bookkeeping and events below must target that pool. */
        pool = arena->owner_pool;
    } else if (header->subpool != NULL) {
        if (header->alloc_type == ALLOC_TYPE_TLSF) {
            tlsf_pool_t *tpool = (tlsf_pool_t *)header->subpool;
            if (tpool->owner_pool) {
                pool = tpool->owner_pool;
            }
        } else {
            pool = (memory_pool_t *)header->subpool;
        }
    } else if (!pool) {
        return;
    }

    if (pool->flags & MP_FLAG_POISON_ON_FREE) {
        memset(ptr, MP_POISON_BYTE, header->requested_size);
    }
    /* Single canary verification point: every free branch below returns
     * early, so the check must run before the slot is recycled. */
    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
        mp_verify_free_canary(pool, ptr);
    }

    /* Snapshot before any cache push: slot->next clobbers the header's
     * low bytes, so gate checks must read these locals, not header. */
    bool counted = (header->flags & MP_BLOCK_FLAG_COUNTED) != 0;
    size_t req_size = header->requested_size;

    if (header->alloc_type == ALLOC_TYPE_SLAB) {
        tls_cache_validate_owner(pool);
        uint8_t class_idx = header->slab_class;

        if (tls_cache.counts[class_idx] < TLS_CACHE_MAX_SLOTS) {
            if (!(pool->flags & MP_FLAG_THREAD_SAFE)) {
                if (counted) {
                    mp_free_stats_update(pool, header);
                }
            } else if (pool->flags & MP_FLAG_FAST_PATH) {
                if (counted) {
                    CMEM_ATOMIC_FETCH_SUB(&pool->active_bytes, req_size, CMEM_ORDER_RELAXED);
                    CMEM_ATOMIC_FETCH_SUB(&pool->active_allocations, 1, CMEM_ORDER_RELAXED);
                }
                CMEM_ATOMIC_FETCH_ADD(&pool->total_free_ops, 1, CMEM_ORDER_RELAXED);
            } else {
                /* Update atomics lock-free; lock only for active-list mutation. */
                if (counted) {
                    mp_free_stats_no_lock(pool, header);
                }
                if (!(pool->flags & MP_FLAG_FAST_PATH)) {
                    pool_lock(pool);
                    active_list_remove(pool, header);
                    pool_unlock(pool);
                }
            }
            if (pool->event_cb) {
                trigger_event(pool, MP_EVENT_FREE, ptr, req_size);
            }

            mp_slab_slot_t *slot = (mp_slab_slot_t *)header->raw_base;
            slot->next = tls_cache.slots[class_idx];
            tls_cache.slots[class_idx] = slot;
            tls_cache.counts[class_idx]++;
            return;
        }

        if (pool->percpu_freelists) {
            int cpu = percpu_cpu_index();
            mp_slab_slot_t *slot = (mp_slab_slot_t *)header->raw_base;
            if (percpu_push(pool, cpu, class_idx, slot)) {
                if (!(pool->flags & MP_FLAG_THREAD_SAFE)) {
                    if (counted) {
                        mp_free_stats_update(pool, header);
                    }
                } else {
                    /* percpu_push is lock-free; update atomics first, then
                     * lock only for active-list removal. */
                    if (counted) {
                        mp_free_stats_no_lock(pool, header);
                    }
                    if (!(pool->flags & MP_FLAG_FAST_PATH)) {
                        pool_lock(pool);
                        active_list_remove(pool, header);
                        pool_unlock(pool);
                    }
                }
                if (pool->event_cb) {
                    trigger_event(pool, MP_EVENT_FREE, ptr, req_size);
                }
                return;
            }
        }

        if (pool->flags & MP_FLAG_THREAD_SAFE) {
            /* remote_free_push is lock-free (CAS on atomic head).
             * Update atomics lock-free; lock only for active-list removal. */
            if (counted) {
                mp_free_stats_no_lock(pool, header);
            }
            if (!(pool->flags & MP_FLAG_FAST_PATH)) {
                pool_lock(pool);
                active_list_remove(pool, header);
                pool_unlock(pool);
            }
            if (pool->event_cb) {
                trigger_event(pool, MP_EVENT_FREE, ptr, req_size);
            }
            mp_slab_slot_t *slot = (mp_slab_slot_t *)header->raw_base;
            remote_free_push(pool, class_idx, slot);
            return;
        }
    }

    pool_lock(pool);

    uint8_t alloc_type = header->alloc_type;
    req_size = header->requested_size; /* re-read: header is now locked-safe */
    void *raw_base = header->raw_base;
    bool call_watermark = false;
    bool call_event = false;

    if (header->flags & MP_BLOCK_FLAG_COUNTED) {
        mp_free_stats_update(pool, header);
    }
    if (alloc_type == ALLOC_TYPE_OS) {
        CMEM_ATOMIC_FETCH_SUB(&pool->os_allocated_bytes, req_size, CMEM_ORDER_RELAXED);
        CMEM_ATOMIC_FETCH_SUB(&pool->total_pool_size,
                              req_size + sizeof(mp_block_header_t) +
                                  ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0),
                              CMEM_ORDER_RELAXED);
    }
    if (pool->watermark_cb) {
        call_watermark = true;
    }
    if (pool->event_cb) {
        call_event = true;
    }

    if (alloc_type == ALLOC_TYPE_SLAB) {
        slab_free(pool, header);
    } else if (alloc_type == ALLOC_TYPE_TLSF) {
        tls_cache_validate_owner(pool);
        tlsf_block_t *block = (tlsf_block_t *)header->raw_base;
        /* Derive from the header: block->size_and_flags may be concurrently OR-ed
         * with BLOCK_STATE_PREV_FREE by tlsf_alloc in another thread (D12 residual). */
        size_t block_size = header->usable_size + sizeof(tlsf_block_t) + sizeof(mp_block_header_t);

        if ((header->flags & MP_BLOCK_FLAG_TLSF_ARENA) != 0) {
            tlsf_arena_free_block((tlsf_arena_t *)header->subpool, block);
        } else {
            /* TLSF cache push: stash the block in the per-thread cache to avoid
             * locking on the next allocation of a similar size. */
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
                /* Stats already updated in tlsf_free; skip it. */
            } else {
                tlsf_free(pool, header);
            }
        }
    } else if (alloc_type == ALLOC_TYPE_OS) {
        sys_mem_free(pool, raw_base, req_size);
    } else if (alloc_type == ALLOC_TYPE_EMERGENCY) {
        pool->emergency_used = false;
    }

    if (call_watermark) {
        check_watermark_after_change(pool);
    }
    if (call_event) {
        trigger_event(pool, MP_EVENT_FREE, ptr, req_size);
    }
    pool_unlock(pool);
}

/**
 * @brief Resize a live allocation in place when possible, else copy elsewhere.
 * @return Pointer to the resized storage, or NULL.
 */
void *mp_realloc(memory_pool_t *pool, void *ptr, size_t new_size)
{
    if (!ptr) {
        return mp_alloc(pool, new_size);
    }
    if (new_size == 0) {
        mp_free(pool, ptr);
        return NULL;
    }

    mp_block_header_t *header = (mp_block_header_t *)((uint8_t *)ptr - sizeof(mp_block_header_t));
    if (header->magic != MP_MAGIC_HEAD) {
        return NULL;
    }

    if (new_size <= header->usable_size) {
        header->requested_size = new_size;
        if (pool->flags & MP_FLAG_DEBUG_CANARY) {
            uint8_t *canary = (uint8_t *)ptr + new_size;
            *canary = MP_CANARY_BYTE;
        }
        trigger_event(pool, MP_EVENT_REALLOC, ptr, new_size);
        return ptr;
    }

    if (header->alloc_type == ALLOC_TYPE_TLSF) {
        /* Flush TLSF cache so adjacent cached blocks return to the free list,
         * enabling correct coalescing during in-place expand. */
        tls_cache_validate_owner(pool);
        for (int i = 0; i < TLSF_CACHE_SIZES; i++) {
            while (tls_cache.tlsf_slots[i]) {
                tlsf_cache_entry_t *entry = tls_cache.tlsf_slots[i];
                tls_cache.tlsf_slots[i] = entry->next;
                tlsf_block_t *blk = (tlsf_block_t *)entry->block;
                tlsf_pool_t *tp = (tlsf_pool_t *)entry->tpool;
                blk->size_and_flags |= BLOCK_STATE_FREE;
                tlsf_insert_free_block(tp, blk);
            }
            tls_cache.tlsf_counts[i] = 0;
        }
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
                size_t acur = tlsf_arena_span(ablock);
                size_t need = sizeof(tlsf_block_t) + sizeof(mp_block_header_t) + new_size + canary;
                need = (need + TLSF_ALIGN_MASK) & ~(size_t)TLSF_ALIGN_MASK;
                if (need < TLSF_MIN_BLOCK_SIZE) {
                    need = TLSF_MIN_BLOCK_SIZE;
                }
                if (need > acur) {
                    tlsf_block_t *anext = (tlsf_block_t *)((uint8_t *)ablock + acur);
                    bool growable = (anext != arena->sentinel) &&
                                    ((anext->size_and_flags & BLOCK_STATE_FREE) != 0) &&
                                    (acur + tlsf_arena_span(anext) >= need);
                    if (growable) {
                        size_t aspan = tlsf_arena_span(anext);
                        size_t total = acur + aspan;
                        tlsf_block_t *after = (tlsf_block_t *)((uint8_t *)anext + aspan);
                        tlsf_arena_unlink_free(arena, anext);
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
                            tlsf_arena_insert_free(arena, remainder);
                            new_span = need;
                        } else {
                            ablock->size_and_flags = total;
                            new_span =
                                total; /* no split: the live span is the whole merged block */
                        }
                        /* live_bytes must track the REAL new span (`need` only when split);
                         * the no-split case owns `total`, else the last-freer detection
                         * (prev_live == span) never reaches zero and retire hangs. */
                        CMEM_ATOMIC_FETCH_ADD(
                            &arena->live_bytes, new_span - acur, CMEM_ORDER_RELAXED);
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
                                              header->usable_size - old_usable,
                                              CMEM_ORDER_RELAXED);
                    } else {
                        CMEM_ATOMIC_FETCH_SUB(&arena->owner_pool->tlsf_allocated_bytes,
                                              old_usable - header->usable_size,
                                              CMEM_ORDER_RELAXED);
                    }
                    trigger_event(pool, MP_EVENT_REALLOC, ptr, new_size);
                    return ptr;
                }
            }
        }
        pool_lock(pool);
        bool expanded = tlsf_try_inplace_expand(pool, header, new_size);
        pool_unlock(pool);
        if (expanded) {
            trigger_event(pool, MP_EVENT_REALLOC, ptr, new_size);
            return ptr;
        }
    }

    void *new_ptr = mp_alloc(pool, new_size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, header->requested_size);
        mp_free(pool, ptr);
    }
    return new_ptr;
}

/**
 * @brief Realloc with overflow-checked element multiplication.
 * @param pool The memory pool.
 * @param ptr Pointer to resize (may be NULL).
 * @param nmemb Element count.
 * @param size Bytes per element.
 * @return Pointer to resized storage, or NULL on overflow/failure.
 */
void *mp_reallocarray(memory_pool_t *pool, void *ptr, size_t nmemb, size_t size)
{
    if (nmemb != 0 && size > SIZE_MAX / nmemb) {
        return NULL;
    }
    return mp_realloc(pool, ptr, nmemb * size);
}

/**
 * @brief Duplicate a NUL-terminated string into the pool.
 * @param pool A memory pool (NULL -> libc strdup).
 * @param str String to copy.
 * @return Newly allocated copy, or NULL on failure.
 */
char *mp_strdup(memory_pool_t *pool, const char *str)
{
    return mp_strdup_loc(pool, str, NULL, 0, NULL);
}

/**
 * @brief Duplicate n bytes into the pool.
 * @param pool A memory pool (NULL -> malloc + memcpy).
 * @param src Source bytes.
 * @param n Length in bytes.
 * @return Copy, or NULL on failure.
 */
void *mp_memdup(memory_pool_t *pool, const void *src, size_t n)
{
    return mp_memdup_loc(pool, src, n, NULL, 0, NULL);
}

/**
 * @brief printf-style formatted allocation into the pool.
 * Formats varargs per fmt and allocates the result via the pool.
 * @param pool A memory pool (NULL -> libc behavior).
 * @param fmt printf-style format string.
 * @return Newly allocated formatted string, or NULL on failure.
 */
char *mp_asprintf(memory_pool_t *pool, const char *fmt, ...)
{
    if (!fmt) {
        return NULL;
    }
    va_list args;
    va_start(args, fmt);
    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (len < 0) {
        va_end(args_copy);
        return NULL;
    }

    char *buf = (char *)mp_alloc(pool, (size_t)len + 1);
    if (buf) {
        (void)vsnprintf(buf, (size_t)len + 1, fmt, args_copy);
    }
    va_end(args_copy);
    return buf;
}

/**
 * @brief Allocate memory aligned to a power-of-two boundary.
 * @param pool A memory pool.
 * @param alignment Required alignment (power of two).
 * @param size Requested bytes.
 * @return Aligned pointer, or NULL.
 */
void *mp_aligned_alloc(memory_pool_t *pool, size_t alignment, size_t size)
{
    if ((alignment & (alignment - 1)) != 0 || alignment < sizeof(void *)) {
        return NULL;
    }

    size_t total_size = size + alignment + sizeof(mp_block_header_t) +
                        ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0);
    void *raw_ptr = mp_alloc_internal(pool, total_size);
    if (!raw_ptr) {
        return NULL;
    }

    if (pool->circuit_breaker_enabled) {
        thread_quota.alloc_bytes += size;
        thread_quota.alloc_count++;
        if (pool->thread_quota_bytes > 0 && thread_quota.alloc_bytes >= pool->thread_quota_bytes) {
            pool_lock(pool);
            pool->circuit_breaker_tripped = true;
            pool_unlock(pool);
        }
    }

    uintptr_t raw_addr = (uintptr_t)raw_ptr;
    uintptr_t aligned_addr =
        (raw_addr + sizeof(mp_block_header_t) + (alignment - 1)) & ~(alignment - 1);

    mp_block_header_t *orig_header =
        (mp_block_header_t *)((uint8_t *)raw_ptr - sizeof(mp_block_header_t));
    mp_block_header_t *new_header = (mp_block_header_t *)(aligned_addr - sizeof(mp_block_header_t));

    if (new_header != orig_header) {
        pool_lock(pool);
        *new_header = *orig_header;
        new_header->requested_size = size;

        if (orig_header->prev) {
            orig_header->prev->next = new_header;
        } else {
            if (pool->active_head == orig_header) {
                pool->active_head = new_header;
            }
        }
        if (orig_header->next) {
            orig_header->next->prev = new_header;
        }
        pool_unlock(pool);
    } else {
        new_header->requested_size = size;
    }

    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
        uint8_t *canary = (uint8_t *)aligned_addr + size;
        *canary = MP_CANARY_BYTE;
    }

    return (void *)aligned_addr;
}

/**
 * @brief Return usable bytes of an allocated pointer.
 * @param pool The memory pool.
 * @param ptr Allocation.
 * @return Usable size, or 0 on invalid.
 */
size_t mp_usable_size(memory_pool_t *pool, void *ptr)
{
    if (!pool || !ptr || !mp_ptr_valid(pool, ptr)) {
        return 0;
    }
    mp_block_header_t *header = (mp_block_header_t *)((uint8_t *)ptr - sizeof(mp_block_header_t));
    return header->usable_size;
}

/**
 * @brief Return the requested size of an allocation.
 * @param pool The memory pool.
 * @param ptr Allocation.
 * @return Requested size, or 0.
 */
size_t mp_alloc_size(memory_pool_t *pool, void *ptr)
{
    if (!pool || !ptr || !mp_ptr_valid(pool, ptr)) {
        return 0;
    }
    mp_block_header_t *header = (mp_block_header_t *)((uint8_t *)ptr - sizeof(mp_block_header_t));
    return header->requested_size;
}

/**
 * @brief Structural liveness check for a slot on a FAST_PATH pool.
 * @param pool Owning pool (FAST_PATH flag already verified by the caller).
 * @param header Header derived from the user pointer under test.
 * @return true when the slot passes structural validation.
 *
 * FAST_PATH pools maintain no active list, so validity is decided by
 * evidence that survives without it: the magic gate (written on every
 * allocation), alloc-type consistency, and for slab slots containment
 * inside a real slab page whose free list does not currently contain the
 * slot (exact liveness for the slab tier). Non-slab tiers are accepted on
 * magic + raw_base only; freed OS/TLSF blocks may therefore still report
 * live until reused.
 */
static bool mp_fastpath_slot_live(memory_pool_t *pool, mp_block_header_t *header)
{
    if (header->alloc_type != ALLOC_TYPE_SLAB) {
        return header->raw_base != NULL;
    }
    if (header->slab_class >= SLAB_CLASS_COUNT || header->raw_base != header) {
        return false;
    }
    uint8_t *payload = (uint8_t *)header + sizeof(mp_block_header_t);
    uintptr_t ptr_val = (uintptr_t)payload;
    uintptr_t page_base = ptr_val & ~(uintptr_t)(SLAB_PAGE_SIZE - 1);
    if (ptr_val - page_base <= sizeof(mp_slab_page_t)) {
        return false;
    }
    mp_slab_page_t *page = (mp_slab_page_t *)page_base;
    if (page->class_index != header->slab_class || page->total_slots == 0) {
        return false;
    }
    size_t free_count = (size_t)CMEM_ATOMIC_LOAD(&page->page_free_count, CMEM_ORDER_RELAXED);
    if (free_count > page->total_slots) {
        return false;
    }
    /* Walk under the class lock: free-side mutations of page->free_list
     * happen while this lock is held. */
    mp_slab_class_t *sc = &pool->slab_classes[header->slab_class];
    pthread_mutex_lock(&sc->lock);
    mp_slab_slot_t *slot = page->free_list;
    bool is_free = false;
    while (slot && free_count > 0) {
        if ((mp_block_header_t *)slot == header) {
            is_free = true;
            break;
        }
        slot = slot->next;
        free_count--;
    }
    pthread_mutex_unlock(&sc->lock);
    return !is_free;
}
size_t mp_enumerate_regions(memory_pool_t *pool, mp_region_info_t *regions, size_t max_regions)
{
    if (!pool || !regions || max_regions == 0) {
        return 0;
    }

    pool_rdlock(pool);
    size_t count = 0;

    for (int cls = 0; cls < SLAB_CLASS_COUNT && count < max_regions; cls++) {
        mp_slab_class_t *sc = &pool->slab_classes[cls];
        mp_slab_page_t *curr = sc->partial_pages;
        while (curr && count < max_regions) {
            regions[count].base = curr->page_raw_mem;
            regions[count].size = SLAB_PAGE_SIZE;
            regions[count].type = ALLOC_TYPE_SLAB;
            regions[count].slab_class = cls;
            regions[count].is_hot = curr->is_hot;

            count++;
            curr = curr->next;
        }
        curr = sc->full_pages;
        while (curr && count < max_regions) {

            regions[count].base = curr->page_raw_mem;
            regions[count].size = SLAB_PAGE_SIZE;
            regions[count].type = ALLOC_TYPE_SLAB;
            regions[count].slab_class = cls;
            regions[count].is_hot = curr->is_hot;

            count++;
            curr = curr->next;
        }
    }

    tlsf_pool_t *tcurr = pool->tlsf_root;
    while (tcurr && count < max_regions) {
        regions[count].base = tcurr->raw_area;
        regions[count].size = tcurr->raw_size;
        regions[count].type = ALLOC_TYPE_TLSF;
        regions[count].slab_class = 0;
        regions[count].is_hot = false;
        count++;
        tcurr = tcurr->next;
    }

    if (pool->emergency_buf && count < max_regions) {
        regions[count].base = pool->emergency_buf;
        regions[count].size = pool->emergency_size;
        regions[count].type = ALLOC_TYPE_EMERGENCY;
        regions[count].slab_class = 0;
        regions[count].is_hot = false;
        count++;
    }

    pool_rdunlock(pool);
    return count;
}

/**
 * @brief Check whether a pointer belongs to this pool.
 * @param pool The memory pool.
 * @param ptr Candidate pointer.
 * @return true for a live valid allocation, false otherwise.
 */
bool mp_ptr_valid(memory_pool_t *pool, void *ptr)
{
    if (!pool || !ptr) {
        return false;
    }
    mp_block_header_t *header = (mp_block_header_t *)((uint8_t *)ptr - sizeof(mp_block_header_t));
    if (header->magic != MP_MAGIC_HEAD) {
        return false;
    }
    if (pool->flags & MP_FLAG_FAST_PATH) {
        /* FAST_PATH pools skip active-list maintenance; validate slots
         * structurally instead (see docs/superpowers/specs fastpath spec). */
        return mp_fastpath_slot_live(pool, header);
    }
    pool_rdlock(pool);
    bool found = false;
    mp_block_header_t *curr = pool->active_head;
    while (curr) {
        if (curr == header) {
            found = true;
            break;
        }
        curr = curr->next;
    }
    pool_rdunlock(pool);
    return found;
}

/**
 * @brief Fetch source/size metadata for a live allocation.
 * @param pool The memory pool.
 * @param ptr Allocation of interest.
 * @param info Receives the metadata on success.
 * @return true when filled, false on invalid pointer/pool.
 */
bool mp_get_allocation_info(memory_pool_t *pool, void *ptr, mp_allocation_info_t *info)
{
    if (!pool || !ptr || !info) {
        return false;
    }

    mp_block_header_t *header = (mp_block_header_t *)((uint8_t *)ptr - sizeof(mp_block_header_t));
    if (header->magic != MP_MAGIC_HEAD) {
        return false;
    }
    if (pool->flags & MP_FLAG_FAST_PATH) {
        /* FAST_PATH source-location fields are never filled; report unknown. */
        if (!mp_fastpath_slot_live(pool, header)) {
            return false;
        }
        info->ptr = ptr;
        info->requested_size = header->requested_size;
        info->usable_size = header->usable_size;
        info->alloc_type = (mp_alloc_type_t)header->alloc_type;
        info->slab_class = header->slab_class;
        info->raw_base = header->raw_base;
        info->alloc_file = NULL;
        info->alloc_line = 0;
        info->alloc_func = NULL;
        info->backtrace_depth = 0;
        for (int i = 0; i < 8; i++) {
            info->backtrace_addrs[i] = NULL;
        }
        return true;
    }
    pool_rdlock(pool);
    bool found = false;
    mp_block_header_t *curr = pool->active_head;
    while (curr) {
        if (curr == header) {
            found = true;
            break;
        }
        curr = curr->next;
    }
    if (!found) {
        pool_rdunlock(pool);
        return false;
    }

    info->ptr = ptr;
    info->requested_size = header->requested_size;
    info->usable_size = header->usable_size;
    info->alloc_type = (mp_alloc_type_t)header->alloc_type;
    info->slab_class = header->slab_class;
    info->raw_base = header->raw_base;
    info->alloc_file = header->alloc_file;
    info->alloc_line = header->alloc_line;
    info->alloc_func = header->alloc_func;
    info->backtrace_depth = header->backtrace_depth;
    if (header->backtrace_depth > 0) {
        for (int i = 0; i < header->backtrace_depth && i < 8; i++) {
            info->backtrace_addrs[i] = header->backtrace_addrs[i];
        }
    } else {
        for (int i = 0; i < 8; i++) {
            info->backtrace_addrs[i] = NULL;
        }
    }

    pool_rdunlock(pool);
    return true;
}

/**
 * @brief Build a double-buffer frame arena for automatic reset.
 * Allocates two child frame pools from the default arena.
 * @param frame_capacity Per-frame capacity in bytes (0 for 1 MiB each).
 * @return New frame arena, or NULL on failure.
 */
cmem_frame_arena_t *mp_frame_arena_create(size_t frame_capacity)
{
    cmem_frame_arena_t *farena = (cmem_frame_arena_t *)malloc(sizeof(cmem_frame_arena_t));
    if (!farena) {
        return NULL;
    }

    size_t cap = frame_capacity > 0 ? frame_capacity : 1024 * 1024;
    farena->pool_a = mp_create(cap, MP_FLAG_DEFAULT);
    farena->pool_b = mp_create(cap, MP_FLAG_DEFAULT);
    if (!farena->pool_a || !farena->pool_b) {
        if (farena->pool_a) {
            mp_destroy(farena->pool_a);
        }
        if (farena->pool_b) {
            mp_destroy(farena->pool_b);
        }
        free(farena);
        return NULL;
    }

    farena->active_pool = farena->pool_a;
    farena->frame_index = 0;
    return farena;
}

/**
 * @brief Allocate from the active frame pool.
 * @param farena Frame arena.
 * @param size Requested bytes.
 * @return Zeroed pointer, or NULL on failure.
 */
void *mp_frame_alloc(cmem_frame_arena_t *farena, size_t size)
{
    if (!farena || !farena->active_pool) {
        return NULL;
    }
    return mp_alloc(farena->active_pool, size);
}

/**
 * @brief Close the current frame and switch to the idle buffer.
 * Afterwards the previous frame's allocations are automatically reclaimed.
 * @param farena Frame arena.
 */
void mp_frame_end(cmem_frame_arena_t *farena)
{
    if (!farena) {
        return;
    }
    farena->frame_index++;
    if (farena->active_pool == farena->pool_a) {
        farena->active_pool = farena->pool_b;
        mp_reset(farena->pool_b);
    } else {
        farena->active_pool = farena->pool_a;
        mp_reset(farena->pool_a);
    }
}

/**
 * @brief Destroy both child frame pools and free the frame arena.
 * @param farena Frame arena.
 */
void mp_frame_arena_destroy(cmem_frame_arena_t *farena)
{
    if (!farena) {
        return;
    }
    if (farena->pool_a) {
        mp_destroy(farena->pool_a);
    }
    if (farena->pool_b) {
        mp_destroy(farena->pool_b);
    }
    free(farena);
}

