/**
 * @file cmem_ring_event.c
 * @brief Ring buffer, event log, pprof export, typed pool, and shared-memory destroy for cmem.
 */
#include "cmem.h"
#include "cmem_internal.h"
#define CMEM_NSEC_PER_SEC_U64 1000000000ULL

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

    size_t total_alloc = CMEM_ATOMIC_LOAD(&pool->total_alloc_ops, CMEM_ORDER_RELAXED);
    size_t active = CMEM_ATOMIC_LOAD(&pool->active_allocations, CMEM_ORDER_RELAXED);
    size_t active_bytes = CMEM_ATOMIC_LOAD(&pool->active_bytes, CMEM_ORDER_RELAXED);
    size_t peak_bytes = CMEM_ATOMIC_LOAD(&pool->peak_bytes, CMEM_ORDER_RELAXED);

    int written =
        snprintf(out_buf,
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
                 total_alloc > 0
                     ? CMEM_ATOMIC_LOAD(&pool->total_alloc_ops, CMEM_ORDER_RELAXED) * sizeof(void *)
                     : 0,
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
