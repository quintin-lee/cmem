/**
 * @file cmem_event.c
 * @brief Event, tracking, arena-lifecycle and extended allocation subsystems.
 *
 * Implements the public cmem API surface beyond the core tiers: dirty-state and
 * fault isolation, per-thread quota and circuit breaker, cgroup awareness, ASan
 * integration, a bounded SPSC ring buffer used as an event log, pprof export,
 * typed object pools, process-shared and buffer-backed arenas, latency
 * histogram instrumentation, slab class tuning, and a double-buffer frame arena
 * for automatic bulk reclamation. Heavy allocation entry points (alloc/free/
 * realloc/strdup/asprintf) live here and dispatch into the slab/TLSF/OS tiers.
 */

#include "cmem.h"
#include "cmem_internal.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>

/* Named constants for readability- and performance-sensitive literals. */
#define CMEM_ALIGN_MASK 7u                   /* Round-up mask for 8-byte alignment      */
#define CMEM_DECIMAL_BASE 10                 /* Radix used by strtoull for plain base-10 */
#define CMEM_SHM_OPEN_MODE 0666              /* Permission bits for created shm segments */
#define CMEM_NSEC_PER_SEC_U64 1000000000ULL  /* Nanoseconds per second                  */
#define CMEM_LATENCY_HISTOGRAM_MAX_INDEX 31u /* Highest valid latency histogram bucket  */
#define CMEM_LATENCY_P99_NUMERATOR 99u       /* Numerator of the P99 latency target      */
#define CMEM_LATENCY_P99_DENOMINATOR 100u    /* Denominator of the P99 latency target    */
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
    if (header->magic != MP_MAGIC_HEAD) {
        return false;
    }

    pool_lock(pool);
    active_list_remove(pool, header);
    pool->stats.active_bytes -= header->requested_size;
    pool->stats.active_allocations--;
    pool->stats.total_free_ops++;
    header->magic = CMEM_FREED_MAGIC;
    pool_unlock(pool);

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
                                pool->stats.active_bytes,
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

/**
 * @brief Create a fixed-capacity single-producer/single-consumer ring buffer.
 * Capacity is rounded up to a power of two; payload storage is 64-byte aligned.
 * @param slot_size Size in bytes of each slot.
 * @param capacity  Requested slot count (rounded up).
 * @return A new ring buffer, or NULL on invalid input or allocation failure.
 */
cmem_ring_buffer_t *mp_ring_create(size_t slot_size, size_t capacity)
{
    if (slot_size == 0 || capacity == 0) {
        return NULL;
    }
    size_t real_cap = 1;
    while (real_cap < capacity) {
        real_cap <<= 1;
    }

    cmem_ring_buffer_t *ring = (cmem_ring_buffer_t *)calloc(1, sizeof(cmem_ring_buffer_t));
    if (!ring) {
        return NULL;
    }

    ring->slot_size = slot_size;
    ring->capacity = real_cap;
    ring->mask = real_cap - 1;
    CMEM_ATOMIC_INIT(&ring->head, 0);
    CMEM_ATOMIC_INIT(&ring->tail, real_cap);

    ring->slots = (void **)calloc(real_cap, sizeof(void *));
    size_t total_buf = slot_size * real_cap;
#ifdef _WIN32
    ring->buffer = cmem_aligned_malloc(total_buf, 64);
    if (!ring->buffer)
#else
    if (posix_memalign(&ring->buffer, 64, total_buf) != 0)
#endif
    {
        free((void *)ring->slots);
        free((void *)ring->buffer);
        free(ring);
        return NULL;
    }

    uint8_t *base = (uint8_t *)ring->buffer;
    for (size_t i = 0; i < real_cap; i++) {
        ring->slots[i] = base + i * slot_size;
    }

    return ring;
}

/**
 * @brief Claim the next producer slot from a ring buffer.
 * @param ring The ring buffer.
 * @return Pointer to the claimed slot, or NULL when full or ring is NULL.
 */
void *mp_ring_alloc(cmem_ring_buffer_t *ring)
{
    if (!ring) {
        return NULL;
    }
    size_t head = CMEM_ATOMIC_FETCH_ADD(&ring->head, 1, CMEM_ORDER_RELAXED);
    size_t tail = CMEM_ATOMIC_LOAD(&ring->tail, CMEM_ORDER_ACQUIRE);

    if (head >= tail) {
        return NULL;
    }

    return ring->slots[head & ring->mask];
}

/**
 * @brief Return a slot to the consumer end of a ring buffer.
 * @param ring The ring buffer.
 * @param ptr  Slot pointer to release back to the consumer tail.
 * @return True on success, false on invalid arguments.
 */
bool mp_ring_free(cmem_ring_buffer_t *ring, void *ptr)
{
    if (!ring || !ptr) {
        return false;
    }
    size_t tail = CMEM_ATOMIC_FETCH_ADD(&ring->tail, 1, CMEM_ORDER_RELEASE);
    ring->slots[tail & ring->mask] = ptr;
    return true;
}

/**
 * @brief Destroy a ring buffer and release its backing storage.
 * @param ring The ring buffer. No-op when NULL.
 */
void mp_ring_destroy(cmem_ring_buffer_t *ring)
{
    if (!ring) {
        return;
    }
    if (ring->slots) {
        free((void *)ring->slots);
    }
    if (ring->buffer) {
#ifdef _WIN32
        cmem_aligned_free(ring->buffer);
#else
        free(ring->buffer);
#endif
    }
    free(ring);
}

/**
 * @brief Create an event log backed by a ring buffer.
 * @param capacity Maximum entry count the ring may hold.
 * @return A new event log, or NULL on allocation failure.
 */
