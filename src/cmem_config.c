/**
 * @file cmem_config.c
 * @brief Environment flags, introspection, hot-reload, compaction, latency stats, and slab table for cmem.
 */
#include "cmem.h"
#include "cmem_internal.h"
#define CMEM_DECIMAL_BASE 10
#define CMEM_LATENCY_HISTOGRAM_MAX_INDEX 31u
#define CMEM_LATENCY_P99_NUMERATOR 99u
#define CMEM_LATENCY_P99_DENOMINATOR 100u

mp_flags_t mp_default_pool_flags(void)
{
    return CMEM_DEFAULT_SAFETY_FLAGS;
}

/**
 * @brief Read and apply the CMEM_CONF environment configuration.
 * Recognises canary/zero/tls/track/poison/aligned/guard/hugepages directives
 * (each as name=1 or name=on) and ORs them into the flags. If CMEM_CONF is unset
 * or empty, returns default_flags unchanged.
 * @param default_flags Flags to retain when the env contributes nothing.
 * @return The effective flags after applying the environment.
 */
mp_flags_t mp_parse_env_flags(mp_flags_t default_flags)
{
    const char *env_conf = getenv("CMEM_CONF");
    if (!env_conf || strlen(env_conf) == 0) {
        return default_flags;
    }

    mp_flags_t flags = default_flags;

    if (strstr(env_conf, "canary=1") || strstr(env_conf, "canary=on")) {
        flags = (mp_flags_t)(flags | MP_FLAG_DEBUG_CANARY);
    }
    if (strstr(env_conf, "zero=1") || strstr(env_conf, "zero=on")) {
        flags = (mp_flags_t)(flags | MP_FLAG_ZERO_ON_ALLOC);
    }
    if (strstr(env_conf, "tls=1") || strstr(env_conf, "tls=on")) {
        flags = (mp_flags_t)(flags | MP_FLAG_THREAD_LOCAL_CACHE);
    }
    if (strstr(env_conf, "track=1") || strstr(env_conf, "track=on")) {
        flags = (mp_flags_t)(flags | MP_FLAG_TRACK_LOCATIONS);
    }
    if (strstr(env_conf, "poison=1") || strstr(env_conf, "poison=on")) {
        flags = (mp_flags_t)(flags | MP_FLAG_POISON_ON_FREE);
    }
    if (strstr(env_conf, "aligned=1") || strstr(env_conf, "aligned=on")) {
        flags = (mp_flags_t)(flags | MP_FLAG_CACHE_ALIGNED);
    }
    if (strstr(env_conf, "guard=1") || strstr(env_conf, "guard=on")) {
        flags = (mp_flags_t)(flags | MP_FLAG_GUARD_PAGES);
    }
    if (strstr(env_conf, "hugepages=1") || strstr(env_conf, "hugepages=on")) {
        flags = (mp_flags_t)(flags | MP_FLAG_HUGE_PAGES);
    }

    return flags;
}

/**
 * @brief Create a memory pool over caller-provided memory (never allocates).
 * 8-byte-aligns the buffer, stamps MP_FLAG_STATIC_BUFFER, initialises the slab
 * tier, and carves a TLSF pool out of the remainder. Used by shared-memory pools
 * and embedded/static use cases.
 * @param buffer      Memory region to manage.
 * @param buffer_size Size of buffer in bytes.
 * @param flags       Configuration bit flags.
 * @return A pool object laid out inside the buffer, or NULL if under-sized.
 */
