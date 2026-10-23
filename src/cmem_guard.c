/**
 * @file cmem_guard.c
 * @brief Error recovery, thread quota, ABI, cgroup, and ASan integration for cmem.
 */
#include "cmem.h"
#include "cmem_internal.h"
#define CMEM_DECIMAL_BASE 10

/**
 * @brief Mark a memory pool corrupted/dirty and emit a MP_EVENT_DIRTY event.
 * Sets pool->is_dirty under the pool lock and notifies the event sink.
 * @param pool The memory pool to mark. No-op when NULL.
 */
void mp_mark_pool_dirty(memory_pool_t *pool)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    pool->is_dirty = true;
    pool_unlock(pool);
    trigger_event(pool, MP_EVENT_DIRTY, NULL, 0);
}

/**
 * @brief Clear the dirty flag of a memory pool.
 * @param pool The memory pool to clear. No-op when NULL.
 */
void mp_clear_pool_dirty(memory_pool_t *pool)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    pool->is_dirty = false;
    pool_unlock(pool);
}

/**
 * @brief Query whether a memory pool is marked dirty.
 * @return True if dirty; false otherwise (including when pool is NULL).
 */
bool mp_is_pool_dirty(memory_pool_t *pool)
{
    if (!pool) {
        return false;
    }
    pool_rdlock(pool);
    bool dirty = pool->is_dirty;
    pool_rdunlock(pool);
    return dirty;
}

/**
 * @brief Detach a corrupt block from the pool's active list.
 * Validates the block magic, unlinks the header from the active list, adjusts
 * active-bytes/allocations accounting, stamps a poison magic to flag the fault,
 * and emits a MP_EVENT_DOUBLE_FREE event.
 * @param pool The memory pool.
 * @param ptr  Pointer presumed to reference an active block.
 * @return True on success; false when pool/ptr are NULL or the magic mismatches.
 */
bool mp_isolate_bad_block(memory_pool_t *pool, void *ptr)
{
    if (!pool || !ptr) {
        return false;
    }
    mp_block_header_t *header = (mp_block_header_t *)((uint8_t *)ptr - sizeof(mp_block_header_t));
    /* FAST_PATH pools don't stamp magic and keep no active list. */
    if ((pool->flags & MP_FLAG_FAST_PATH) || header->magic != MP_MAGIC_HEAD) {
        return false;
    }

    pool_lock(pool);
    active_list_remove(pool, header);
    pool_unlock(pool);
    /* Stats are atomic — no lock needed. */
    CMEM_ATOMIC_FETCH_SUB(&pool->active_bytes, header->requested_size, CMEM_ORDER_RELAXED);
    CMEM_ATOMIC_FETCH_SUB(&pool->active_allocations, 1, CMEM_ORDER_RELAXED);
    CMEM_ATOMIC_FETCH_ADD(&pool->total_free_ops, 1, CMEM_ORDER_RELAXED);
    header->magic = CMEM_FREED_MAGIC;

    trigger_event(pool, MP_EVENT_DOUBLE_FREE, ptr, 0);
    return true;
}

/**
 * @brief Set the per-thread byte quota used by the circuit breaker.
 * The breaker trips once a thread's cumulative allocation reaches the quota.
 * @param pool        The memory pool.
 * @param quota_bytes Maximum cumulative bytes a thread may allocate.
 */
void mp_set_thread_quota(memory_pool_t *pool, size_t quota_bytes)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    pool->thread_quota_bytes = quota_bytes;
    pool_unlock(pool);
}

/**
 * @brief Enable or disable the per-thread circuit breaker.
 * When enabled, allocations exceeding the thread quota trip the pool.
 * @param pool   The memory pool.
 * @param enable True to arm, false to disarm.
 */
void mp_set_circuit_breaker(memory_pool_t *pool, bool enable)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    pool->circuit_breaker_enabled = enable;
    pool_unlock(pool);
}

/**
 * @brief Return the calling thread's cumulative allocated bytes.
 * Read from the thread-local quota accumulator.
 * @param pool The memory pool (may be NULL).
 * @return Cumulative bytes allocated by the calling thread.
 */
size_t mp_get_thread_allocated_bytes(memory_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    return thread_quota.alloc_bytes;
}

/**
 * @brief Reset the calling thread's quota accounting to zero.
 * @param pool The memory pool (may be NULL).
 */
void mp_reset_thread_quota(memory_pool_t *pool)
{
    if (!pool) {
        return;
    }
    thread_quota.alloc_bytes = 0;
    thread_quota.alloc_count = 0;
}

/**
 * @brief Query whether the circuit breaker is currently tripped.
 * @param pool The memory pool.
 * @return True if tripped, false otherwise (including when pool is NULL).
 */