mp_event_log_t *mp_event_log_create(size_t capacity)
{
    mp_event_log_t *log = (mp_event_log_t *)calloc(1, sizeof(mp_event_log_t));
    if (!log) {
        return NULL;
    }

    log->ring = mp_ring_create(sizeof(mp_event_log_entry_t), capacity);
    if (!log->ring) {
        free(log);
        return NULL;
    }
    log->capacity = capacity;
    CMEM_ATOMIC_INIT(&log->count, 0);
    return log;
}

/**
 * @brief Destroy an event log and release its resources.
 * @param log The event log. No-op when NULL.
 */
void mp_event_log_destroy(mp_event_log_t *log)
{
    if (!log) {
        return;
    }
    if (log->ring) {
        mp_ring_destroy(log->ring);
    }
    free(log);
}

/**
 * @brief Append an event entry to an event log.
 * Timestamps the entry with CLOCK_MONOTONIC and pushes it into the ring.
 * @param log        The event log.
 * @param event_type Event class (see mp_event_type_t).
 * @param ptr        Optional pointer associated with the event.
 * @param size       Optional size associated with the event.
 * @return True on success, false when the ring is full or args invalid.
 */
bool mp_event_log_record(mp_event_log_t *log, mp_event_type_t event_type, void *ptr, size_t size)
{
    if (!log || !log->ring) {
        return false;
    }

    mp_event_log_entry_t *entry = (mp_event_log_entry_t *)mp_ring_alloc(log->ring);
    if (!entry) {
        return false;
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    entry->timestamp_ns = (uint64_t)ts.tv_sec * CMEM_NSEC_PER_SEC_U64 + (uint64_t)ts.tv_nsec;
    entry->event_type = event_type;
    entry->size = size;
    entry->ptr = (uintptr_t)ptr;

    CMEM_ATOMIC_FETCH_ADD(&log->count, 1, CMEM_ORDER_RELAXED);
    return true;
}

/**
 * @brief Pop the oldest event record into the caller's buffer.
 * @param log   The event log.
 * @param entry Receives a copy of the next record.
 * @return True when a record is consumed, false when empty or args invalid.
 */
bool mp_event_log_consume(mp_event_log_t *log, mp_event_log_entry_t *entry)
{
    if (!log || !log->ring || !entry) {
        return false;
    }

    mp_event_log_entry_t *slot = (mp_event_log_entry_t *)mp_ring_alloc(log->ring);
    if (!slot) {
        return false;
    }

    *entry = *slot;
    CMEM_ATOMIC_FETCH_SUB(&log->count, 1, CMEM_ORDER_RELAXED);
    return true;
}

/**
 * @brief Return the number of unread event records.
 * @param log The event log.
 * @return Count of pending records (0 when NULL).
 */
size_t mp_event_log_pending(mp_event_log_t *log)
{
    if (!log) {
        return 0;
    }
    return (size_t)CMEM_ATOMIC_LOAD(&log->count, CMEM_ORDER_ACQUIRE);
}

/**
 * @brief Drop all recorded events and reallocate a fresh ring.
 * @param log The event log. No-op when NULL.
 */
void mp_event_log_clear(mp_event_log_t *log)
{
    if (!log) {
        return;
    }
    CMEM_ATOMIC_STORE(&log->count, 0, CMEM_ORDER_RELAXED);
    mp_ring_destroy(log->ring);
    log->ring = mp_ring_create(sizeof(mp_event_log_entry_t), log->capacity);
}

/**
 * @brief Render pool statistics in the pprof text profile format.
 * Emits heap, alloc_objects, alloc_space, inuse_objects, inuse_space and peak_space rows.
 * @param pool    The memory pool.
 * @param out_buf Caller-owned output buffer.
 * @param max_len Capacity of out_buf.
 * @return Bytes written (excluding NUL), max_len on truncation, or 0 on invalid args.
 */
size_t mp_export_pprof(memory_pool_t *pool, char *out_buf, size_t max_len)
{
    if (!pool || !out_buf || max_len == 0) {
        return 0;
    }

    size_t total_alloc = pool->stats.total_alloc_ops;
    size_t active = pool->stats.active_allocations;
    size_t active_bytes = pool->stats.active_bytes;
    size_t peak_bytes = pool->stats.peak_bytes;

    int written = snprintf(out_buf,
                           max_len,
                           "heap: %zu %zu\n"
                           "alloc_objects: total %zu\n"
                           "alloc_space: total %zu\n"
                           "inuse_objects: %zu\n"
                           "inuse_space: %zu\n"
                           "peak_space: %zu\n",
                           active_bytes,
                           active,
                           total_alloc,
                           total_alloc > 0 ? pool->stats.total_alloc_ops * sizeof(void *) : 0,
                           active,
                           active_bytes,
                           peak_bytes);

    if (written < 0 || (size_t)written >= max_len) {
        return written < 0 ? 0 : max_len;
    }
    return (size_t)written;
}

/**
 * @brief Create a fixed-size typed object pool.
 * Element size is padded up to sizeof(void*) and 8-byte aligned; capacity entries
 * are pre-linked onto an intrusive free list.
 * @param elem_size Size of each object in bytes (padded up).
 * @param capacity  Number of objects the pool holds.
 * @return A new typed pool, or NULL on invalid input or allocation failure.
 */
mp_typed_pool_t *mp_typed_pool_create(size_t elem_size, size_t capacity)
{
    if (elem_size == 0 || capacity == 0) {
        return NULL;
    }
    size_t real_elem_sz = (elem_size < sizeof(void *)) ? sizeof(void *) : elem_size;

    real_elem_sz = (real_elem_sz + CMEM_ALIGN_MASK) & ~(size_t)CMEM_ALIGN_MASK;

    mp_typed_pool_t *tpool = (mp_typed_pool_t *)calloc(1, sizeof(mp_typed_pool_t));
    if (!tpool) {
        return NULL;
    }

    tpool->elem_size = real_elem_sz;
    tpool->capacity = capacity;
    tpool->active_count = 0;

    size_t total_sz = real_elem_sz * capacity;
#ifdef _WIN32
    tpool->raw_buf = cmem_aligned_malloc(total_sz, 64);
    if (!tpool->raw_buf)
#else
    if (posix_memalign(&tpool->raw_buf, 64, total_sz) != 0)
#endif
    {
        free(tpool);
        return NULL;
    }

    uint8_t *base = (uint8_t *)tpool->raw_buf;
    tpool->free_list = base;

    for (size_t i = 0; i < capacity - 1; i++) {
        void **curr = (void **)(base + i * real_elem_sz);
        *curr = base + (i + 1) * real_elem_sz;
    }
    void **last = (void **)(base + (capacity - 1) * real_elem_sz);
    *last = NULL;

    return tpool;
}

/**
 * @brief Pop one object from a typed pool's free list.
 * @param tpool The typed pool.
 * @return An object pointer, or NULL when empty or tpool is NULL.
 */
void *mp_typed_alloc(mp_typed_pool_t *tpool)
{
    if (!tpool || !tpool->free_list) {
        return NULL;
    }

    void *ptr = tpool->free_list;
    tpool->free_list = *(void **)ptr;
    tpool->active_count++;
    return ptr;
}

/**
 * @brief Push an object back onto a typed pool's free list.
 * @param tpool The typed pool.
 * @param ptr   Object previously allocated from the pool.
 */
void mp_typed_free(mp_typed_pool_t *tpool, void *ptr)
{
    if (!tpool || !ptr) {
        return;
    }

    *(void **)ptr = tpool->free_list;
    tpool->free_list = ptr;
    if (tpool->active_count > 0) {
        tpool->active_count--;
    }
}

/**
 * @brief Destroy a typed pool and release its object buffer.
 * @param tpool The typed pool. No-op when NULL.
 */
void mp_typed_pool_destroy(mp_typed_pool_t *tpool)
{
    if (!tpool) {
        return;
    }
    if (tpool->raw_buf) {
#ifdef _WIN32
        cmem_aligned_free(tpool->raw_buf);
#else
        free(tpool->raw_buf);
#endif
    }
    free(tpool);
}

/**
 * @brief Create a default memory pool with no custom system allocator.
 * Thin wrapper for mp_create_custom with a NULL sys-alloc vtable.
 * @param initial_capacity Initial/expansion capacity hint in bytes.
 * @param flags            Configuration bitmask (see mp_flags_t).
 * @return A new memory pool, or NULL on failure.
 */
memory_pool_t *mp_create(size_t initial_capacity, mp_flags_t flags)
{
    return mp_create_custom(initial_capacity, flags, NULL);
}

/**
 * @brief Create (or attach to) a shared-memory-backed pool (POSIX only).
 * Opens/creates a POSIX shared-memory object, sizes it, and lays out the pool
 * inside it; the pool lock is configured process-shared when MP_FLAG_THREAD_SAFE
 * is set. Unsupported on Windows (returns NULL).
 * @param shm_name POSIX shm object name (e.g. "/cmem0").
 * @param capacity Minimum mapping size; clamped to 1 MiB when smaller than 64 KiB.
 * @param flags    Configuration bit flags.
 * @return A shared memory pool, or NULL on failure.
 */
memory_pool_t *mp_create_shared(const char *shm_name, size_t capacity, mp_flags_t flags)
{
    (void)shm_name;
    (void)capacity;
    (void)flags;
#ifdef _WIN32
    return NULL;
#else
    if (!shm_name || capacity < 64 * 1024) {
        capacity = 1024 * 1024;
    }

    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, CMEM_SHM_OPEN_MODE);
    if (shm_fd == -1) {
        return NULL;
    }

    if (ftruncate(shm_fd, (off_t)capacity) == -1) {
        close(shm_fd);
        return NULL;
    }

    void *shm_ptr = mmap(NULL, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);

    if (shm_ptr == MAP_FAILED) {
        return NULL;
    }

    memory_pool_t *pool =
        mp_create_from_buffer(shm_ptr, capacity, (mp_flags_t)(flags | MP_FLAG_SHARED_MEMORY));
    if (pool) {
        (void)snprintf(pool->arena_name, sizeof(pool->arena_name), "SharedIPC[%s]", shm_name);
        if (flags & MP_FLAG_THREAD_SAFE) {
            pthread_rwlock_destroy(&pool->rwlock);
            pthread_mutex_destroy(&pool->lock);

            pthread_rwlockattr_t rwattr;
            pthread_rwlockattr_init(&rwattr);
            pthread_rwlockattr_setpshared(&rwattr, PTHREAD_PROCESS_SHARED);
            pthread_rwlock_init(&pool->rwlock, &rwattr);
            pthread_rwlockattr_destroy(&rwattr);

            pthread_mutexattr_t mattr;
            pthread_mutexattr_init(&mattr);
            pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
            pthread_mutex_init(&pool->lock, &mattr);
            pthread_mutexattr_destroy(&mattr);
        }
    }
    return pool;
#endif
}

