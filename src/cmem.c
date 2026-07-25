/**
 * @file cmem.c
 * @brief cmem - Universal Tiered Memory Manager Implementation (Slab + TLSF + OS + Child Arenas + Diagnostics).
 */

#define _POSIX_C_SOURCE 200809L

#include "cmem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include <stdint.h>
#include <execinfo.h>

#define MP_MAGIC_HEAD 0x4D504F4F  // "MPOO" in ASCII
#define MP_CANARY_BYTE 0xDE
#define MP_POISON_BYTE 0xDD

#define SLAB_CLASS_COUNT 7
static const size_t kSlabSizes[SLAB_CLASS_COUNT] = {8, 16, 32, 64, 128, 256, 512};
#define SLAB_MAX_SIZE 512
#define SLAB_PAGE_SIZE (16 * 1024) // 16 KB per slab page
#define TLS_CACHE_MAX_SLOTS 64
#define MAX_BACKTRACE_FRAMES 8

/* TLSF Allocator Constants */
#define TLSF_SL_SHIFT 4
#define TLSF_SL_COUNT (1 << TLSF_SL_SHIFT) // 16 subdivisions per FL
#define TLSF_FL_MAX 30                     // Up to 1GB
#define TLSF_MIN_BLOCK_SIZE 32
#define TLSF_MAX_SIZE (4 * 1024 * 1024)   // 4 MB threshold for TLSF vs Direct OS

#define BLOCK_STATE_FREE 0x1
#define BLOCK_STATE_PREV_FREE 0x2
#define BLOCK_SIZE_MASK (~(size_t)(BLOCK_STATE_FREE | BLOCK_STATE_PREV_FREE))

typedef enum {
    ALLOC_TYPE_SLAB = 1,
    ALLOC_TYPE_TLSF = 2,
    ALLOC_TYPE_OS   = 3
} mp_alloc_type_t;

/* Header prepended to every user payload */
typedef struct mp_block_header {
    uint32_t magic;
    uint8_t  alloc_type;
    uint8_t  slab_class;
    uint16_t flags;
    size_t   requested_size;
    size_t   usable_size;
    void*    raw_base;       // Base address from system/slab allocation
    void*    subpool;        // Pointer to owning sub-pool (e.g. tlsf_pool_t*)

    // Debug location tracking
    const char* alloc_file;
    int alloc_line;
    const char* alloc_func;
    void* backtrace_addrs[MAX_BACKTRACE_FRAMES];
    int backtrace_depth;

    struct mp_block_header* prev;
    struct mp_block_header* next;
} mp_block_header_t;

/* --- Slab Structs --- */
typedef struct mp_slab_slot {
    struct mp_slab_slot* next;
} mp_slab_slot_t;

typedef struct mp_slab_page {
    uint8_t class_index;
    uint16_t free_count;
    uint16_t total_slots;
    mp_slab_slot_t* free_list;
    struct mp_slab_page* next;
    struct mp_slab_page* prev;
    void* page_raw_mem;
} mp_slab_page_t;

typedef struct {
    size_t slot_size;
    mp_slab_page_t* partial_pages; // Pages with available free slots
    mp_slab_page_t* full_pages;    // Completely allocated pages
} mp_slab_class_t;

/* --- TLS Cache Struct for Lock-Free Small Allocations --- */
typedef struct {
    mp_slab_slot_t* slots[SLAB_CLASS_COUNT];
    uint16_t counts[SLAB_CLASS_COUNT];
} thread_cache_t;

#ifdef __cplusplus
#define MP_THREAD_LOCAL thread_local
#else
#define MP_THREAD_LOCAL _Thread_local
#endif

static MP_THREAD_LOCAL thread_cache_t tls_cache = {{0}, {0}};

/* --- TLSF Structs --- */
typedef struct tlsf_block {
    size_t size_and_flags;
    struct tlsf_block* prev_physical;
    struct tlsf_block* next_free;
    struct tlsf_block* prev_free;
} tlsf_block_t;

typedef struct tlsf_pool {
    uint32_t fl_bitmap;
    uint32_t sl_bitmap[TLSF_FL_MAX];
    tlsf_block_t* blocks[TLSF_FL_MAX][TLSF_SL_COUNT];
    void* raw_area;
    size_t raw_size;
    struct tlsf_pool* next;
} tlsf_pool_t;

/* --- Main Memory Pool Struct --- */
struct memory_pool {
    mp_flags_t flags;
    pthread_mutex_t lock;
    char arena_name[64];

    // Parent-Child Hierarchical Arena Tree
    struct memory_pool* parent;
    struct memory_pool* first_child;
    struct memory_pool* next_sibling;

    // Custom system allocator vtable
    bool has_custom_sys_alloc;
    mp_sys_allocator_t sys_allocator;

    // Profiling Event Callback
    mp_event_callback_t event_cb;
    void* event_user_data;

    // Diagnostics & Statistics
    mp_stats_t stats;
    mp_block_header_t* active_head; // Linked list of current active allocations for leak detection

    // Tier 1: Slab Allocators (8B - 512B)
    mp_slab_class_t slab_classes[SLAB_CLASS_COUNT];

    // Tier 2: TLSF Allocator (512B - 4MB)
    tlsf_pool_t* tlsf_root;
};

/* Lock Utilities */
static inline void pool_lock(memory_pool_t* pool) {
    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_lock(&pool->lock);
    }
}

static inline void pool_unlock(memory_pool_t* pool) {
    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_unlock(&pool->lock);
    }
}

/* Event Profiling Dispatcher */
static inline void trigger_event(memory_pool_t* pool, mp_event_type_t ev, void* ptr, size_t size) {
    if (pool->event_cb) {
        pool->event_cb(pool, ev, ptr, size, pool->event_user_data);
    }
}

/* Backing Memory Allocator Helpers */
static void* sys_mem_alloc(memory_pool_t* pool, size_t size, size_t alignment) {
    if (pool->has_custom_sys_alloc && pool->sys_allocator.sys_alloc) {
        return pool->sys_allocator.sys_alloc(size, pool->sys_allocator.user_data);
    }
    if (alignment > sizeof(void*)) {
        void* ptr = NULL;
        if (posix_memalign(&ptr, alignment, size) != 0) return NULL;
        return ptr;
    }
    return malloc(size);
}

static void sys_mem_free(memory_pool_t* pool, void* ptr, size_t size) {
    if (pool->flags & MP_FLAG_STATIC_BUFFER) return;
    if (pool->has_custom_sys_alloc && pool->sys_allocator.sys_free) {
        pool->sys_allocator.sys_free(ptr, size, pool->sys_allocator.user_data);
        return;
    }
    free(ptr);
}

/* Bitwise Utilities for TLSF */
static inline int tlsf_fls(size_t val) {
    if (val == 0) return -1;
    return 31 - __builtin_clz((uint32_t)val);
}

static inline int tlsf_ffs(uint32_t val) {
    if (val == 0) return -1;
    return __builtin_ctz(val);
}

static void tlsf_mapping_insert(size_t size, int* fl, int* sl) {
    if (size < (1 << TLSF_SL_SHIFT)) {
        *fl = 0;
        *sl = (int)size;
    } else {
        *fl = tlsf_fls(size);
        *sl = (int)((size >> (*fl - TLSF_SL_SHIFT)) ^ (1 << TLSF_SL_SHIFT));
    }
}

