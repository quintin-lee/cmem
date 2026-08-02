/**
 * @file cmem_slab.c
 * @brief Extracted module implementation.
 */

#include "cmem.h"
#include "cmem_internal.h"

const size_t      kSlabSizes[SLAB_CLASS_COUNT] = {8, 16, 32, 64, 128, 256, 512};
thread_cache_t    tls_cache                    = {{0}, {0}};
mp_thread_quota_t thread_quota                 = {0, 0};

bool slab_init(memory_pool_t *pool)
{
    for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
        pool->slab_classes[i].slot_size =
            pool->use_custom_slab_sizes ? pool->custom_slab_sizes[i] : kSlabSizes[i];
        pthread_mutex_init(&pool->slab_classes[i].lock, NULL);
        pool->slab_classes[i].partial_pages = NULL;
        pool->slab_classes[i].full_pages    = NULL;
    }
    return true;
}

mp_slab_page_t *slab_create_page(memory_pool_t *pool, uint8_t class_idx)
{
    size_t slot_payload_size = pool->slab_classes[class_idx].slot_size;
    size_t header_overhead   = sizeof(mp_block_header_t);
    size_t total_slot_size =
        header_overhead + slot_payload_size + ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0);
    total_slot_size = (total_slot_size + 7) & ~7;

#ifdef _WIN32
    void *raw_mem = cmem_aligned_malloc(SLAB_PAGE_SIZE, SLAB_PAGE_SIZE);
    if (raw_mem == NULL) {
        return NULL;
    }
#else
    size_t map_size = SLAB_PAGE_SIZE + SLAB_PAGE_SIZE - 1;
    void  *raw_mem =
        mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw_mem == MAP_FAILED) {
        return NULL;
    }

    uintptr_t start   = (uintptr_t)raw_mem;
    uintptr_t aligned = (start + SLAB_PAGE_SIZE - 1) & ~(SLAB_PAGE_SIZE - 1);
    if (aligned + SLAB_PAGE_SIZE > start + map_size) {
        munmap(raw_mem, map_size);
        return NULL;
    }

    if (aligned > start) {
        munmap((void *)start, aligned - start);
    }
    if (aligned + SLAB_PAGE_SIZE < start + map_size) {
        munmap((void *)(aligned + SLAB_PAGE_SIZE), start + map_size - aligned - SLAB_PAGE_SIZE);
    }
#endif

#ifdef _WIN32
    void *page_mem = raw_mem;
#else
    void *page_mem = (void *)aligned;
#endif
    mp_slab_page_t *page = (mp_slab_page_t *)page_mem;
    page->class_index    = class_idx;
    page->page_raw_mem   = page_mem;
    page->next           = NULL;
    page->prev           = NULL;
    page->is_hot         = false;

    size_t usable_bytes = SLAB_PAGE_SIZE - sizeof(mp_slab_page_t);
    page->total_slots   = (uint16_t)(usable_bytes / total_slot_size);
    page->free_count    = page->total_slots;

    uint8_t *ptr    = (uint8_t *)page_mem + sizeof(mp_slab_page_t);
    page->free_list = (mp_slab_slot_t *)ptr;

    for (uint16_t i = 0; i < page->total_slots; i++) {
        mp_slab_slot_t *slot = (mp_slab_slot_t *)(ptr + i * total_slot_size);
        if (i < page->total_slots - 1) {
            slot->next = (mp_slab_slot_t *)(ptr + (i + 1) * total_slot_size);
        } else {
            slot->next = NULL;
        }
    }

    pool->stats.total_pool_size += SLAB_PAGE_SIZE;
    return page;
}