/**
 * @brief Detach and unlink a shared-memory pool.
 * Unmaps the pool mapping range and unlinks the named shm object. No-op on Windows.
 * @param pool     The shared pool.
 * @param shm_name The shm object to unlink (NULL skips unlink).
 */
void mp_destroy_shared(memory_pool_t *pool, const char *shm_name)
{
    if (!pool) {
        return;
    }
    tls_cache_flush_pool(pool);
#ifdef _WIN32
    (void)shm_name;
#else
    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_rwlock_destroy(&pool->rwlock);
        pthread_mutex_destroy(&pool->lock);
    }
    size_t sz = pool->stats.total_pool_size;
    void *shm_ptr = (void *)pool;
    munmap(shm_ptr, sz);
    if (shm_name) {
        shm_unlink(shm_name);
    }
#endif
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

    memory_pool_t *pool = (memory_pool_t *)aligned_addr;
    memset(pool, 0, sizeof(memory_pool_t));
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

    pool->stats.total_pool_size = buffer_size;
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
        ratio = (double)pool->stats.active_bytes / (double)pool->stats.max_memory_limit;
    } else if (pool->stats.total_pool_size > 0) {
        ratio = (double)pool->stats.active_bytes / (double)pool->stats.total_pool_size;
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
 * @return pool->stats.total_pool_size, or 0 when pool is NULL.
 */
size_t mp_resident(memory_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    pool_rdlock(pool);
    size_t res = pool->stats.total_pool_size;
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
    double pressure = (double)pool->stats.active_bytes /
                      (double)(pool->stats.max_memory_limit > 0 ? pool->stats.max_memory_limit
                                                                : pool->stats.total_pool_size);
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
    bool ok = pool->stats.active_bytes <= pool->arena_quota_limit;
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
}

/**
 * @brief Estimate the P99 allocation latency.
 * Walks the power-of-two histogram until 99% of samples are covered.
 * @return Upper-bound P99 latency in ns, or 0 when no sample/pool NULL.
 */
uint64_t mp_get_latency_p99(memory_pool_t *pool)
{
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
}

/**
 * @brief Compute the mean allocation latency.
 * @return Average latency in ns, or 0 when no sample/pool NULL.
 */
uint64_t mp_get_latency_avg(memory_pool_t *pool)
{
    if (!pool || pool->alloc_latency_count == 0) {
        return 0;
    }
    pool_rdlock(pool);
    uint64_t avg = pool->alloc_latency_sum_ns / pool->alloc_latency_count;
    pool_rdunlock(pool);
    return avg;
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
    pool_lock(pool);
    memset(pool->alloc_latency_histogram, 0, sizeof(pool->alloc_latency_histogram));
    pool->alloc_latency_count = 0;
    pool->alloc_latency_sum_ns = 0;
    pool_unlock(pool);
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
    pool->stats.total_alloc_ops = 0;
    pool->stats.total_free_ops = 0;
    pool->stats.peak_bytes = pool->stats.active_bytes;
    pool->window_alloc_ops = 0;
    pool->window_alloc_bytes = 0;
    pool->window_start_time.tv_sec = 0;
    pool->window_start_time.tv_nsec = 0;
    memset(pool->stats.size_histogram, 0, sizeof(pool->stats.size_histogram));
    memset(pool->alloc_latency_histogram, 0, sizeof(pool->alloc_latency_histogram));
    pool->alloc_latency_count = 0;
    pool->alloc_latency_sum_ns = 0;
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
            if (kSlabSizes[i] >= size) {
                return kSlabSizes[i];
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
void mp_destroy(memory_pool_t *pool)
{
    if (!pool) {
        return;
    }

    tls_cache_flush_pool(pool);
    percpu_flush(pool);

    if (pool->flags & MP_FLAG_REPORT_LEAKS_ON_DESTROY) {
        mp_check_leaks(pool);
    }

    memory_pool_t *child = pool->first_child;
    while (child) {
        memory_pool_t *next = child->next_sibling;
        mp_destroy(child);
        child = next;
    }

    if (!(pool->flags & MP_FLAG_STATIC_BUFFER)) {
        for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
            mp_slab_page_t *curr = pool->slab_classes[i].partial_pages;
            while (curr) {
                mp_slab_page_t *next = curr->next;
#ifdef _WIN32
                cmem_aligned_free(curr->page_raw_mem);
#else
                cmem_munmap(curr->page_raw_mem, SLAB_PAGE_SIZE);
#endif
                curr = next;
            }
            curr = pool->slab_classes[i].full_pages;
            while (curr) {
                mp_slab_page_t *next = curr->next;
#ifdef _WIN32
                cmem_aligned_free(curr->page_raw_mem);
#else
                cmem_munmap(curr->page_raw_mem, SLAB_PAGE_SIZE);
#endif
                curr = next;
            }
        }

        tlsf_pool_t *tcurr = pool->tlsf_root;
        while (tcurr) {
            tlsf_pool_t *tnext = tcurr->next;
            sys_mem_free(pool, tcurr, tcurr->raw_size + sizeof(tlsf_pool_t));
            tcurr = tnext;
        }

        for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
            pthread_mutex_destroy(&pool->slab_classes[i].lock);
        }

        if (pool->flags & MP_FLAG_THREAD_SAFE) {
            pthread_rwlock_destroy(&pool->rwlock);
            pthread_mutex_destroy(&pool->lock);
        }

        if (pool->emergency_buf) {
            free(pool->emergency_buf);
        }

        percpu_destroy(pool);

        free(pool);
    }
}

/**
 * @brief Return the pool to a fresh state without destroying it.
 * Clears all statistics, releases every slab slot back to its page lists and
 * re-seeds the TLSF arenas so the pool can be reused.
 * @param pool The memory pool.
 */
void mp_reset(memory_pool_t *pool)
{
    if (!pool) {
        return;
    }
    tls_cache_flush_pool(pool);
    percpu_flush(pool);
    pool_lock(pool);

    memory_pool_t *child = pool->first_child;
    while (child) {
        mp_reset(child);
        child = child->next_sibling;
    }

    pool->stats.active_bytes = 0;
    pool->stats.active_allocations = 0;
    pool->stats.slab_allocated_bytes = 0;
    pool->stats.tlsf_allocated_bytes = 0;
    pool->stats.os_allocated_bytes = 0;
    pool->active_head = NULL;

    for (int cls = 0; cls < SLAB_CLASS_COUNT; cls++) {
        mp_slab_class_t *sc = &pool->slab_classes[cls];

        while (sc->full_pages) {
            mp_slab_page_t *node = sc->full_pages;
            sc->full_pages = node->next;
            node->next = sc->partial_pages;

            if (sc->partial_pages) {
                sc->partial_pages->prev = node;
            }

            node->prev = NULL;
            sc->partial_pages = node;
        }

        mp_slab_page_t *page = sc->partial_pages;
        size_t slot_payload_size = pool->slab_classes[cls].slot_size;
        size_t header_overhead = sizeof(mp_block_header_t);
        size_t total_slot_size =

            header_overhead + slot_payload_size + ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0);
        total_slot_size = (total_slot_size + CMEM_ALIGN_MASK) & ~(size_t)CMEM_ALIGN_MASK;

        while (page) {
            page->free_count = page->total_slots;
            page->idle_since_ts = 0;
            uint8_t *ptr = (uint8_t *)page->page_raw_mem + sizeof(mp_slab_page_t);
            page->free_list = (mp_slab_slot_t *)ptr;

            for (uint16_t i = 0; i < page->total_slots; i++) {
                mp_slab_slot_t *slot = (mp_slab_slot_t *)(ptr + i * total_slot_size);
                slot->next = (i < page->total_slots - 1)
                                 ? (mp_slab_slot_t *)(ptr + (i + 1) * total_slot_size)
                                 : NULL;
            }
            page = page->next;
        }
    }

    tlsf_pool_t *tcurr = pool->tlsf_root;
    while (tcurr) {
        memset(tcurr->sl_bitmap, 0, sizeof(tcurr->sl_bitmap));
        tcurr->fl_bitmap = 0;
        memset(tcurr->blocks, 0, sizeof(tcurr->blocks));

        tlsf_block_t *block = (tlsf_block_t *)tcurr->raw_area;
        block->size_and_flags = (tcurr->raw_size - sizeof(tlsf_block_t)) | BLOCK_STATE_FREE;
        block->prev_physical = NULL;
        block->next_free = NULL;
        block->prev_free = NULL;

        tlsf_block_t *sentinel =
            (tlsf_block_t *)((uint8_t *)block + (block->size_and_flags & BLOCK_SIZE_MASK));
        sentinel->size_and_flags = 0;
        sentinel->prev_physical = block;

        tlsf_insert_free_block(tcurr, block);
        tcurr = tcurr->next;
    }

    trigger_event(pool, MP_EVENT_RESET, NULL, 0);
    pool_unlock(pool);
}