memory_pool_t *
mp_create_from_buffer(void *buffer,
                      size_t buffer_size, // NOLINT(bugprone-easily-swappable-parameters)
                      mp_flags_t flags)
{
    if (!buffer ||
        buffer_size < sizeof(memory_pool_t) + sizeof(tlsf_pool_t) + TLSF_MIN_BLOCK_SIZE) {
        return NULL;
    }

    uintptr_t buf_addr = (uintptr_t)buffer;
    uintptr_t aligned_addr = (buf_addr + CMEM_ALIGN_MASK) & ~(uintptr_t)CMEM_ALIGN_MASK;
    size_t align_offset = aligned_addr - buf_addr;

    if (buffer_size <=
        align_offset + sizeof(memory_pool_t) + sizeof(tlsf_pool_t) + TLSF_MIN_BLOCK_SIZE) {
        return NULL;
    }

    flags = mp_parse_env_flags(cmem_resolve_default_flags(flags));

    memory_pool_t *pool = (memory_pool_t *)aligned_addr;
    memset(pool, 0, sizeof(memory_pool_t));
    pthread_mutex_init(&pool->arena_registry_lock, NULL);
    pool->flags = (mp_flags_t)(flags | MP_FLAG_STATIC_BUFFER);
    (void)snprintf(pool->arena_name, sizeof(pool->arena_name), "StaticBufferArena");

    slab_init(pool);

    if (flags & MP_FLAG_PERCPU_FREELIST) {
        percpu_init(pool);
    }

    uint8_t *remain_mem = (uint8_t *)aligned_addr + sizeof(memory_pool_t);
    size_t remain_sz = buffer_size - align_offset - sizeof(memory_pool_t) - sizeof(tlsf_pool_t);

    pool->tlsf_root = tlsf_create_pool_custom(pool, remain_sz, remain_mem);
    if (!pool->tlsf_root) {
        return NULL;
    }

    CMEM_ATOMIC_STORE(&pool->total_pool_size, buffer_size, CMEM_ORDER_RELAXED);
    return pool;
}

/**
 * @brief Set the human-readable name of an arena.
 * @param name Name text (copied, truncated to arena_name capacity).
 * @param The memory pool.
 */
void mp_set_name(memory_pool_t *pool, const char *name)
{
    if (!pool || !name) {
        return;
    }
    pool_lock(pool);
    (void)snprintf(pool->arena_name, sizeof(pool->arena_name), "%s", name);
    pool_unlock(pool);
}

/**
 * @brief Return the human-readable name of an arena.
 * @return The arena name string, or NULL when pool is NULL.
 */
const char *mp_get_name(memory_pool_t *pool)
{
    if (!pool) {
        return NULL;
    }
    return pool->arena_name;
}

/**
 * @brief Return the parent arena of a pool.
 * @return Parent pool, or NULL when pool is NULL or an arena root.
 */
memory_pool_t *mp_get_parent(memory_pool_t *pool)
{
    if (!pool) {
        return NULL;
    }
    return pool->parent;
}

/**
 * @brief Count the direct child arenas of a pool.
 * Walks the child sibling list under the read lock.
 * @return Number of direct child arenas (0 when NULL).
 */
size_t mp_get_child_count(memory_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    pool_rdlock(pool);
    size_t count = 0;
    memory_pool_t *child = pool->first_child;
    while (child) {
        count++;
        child = child->next_sibling;
    }
    pool_rdunlock(pool);
    return count;
}

/**
 * @brief Compute the current memory-pressure ratio of a pool.
 * active_bytes / max_memory_limit (or total_pool_size when no limit), clamped to
 * [0.0, 1.0].
 * @return Pressure ratio in [0.0, 1.0].
 */
double mp_pressure(memory_pool_t *pool)
{
    if (!pool) {
        return 0.0;
    }
    pool_rdlock(pool);
    double ratio = 0.0;
    if (pool->stats.max_memory_limit > 0) {
        ratio = (double)CMEM_ATOMIC_LOAD(&pool->active_bytes, CMEM_ORDER_RELAXED) /
                (double)pool->stats.max_memory_limit;
    } else if (CMEM_ATOMIC_LOAD(&pool->total_pool_size, CMEM_ORDER_RELAXED) > 0) {
        ratio = (double)CMEM_ATOMIC_LOAD(&pool->active_bytes, CMEM_ORDER_RELAXED) /
                (double)CMEM_ATOMIC_LOAD(&pool->total_pool_size, CMEM_ORDER_RELAXED);
    }
    pool_rdunlock(pool);
    if (ratio < 0.0) {
        ratio = 0.0;
    }
    if (ratio > 1.0) {
        ratio = 1.0;
    }
    return ratio;
}

/**
 * @brief Estimate bytes of freeable, fully-empty slab pages.
 * Counts only pages whose every slot is free.
 * @param pool The memory pool.
 * @return Summed SLAB_PAGE_SIZE for each completely free page.
 */
size_t mp_freeable(memory_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    pool_rdlock(pool);
    size_t freeable_bytes = 0;

    for (int cls = 0; cls < SLAB_CLASS_COUNT; cls++) {
        mp_slab_class_t *sc = &pool->slab_classes[cls];
        mp_slab_page_t *curr = sc->partial_pages;

        while (curr) {
            if (curr->free_count == curr->total_slots) {
                freeable_bytes += SLAB_PAGE_SIZE;
            }
            curr = curr->next;
        }
    }

    pool_rdunlock(pool);
    return freeable_bytes;
}