void *slab_alloc(memory_pool_t *pool, uint8_t class_idx, size_t req_size)
{
    mp_slab_class_t *sc = &pool->slab_classes[class_idx];
    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_lock(&sc->lock);
    }

    mp_slab_page_t *page = sc->partial_pages;

    if (!page) {
        page = slab_create_page(pool, class_idx);
        if (!page) {
            if (pool->flags & MP_FLAG_THREAD_SAFE) {
                pthread_mutex_unlock(&sc->lock);
            }
            return NULL;
        }

        page->next = sc->partial_pages;
        if (sc->partial_pages) {
            sc->partial_pages->prev = page;
        }
        sc->partial_pages = page;
    }

    mp_slab_slot_t *slot = page->free_list;
    page->free_list      = slot->next;
    page->free_count--;

    if (page->free_count == 0) {
        sc->partial_pages = page->next;
        if (page->next) {
            page->next->prev = NULL;
        }

        page->next = sc->full_pages;
        page->prev = NULL;
        if (sc->full_pages) {
            sc->full_pages->prev = page;
        }
        sc->full_pages = page;
    }

    pool->stats.slab_allocated_bytes += sc->slot_size;

    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_unlock(&sc->lock);
    }

    mp_block_header_t *header = (mp_block_header_t *)slot;
    header->magic             = MP_MAGIC_HEAD;
    header->alloc_type        = ALLOC_TYPE_SLAB;
    header->slab_class        = class_idx;
    header->flags             = 0;
    header->requested_size    = req_size;
    header->usable_size       = sc->slot_size;
    header->raw_base          = slot;
    header->subpool           = NULL;
    header->alloc_file        = NULL;
    header->alloc_line        = 0;
    header->alloc_func        = NULL;
    header->backtrace_depth   = 0;

    void *payload = (void *)((uint8_t *)header + sizeof(mp_block_header_t));

    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
        uint8_t *canary = (uint8_t *)payload + req_size;
        *canary         = MP_CANARY_BYTE;
    }

    if (pool->flags & MP_FLAG_ZERO_ON_ALLOC) {
        memset(payload, 0, req_size);
    }

    return payload;
}

void slab_free(memory_pool_t *pool, mp_block_header_t *header)
{
    uint8_t          class_idx = header->slab_class;
    mp_slab_class_t *sc        = &pool->slab_classes[class_idx];

    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_lock(&sc->lock);
    }

    uintptr_t       ptr_val   = (uintptr_t)header->raw_base;
    uintptr_t       page_base = ptr_val & ~(SLAB_PAGE_SIZE - 1);
    mp_slab_page_t *page      = (mp_slab_page_t *)page_base;

    bool was_full = (page->free_count == 0);

    mp_slab_slot_t *slot = (mp_slab_slot_t *)header->raw_base;
    slot->next           = page->free_list;
    page->free_list      = slot;
    page->free_count++;

    pool->stats.slab_allocated_bytes -= sc->slot_size;

    if (was_full) {
        if (page->prev) {
            page->prev->next = page->next;
        } else {
            sc->full_pages = page->next;
        }
        if (page->next) {
            page->next->prev = page->prev;
        }

        page->next = sc->partial_pages;
        page->prev = NULL;
        if (sc->partial_pages) {
            sc->partial_pages->prev = page;
        }
        sc->partial_pages = page;
    }

    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_unlock(&sc->lock);
    }
}

inline void tls_cache_refill(memory_pool_t *pool, uint8_t class_idx)
{
    for (int i = 0; i < 32; i++) {
        void *ptr = slab_alloc(pool, class_idx, pool->slab_classes[class_idx].slot_size);
        if (!ptr) {
            break;
        }
        mp_block_header_t *header =
            (mp_block_header_t *)((uint8_t *)ptr - sizeof(mp_block_header_t));
        mp_slab_slot_t *slot       = (mp_slab_slot_t *)header->raw_base;
        slot->next                 = tls_cache.slots[class_idx];
        tls_cache.slots[class_idx] = slot;
        tls_cache.counts[class_idx]++;
    }
}

void percpu_init(memory_pool_t *pool)
{
    if (!pool || pool->percpu_freelists) {
        return;
    }
#ifdef _WIN32
    pool->num_cpus = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
    pool->num_cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (pool->num_cpus <= 0) {
        pool->num_cpus = 1;
    }
    if (pool->num_cpus > 256) {
        pool->num_cpus = 256;
    }

    size_t sz = (size_t)pool->num_cpus * SLAB_CLASS_COUNT * sizeof(mp_percpu_freelist_entry_t);
    pool->percpu_freelists = (mp_percpu_freelist_entry_t *)calloc(1, sz);
    if (!pool->percpu_freelists) {
        pool->num_cpus = 0;
        return;
    }
}

void percpu_destroy(memory_pool_t *pool)
{
    if (!pool || !pool->percpu_freelists) {
        return;
    }
    free(pool->percpu_freelists);
    pool->percpu_freelists = NULL;
    pool->num_cpus         = 0;
}

inline int percpu_cpu_index(void)
{
#ifdef _WIN32
    return 0;
#else
    int cpu = cmem_sched_getcpu();
    if (cpu < 0) {
        cpu = 0;
    }
    return cpu;
#endif
}