static void tlsf_mapping_search(size_t size, int* fl, int* sl) {
    if (size >= (1 << TLSF_SL_SHIFT)) {
        size_t round = (1 << (*fl = tlsf_fls(size) - TLSF_SL_SHIFT)) - 1;
        size += round;
    }
    tlsf_mapping_insert(size, fl, sl);
}

/* --- Slab Allocator Implementation --- */
static bool slab_init(memory_pool_t* pool) {
    for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
        pool->slab_classes[i].slot_size = kSlabSizes[i];
        pool->slab_classes[i].partial_pages = NULL;
        pool->slab_classes[i].full_pages = NULL;
    }
    return true;
}

static mp_slab_page_t* slab_create_page(memory_pool_t* pool, uint8_t class_idx) {
    size_t slot_payload_size = kSlabSizes[class_idx];
    size_t header_overhead = sizeof(mp_block_header_t);
    size_t total_slot_size = header_overhead + slot_payload_size + ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0);
    total_slot_size = (total_slot_size + 7) & ~7;

    void* raw_mem = sys_mem_alloc(pool, SLAB_PAGE_SIZE, SLAB_PAGE_SIZE);
    if (!raw_mem) return NULL;

    mp_slab_page_t* page = (mp_slab_page_t*)raw_mem;
    page->class_index = class_idx;
    page->page_raw_mem = raw_mem;
    page->next = NULL;
    page->prev = NULL;

    size_t usable_bytes = SLAB_PAGE_SIZE - sizeof(mp_slab_page_t);
    page->total_slots = (uint16_t)(usable_bytes / total_slot_size);
    page->free_count = page->total_slots;

    uint8_t* ptr = (uint8_t*)raw_mem + sizeof(mp_slab_page_t);
    page->free_list = (mp_slab_slot_t*)ptr;

    for (uint16_t i = 0; i < page->total_slots; i++) {
        mp_slab_slot_t* slot = (mp_slab_slot_t*)(ptr + i * total_slot_size);
        if (i < page->total_slots - 1) {
            slot->next = (mp_slab_slot_t*)(ptr + (i + 1) * total_slot_size);
        } else {
            slot->next = NULL;
        }
    }

    pool->stats.total_pool_size += SLAB_PAGE_SIZE;
    return page;
}

static void* slab_alloc(memory_pool_t* pool, uint8_t class_idx, size_t req_size) {
    mp_slab_class_t* sc = &pool->slab_classes[class_idx];
    mp_slab_page_t* page = sc->partial_pages;

    if (!page) {
        page = slab_create_page(pool, class_idx);
        if (!page) return NULL;

        page->next = sc->partial_pages;
        if (sc->partial_pages) sc->partial_pages->prev = page;
        sc->partial_pages = page;
    }

    mp_slab_slot_t* slot = page->free_list;
    page->free_list = slot->next;
    page->free_count--;

    if (page->free_count == 0) {
        sc->partial_pages = page->next;
        if (page->next) page->next->prev = NULL;

        page->next = sc->full_pages;
        page->prev = NULL;
        if (sc->full_pages) sc->full_pages->prev = page;
        sc->full_pages = page;
    }

    mp_block_header_t* header = (mp_block_header_t*)slot;
    header->magic = MP_MAGIC_HEAD;
    header->alloc_type = ALLOC_TYPE_SLAB;
    header->slab_class = class_idx;
    header->flags = 0;
    header->requested_size = req_size;
    header->usable_size = sc->slot_size;
    header->raw_base = slot;
    header->subpool = NULL;
    header->alloc_file = NULL;
    header->alloc_line = 0;
    header->alloc_func = NULL;
    header->backtrace_depth = 0;

    void* payload = (void*)((uint8_t*)header + sizeof(mp_block_header_t));

    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
        uint8_t* canary = (uint8_t*)payload + req_size;
        *canary = MP_CANARY_BYTE;
    }

    if (pool->flags & MP_FLAG_ZERO_ON_ALLOC) {
        memset(payload, 0, req_size);
    }

    pool->stats.slab_allocated_bytes += sc->slot_size;
    return payload;
}

static void slab_free(memory_pool_t* pool, mp_block_header_t* header) {
    uint8_t class_idx = header->slab_class;
    mp_slab_class_t* sc = &pool->slab_classes[class_idx];

    uintptr_t ptr_val = (uintptr_t)header->raw_base;
    uintptr_t page_base = ptr_val & ~(SLAB_PAGE_SIZE - 1);
    mp_slab_page_t* page = (mp_slab_page_t*)page_base;

    bool was_full = (page->free_count == 0);

    mp_slab_slot_t* slot = (mp_slab_slot_t*)header->raw_base;
    slot->next = page->free_list;
    page->free_list = slot;
    page->free_count++;

    pool->stats.slab_allocated_bytes -= sc->slot_size;

    if (was_full) {
        if (page->prev) page->prev->next = page->next;
        else sc->full_pages = page->next;
        if (page->next) page->next->prev = page->prev;

        page->next = sc->partial_pages;
        page->prev = NULL;
        if (sc->partial_pages) sc->partial_pages->prev = page;
        sc->partial_pages = page;
    }
}

/* --- TLSF Implementation --- */
static tlsf_pool_t* tlsf_create_pool_custom(memory_pool_t* pool, size_t size, void* custom_mem) {
    size = (size + 7) & ~7;
    void* raw_mem = custom_mem;
    if (!raw_mem) {
        raw_mem = sys_mem_alloc(pool, sizeof(tlsf_pool_t) + size, 8);
        if (!raw_mem) return NULL;
    }

    tlsf_pool_t* tpool = (tlsf_pool_t*)raw_mem;
    memset(tpool, 0, sizeof(tlsf_pool_t));
    tpool->raw_area = (void*)((uint8_t*)raw_mem + sizeof(tlsf_pool_t));
    tpool->raw_size = size;

    tlsf_block_t* block = (tlsf_block_t*)tpool->raw_area;
    block->size_and_flags = (size - sizeof(tlsf_block_t)) | BLOCK_STATE_FREE;
    block->prev_physical = NULL;
    block->next_free = NULL;
    block->prev_free = NULL;

    tlsf_block_t* sentinel = (tlsf_block_t*)((uint8_t*)block + (block->size_and_flags & BLOCK_SIZE_MASK));
    sentinel->size_and_flags = 0;
    sentinel->prev_physical = block;

    int fl, sl;
    tlsf_mapping_insert(block->size_and_flags & BLOCK_SIZE_MASK, &fl, &sl);
    tpool->fl_bitmap |= (1U << fl);
    tpool->sl_bitmap[fl] |= (1U << sl);
    tpool->blocks[fl][sl] = block;

    return tpool;
}

static void tlsf_insert_free_block(tlsf_pool_t* tpool, tlsf_block_t* block) {
    int fl, sl;
    size_t size = block->size_and_flags & BLOCK_SIZE_MASK;
    tlsf_mapping_insert(size, &fl, &sl);

    block->next_free = tpool->blocks[fl][sl];
    block->prev_free = NULL;
    if (tpool->blocks[fl][sl]) {
        tpool->blocks[fl][sl]->prev_free = block;
    }
    tpool->blocks[fl][sl] = block;

    tpool->fl_bitmap |= (1U << fl);
    tpool->sl_bitmap[fl] |= (1U << sl);
}