/**
 * @brief Return the total resident size of a pool.
 * @return CMEM_ATOMIC_LOAD(&pool->total_pool_size, CMEM_ORDER_RELAXED), or 0 when pool is NULL.
 */
size_t mp_resident(memory_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    pool_rdlock(pool);
    size_t res = CMEM_ATOMIC_LOAD(&pool->total_pool_size, CMEM_ORDER_RELAXED);
    pool_rdunlock(pool);
    return res;
}

/**
 * @brief Re-read CMEM_CONF and update the pool flags if they changed.
 * Bumps env_flags_generation when the flags change; otherwise leaves them intact.
 * @param pool The memory pool.
 * @return The new flag set.
 */
mp_flags_t mp_reparse_env_flags(memory_pool_t *pool)
{
    if (!pool) {
        return (mp_flags_t)0;
    }
    mp_flags_t new_flags = mp_parse_env_flags(pool->flags);
    if (new_flags == pool->flags) {
        return pool->flags;
    }

    pool_lock(pool);
    pool->flags = new_flags;
    pool->env_flags_generation++;
    pool_unlock(pool);

    return pool->flags;
}

/**
 * @brief Return how many times the environment flags were re-parsed.
 * @return Generation counter (0 when pool is NULL).
 */
uint64_t mp_get_env_generation(memory_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    return pool->env_flags_generation;
}

/**
 * @brief Trigger auto-compaction whenever pressure/fragmentation exceed thresholds.
 * Runs at most once per second to avoid thrashing.
 * @return True if compaction ran, false when disabled or below thresholds/NULL.
 */
bool mp_auto_compact_check(memory_pool_t *pool)
{
    if (!pool || !pool->auto_compact_enabled) {
        return false;
    }

    pool_lock(pool);
    double pressure = (double)CMEM_ATOMIC_LOAD(&pool->active_bytes, CMEM_ORDER_RELAXED) /
                      (double)(pool->stats.max_memory_limit > 0
                                   ? pool->stats.max_memory_limit
                                   : CMEM_ATOMIC_LOAD(&pool->total_pool_size, CMEM_ORDER_RELAXED));
    double frag = pool->stats.fragmentation_ratio;
    pool_unlock(pool);

    if (pressure > pool->auto_compact_pressure_threshold ||
        frag > pool->auto_compact_fragmentation_threshold) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        pool_lock(pool);
        if (now.tv_sec - pool->last_auto_compact_time.tv_sec < 1) {
            pool_unlock(pool);
            return false;
        }
        pool->last_auto_compact_time = now;
        pool_unlock(pool);

        mp_compact(pool);
        return true;
    }
    return false;
}

/**
 * @brief Verify the pool's active bytes are within its arena quota.
 * Always returns true when no quota is configured.
 * @return True within quota (or quota unlimited), false when exceeded.
 */
bool mp_check_arena_quota(memory_pool_t *pool)
{
    if (!pool || pool->arena_quota_limit == 0) {
        return true;
    }
    pool_rdlock(pool);
    bool ok = CMEM_ATOMIC_LOAD(&pool->active_bytes, CMEM_ORDER_RELAXED) <= pool->arena_quota_limit;
    pool_rdunlock(pool);
    return ok;
}

/**
 * @brief Set whether allocation should fall back to the OS on OOM.
 * @param pool   The memory pool.
 * @param enable When set, OOM allocations go via sys_mem_alloc.
 */
void mp_set_fallback_on_oom(memory_pool_t *pool, bool enable)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    pool->fallback_to_sys_alloc_on_oom = enable;
    pool_unlock(pool);
}

/**
 * @brief Install the garbage-collection pressure callback.
 * @param pool      The memory pool.
 * @param cb        Callback invoked under allocation pressure.
 * @param user_data Opaque data passed to cb.
 */
void mp_set_gc_callback(memory_pool_t *pool, mp_watermark_callback_t cb, void *user_data)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    pool->gc_cb = cb;
    pool->gc_user_data = user_data;
    pool_unlock(pool);
}

/**
 * @brief Install the eviction pressure callback.
 * @param pool      The memory pool.
 * @param cb        Callback invoked under eviction pressure.
 * @param user_data Opaque data passed to cb.
 */