/**
 * @brief Configure idle page reclamation for the pool.
 *
 * When enabled, completely-free or long-idle slab pages are eligible for
 * reclamation on mp_compact or mp_reclaim_idle_pages.
 *
 * @param pool Pointer to the memory pool
 * @param enable true to enable, false to disable
 * @param timeout_ms Idle timeout in milliseconds (0 = reclaim immediately when idle)
 * @param min_pages Minimum idle pages before reclamation is allowed
 */
void mp_set_idle_page_reclaim(memory_pool_t *pool,
                              bool enable,
                              uint64_t timeout_ms,
                              size_t min_pages)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    pool->idle_reclaim_enabled = enable;
    pool->idle_reclaim_timeout_ms = timeout_ms;
    pool->idle_reclaim_min_pages = min_pages;
    pool_unlock(pool);
}

/**
 * @brief Reclaim idle slab pages that have exceeded the configured timeout.
 *
 * Scans partial page lists for pages that are either completely free or
 * have been idle longer than the pool's idle_reclaim_timeout_ms.
 *
 * @param pool Pointer to the memory pool
 * @return Number of bytes reclaimed
 */
size_t mp_reclaim_idle_pages(memory_pool_t *pool)
{
    if (!pool || (pool->flags & MP_FLAG_STATIC_BUFFER)) {
        return 0;
    }
    tls_cache_flush_pool(pool);
    percpu_flush(pool);
    pool_lock(pool);

    size_t freed_bytes = 0;
    int64_t now = cmem_now_ms();
    size_t idle_count = 0;

    for (int cls = 0; cls < SLAB_CLASS_COUNT; cls++) {
        mp_slab_class_t *sc = &pool->slab_classes[cls];

        while (sc->empty_pages) {
            mp_slab_page_t *curr = sc->empty_pages;
            sc->empty_pages = curr->next;
            if (sc->empty_pages) {
                sc->empty_pages->prev = NULL;
            }
            if (sc->empty_page_count > 0) {
                sc->empty_page_count--;
            }

#ifdef _WIN32
            cmem_aligned_free(curr->page_raw_mem);
#else
            cmem_munmap(curr->page_raw_mem, SLAB_PAGE_SIZE);
#endif
            freed_bytes += SLAB_PAGE_SIZE;
            if (pool->stats.total_pool_size >= SLAB_PAGE_SIZE) {
                pool->stats.total_pool_size -= SLAB_PAGE_SIZE;
            }
        }

        mp_slab_page_t *curr = sc->partial_pages;
        while (curr) {
            mp_slab_page_t *next = curr->next;
            bool should_free = false;

            if (curr->free_count == curr->total_slots) {
                should_free = true;
            } else if (pool->idle_reclaim_enabled && curr->idle_since_ts > 0) {
                uint64_t idle_ms = (uint64_t)(now - curr->idle_since_ts);
                if (idle_ms >= pool->idle_reclaim_timeout_ms) {
                    should_free = true;
                }
            }

            if (should_free) {
                idle_count++;
                if (idle_count < pool->idle_reclaim_min_pages) {
                    curr = next;
                    continue;
                }

                if (curr->prev) {
                    curr->prev->next = curr->next;
                } else {
                    sc->partial_pages = curr->next;
                }
                if (curr->next) {
                    curr->next->prev = curr->prev;
                }

#ifdef _WIN32
                cmem_aligned_free(curr->page_raw_mem);
#else
                cmem_munmap(curr->page_raw_mem, SLAB_PAGE_SIZE);
#endif
                freed_bytes += SLAB_PAGE_SIZE;
                if (pool->stats.total_pool_size >= SLAB_PAGE_SIZE) {
                    pool->stats.total_pool_size -= SLAB_PAGE_SIZE;
                }
            }
            curr = next;
        }
    }

    trigger_event(pool, MP_EVENT_COMPACT, NULL, freed_bytes);
    pool_unlock(pool);
    return freed_bytes;
}