static void tlsf_remove_free_block(tlsf_pool_t* tpool, tlsf_block_t* block) {
    int fl, sl;
    size_t size = block->size_and_flags & BLOCK_SIZE_MASK;
    tlsf_mapping_insert(size, &fl, &sl);

    if (block->prev_free) {
        block->prev_free->next_free = block->next_free;
    } else {
        tpool->blocks[fl][sl] = block->next_free;
    }

    if (block->next_free) {
        block->next_free->prev_free = block->prev_free;
    }

    if (!tpool->blocks[fl][sl]) {
        tpool->sl_bitmap[fl] &= ~(1U << sl);
        if (!tpool->sl_bitmap[fl]) {
            tpool->fl_bitmap &= ~(1U << fl);
        }
    }
}

static tlsf_block_t* tlsf_find_suitable_block(tlsf_pool_t* tpool, size_t total_needed) {
    int fl = 0, sl = 0;
    tlsf_mapping_search(total_needed, &fl, &sl);

    uint32_t sl_map = tpool->sl_bitmap[fl] & (~0U << sl);
    if (sl_map) {
        sl = tlsf_ffs(sl_map);
        return tpool->blocks[fl][sl];
    }

    uint32_t fl_map = tpool->fl_bitmap & (~0U << (fl + 1));
    if (fl_map) {
        fl = tlsf_ffs(fl_map);
        sl_map = tpool->sl_bitmap[fl];
        sl = tlsf_ffs(sl_map);
        return tpool->blocks[fl][sl];
    }

    return NULL;
}

static void* tlsf_alloc(memory_pool_t* pool, size_t req_size) {
    size_t total_needed = sizeof(tlsf_block_t) + sizeof(mp_block_header_t) + req_size + ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0);
    total_needed = (total_needed + 7) & ~7;
    if (total_needed < TLSF_MIN_BLOCK_SIZE) total_needed = TLSF_MIN_BLOCK_SIZE;

    tlsf_pool_t* tpool = pool->tlsf_root;
    if (!tpool) {
        size_t init_sz = 4 * 1024 * 1024;
        pool->tlsf_root = tlsf_create_pool_custom(pool, init_sz, NULL);
        if (!pool->tlsf_root) return NULL;
        tpool = pool->tlsf_root;
        pool->stats.total_pool_size += init_sz + sizeof(tlsf_pool_t);
    }

    tlsf_block_t* block = NULL;
    tlsf_pool_t* target_pool = tpool;

    while (target_pool) {
        block = tlsf_find_suitable_block(target_pool, total_needed);
        if (block) break;
        target_pool = target_pool->next;
    }

    if (!block) {
        if (pool->flags & MP_FLAG_STATIC_BUFFER) return NULL;
        size_t expand_sz = (total_needed * 2 > 4 * 1024 * 1024) ? total_needed * 2 : 4 * 1024 * 1024;
        tlsf_pool_t* new_p = tlsf_create_pool_custom(pool, expand_sz, NULL);
        if (!new_p) return NULL;
        new_p->next = pool->tlsf_root;
        pool->tlsf_root = new_p;
        target_pool = new_p;
        pool->stats.total_pool_size += expand_sz + sizeof(tlsf_pool_t);

        block = tlsf_find_suitable_block(target_pool, total_needed);
        if (!block) return NULL;
    }

    tpool = target_pool;
    tlsf_remove_free_block(tpool, block);

    size_t current_size = block->size_and_flags & BLOCK_SIZE_MASK;
    size_t remaining = current_size - total_needed;

    if (remaining >= TLSF_MIN_BLOCK_SIZE + sizeof(tlsf_block_t)) {
        block->size_and_flags = total_needed | (block->size_and_flags & BLOCK_STATE_PREV_FREE);

        tlsf_block_t* split_block = (tlsf_block_t*)((uint8_t*)block + total_needed);
        split_block->size_and_flags = remaining | BLOCK_STATE_FREE;
        split_block->prev_physical = block;

        tlsf_block_t* next_phys = (tlsf_block_t*)((uint8_t*)split_block + remaining);
        next_phys->prev_physical = split_block;
        next_phys->size_and_flags |= BLOCK_STATE_PREV_FREE;

        tlsf_insert_free_block(tpool, split_block);
    } else {
        block->size_and_flags &= ~BLOCK_STATE_FREE;
        tlsf_block_t* next_phys = (tlsf_block_t*)((uint8_t*)block + current_size);
        next_phys->size_and_flags &= ~BLOCK_STATE_PREV_FREE;
    }

    mp_block_header_t* header = (mp_block_header_t*)((uint8_t*)block + sizeof(tlsf_block_t));
    header->magic = MP_MAGIC_HEAD;
    header->alloc_type = ALLOC_TYPE_TLSF;
    header->slab_class = 0;
    header->flags = 0;
    header->requested_size = req_size;
    header->usable_size = total_needed - sizeof(tlsf_block_t) - sizeof(mp_block_header_t);
    header->raw_base = block;
    header->subpool = tpool;
    header->alloc_file = NULL;
    header->alloc_line = 0;
    header->alloc_func = NULL;
    header->backtrace_depth = 0;

    void* payload = (void*)((uint8_t*)header + sizeof(mp_block_header_t));

    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
        uint8_t* canary = (uint8_t*)payload + req_size;
        *canary = MP_CANARY_BYTE;
    }

    if (pool->flags & MP_FLAG_ZERO_ON_ALLOC) {
        memset(payload, 0, req_size);
    }

    pool->stats.tlsf_allocated_bytes += header->usable_size;
    return payload;
}

static void tlsf_free(memory_pool_t* pool, mp_block_header_t* header) {
    tlsf_block_t* block = (tlsf_block_t*)header->raw_base;
    tlsf_pool_t* tpool = (tlsf_pool_t*)header->subpool;

    pool->stats.tlsf_allocated_bytes -= header->usable_size;

    size_t size = block->size_and_flags & BLOCK_SIZE_MASK;
    block->size_and_flags |= BLOCK_STATE_FREE;

    tlsf_block_t* next_phys = (tlsf_block_t*)((uint8_t*)block + size);
    if (next_phys->size_and_flags & BLOCK_STATE_FREE) {
        tlsf_remove_free_block(tpool, next_phys);
        size += (next_phys->size_and_flags & BLOCK_SIZE_MASK);
        block->size_and_flags = size | (block->size_and_flags & BLOCK_STATE_PREV_FREE) | BLOCK_STATE_FREE;

        tlsf_block_t* after_next = (tlsf_block_t*)((uint8_t*)block + size);
        after_next->prev_physical = block;
    }

    if (block->size_and_flags & BLOCK_STATE_PREV_FREE) {
        tlsf_block_t* prev_phys = block->prev_physical;
        if (prev_phys && (prev_phys->size_and_flags & BLOCK_STATE_FREE)) {
            tlsf_remove_free_block(tpool, prev_phys);
            size_t prev_size = prev_phys->size_and_flags & BLOCK_SIZE_MASK;
            prev_phys->size_and_flags = (prev_size + size) | (prev_phys->size_and_flags & BLOCK_STATE_PREV_FREE) | BLOCK_STATE_FREE;

            tlsf_block_t* after_block = (tlsf_block_t*)((uint8_t*)prev_phys + prev_size + size);
            after_block->prev_physical = prev_phys;
            block = prev_phys;
        }
    }

    next_phys = (tlsf_block_t*)((uint8_t*)block + (block->size_and_flags & BLOCK_SIZE_MASK));
    next_phys->size_and_flags |= BLOCK_STATE_PREV_FREE;

    tlsf_insert_free_block(tpool, block);
}