inline mp_slab_slot_t *percpu_pop(memory_pool_t *pool, int cpu, uint8_t class_idx)
{
    if (!pool->percpu_freelists || cpu < 0 || cpu >= pool->num_cpus) {
        return NULL;
    }
    size_t                      idx = (size_t)cpu * SLAB_CLASS_COUNT + class_idx;
    mp_percpu_freelist_entry_t *entry =
        &((mp_percpu_freelist_entry_t *)pool->percpu_freelists)[idx];
    cmem_atomic_size_t *headp = &entry->head;
    size_t              head  = CMEM_ATOMIC_LOAD(headp, CMEM_ORDER_ACQUIRE);
    if (head == 0) {
        return NULL;
    }

    mp_slab_slot_t *slot = (mp_slab_slot_t *)head;
    mp_slab_slot_t *next = slot->next;
    if (!CMEM_ATOMIC_COMPARE_EXCHANGE(
            headp, &head, (size_t)next, CMEM_ORDER_ACQUIRE, CMEM_ORDER_RELAXED)) {
        return NULL;
    }
    entry->count--;
    return slot;
}

inline void percpu_refill(memory_pool_t *pool, int cpu, uint8_t class_idx)
{
    if (!pool->percpu_freelists || cpu < 0 || cpu >= pool->num_cpus) {
        return;
    }
    size_t                      idx = (size_t)cpu * SLAB_CLASS_COUNT + class_idx;
    mp_percpu_freelist_entry_t *entry =
        &((mp_percpu_freelist_entry_t *)pool->percpu_freelists)[idx];
    if (entry->count > MP_PERCPU_MAX_BATCH / 2) {
        return;
    }

    mp_slab_slot_t *slots[MP_PERCPU_MAX_BATCH];
    int             got = 0;
    for (int i = 0; i < MP_PERCPU_MAX_BATCH; i++) {
        void *ptr = slab_alloc(pool, class_idx, pool->slab_classes[class_idx].slot_size);
        if (!ptr) {
            break;
        }
        mp_block_header_t *header =
            (mp_block_header_t *)((uint8_t *)ptr - sizeof(mp_block_header_t));
        slots[got++] = (mp_slab_slot_t *)header->raw_base;
    }
    if (got == 0) {
        return;
    }

    mp_slab_slot_t *head = slots[0];
    mp_slab_slot_t *tail = head;
    for (int i = 1; i < got; i++) {
        tail->next = slots[i];
        tail       = slots[i];
    }
    tail->next = (mp_slab_slot_t *)CMEM_ATOMIC_LOAD(&entry->head, CMEM_ORDER_RELAXED);
    CMEM_ATOMIC_STORE(&entry->head, (size_t)head, CMEM_ORDER_RELAXED);
    entry->count += (uint16_t)got;
}

void mp_set_percpu_freelist(memory_pool_t *pool, bool enable)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    if (enable) {
        pool->flags = (mp_flags_t)(pool->flags | MP_FLAG_PERCPU_FREELIST);
        if (!pool->percpu_freelists) {
            percpu_init(pool);
        }
    } else {
        pool->flags = (mp_flags_t)(pool->flags & ~MP_FLAG_PERCPU_FREELIST);
        percpu_destroy(pool);
    }
    pool_unlock(pool);
}

bool mp_get_percpu_freelist(memory_pool_t *pool)
{
    if (!pool) {
        return false;
    }
    pool_rdlock(pool);
    bool enabled = (pool->flags & MP_FLAG_PERCPU_FREELIST) != 0 && pool->percpu_freelists != NULL;
    pool_rdunlock(pool);
    return enabled;
}

int mp_get_percpu_cpu_count(memory_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    pool_rdlock(pool);
    int count = pool->num_cpus;
    pool_rdunlock(pool);
    return count;
}

bool mp_mark_page_hot(memory_pool_t *pool, void *page_raw_mem)
{
    if (!pool || !page_raw_mem) {
        return false;
    }
    bool found = false;
    pool_lock(pool);
    for (int c = 0; c < SLAB_CLASS_COUNT; c++) {
        mp_slab_class_t *sc   = &pool->slab_classes[c];
        mp_slab_page_t  *curr = sc->partial_pages;
        while (curr) {
            if (curr->page_raw_mem == page_raw_mem) {
                curr->is_hot = true;
                found        = true;
                break;
            }
            curr = curr->next;
        }
        if (found) {
            break;
        }
        curr = sc->full_pages;
        while (curr) {
            if (curr->page_raw_mem == page_raw_mem) {
                curr->is_hot = true;
                found        = true;
                break;
            }
            curr = curr->next;
        }
        if (found) {
            break;
        }
    }
    pool_unlock(pool);
    return found;
}