void mp_set_eviction_callback(memory_pool_t *pool, mp_watermark_callback_t cb, void *user_data)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    pool->eviction_cb = cb;
    pool->eviction_user_data = user_data;
    pool_unlock(pool);
}

/**
 * @brief Record one allocation-latency sample into a power-of-two histogram.
 * @param pool       The memory pool.
 * @param latency_ns Sample latency in nanoseconds.
 */
void mp_record_latency(memory_pool_t *pool, uint64_t latency_ns)
{
    if (!pool) {
        return;
    }
#if !defined(CMEM_DISABLE_DIAGNOSTICS)
    pool_lock(pool);
    pool->alloc_latency_sum_ns += latency_ns;
    pool->alloc_latency_count++;

    size_t idx = 0;
    uint64_t val = latency_ns;
    while (val >= 1024 && idx < CMEM_LATENCY_HISTOGRAM_MAX_INDEX) {
        val >>= 1;

        idx++;
    }
    pool->alloc_latency_histogram[idx]++;
    pool_unlock(pool);
#else
    (void)latency_ns;
#endif
}

/**
 * @brief Estimate the P99 allocation latency.
 * Walks the power-of-two histogram until 99% of samples are covered.
 * @return Upper-bound P99 latency in ns, or 0 when no sample/pool NULL.
 */
uint64_t mp_get_latency_p99(memory_pool_t *pool)
{
#if !defined(CMEM_DISABLE_DIAGNOSTICS)
    if (!pool || pool->alloc_latency_count == 0) {
        return 0;
    }
    pool_rdlock(pool);

    size_t total = pool->alloc_latency_count;
    size_t target = (total * CMEM_LATENCY_P99_NUMERATOR) / CMEM_LATENCY_P99_DENOMINATOR;
    size_t cum = 0;

    uint64_t p99_ns = 0;
    for (int i = 0; i < 32; i++) {
        cum += pool->alloc_latency_histogram[i];
        if (cum >= target) {
            p99_ns = (uint64_t)1 << i;
            break;
        }
    }
    pool_rdunlock(pool);
    return p99_ns;
#else
    (void)pool;
    return 0;
#endif
}

/**
 * @brief Compute the mean allocation latency.
 * @return Average latency in ns, or 0 when no sample/pool NULL.
 */
uint64_t mp_get_latency_avg(memory_pool_t *pool)
{
#if !defined(CMEM_DISABLE_DIAGNOSTICS)
    if (!pool || pool->alloc_latency_count == 0) {
        return 0;
    }
    pool_rdlock(pool);
    uint64_t avg = pool->alloc_latency_sum_ns / pool->alloc_latency_count;
    pool_rdunlock(pool);
    return avg;
#else
    (void)pool;
    return 0;
#endif
}

/**
 * @brief Zero the allocation-latency histogram and aggregates.
 * @param pool The memory pool.
 */
void mp_reset_latency_stats(memory_pool_t *pool)
{
    if (!pool) {
        return;
    }
#if !defined(CMEM_DISABLE_DIAGNOSTICS)
    pool_lock(pool);
    memset(pool->alloc_latency_histogram, 0, sizeof(pool->alloc_latency_histogram));
    pool->alloc_latency_count = 0;
    pool->alloc_latency_sum_ns = 0;
    pool_unlock(pool);
#endif
}

/**
 * @brief Reset the pool's cumulative runtime statistics.
 * Clears counters, peak (reset to active), window, size and latency histograms.
 * @param pool The memory pool.
 */
void mp_reset_stats(memory_pool_t *pool)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    CMEM_ATOMIC_STORE(&pool->total_alloc_ops, 0, CMEM_ORDER_RELAXED);
    CMEM_ATOMIC_STORE(&pool->total_free_ops, 0, CMEM_ORDER_RELAXED);
    CMEM_ATOMIC_STORE(&pool->peak_bytes,
                      CMEM_ATOMIC_LOAD(&pool->active_bytes, CMEM_ORDER_RELAXED),
                      CMEM_ORDER_RELAXED);
    pool->window_alloc_ops = 0;
    pool->window_alloc_bytes = 0;
    pool->window_start_time.tv_sec = 0;
    pool->window_start_time.tv_nsec = 0;
    memset(pool->stats.size_histogram, 0, sizeof(pool->stats.size_histogram));