/* --- Public API Implementation --- */
memory_pool_t* mp_create(size_t initial_capacity, mp_flags_t flags) {
    return mp_create_custom(initial_capacity, flags, NULL);
}

memory_pool_t* mp_create_child(memory_pool_t* parent, size_t initial_capacity, mp_flags_t flags, const char* arena_name) {
    memory_pool_t* child = mp_create(initial_capacity, flags);
    if (!child) return NULL;

    child->parent = parent;
    if (arena_name) snprintf(child->arena_name, sizeof(child->arena_name), "%s", arena_name);
    else snprintf(child->arena_name, sizeof(child->arena_name), "ChildArena");

    if (parent) {
        pool_lock(parent);
        child->next_sibling = parent->first_child;
        parent->first_child = child;
        pool_unlock(parent);
    }
    return child;
}

memory_pool_t* mp_create_custom(size_t initial_capacity, mp_flags_t flags, const mp_sys_allocator_t* sys_allocator) {
    memory_pool_t* pool = (memory_pool_t*)calloc(1, sizeof(memory_pool_t));
    if (!pool) return NULL;

    pool->flags = flags;
    snprintf(pool->arena_name, sizeof(pool->arena_name), "RootArena");
    if (sys_allocator) {
        pool->has_custom_sys_alloc = true;
        pool->sys_allocator = *sys_allocator;
    }

    if (flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_init(&pool->lock, NULL);
    }

    slab_init(pool);

    if (initial_capacity > 0) {
        pool->tlsf_root = tlsf_create_pool_custom(pool, initial_capacity, NULL);
        if (pool->tlsf_root) {
            pool->stats.total_pool_size += initial_capacity + sizeof(tlsf_pool_t);
        }
    }

    return pool;
}

memory_pool_t* mp_create_from_buffer(void* buffer, size_t buffer_size, mp_flags_t flags) {
    if (!buffer || buffer_size < sizeof(memory_pool_t) + sizeof(tlsf_pool_t) + TLSF_MIN_BLOCK_SIZE) {
        return NULL;
    }

    uintptr_t buf_addr = (uintptr_t)buffer;
    uintptr_t aligned_addr = (buf_addr + 7) & ~7;
    size_t align_offset = aligned_addr - buf_addr;

    if (buffer_size <= align_offset + sizeof(memory_pool_t) + sizeof(tlsf_pool_t) + TLSF_MIN_BLOCK_SIZE) {
        return NULL;
    }

    memory_pool_t* pool = (memory_pool_t*)aligned_addr;
    memset(pool, 0, sizeof(memory_pool_t));
    pool->flags = (mp_flags_t)(flags | MP_FLAG_STATIC_BUFFER);
    snprintf(pool->arena_name, sizeof(pool->arena_name), "StaticBufferArena");

    slab_init(pool);

    uint8_t* remain_mem = (uint8_t*)aligned_addr + sizeof(memory_pool_t);
    size_t remain_sz = buffer_size - align_offset - sizeof(memory_pool_t) - sizeof(tlsf_pool_t);

    pool->tlsf_root = tlsf_create_pool_custom(pool, remain_sz, remain_mem);
    if (!pool->tlsf_root) return NULL;

    pool->stats.total_pool_size = buffer_size;
    return pool;
}

void mp_destroy(memory_pool_t* pool) {
    if (!pool) return;

    memory_pool_t* child = pool->first_child;
    while (child) {
        memory_pool_t* next = child->next_sibling;
        mp_destroy(child);
        child = next;
    }

    if (!(pool->flags & MP_FLAG_STATIC_BUFFER)) {
        for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
            mp_slab_page_t* curr = pool->slab_classes[i].partial_pages;
            while (curr) {
                mp_slab_page_t* next = curr->next;
                sys_mem_free(pool, curr->page_raw_mem, SLAB_PAGE_SIZE);
                curr = next;
            }
            curr = pool->slab_classes[i].full_pages;
            while (curr) {
                mp_slab_page_t* next = curr->next;
                sys_mem_free(pool, curr->page_raw_mem, SLAB_PAGE_SIZE);
                curr = next;
            }
        }

        tlsf_pool_t* tcurr = pool->tlsf_root;
        while (tcurr) {
            tlsf_pool_t* tnext = tcurr->next;
            sys_mem_free(pool, tcurr, tcurr->raw_size + sizeof(tlsf_pool_t));
            tcurr = tnext;
        }

        if (pool->flags & MP_FLAG_THREAD_SAFE) {
            pthread_mutex_destroy(&pool->lock);
        }

        sys_mem_free(pool, pool, sizeof(memory_pool_t));
    }
}

void mp_reset(memory_pool_t* pool) {
    if (!pool) return;
    pool_lock(pool);

    memory_pool_t* child = pool->first_child;
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

    for (int c = 0; c < SLAB_CLASS_COUNT; c++) {
        mp_slab_class_t* sc = &pool->slab_classes[c];
        mp_slab_page_t* page = sc->partial_pages;

        while (sc->full_pages) {
            mp_slab_page_t* p = sc->full_pages;
            sc->full_pages = p->next;
            p->next = sc->partial_pages;
            if (sc->partial_pages) sc->partial_pages->prev = p;
            p->prev = NULL;
            sc->partial_pages = p;
        }

        page = sc->partial_pages;
        size_t slot_payload_size = kSlabSizes[c];
        size_t header_overhead = sizeof(mp_block_header_t);
        size_t total_slot_size = header_overhead + slot_payload_size + ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0);
        total_slot_size = (total_slot_size + 7) & ~7;

        while (page) {
            page->free_count = page->total_slots;
            uint8_t* ptr = (uint8_t*)page->page_raw_mem + sizeof(mp_slab_page_t);
            page->free_list = (mp_slab_slot_t*)ptr;

            for (uint16_t i = 0; i < page->total_slots; i++) {
                mp_slab_slot_t* slot = (mp_slab_slot_t*)(ptr + i * total_slot_size);
                slot->next = (i < page->total_slots - 1) ? (mp_slab_slot_t*)(ptr + (i + 1) * total_slot_size) : NULL;
            }
            page = page->next;
        }
    }

    tlsf_pool_t* tcurr = pool->tlsf_root;
    while (tcurr) {
        memset(tcurr->sl_bitmap, 0, sizeof(tcurr->sl_bitmap));
        tcurr->fl_bitmap = 0;
        memset(tcurr->blocks, 0, sizeof(tcurr->blocks));

        tlsf_block_t* block = (tlsf_block_t*)tcurr->raw_area;
        block->size_and_flags = (tcurr->raw_size - sizeof(tlsf_block_t)) | BLOCK_STATE_FREE;
        block->prev_physical = NULL;
        block->next_free = NULL;
        block->prev_free = NULL;

        tlsf_block_t* sentinel = (tlsf_block_t*)((uint8_t*)block + (block->size_and_flags & BLOCK_SIZE_MASK));
        sentinel->size_and_flags = 0;
        sentinel->prev_physical = block;

        tlsf_insert_free_block(tcurr, block);
        tcurr = tcurr->next;
    }

    trigger_event(pool, MP_EVENT_RESET, NULL, 0);
    pool_unlock(pool);
}