/**
 * @brief Return the number of idle (fully free or long-idle) slab pages.
 *
 * @param pool Pointer to the memory pool
 * @return Number of idle pages
 */
size_t mp_get_idle_page_count(memory_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    tls_cache_flush_pool(pool);
    percpu_flush(pool);
    pool_rdlock(pool);
    size_t count = 0;
    int64_t now = cmem_now_ms();

    for (int cls = 0; cls < SLAB_CLASS_COUNT; cls++) {
        mp_slab_class_t *sc = &pool->slab_classes[cls];
        count += sc->empty_page_count;
        mp_slab_page_t *curr = sc->partial_pages;

        while (curr) {
            if (curr->free_count == curr->total_slots) {
                count++;
            } else if (pool->idle_reclaim_enabled && curr->idle_since_ts > 0) {
                uint64_t idle_ms = (uint64_t)(now - curr->idle_since_ts);
                if (idle_ms >= pool->idle_reclaim_timeout_ms) {
                    count++;
                }
            }
            curr = curr->next;
        }
    }

    pool_rdunlock(pool);
    return count;
}

/**
 * @brief Install the diagnostics event callback for a pool.
 * @param pool The memory pool.
 * @param callback Event callback; NULL to clear.
 * @param user_data Opaque data passed to callback.
 */
