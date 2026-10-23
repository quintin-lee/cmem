/**
 * @file cmem_lifecycle.c
 * @brief Pool create/destroy/reset, idle-page reclaim, callbacks, NUMA, and emergency reserve for
 * cmem.
 */
#include "cmem.h"
#include "cmem_internal.h"
#include <fcntl.h>
#define CMEM_SHM_OPEN_MODE 0666

void mp_destroy(memory_pool_t *pool)
{
    if (!pool) {
        return;
    }

    mp_purge_thread_arena_bindings(pool);
    tls_cache_flush_pool(pool);
    percpu_flush(pool);

    if (pool->flags & MP_FLAG_REPORT_LEAKS_ON_DESTROY) {
#if !defined(CMEM_DISABLE_DIAGNOSTICS)
        mp_check_leaks(pool);
#endif
    }

    memory_pool_t *child = pool->first_child;
    while (child) {
        memory_pool_t *next = child->next_sibling;
        mp_destroy(child);
        child = next;
    }

    /* Release compressed-storage state (blocks are owned by the pool). */
    if (pool->compressed_area != NULL) {
        mp_free(pool, pool->compressed_area);
        pool->compressed_area = NULL;
        pool->compressed_area_size = 0;
    }
    free(pool->compressed_entries);
    pool->compressed_entries = NULL;
    pool->compressed_capacity = 0;
    pool->compressed_used = 0;

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
            curr = pool->slab_classes[i].empty_pages;
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
            pthread_mutex_destroy(&tcurr->lock);
            for (int fl = 0; fl < TLSF_FL_MAX; fl++) {
                for (int sl = 0; sl < TLSF_SL_COUNT; sl++) {
                    pthread_mutex_destroy(&tcurr->bucket_locks[fl][sl]);
                }
            }
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

        if (pool->arenas) {
            free(pool->arenas);
            pool->arenas = NULL;
            pool->num_arenas = 0;
        }

        /* Arena descriptors and any still-mapped regions are pool-owned;
         * they must be released before the pool struct goes away. */
        tlsf_arena_registry_release(pool);

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

    CMEM_ATOMIC_STORE(&pool->active_bytes, 0, CMEM_ORDER_RELAXED);
    CMEM_ATOMIC_STORE(&pool->active_allocations, 0, CMEM_ORDER_RELAXED);
    CMEM_ATOMIC_STORE(&pool->slab_allocated_bytes, 0, CMEM_ORDER_RELAXED);
    CMEM_ATOMIC_STORE(&pool->tlsf_allocated_bytes, 0, CMEM_ORDER_RELAXED);
    CMEM_ATOMIC_STORE(&pool->os_allocated_bytes, 0, CMEM_ORDER_RELAXED);
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
            if (CMEM_ATOMIC_LOAD(&pool->total_pool_size, CMEM_ORDER_RELAXED) >= SLAB_PAGE_SIZE) {
                CMEM_ATOMIC_FETCH_SUB(&pool->total_pool_size, SLAB_PAGE_SIZE, CMEM_ORDER_RELAXED);
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
                if (CMEM_ATOMIC_LOAD(&pool->total_pool_size, CMEM_ORDER_RELAXED) >=
                    SLAB_PAGE_SIZE) {
                    CMEM_ATOMIC_FETCH_SUB(
                        &pool->total_pool_size, SLAB_PAGE_SIZE, CMEM_ORDER_RELAXED);
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
    size_t active = CMEM_ATOMIC_LOAD(&pool->active_bytes, CMEM_ORDER_RELAXED);

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
    if (!header) {
        return;
    }
    if (header->prev) {
        header->prev->next = header->next;
    } else {
        pool->active_head = header->next;
    }
    if (header->next) {
        header->next->prev = header->prev;
    }
    header->prev = NULL;
    header->next = NULL;
}

/**
 * @brief Update free-path accounting for one block.
 * Under MP_FLAG_FAST_PATH the active-allocation list is not maintained, so the
 * linked-list unlink is skipped; counters are always kept for leak-count verdicts.
 * @param pool   The memory pool.
 * @param header Block header being freed.
 */
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

    flags = cmem_resolve_default_flags(flags);

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
            pthread_mutex_init(&pool->arena_registry_lock, &mattr);
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
    tlsf_arena_registry_release(pool);
#ifdef _WIN32
    (void)shm_name;
#else
    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_rwlock_destroy(&pool->rwlock);
        pthread_mutex_destroy(&pool->lock);
        pthread_mutex_destroy(&pool->arena_registry_lock);
    }
    size_t sz = CMEM_ATOMIC_LOAD(&pool->total_pool_size, CMEM_ORDER_RELAXED);
    void *shm_ptr = (void *)pool;
    munmap(shm_ptr, sz);
    if (shm_name) {
        shm_unlink(shm_name);
    }
#endif
}