#if !defined(CMEM_DISABLE_DIAGNOSTICS)
    memset(pool->alloc_latency_histogram, 0, sizeof(pool->alloc_latency_histogram));
    pool->alloc_latency_count = 0;
    pool->alloc_latency_sum_ns = 0;
#endif
    pool_unlock(pool);
}

/**
 * @brief Choose the ideal slot size for an allocation request.
 * Returns the smallest slab class covering size, else an 8-byte multiple.
 * @param size Requested allocation size.
 * @return Preferred slot/class size.
 */
size_t mp_preferred_size(size_t size)
{
    if (size == 0) {
        return 0;
    }
    if (size <= SLAB_MAX_SIZE) {
        for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
            if (cmem_slab_class_size[i] >= size) {
                return cmem_slab_class_size[i];
            }
        }
    }
    return (size + CMEM_ALIGN_MASK) & ~(size_t)CMEM_ALIGN_MASK;
}

/**
 * @brief Choose the best fitting size within a specific pool.
 * Uses the pool's (possibly custom) slab classes under the read lock.
 * @param pool The memory pool.
 * @param size Requested allocation size.
 * @return Preferred slot size.
 */
size_t mp_preferred_size_for_pool(memory_pool_t *pool, size_t size)
{
    if (!pool || size == 0) {
        return 0;
    }
    pool_rdlock(pool);
    if (size <= SLAB_MAX_SIZE) {
        for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
            if (pool->slab_classes[i].slot_size >= size) {
                pool_rdunlock(pool);
                return pool->slab_classes[i].slot_size;
            }
        }
    }
    pool_rdunlock(pool);
    return (size + CMEM_ALIGN_MASK) & ~(size_t)CMEM_ALIGN_MASK;
}

/**
 * @brief Override the slab size classes used by a pool.
 * Requires strictly increasing sizes; unsupported remainder slots are doubled.
 * @param pool  The memory pool.
 * @param sizes Array of slot sizes in bytes.
 * @param count Number of classes (<= SLAB_CLASS_COUNT).
 * @return true on success, false on invalid argument or non-increasing sizes.
 */
bool mp_set_slab_classes(memory_pool_t *pool, const size_t *sizes, size_t count)
{
    if (!pool || !sizes || count == 0 || count > SLAB_CLASS_COUNT) {
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        if (sizes[i] == 0 || (i > 0 && sizes[i] <= sizes[i - 1])) {
            return false;
        }
    }

    pool_lock(pool);
    pool->use_custom_slab_sizes = true;
    for (size_t i = 0; i < count; i++) {
        pool->custom_slab_sizes[i] = sizes[i];
    }
    for (size_t i = count; i < SLAB_CLASS_COUNT; i++) {
        pool->custom_slab_sizes[i] = pool->custom_slab_sizes[i - 1] * 2;
    }

    for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
        pool->slab_classes[i].slot_size = pool->custom_slab_sizes[i];
    }

    pool_unlock(pool);
    return true;
}

/**
 * @brief Return how many slab size classes the pool uses.
 * @param pool The memory pool.
 * @return Class count, or 0 when pool is NULL.
 */
size_t mp_get_slab_class_count(memory_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    pool_rdlock(pool);
    size_t count = SLAB_CLASS_COUNT;
    pool_rdunlock(pool);
    return count;
}

/**
 * @brief Copy the pool's active slab class sizes into an output array.
 * @param pool The memory pool.
 * @param out_sizes Receives the class sizes (may be NULL to query count).
 * @param max_count Capacity of out_sizes.
 * @return Number of sizes written, or 0 on invalid arguments.
 */
size_t mp_get_slab_classes(memory_pool_t *pool, size_t *out_sizes, size_t max_count)
{
    if (!pool || !out_sizes || max_count == 0) {
        return 0;
    }
    pool_rdlock(pool);
    size_t count = (max_count < SLAB_CLASS_COUNT) ? max_count : SLAB_CLASS_COUNT;
    for (size_t i = 0; i < count; i++) {
        out_sizes[i] = pool->slab_classes[i].slot_size;
    }
    pool_rdunlock(pool);
    return count;
}

/**
 * @brief Tear down a pool, releasing all of its resident memory.
 * Destroys any child arenas recursively, unmaps slab pages and TLSF arenas,
 * then frees the slab class locks, emergency reserve, per-CPU freelists and
 * the pool structure itself.
 *
 * @param pool Pool to destroy; now invalid afterwards.
 */