void mp_set_event_callback(memory_pool_t *pool, mp_event_callback_t callback, void *user_data)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    pool->event_cb = callback;
    pool->event_user_data = user_data;
    pool_unlock(pool);
}

/**
 * @brief Pin this pool's future allocations to a NUMA node.
 * @param pool The memory pool.
 * @param numa_node NUMA node id, or -1 for system default.
 * @return true on success.
 */
bool mp_set_numa_node(memory_pool_t *pool, int numa_node)
{
    if (!pool) {
        return false;
    }
    pool_lock(pool);
    pool->numa_node = numa_node;
    pool_unlock(pool);
    printf(
        "[CMEM NUMA] Memory Pool [%s] bound to NUMA CPU Node #%d\n", pool->arena_name, numa_node);
    return true;
}

/**
 * @brief Set aside an emergency memory reserve for OOM recovery.
 * Allocates a dedicated buffer (with canary on) used as a last-resort fallback
 * when the pool would otherwise fail an allocation.
 * @param pool The memory pool.
 * @param reserve_bytes Size of the reserve to set aside.
 * @return true on success.
 */
bool mp_enable_emergency_reserve(memory_pool_t *pool, size_t reserve_bytes)
{
    if (!pool || reserve_bytes == 0) {
        return false;
    }
    pool_lock(pool);
    if (pool->emergency_buf) {
        free(pool->emergency_buf);
    }

    pool->emergency_buf = malloc(reserve_bytes);
    if (!pool->emergency_buf) {
        pool_unlock(pool);
        return false;
    }
    pool->emergency_size = reserve_bytes;
    pool->emergency_used = 0;
    pool->in_emergency_state = false;
    pool_unlock(pool);
    printf("[CMEM RELIABILITY] Emergency OOM reserve buffer (%zu bytes) configured for [%s]\n",
           reserve_bytes,
           pool->arena_name);
    return true;
}

/**
 * @brief Fire the high/low watermark callback when a threshold is crossed.
 * @param pool The memory pool.
 */
inline void check_watermark_after_change(memory_pool_t *pool)
{
    if (!pool->watermark_cb || pool->stats.max_memory_limit == 0) {
        return;
    }

    size_t limit = pool->stats.max_memory_limit;
    size_t active = pool->stats.active_bytes;

    if (!pool->in_high_watermark_state && pool->high_watermark_ratio > 0.0) {
        size_t high_thresh = (size_t)(pool->high_watermark_ratio * (double)limit);
        if (active >= high_thresh) {
            pool->in_high_watermark_state = true;
            pool->watermark_cb(pool, true, active, limit, pool->watermark_user_data);
        }
    } else if (pool->in_high_watermark_state && pool->low_watermark_ratio > 0.0) {
        size_t low_thresh = (size_t)(pool->low_watermark_ratio * (double)limit);
        if (active <= low_thresh) {
            pool->in_high_watermark_state = false;
            pool->watermark_cb(pool, false, active, limit, pool->watermark_user_data);
        }
    }
}

/**
 * @brief Push an allocation onto the pool's active-allocation list.
 * @param pool The memory pool.
 * @param header Block header to track as live.
 */
void active_list_add(memory_pool_t *pool, mp_block_header_t *header)
{
    header->next = pool->active_head;
    header->prev = NULL;
    if (pool->active_head) {
        pool->active_head->prev = header;
    }
    pool->active_head = header;
}

/**
 * @brief Unlink an allocation from the pool's active-allocation list.
 * @param pool The memory pool.
 * @param header Block header to untrack.
 */