bool mp_is_circuit_breaker_tripped(memory_pool_t *pool)
{
    if (!pool) {
        return false;
    }
    pool_rdlock(pool);
    bool tripped = pool->circuit_breaker_tripped;
    pool_rdunlock(pool);
    return tripped;
}

/**
 * @brief Return the current ABI version of the library.
 * @return Constant ABI version tag of this build.
 */
uint32_t mp_abi_version(void)
{
    return 1;
}

/**
 * @brief Enable cgroup-aware memory-limit detection.
 * When enabled, probes /sys/fs/cgroup (v1 then v2) for the memory limit and caches
 * it in pool->cgroup_mem_limit.
 * @param pool   The memory pool.
 * @param enable True to probe and cache the limit.
 */
void mp_set_cgroup_aware(memory_pool_t *pool, bool enable)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    pool->cgroup_aware = enable;
    if (enable) {
        FILE *fp = fopen("/sys/fs/cgroup/memory/memory.limit_in_bytes", "r");
        if (!fp) {
            fp = fopen("/sys/fs/cgroup/memory.max", "r");
        }
        if (fp) {
            char buf[64] = {0};

            if (fgets(buf, sizeof(buf), fp) != NULL) {
                char *endptr = NULL;
                unsigned long long limit = strtoull(buf, &endptr, CMEM_DECIMAL_BASE);

                if (endptr != buf && limit > 0) {
                    pool->cgroup_mem_limit = (size_t)limit;
                }
            }
            (void)fclose(fp);
        }
    }
    pool_unlock(pool);
}

/**
 * @brief Return the cached cgroup memory limit in bytes.
 * @return The cached limit, or 0 when none known or pool is NULL.
 */
size_t mp_get_cgroup_mem_limit(memory_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    pool_rdlock(pool);
    size_t limit = pool->cgroup_mem_limit;
    pool_rdunlock(pool);
    return limit;
}

/**
 * @brief Detect whether the binary was built with AddressSanitizer.
 * @return True when __SANITIZE_ADDRESS__ is defined.
 */
bool mp_asan_is_enabled(void)
{
#ifdef __SANITIZE_ADDRESS__
    return true;
#else
    return false;
#endif
}

/**
 * @brief Report an ASan-detected memory error.
 * Logs the faulting address/size/access, invokes the error-recovery callback and
 * traps the process. Only does work under AddressSanitizer.
 * @param pool     The memory pool.
 * @param ptr      Address of the fault.
 * @param size     Access span in bytes.
 * @param is_write Non-zero for a write access.
 */
void mp_asan_report_error(memory_pool_t *pool, void *ptr, size_t size, bool is_write)
{
    (void)pool;
    (void)ptr;
    (void)size;
    (void)is_write;
#ifdef __SANITIZE_ADDRESS__
    (void)fprintf(stderr,
                  "[CMEM ASan] Memory error detected at %p (size=%zu, write=%d)\n",
                  ptr,
                  size,
                  is_write ? 1 : 0);
    if (pool && pool->error_recovery_cb) {
        pool->error_recovery_cb(pool,
                                true,
                                CMEM_ATOMIC_LOAD(&pool->active_bytes, CMEM_ORDER_RELAXED),
                                pool->stats.max_memory_limit,
                                pool->error_recovery_user_data);
    }
    __builtin_trap();
#endif
}

/**
 * @brief Validate that an address is a live active allocation.
 * @param pool The memory pool.
 * @param ptr  Address to validate.
 * @param size Access span (unused for validation).
 * @return True when the pointer is a valid active allocation in a clean pool.
 */
bool mp_asan_check_memory(memory_pool_t *pool, void *ptr, size_t size)
{
    if (!pool || !ptr || size == 0) {
        return false;
    }
    pool_rdlock(pool);
    bool dirty = pool->is_dirty;
    pool_rdunlock(pool);
    if (dirty) {
        return false;
    }
    if (!mp_ptr_valid(pool, ptr)) {
        return false;
    }
#ifdef __SANITIZE_ADDRESS__
    return true;
#else
    return true;
#endif
}

/**
 * @brief Set or clear the ASan-integration flag on a pool.
 * @param pool   The memory pool.
 * @param enable True to set MP_FLAG_ASAN_INTEGRATION.
 */
void mp_set_asan_integration(memory_pool_t *pool, bool enable)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    if (enable) {
        pool->flags = (mp_flags_t)(pool->flags | MP_FLAG_ASAN_INTEGRATION);
    } else {
        pool->flags = (mp_flags_t)(pool->flags & ~MP_FLAG_ASAN_INTEGRATION);
    }
    pool_unlock(pool);
}