bool mp_mark_page_cold(memory_pool_t *pool, void *page_raw_mem)
{
    if (!pool || !page_raw_mem) {
        return false;
    }
    bool found = false;
    pool_lock(pool);
    for (int c = 0; c < SLAB_CLASS_COUNT; c++) {
        mp_slab_class_t *sc   = &pool->slab_classes[c];
        mp_slab_page_t  *curr = sc->partial_pages;
        while (curr) {
            if (curr->page_raw_mem == page_raw_mem) {
                curr->is_hot = false;
                found        = true;
                break;
            }
            curr = curr->next;
        }
        if (found) {
            break;
        }
        curr = sc->full_pages;
        while (curr) {
            if (curr->page_raw_mem == page_raw_mem) {
                curr->is_hot = false;
                found        = true;
                break;
            }
            curr = curr->next;
        }
        if (found) {
            break;
        }
    }
    pool_unlock(pool);
    return found;
}

size_t mp_get_hot_page_count(memory_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    size_t count = 0;
    pool_rdlock(pool);
    for (int c = 0; c < SLAB_CLASS_COUNT; c++) {
        mp_slab_class_t *sc   = &pool->slab_classes[c];
        mp_slab_page_t  *curr = sc->partial_pages;
        while (curr) {
            if (curr->is_hot) {
                count++;
            }
            curr = curr->next;
        }
        curr = sc->full_pages;
        while (curr) {
            if (curr->is_hot) {
                count++;
            }
            curr = curr->next;
        }
    }
    pool_rdunlock(pool);
    return count;
}

size_t mp_get_cold_page_count(memory_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    size_t total = 0;
    size_t hot   = 0;
    pool_rdlock(pool);
    for (int c = 0; c < SLAB_CLASS_COUNT; c++) {
        mp_slab_class_t *sc   = &pool->slab_classes[c];
        mp_slab_page_t  *curr = sc->partial_pages;
        while (curr) {
            total++;
            if (curr->is_hot) {
                hot++;
            }
            curr = curr->next;
        }
        curr = sc->full_pages;
        while (curr) {
            total++;
            if (curr->is_hot) {
                hot++;
            }
            curr = curr->next;
        }
    }
    pool_rdunlock(pool);
    return total > hot ? total - hot : 0;
}

size_t mp_separate_hot_cold_pages(memory_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    if (!(pool->flags & MP_FLAG_HOT_COLD_SEPARATION)) {
        return 0;
    }
    size_t separated = 0;
    pool_lock(pool);
    for (int c = 0; c < SLAB_CLASS_COUNT; c++) {
        mp_slab_class_t *sc   = &pool->slab_classes[c];
        mp_slab_page_t  *curr = sc->partial_pages;
        while (curr) {
            if (!curr->is_hot) {
                separated++;
            }
            curr = curr->next;
        }
        curr = sc->full_pages;
        while (curr) {
            if (!curr->is_hot) {
                separated++;
            }
            curr = curr->next;
        }
    }
    pool_unlock(pool);
    return separated;
}

bool percpu_push(memory_pool_t *pool, int cpu, uint8_t class_idx, mp_slab_slot_t *slot)
{
    if (!pool->percpu_freelists || cpu < 0 || cpu >= pool->num_cpus) {
        return false;
    }
    size_t                      idx = (size_t)cpu * SLAB_CLASS_COUNT + class_idx;
    mp_percpu_freelist_entry_t *entry =
        &((mp_percpu_freelist_entry_t *)pool->percpu_freelists)[idx];
    if (entry->count >= MP_PERCPU_MAX_BATCH) {
        return false;
    }

    cmem_atomic_size_t *headp = &entry->head;
    size_t              old_head;
    size_t              new_head;
    do {
        old_head   = CMEM_ATOMIC_LOAD(headp, CMEM_ORDER_RELAXED);
        slot->next = (mp_slab_slot_t *)(uintptr_t)old_head;
        new_head   = (size_t)(uintptr_t)slot;
    } while (!CMEM_ATOMIC_COMPARE_EXCHANGE(
        headp, &old_head, new_head, CMEM_ORDER_RELAXED, CMEM_ORDER_RELAXED));

    entry->count++;
    return true;
}