void active_list_remove(memory_pool_t *pool, mp_block_header_t *header)
{
    if (header->prev) {
        header->prev->next = header->next;
    } else {
        pool->active_head = header->next;
    }
    if (header->next) {
        header->next->prev = header->prev;
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
            if (pool->stats.active_bytes + size > pool->stats.max_memory_limit) {
                exceed = true;
            }
            pool_rdunlock(pool);
        } else {
            if (pool->stats.active_bytes + size > pool->stats.max_memory_limit) {
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
                    header->magic = MP_MAGIC_HEAD;
                    header->alloc_type = ALLOC_TYPE_EMERGENCY;
                    header->slab_class = 0;
                    header->flags = 0;
                    header->requested_size = size;
                    header->usable_size = pool->emergency_size - sizeof(mp_block_header_t);
                    header->raw_base = pool->emergency_buf;
                    header->subpool = NULL;
                    header->alloc_file = NULL;
                    header->alloc_line = 0;
                    header->alloc_func = NULL;
                    header->backtrace_depth = 0;
                    header->prev = NULL;
                    header->next = NULL;

                    void *emerg_ptr = (void *)((uint8_t *)header + sizeof(mp_block_header_t));
                    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
                        uint8_t *canary = (uint8_t *)emerg_ptr + size;
                        *canary = MP_CANARY_BYTE;
                    }
                    if (pool->flags & MP_FLAG_ZERO_ON_ALLOC) {
                        memset(emerg_ptr, 0, size);
                    }

                    if (pool->flags & MP_FLAG_THREAD_SAFE) {
                        pool_lock(pool);
                    }
                    active_list_add(pool, header);
                    pool->stats.active_bytes += size;
                    if (pool->stats.active_bytes > pool->stats.peak_bytes) {
                        pool->stats.peak_bytes = pool->stats.active_bytes;
                    }
                    pool->stats.active_allocations++;
                    pool->stats.total_alloc_ops++;
                    if (pool->flags & MP_FLAG_THREAD_SAFE) {
                        pool_unlock(pool);
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

        /* Tier 3: Bottom-level Slab replenishment via tls_cache_refill() / slab_alloc_slot() */
        if (!slot) {
            tls_cache_refill(pool, class_idx);
            if (tls_cache.counts[class_idx] > 0) {
                slot = tls_cache.slots[class_idx];
                tls_cache.slots[class_idx] = slot->next;
                tls_cache.counts[class_idx]--;
            } else {
                slot = slab_alloc_slot(pool, class_idx);
            }
        }

        if (slot) {
            mp_block_header_t *header = (mp_block_header_t *)slot;
            header->magic = MP_MAGIC_HEAD;
            header->alloc_type = ALLOC_TYPE_SLAB;
            header->slab_class = class_idx;
            header->flags = 0;
            header->requested_size = size;
            header->usable_size = pool->slab_classes[class_idx].slot_size;
            header->raw_base = slot;
            header->subpool = NULL;
            header->alloc_file = NULL;
            header->alloc_line = 0;
            header->alloc_func = NULL;
            header->backtrace_depth = 0;
            header->prev = NULL;
            header->next = NULL;

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
        if (pool->flags & MP_FLAG_THREAD_SAFE) {
            pool_lock(pool);
            ptr = tlsf_alloc(pool, size);
            pool_unlock(pool);
        } else {
            ptr = tlsf_alloc(pool, size);
        }
    } else {
        size_t total_sz =
            size + sizeof(mp_block_header_t) + ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0);
        void *raw_mem = sys_mem_alloc(pool, total_sz, 8);
        if (raw_mem) {
            mp_block_header_t *header = (mp_block_header_t *)raw_mem;
            header->magic = MP_MAGIC_HEAD;
            header->alloc_type = ALLOC_TYPE_OS;
            header->slab_class = 0;
            header->flags = 0;
            header->requested_size = size;
            header->usable_size = size;
            header->raw_base = raw_mem;
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
            pool->stats.os_allocated_bytes += size;
            pool->stats.total_pool_size += total_sz;
        }
    }

    if (ptr) {
        mp_block_header_t *header =
            (mp_block_header_t *)((uint8_t *)ptr - sizeof(mp_block_header_t));

        if (pool->flags & MP_FLAG_THREAD_SAFE) {
            pool_lock(pool);
        }

        active_list_add(pool, header);

        pool->stats.active_bytes += size;
        if (pool->stats.active_bytes > pool->stats.peak_bytes) {
            pool->stats.peak_bytes = pool->stats.active_bytes;
        }
        pool->stats.active_allocations++;
        pool->stats.total_alloc_ops++;

        int bucket = get_slab_class_index(pool, size);
        if (bucket < CMEM_HISTOGRAM_BUCKETS) {
            pool->stats.size_histogram[bucket]++;
        }

        if (pool->watermark_cb) {
            check_watermark_after_change(pool);
        }
        if (pool->event_cb) {
            trigger_event(pool, MP_EVENT_ALLOC, ptr, size);
        }

        if (pool->flags & MP_FLAG_THREAD_SAFE) {
            pool_unlock(pool);
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
    if (!pool || !out_ptrs || count == 0) {
        return 0;
    }
    size_t allocated = 0;
    for (size_t i = 0; i < count; i++) {
        out_ptrs[i] = mp_alloc(pool, size);
        if (out_ptrs[i]) {
            allocated++;
        } else {
            break;
        }
    }
    return allocated;
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
    for (size_t i = 0; i < count; i++) {
        if (ptrs[i]) {
            mp_free(pool, ptrs[i]);
            ptrs[i] = NULL;
        }
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
 * @brief Release a pointer back to its owning tier.
 * Validates magic, optionally poisons, handles TLS/percpu caches and canary
 * check, unlinks from the active list, and routes to slab/tlsf/OS-free.
 * @param pool The memory pool.
 * @param ptr Allocation to release.
 */
void mp_free(memory_pool_t *pool, void *ptr)
{
    if (!pool || !ptr) {
        return;
    }

    mp_block_header_t *header = (mp_block_header_t *)((uint8_t *)ptr - sizeof(mp_block_header_t));

    if (header->magic != MP_MAGIC_HEAD) {
        (void)fprintf(
            stderr, "[MEMORY_POOL ERROR] Corrupt header or invalid free on pointer %p!\n", ptr);
        trigger_event(pool, MP_EVENT_DOUBLE_FREE, ptr, 0);
        mp_mark_pool_dirty(pool);
        if (pool->error_recovery_cb) {
            pool->error_recovery_cb(pool,
                                    true,
                                    pool->stats.active_bytes,
                                    pool->stats.max_memory_limit,
                                    pool->error_recovery_user_data);
        }
        return;
    }

    if (pool->flags & MP_FLAG_POISON_ON_FREE) {
        memset(ptr, MP_POISON_BYTE, header->requested_size);
    }

    if (header->alloc_type == ALLOC_TYPE_SLAB) {
        tls_cache_validate_owner(pool);
        uint8_t class_idx = header->slab_class;

        if (tls_cache.counts[class_idx] < TLS_CACHE_MAX_SLOTS) {
            if (!(pool->flags & MP_FLAG_THREAD_SAFE)) {
                active_list_remove(pool, header);
                pool->stats.active_bytes -= header->requested_size;
                pool->stats.active_allocations--;
                pool->stats.total_free_ops++;
            } else {
                pool_lock(pool);
                active_list_remove(pool, header);
                pool->stats.active_bytes -= header->requested_size;
                pool->stats.active_allocations--;
                pool->stats.total_free_ops++;
                pool_unlock(pool);
            }
            if (pool->event_cb) {
                trigger_event(pool, MP_EVENT_FREE, ptr, header->requested_size);
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
                    active_list_remove(pool, header);
                    pool->stats.active_bytes -= header->requested_size;
                    pool->stats.active_allocations--;
                    pool->stats.total_free_ops++;
                } else {
                    pool_lock(pool);
                    active_list_remove(pool, header);
                    pool->stats.active_bytes -= header->requested_size;
                    pool->stats.active_allocations--;
                    pool->stats.total_free_ops++;
                    pool_unlock(pool);
                }
                if (pool->event_cb) {
                    trigger_event(pool, MP_EVENT_FREE, ptr, header->requested_size);
                }
                return;
            }
        }

        if (pool->flags & MP_FLAG_THREAD_SAFE) {
            pool_lock(pool);
            active_list_remove(pool, header);
            pool->stats.active_bytes -= header->requested_size;
            pool->stats.active_allocations--;
            pool->stats.total_free_ops++;
            pool_unlock(pool);
            if (pool->event_cb) {
                trigger_event(pool, MP_EVENT_FREE, ptr, header->requested_size);
            }
            mp_slab_slot_t *slot = (mp_slab_slot_t *)header->raw_base;
            remote_free_push(pool, class_idx, slot);
            return;
        }
    }

    pool_lock(pool);

    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
        uint8_t *canary = (uint8_t *)ptr + header->requested_size;
        if (*canary != MP_CANARY_BYTE) {
            (void)fprintf(
                stderr, "[MEMORY_POOL BUG] Buffer overflow detected at pointer %p!\n", ptr);
            trigger_event(pool, MP_EVENT_CANARY_CORRUPTION, ptr, header->requested_size);
            mp_mark_pool_dirty(pool);
            if (pool->error_recovery_cb) {
                pool->error_recovery_cb(pool,
                                        true,
                                        pool->stats.active_bytes,
                                        pool->stats.max_memory_limit,
                                        pool->error_recovery_user_data);
            }
        }
    }

    uint8_t alloc_type = header->alloc_type;
    size_t req_size = header->requested_size;
    void *raw_base = header->raw_base;

    active_list_remove(pool, header);
    pool->stats.active_bytes -= req_size;
    pool->stats.active_allocations--;
    pool->stats.total_free_ops++;
    if (alloc_type == ALLOC_TYPE_OS) {
        pool->stats.os_allocated_bytes -= req_size;
        pool->stats.total_pool_size -=
            (req_size + sizeof(mp_block_header_t) + ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0));
    }
    check_watermark_after_change(pool);
    trigger_event(pool, MP_EVENT_FREE, ptr, req_size);

    if (alloc_type == ALLOC_TYPE_SLAB) {
        pool_unlock(pool);
        slab_free(pool, header);
    } else if (alloc_type == ALLOC_TYPE_TLSF) {
        tlsf_free(pool, header);
        pool_unlock(pool);
    } else if (alloc_type == ALLOC_TYPE_OS) {
        pool_unlock(pool);
        sys_mem_free(pool, raw_base, req_size);
    } else if (alloc_type == ALLOC_TYPE_EMERGENCY) {
        pool->emergency_used = false;
        pool_unlock(pool);
    }
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
            pool->active_head = new_header;
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
    pool_rdlock(pool);
    mp_block_header_t *header = (mp_block_header_t *)((uint8_t *)ptr - sizeof(mp_block_header_t));
    if (header->magic != MP_MAGIC_HEAD) {
        pool_rdunlock(pool);
        return false;
    }
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

    pool_rdlock(pool);
    mp_block_header_t *header = (mp_block_header_t *)((uint8_t *)ptr - sizeof(mp_block_header_t));
    if (header->magic != MP_MAGIC_HEAD) {
        pool_rdunlock(pool);
        return false;
    }
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
 * @brief List the pool's slab pages, TLSF arenas and emergency buffer.
 * @param pool The memory pool.
 * @param regions Receives region descriptors.
 * @param max_regions Capacity of regions (0 to query count).
 * @return Number of regions written, or 0 on invalid pool.
 */
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