size_t mp_compact(memory_pool_t* pool) {
    if (!pool || (pool->flags & MP_FLAG_STATIC_BUFFER)) return 0;
    pool_lock(pool);

    size_t freed_bytes = 0;

    for (int c = 0; c < SLAB_CLASS_COUNT; c++) {
        mp_slab_class_t* sc = &pool->slab_classes[c];
        mp_slab_page_t* curr = sc->partial_pages;

        while (curr) {
            mp_slab_page_t* next = curr->next;
            if (curr->free_count == curr->total_slots) {
                // Page is completely empty, remove it and return to system OS
                if (curr->prev) curr->prev->next = curr->next;
                else sc->partial_pages = curr->next;
                if (curr->next) curr->next->prev = curr->prev;

                sys_mem_free(pool, curr->page_raw_mem, SLAB_PAGE_SIZE);
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

void mp_set_event_callback(memory_pool_t* pool, mp_event_callback_t callback, void* user_data) {
    if (!pool) return;
    pool_lock(pool);
    pool->event_cb = callback;
    pool->event_user_data = user_data;
    pool_unlock(pool);
}

static void active_list_add(memory_pool_t* pool, mp_block_header_t* header) {
    header->next = pool->active_head;
    header->prev = NULL;
    if (pool->active_head) {
        pool->active_head->prev = header;
    }
    pool->active_head = header;
}

static void active_list_remove(memory_pool_t* pool, mp_block_header_t* header) {
    if (header->prev) header->prev->next = header->next;
    else pool->active_head = header->next;
    if (header->next) header->next->prev = header->prev;
}

void* mp_alloc_loc(memory_pool_t* pool, size_t size, const char* file, int line, const char* func) {
    void* ptr = mp_alloc(pool, size);
    if (ptr) {
        mp_block_header_t* header = (mp_block_header_t*)((uint8_t*)ptr - sizeof(mp_block_header_t));
        header->alloc_file = file;
        header->alloc_line = line;
        header->alloc_func = func;
        if (pool->flags & MP_FLAG_TRACK_LOCATIONS) {
            header->backtrace_depth = backtrace(header->backtrace_addrs, MAX_BACKTRACE_FRAMES);
        }
    }
    return ptr;
}

void* mp_calloc_loc(memory_pool_t* pool, size_t num, size_t size, const char* file, int line, const char* func) {
    void* ptr = mp_calloc(pool, num, size);
    if (ptr) {
        mp_block_header_t* header = (mp_block_header_t*)((uint8_t*)ptr - sizeof(mp_block_header_t));
        header->alloc_file = file;
        header->alloc_line = line;
        header->alloc_func = func;
        if (pool->flags & MP_FLAG_TRACK_LOCATIONS) {
            header->backtrace_depth = backtrace(header->backtrace_addrs, MAX_BACKTRACE_FRAMES);
        }
    }
    return ptr;
}

void* mp_realloc_loc(memory_pool_t* pool, void* ptr, size_t new_size, const char* file, int line, const char* func) {
    void* new_ptr = mp_realloc(pool, ptr, new_size);
    if (new_ptr) {
        mp_block_header_t* header = (mp_block_header_t*)((uint8_t*)new_ptr - sizeof(mp_block_header_t));
        header->alloc_file = file;
        header->alloc_line = line;
        header->alloc_func = func;
        if (pool->flags & MP_FLAG_TRACK_LOCATIONS) {
            header->backtrace_depth = backtrace(header->backtrace_addrs, MAX_BACKTRACE_FRAMES);
        }
    }
    return new_ptr;
}

void* mp_alloc(memory_pool_t* pool, size_t size) {
    if (!pool || size == 0) return NULL;

    if ((pool->flags & MP_FLAG_THREAD_LOCAL_CACHE) && size <= SLAB_MAX_SIZE) {
        uint8_t class_idx = 0;
        while (class_idx < SLAB_CLASS_COUNT && kSlabSizes[class_idx] < size) {
            class_idx++;
        }
        if (tls_cache.counts[class_idx] > 0) {
            mp_slab_slot_t* slot = tls_cache.slots[class_idx];
            tls_cache.slots[class_idx] = slot->next;
            tls_cache.counts[class_idx]--;

            mp_block_header_t* header = (mp_block_header_t*)slot;
            header->magic = MP_MAGIC_HEAD;
            header->alloc_type = ALLOC_TYPE_SLAB;
            header->slab_class = class_idx;
            header->flags = 0;
            header->requested_size = size;
            header->usable_size = kSlabSizes[class_idx];
            header->raw_base = slot;
            header->alloc_file = NULL;
            header->alloc_line = 0;
            header->alloc_func = NULL;
            header->backtrace_depth = 0;

            void* payload = (void*)((uint8_t*)header + sizeof(mp_block_header_t));
            if (pool->flags & MP_FLAG_ZERO_ON_ALLOC) memset(payload, 0, size);

            pool_lock(pool);
            active_list_add(pool, header);
            pool->stats.active_bytes += size;
            if (pool->stats.active_bytes > pool->stats.peak_bytes) {
                pool->stats.peak_bytes = pool->stats.active_bytes;
            }
            pool->stats.active_allocations++;
            pool->stats.total_alloc_ops++;
            pool_unlock(pool);

            trigger_event(pool, MP_EVENT_ALLOC, payload, size);
            return payload;
        }
    }

    pool_lock(pool);
    void* ptr = NULL;

    if ((pool->flags & MP_FLAG_STATIC_BUFFER) == 0 && size <= SLAB_MAX_SIZE) {
        uint8_t class_idx = 0;
        while (class_idx < SLAB_CLASS_COUNT && kSlabSizes[class_idx] < size) {
            class_idx++;
        }
        ptr = slab_alloc(pool, class_idx, size);
    } else if (size <= TLSF_MAX_SIZE || (pool->flags & MP_FLAG_STATIC_BUFFER)) {
        ptr = tlsf_alloc(pool, size);
    } else {
        size_t total_sz = size + sizeof(mp_block_header_t) + ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0);
        void* raw_mem = sys_mem_alloc(pool, total_sz, 8);
        if (raw_mem) {
            mp_block_header_t* header = (mp_block_header_t*)raw_mem;
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

            ptr = (void*)((uint8_t*)header + sizeof(mp_block_header_t));

            if (pool->flags & MP_FLAG_DEBUG_CANARY) {
                uint8_t* canary = (uint8_t*)ptr + size;
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
        mp_block_header_t* header = (mp_block_header_t*)((uint8_t*)ptr - sizeof(mp_block_header_t));
        active_list_add(pool, header);

        pool->stats.active_bytes += size;
        if (pool->stats.active_bytes > pool->stats.peak_bytes) {
            pool->stats.peak_bytes = pool->stats.active_bytes;
        }
        pool->stats.active_allocations++;
        pool->stats.total_alloc_ops++;

        trigger_event(pool, MP_EVENT_ALLOC, ptr, size);
    }

    pool_unlock(pool);
    return ptr;
}

size_t mp_alloc_batch(memory_pool_t* pool, size_t size, void** out_ptrs, size_t count) {
    if (!pool || !out_ptrs || count == 0) return 0;
    size_t allocated = 0;
    for (size_t i = 0; i < count; i++) {
        out_ptrs[i] = mp_alloc(pool, size);
        if (out_ptrs[i]) allocated++;
        else break;
    }
    return allocated;
}

void mp_free_batch(memory_pool_t* pool, void** ptrs, size_t count) {
    if (!pool || !ptrs || count == 0) return;
    for (size_t i = 0; i < count; i++) {
        if (ptrs[i]) {
            mp_free(pool, ptrs[i]);
            ptrs[i] = NULL;
        }
    }
}

void* mp_calloc(memory_pool_t* pool, size_t num, size_t size) {
    size_t total_size = num * size;
    void* ptr = mp_alloc(pool, total_size);
    if (ptr && !(pool->flags & MP_FLAG_ZERO_ON_ALLOC)) {
        memset(ptr, 0, total_size);
    }
    return ptr;
}

void mp_free(memory_pool_t* pool, void* ptr) {
    if (!pool || !ptr) return;

    mp_block_header_t* header = (mp_block_header_t*)((uint8_t*)ptr - sizeof(mp_block_header_t));

    if (header->magic != MP_MAGIC_HEAD) {
        fprintf(stderr, "[MEMORY_POOL ERROR] Corrupt header or invalid free on pointer %p!\n", ptr);
        trigger_event(pool, MP_EVENT_DOUBLE_FREE, ptr, 0);
        return;
    }

    if (pool->flags & MP_FLAG_POISON_ON_FREE) {
        memset(ptr, MP_POISON_BYTE, header->requested_size);
    }

    if ((pool->flags & MP_FLAG_THREAD_LOCAL_CACHE) && header->alloc_type == ALLOC_TYPE_SLAB) {
        uint8_t class_idx = header->slab_class;
        if (tls_cache.counts[class_idx] < TLS_CACHE_MAX_SLOTS) {
            pool_lock(pool);
            active_list_remove(pool, header);
            pool->stats.active_bytes -= header->requested_size;
            pool->stats.active_allocations--;
            pool->stats.total_free_ops++;
            trigger_event(pool, MP_EVENT_FREE, ptr, header->requested_size);
            pool_unlock(pool);

            mp_slab_slot_t* slot = (mp_slab_slot_t*)header->raw_base;
            slot->next = tls_cache.slots[class_idx];
            tls_cache.slots[class_idx] = slot;
            tls_cache.counts[class_idx]++;
            return;
        }
    }

    pool_lock(pool);

    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
        uint8_t* canary = (uint8_t*)ptr + header->requested_size;
        if (*canary != MP_CANARY_BYTE) {
            fprintf(stderr, "[MEMORY_POOL BUG] Buffer overflow detected at pointer %p!\n", ptr);
            trigger_event(pool, MP_EVENT_CANARY_CORRUPTION, ptr, header->requested_size);
        }
    }

    active_list_remove(pool, header);
    pool->stats.active_bytes -= header->requested_size;
    pool->stats.active_allocations--;
    pool->stats.total_free_ops++;
    trigger_event(pool, MP_EVENT_FREE, ptr, header->requested_size);

    if (header->alloc_type == ALLOC_TYPE_SLAB) {
        slab_free(pool, header);
    } else if (header->alloc_type == ALLOC_TYPE_TLSF) {
        tlsf_free(pool, header);
    } else if (header->alloc_type == ALLOC_TYPE_OS) {
        pool->stats.os_allocated_bytes -= header->requested_size;
        pool->stats.total_pool_size -= (header->requested_size + sizeof(mp_block_header_t) + ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0));
        sys_mem_free(pool, header->raw_base, header->requested_size);
    }

    pool_unlock(pool);
}

void* mp_realloc(memory_pool_t* pool, void* ptr, size_t new_size) {
    if (!ptr) return mp_alloc(pool, new_size);
    if (new_size == 0) {
        mp_free(pool, ptr);
        return NULL;
    }

    mp_block_header_t* header = (mp_block_header_t*)((uint8_t*)ptr - sizeof(mp_block_header_t));
    if (header->magic != MP_MAGIC_HEAD) return NULL;

    if (new_size <= header->usable_size) {
        header->requested_size = new_size;
        trigger_event(pool, MP_EVENT_REALLOC, ptr, new_size);
        return ptr;
    }

    void* new_ptr = mp_alloc(pool, new_size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, header->requested_size);
        mp_free(pool, ptr);
    }
    return new_ptr;
}

void* mp_aligned_alloc(memory_pool_t* pool, size_t alignment, size_t size) {
    if ((alignment & (alignment - 1)) != 0 || alignment < sizeof(void*)) {
        return NULL;
    }

    size_t total_size = size + alignment + sizeof(mp_block_header_t);
    void* raw_ptr = mp_alloc(pool, total_size);
    if (!raw_ptr) return NULL;

    uintptr_t raw_addr = (uintptr_t)raw_ptr;
    uintptr_t aligned_addr = (raw_addr + sizeof(mp_block_header_t) + (alignment - 1)) & ~(alignment - 1);

    mp_block_header_t* orig_header = (mp_block_header_t*)((uint8_t*)raw_ptr - sizeof(mp_block_header_t));
    mp_block_header_t* new_header = (mp_block_header_t*)(aligned_addr - sizeof(mp_block_header_t));

    if (new_header != orig_header) {
        pool_lock(pool);
        *new_header = *orig_header;
        new_header->requested_size = size;

        if (orig_header->prev) orig_header->prev->next = new_header;
        else pool->active_head = new_header;
        if (orig_header->next) orig_header->next->prev = new_header;
        pool_unlock(pool);
    }

    return (void*)aligned_addr;
}

/* --- Heap Integrity, Leak Analysis & HTML Dashboard --- */

bool mp_audit_heap(memory_pool_t* pool) {
    if (!pool) return true;
    pool_lock(pool);

    bool healthy = true;
    mp_block_header_t* curr = pool->active_head;

    while (curr) {
        void* payload = (void*)((uint8_t*)curr + sizeof(mp_block_header_t));

        if (curr->magic != MP_MAGIC_HEAD) {
            fprintf(stderr, "[HEAP AUDIT ERROR] Corrupted header magic at %p! (Found: 0x%X, Expected: 0x%X)\n",
                    payload, curr->magic, MP_MAGIC_HEAD);
            healthy = false;
        }

        if (pool->flags & MP_FLAG_DEBUG_CANARY) {
            uint8_t* canary = (uint8_t*)payload + curr->requested_size;
            if (*canary != MP_CANARY_BYTE) {
                fprintf(stderr, "[HEAP AUDIT ERROR] Redzone canary corruption at %p! (Size: %zu, Source: %s:%d in %s)\n",
                        payload, curr->requested_size,
                        curr->alloc_file ? curr->alloc_file : "unknown",
                        curr->alloc_line,
                        curr->alloc_func ? curr->alloc_func : "unknown");
                healthy = false;
            }
        }
        curr = curr->next;
    }

    if (healthy) {
        printf("[HEAP AUDIT HEALTH] Heap integrity check passed cleanly! All active blocks valid.\n");
    }

    pool_unlock(pool);
    return healthy;
}

size_t mp_analyze_leaks(memory_pool_t* pool, char* report_buf, size_t max_len) {
    if (!pool || !report_buf || max_len == 0) return 0;
    pool_lock(pool);

    size_t offset = 0;
    offset += snprintf(report_buf + offset, max_len - offset,
        "=================== DETAILED MEMORY LEAK ANALYSIS REPORT ===================\n"
        "  Total Managed System Memory: %zu bytes (%.2f KB)\n"
        "  Active Leaked Allocations  : %zu blocks\n"
        "  Total Leaked Payload Bytes : %zu bytes (%.2f KB)\n"
        "============================================================================\n",
        pool->stats.total_pool_size, pool->stats.total_pool_size / 1024.0,
        pool->stats.active_allocations,
        pool->stats.active_bytes, pool->stats.active_bytes / 1024.0
    );

    if (pool->stats.active_allocations == 0) {
        offset += snprintf(report_buf + offset, max_len - offset, "  No memory leaks detected! Clean execution.\n");
        pool_unlock(pool);
        return offset;
    }

    mp_block_header_t* curr = pool->active_head;
    size_t idx = 1;

    while (curr && offset < max_len) {
        void* payload = (void*)((uint8_t*)curr + sizeof(mp_block_header_t));
        const char* tier_str = (curr->alloc_type == ALLOC_TYPE_SLAB) ? "SLAB" :
                               ((curr->alloc_type == ALLOC_TYPE_TLSF) ? "TLSF" : "DIRECT OS");

        offset += snprintf(report_buf + offset, max_len - offset,
            "\n[Leak #%zu] Address: %p | Payload Size: %zu bytes | Tier: %s\n",
            idx++, payload, curr->requested_size, tier_str
        );

        if (curr->alloc_file) {
            offset += snprintf(report_buf + offset, max_len - offset,
                "  Source Location : %s:%d (function '%s')\n",
                curr->alloc_file, curr->alloc_line, curr->alloc_func ? curr->alloc_func : "unknown"
            );
        } else {
            offset += snprintf(report_buf + offset, max_len - offset,
                "  Source Location : (Location tracking disabled, enable MP_FLAG_TRACK_LOCATIONS)\n"
            );
        }

        if (curr->backtrace_depth > 0) {
            char** symbols = backtrace_symbols(curr->backtrace_addrs, curr->backtrace_depth);
            offset += snprintf(report_buf + offset, max_len - offset, "  Callstack Frames:\n");
            for (int f = 0; f < curr->backtrace_depth && offset < max_len; f++) {
                offset += snprintf(report_buf + offset, max_len - offset,
                    "    #%d %s\n", f, symbols ? symbols[f] : "unknown"
                );
            }
            if (symbols) free(symbols);
        }

        curr = curr->next;
    }

    pool_unlock(pool);
    return offset;
}

bool mp_export_leak_report(memory_pool_t* pool, const char* filepath) {
    if (!pool || !filepath) return false;
    char buffer[16384];
    size_t report_len = mp_analyze_leaks(pool, buffer, sizeof(buffer));

    FILE* f = fopen(filepath, "w");
    if (!f) return false;

    fwrite(buffer, 1, report_len, f);
    fclose(f);
    printf("[CMEM DIAGNOSTICS] Detailed memory leak report exported to: %s\n", filepath);
    return true;
}

bool mp_export_html_report(memory_pool_t* pool, const char* filepath) {
    if (!pool || !filepath) return false;
    FILE* f = fopen(filepath, "w");
    if (!f) return false;

    mp_stats_t stats;
    mp_get_stats(pool, &stats);

    fprintf(f, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
               "<meta charset=\"UTF-8\">\n"
               "<title>cmem Profile & Leak Analysis Dashboard</title>\n"
               "<style>\n"
               "  body { font-family: 'Inter', system-ui, sans-serif; background: #0f172a; color: #f8fafc; margin: 0; padding: 2rem; }\n"
               "  .container { max-width: 1100px; margin: 0 auto; }\n"
               "  h1 { color: #38bdf8; font-size: 2rem; border-bottom: 2px solid #334155; padding-bottom: 0.5rem; }\n"
               "  .cards { display: grid; grid-template-columns: repeat(4, 1fr); gap: 1rem; margin: 1.5rem 0; }\n"
               "  .card { background: #1e293b; padding: 1.2rem; border-radius: 10px; border: 1px solid #334155; box-shadow: 0 4px 6px -1px rgba(0,0,0,0.3); }\n"
               "  .card h3 { margin: 0; font-size: 0.85rem; color: #94a3b8; text-transform: uppercase; }\n"
               "  .card .val { font-size: 1.6rem; font-weight: bold; color: #38bdf8; margin-top: 0.4rem; }\n"
               "  .progress-bar { background: #334155; height: 24px; border-radius: 12px; overflow: hidden; display: flex; margin: 1.5rem 0; }\n"
               "  .bar-slab { background: #10b981; text-align: center; font-size: 0.8rem; line-height: 24px; color: #fff; }\n"
               "  .bar-tlsf { background: #6366f1; text-align: center; font-size: 0.8rem; line-height: 24px; color: #fff; }\n"
               "  .bar-os { background: #f59e0b; text-align: center; font-size: 0.8rem; line-height: 24px; color: #fff; }\n"
               "  table { width: 100%%; border-collapse: collapse; background: #1e293b; border-radius: 8px; overflow: hidden; margin-top: 1rem; }\n"
               "  th, td { padding: 0.8rem 1rem; text-align: left; border-bottom: 1px solid #334155; }\n"
               "  th { background: #334155; color: #cbd5e1; font-weight: 600; }\n"
               "  tr:hover { background: #334155; }\n"
               "  .badge { padding: 0.25rem 0.5rem; border-radius: 4px; font-size: 0.75rem; font-weight: bold; }\n"
               "  .badge-slab { background: #064e3b; color: #34d399; }\n"
               "  .badge-tlsf { background: #312e81; color: #818cf8; }\n"
               "  .badge-os { background: #78350f; color: #fbbf24; }\n"
               "</style>\n</head>\n<body>\n"
               "<div class=\"container\">\n"
               "  <h1>cmem Visual Profiler & Leak Analysis Dashboard</h1>\n"
               "  <div class=\"cards\">\n"
               "    <div class=\"card\"><h3>Total Reserved</h3><div class=\"val\">%.2f KB</div></div>\n"
               "    <div class=\"card\"><h3>Active Payload</h3><div class=\"val\">%.2f KB</div></div>\n"
               "    <div class=\"card\"><h3>Active Blocks</h3><div class=\"val\">%zu</div></div>\n"
               "    <div class=\"card\"><h3>Fragmentation</h3><div class=\"val\">%.1f%%</div></div>\n"
               "  </div>\n",
               stats.total_pool_size / 1024.0, stats.active_bytes / 1024.0, stats.active_allocations, stats.fragmentation_ratio * 100.0
    );

    size_t total_alloc = stats.slab_allocated_bytes + stats.tlsf_allocated_bytes + stats.os_allocated_bytes;
    size_t tot = (total_alloc > 0) ? total_alloc : 1;
    double p_slab = (stats.slab_allocated_bytes * 100.0) / tot;
    double p_tlsf = (stats.tlsf_allocated_bytes * 100.0) / tot;
    double p_os   = (stats.os_allocated_bytes * 100.0) / tot;

    fprintf(f, "  <h2>Allocation Tier Distribution</h2>\n"
               "  <div class=\"progress-bar\">\n"
               "    <div class=\"bar-slab\" style=\"width: %.1f%%;\">Slab (%.1f%%)</div>\n"
               "    <div class=\"bar-tlsf\" style=\"width: %.1f%%;\">TLSF (%.1f%%)</div>\n"
               "    <div class=\"bar-os\" style=\"width: %.1f%%;\">OS (%.1f%%)</div>\n"
               "  </div>\n",
               p_slab, p_slab, p_tlsf, p_tlsf, p_os, p_os
    );

    fprintf(f, "  <h2>Active Memory Allocations & Leak Inventory (%zu Blocks)</h2>\n"
               "  <table>\n"
               "    <thead><tr><th>#</th><th>Address</th><th>Size</th><th>Tier</th><th>Source Location</th><th>Function</th></tr></thead>\n"
               "    <tbody>\n",
               stats.active_allocations
    );

    pool_lock(pool);
    mp_block_header_t* curr = pool->active_head;
    size_t idx = 1;

    while (curr) {
        void* payload = (void*)((uint8_t*)curr + sizeof(mp_block_header_t));
        const char* badge_cls = (curr->alloc_type == ALLOC_TYPE_SLAB) ? "badge-slab" :
                               ((curr->alloc_type == ALLOC_TYPE_TLSF) ? "badge-tlsf" : "badge-os");
        const char* tier_name = (curr->alloc_type == ALLOC_TYPE_SLAB) ? "SLAB" :
                               ((curr->alloc_type == ALLOC_TYPE_TLSF) ? "TLSF" : "OS");

        fprintf(f, "      <tr><td>%zu</td><td><code>%p</code></td><td>%zu B</td>"
                   "<td><span class=\"badge %s\">%s</span></td><td>%s:%d</td><td><code>%s</code></td></tr>\n",
                   idx++, payload, curr->requested_size, badge_cls, tier_name,
                   curr->alloc_file ? curr->alloc_file : "-", curr->alloc_line,
                   curr->alloc_func ? curr->alloc_func : "-"
        );
        curr = curr->next;
    }
    pool_unlock(pool);

    fprintf(f, "    </tbody>\n  </table>\n</div>\n</body>\n</html>\n");
    fclose(f);

    printf("[CMEM DIAGNOSTICS] Interactive HTML Profiler Report exported to: %s\n", filepath);
    return true;
}

void mp_get_stats(memory_pool_t* pool, mp_stats_t* stats) {
    if (!pool || !stats) return;
    pool_lock(pool);
    *stats = pool->stats;
    size_t total_sys = pool->stats.total_pool_size > 0 ? pool->stats.total_pool_size : 1;
    stats->fragmentation_ratio = 1.0 - ((double)pool->stats.active_bytes / (double)total_sys);
    pool_unlock(pool);
}

void mp_dump_info(memory_pool_t* pool) {
    if (!pool) return;
    mp_stats_t stats;
    mp_get_stats(pool, &stats);

    printf("\n================ CMEM DIAGNOSTICS DUMP [%s] ================\n", pool->arena_name);
    printf("  Total System Reserved Memory: %zu bytes (%.2f KB)\n", stats.total_pool_size, stats.total_pool_size / 1024.0);
    printf("  Current Active Allocations  : %zu blocks, %zu bytes (%.2f KB)\n", stats.active_allocations, stats.active_bytes, stats.active_bytes / 1024.0);
    printf("  Peak Memory Allocation      : %zu bytes (%.2f KB)\n", stats.peak_bytes, stats.peak_bytes / 1024.0);
    printf("  Estimated Fragmentation     : %.2f%%\n", stats.fragmentation_ratio * 100.0);
    printf("  Cumulative Stats            : %zu Allocations, %zu Frees\n", stats.total_alloc_ops, stats.total_free_ops);
    printf("  Allocation Tier Breakdown   :\n");
    printf("    - Slab Pool (Small <=512B): %zu bytes\n", stats.slab_allocated_bytes);
    printf("    - TLSF Pool (Med <=4MB)   : %zu bytes\n", stats.tlsf_allocated_bytes);
    printf("    - Direct OS (Large >4MB)  : %zu bytes\n", stats.os_allocated_bytes);
    printf("==============================================================\n\n");
}

static void print_arena_node(memory_pool_t* pool, int indent) {
    if (!pool) return;
    for (int i = 0; i < indent; i++) printf("  ");
    printf("|- [Arena: %s] Active Bytes: %zu B, Active Allocations: %zu\n",
           pool->arena_name, pool->stats.active_bytes, pool->stats.active_allocations);

    memory_pool_t* child = pool->first_child;
    while (child) {
        print_arena_node(child, indent + 1);
        child = child->next_sibling;
    }
}

void mp_dump_tree_info(memory_pool_t* pool) {
    if (!pool) return;
    pool_lock(pool);
    printf("\n================ CMEM ARENA TREE DUMP ================\n");
    print_arena_node(pool, 0);
    printf("======================================================\n\n");
    pool_unlock(pool);
}

size_t mp_dump_json_stats(memory_pool_t* pool, char* buf, size_t max_len) {
    if (!pool || !buf || max_len == 0) return 0;
    mp_stats_t stats;
    mp_get_stats(pool, &stats);

    int len = snprintf(buf, max_len,
        "{\n"
        "  \"arena_name\": \"%s\",\n"
        "  \"total_pool_size\": %zu,\n"
        "  \"active_bytes\": %zu,\n"
        "  \"peak_bytes\": %zu,\n"
        "  \"active_allocations\": %zu,\n"
        "  \"total_alloc_ops\": %zu,\n"
        "  \"total_free_ops\": %zu,\n"
        "  \"slab_allocated_bytes\": %zu,\n"
        "  \"tlsf_allocated_bytes\": %zu,\n"
        "  \"os_allocated_bytes\": %zu,\n"
        "  \"fragmentation_ratio\": %.4f\n"
        "}",
        pool->arena_name,
        stats.total_pool_size, stats.active_bytes, stats.peak_bytes,
        stats.active_allocations, stats.total_alloc_ops, stats.total_free_ops,
        stats.slab_allocated_bytes, stats.tlsf_allocated_bytes, stats.os_allocated_bytes,
        stats.fragmentation_ratio
    );

    return (len > 0 && (size_t)len < max_len) ? (size_t)len : max_len - 1;
}

bool mp_check_leaks(memory_pool_t* pool) {
    if (!pool) return true;
    pool_lock(pool);

    bool clean = (pool->stats.active_allocations == 0);
    if (!clean) {
        char report[4096];
        pool_unlock(pool);
        mp_analyze_leaks(pool, report, sizeof(report));
        fprintf(stderr, "%s\n", report);
        return false;
    }

    printf("[CMEM HEALTH] No memory leaks detected in [%s]. All memory safely freed!\n", pool->arena_name);
    pool_unlock(pool);
    return true;
}
