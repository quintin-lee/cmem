/**
 * @file cmem.c
 * @brief cmem - Universal Tiered Memory Manager Implementation (Slab + TLSF + OS + Child Arenas + Diagnostics).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "cmem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <assert.h>
#include <stdint.h>
#include <inttypes.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <execinfo.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <sys/syscall.h>
#ifndef CMEM_MPOL_BIND
#define CMEM_MPOL_BIND 2
#endif
#endif

#ifdef __cplusplus
#include <atomic>
typedef std::atomic<size_t> cmem_atomic_size_t;
#define CMEM_ATOMIC_FETCH_ADD(obj, arg, order) std::atomic_fetch_add_explicit(obj, arg, order)
#define CMEM_ATOMIC_FETCH_SUB(obj, arg, order) std::atomic_fetch_sub_explicit(obj, arg, order)
#define CMEM_ATOMIC_LOAD(obj, order) std::atomic_load_explicit(obj, order)
#define CMEM_ATOMIC_STORE(obj, val, order) std::atomic_store_explicit(obj, val, order)
#define CMEM_ATOMIC_COMPARE_EXCHANGE(obj, expected, desired, succ, fail) \
    std::atomic_compare_exchange_weak_explicit(obj, expected, desired, succ, fail)
#define CMEM_ATOMIC_INIT(obj, val) std::atomic_init(obj, val)
#define CMEM_ORDER_RELAXED std::memory_order_relaxed
#define CMEM_ORDER_ACQUIRE std::memory_order_acquire
#define CMEM_ORDER_RELEASE std::memory_order_release
#else
#include <stdatomic.h>
typedef atomic_size_t cmem_atomic_size_t;
#define CMEM_ATOMIC_FETCH_ADD(obj, arg, order) atomic_fetch_add_explicit(obj, arg, order)
#define CMEM_ATOMIC_FETCH_SUB(obj, arg, order) atomic_fetch_sub_explicit(obj, arg, order)
#define CMEM_ATOMIC_LOAD(obj, order) atomic_load_explicit(obj, order)
#define CMEM_ATOMIC_STORE(obj, val, order) atomic_store_explicit(obj, val, order)
#define CMEM_ATOMIC_COMPARE_EXCHANGE(obj, expected, desired, succ, fail) \
    atomic_compare_exchange_weak_explicit(obj, expected, desired, succ, fail)
#define CMEM_ATOMIC_INIT(obj, val) atomic_init(obj, val)
#define CMEM_ORDER_RELAXED memory_order_relaxed
#define CMEM_ORDER_ACQUIRE memory_order_acquire
#define CMEM_ORDER_RELEASE memory_order_release
#endif

#define MP_MAGIC_HEAD 0x4D504F4F  // "MPOO" in ASCII
#define MP_CANARY_BYTE 0xDE
#define MP_POISON_BYTE 0xDD

#define SLAB_CLASS_COUNT 7
static const size_t kSlabSizes[SLAB_CLASS_COUNT] = {8, 16, 32, 64, 128, 256, 512};
#define SLAB_MAX_SIZE 512
#define SLAB_PAGE_SIZE (64 * 1024) // 64 KB per slab page
#define TLS_CACHE_MAX_SLOTS 256
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
    ALLOC_TYPE_OS   = 3,
    ALLOC_TYPE_EMERGENCY = 4
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
    pthread_mutex_t lock;
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

/* --- Per-CPU Lock-Free Freelist Entry --- */
typedef struct {
    cmem_atomic_size_t head;
    uint16_t count;
} mp_percpu_freelist_entry_t;

/* --- Main Memory Pool Struct --- */
struct memory_pool {
    mp_flags_t flags;
    pthread_rwlock_t rwlock;
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

    // Watermark Alert Callback
    mp_watermark_callback_t watermark_cb;
    double high_watermark_ratio;
    double low_watermark_ratio;
    bool in_high_watermark_state;
    void* watermark_user_data;

    // NUMA CPU Node Affinity
    int numa_node;

    // Emergency Fallback Reserve Cushion
    void* emergency_buf;
    size_t emergency_size;
    size_t emergency_used;
    bool in_emergency_state;

    // Diagnostics & Statistics
    mp_stats_t stats;
    mp_block_header_t* active_head; // Linked list of current active allocations for leak detection
    uint64_t window_alloc_ops;
    uint64_t window_alloc_bytes;
    struct timespec window_start_time;

    // Tier 1: Slab Allocators (8B - 512B)
    mp_slab_class_t slab_classes[SLAB_CLASS_COUNT];
    bool use_custom_slab_sizes;
    size_t custom_slab_sizes[SLAB_CLASS_COUNT];

    // Tier 2: TLSF Allocator (512B - 4MB)
    tlsf_pool_t* tlsf_root;

    // Runtime config hot-reload tracking
    uint64_t env_flags_generation;

    // Auto-compaction trigger configuration
    bool auto_compact_enabled;
    double auto_compact_pressure_threshold;
    double auto_compact_fragmentation_threshold;
    struct timespec last_auto_compact_time;

    // Allocation latency histogram for P99 tracking (ns buckets)
    size_t alloc_latency_histogram[32];
    size_t alloc_latency_count;
    uint64_t alloc_latency_sum_ns;

    // Per-arena quota
    size_t arena_quota_limit;
    mp_watermark_callback_t arena_quota_cb;
    void* arena_quota_user_data;

    // Per-CPU lock-free freelist for low-contention fast path
    int num_cpus;
    mp_percpu_freelist_entry_t* percpu_freelists;
};

/* Lock Utilities */

/**
 * @brief Acquires a read lock on the memory pool for thread-safe concurrent reads.
 * @param pool Pointer to the memory pool
 */
static inline void pool_rdlock(memory_pool_t* pool) {
    if (pool && (pool->flags & MP_FLAG_THREAD_SAFE)) {
        pthread_rwlock_rdlock(&pool->rwlock);
    }
}

/**
 * @brief Releases the read lock on the memory pool.
 * @param pool Pointer to the memory pool
 */
static inline void pool_rdunlock(memory_pool_t* pool) {
    if (pool && (pool->flags & MP_FLAG_THREAD_SAFE)) {
        pthread_rwlock_unlock(&pool->rwlock);
    }
}

/**
 * @brief Acquires a write lock on the memory pool for exclusive access.
 * @param pool Pointer to the memory pool
 */
static inline void pool_wrlock(memory_pool_t* pool) {
    if (pool && (pool->flags & MP_FLAG_THREAD_SAFE)) {
        pthread_rwlock_wrlock(&pool->rwlock);
    }
}

/**
 * @brief Releases the write lock on the memory pool.
 * @param pool Pointer to the memory pool
 */
static inline void pool_wrunlock(memory_pool_t* pool) {
    if (pool && (pool->flags & MP_FLAG_THREAD_SAFE)) {
        pthread_rwlock_unlock(&pool->rwlock);
    }
}

/**
 * @brief Acquires the write lock (alias for pool_wrlock).
 * @param pool Pointer to the memory pool
 */
static inline void pool_lock(memory_pool_t* pool) {
    pool_wrlock(pool);
}

/**
 * @brief Releases the write lock (alias for pool_wrunlock).
 * @param pool Pointer to the memory pool
 */
static inline void pool_unlock(memory_pool_t* pool) {
    pool_wrunlock(pool);
}

/* Event Profiling Dispatcher */
/**
 * @brief Dispatches a profiling/debug event to the registered callback if present.
 * @param pool Pointer to the memory pool
 * @param ev Event type (alloc, free, realloc, etc.)
 * @param ptr Pointer involved in the event
 * @param size Size of the allocation
 */
static inline void trigger_event(memory_pool_t* pool, mp_event_type_t ev, void* ptr, size_t size) {
    if (pool->event_cb) {
        pool->event_cb(pool, ev, ptr, size, pool->event_user_data);
    }
}

/* Backing Memory Allocator Helpers */
/**
 * @brief Allocates raw system memory with optional alignment, HugePages, Guard Pages, and NUMA binding.
 * @param pool Pointer to the memory pool (for flags and NUMA node)
 * @param size Number of bytes to allocate
 * @param alignment Memory alignment requirement (power of two)
 * @return Pointer to allocated memory, or NULL on failure
 */
static void* sys_mem_alloc(memory_pool_t* pool, size_t size, size_t alignment) {
    void* ptr = NULL;
    if (pool && pool->has_custom_sys_alloc && pool->sys_allocator.sys_alloc) {
        return pool->sys_allocator.sys_alloc(size, pool->sys_allocator.user_data);
    }
    if (pool && (pool->flags & MP_FLAG_HUGE_PAGES)) {
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
        ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (ptr == MAP_FAILED) {
            ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#ifdef MADV_HUGEPAGE
            if (ptr != MAP_FAILED) {
                madvise(ptr, size, MADV_HUGEPAGE);
            }
#endif
        }
        if (ptr == MAP_FAILED) return NULL;
    } else if (pool && (pool->flags & MP_FLAG_GUARD_PAGES)) {
        long pg = sysconf(_SC_PAGESIZE);
        size_t page_sz = (pg > 0) ? (size_t)pg : 4096;
        size_t aligned_payload = (size + page_sz - 1) & ~(page_sz - 1);
        size_t total_map = page_sz + aligned_payload + page_sz;
        void* raw_map = mmap(NULL, total_map, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw_map == MAP_FAILED) return NULL;

        uint8_t* base = (uint8_t*)raw_map;
        mprotect(base, page_sz, PROT_NONE);
        mprotect(base + page_sz + aligned_payload, page_sz, PROT_NONE);

        ptr = base + page_sz;
    } else if (alignment > sizeof(void*)) {
        if (posix_memalign(&ptr, alignment, size) != 0) return NULL;
    } else {
        ptr = malloc(size);
    }

#if defined(__linux__) && defined(SYS_mbind)
    if (pool && pool->numa_node >= 0 && ptr) {
        unsigned long nodemask = (1UL << pool->numa_node);
        syscall(SYS_mbind, ptr, size, CMEM_MPOL_BIND, &nodemask, sizeof(nodemask) * 8, 0);
    }
#endif

    return ptr;
}

/**
 * @brief Frees raw system memory, respecting static buffer, custom allocator, HugePages, and Guard Pages modes.
 * @param pool Pointer to the memory pool
 * @param ptr Pointer to the memory to free
 * @param size Size of the allocation
 */
static void sys_mem_free(memory_pool_t* pool, void* ptr, size_t size) {
    if (pool->flags & MP_FLAG_STATIC_BUFFER) return;
    if (pool->has_custom_sys_alloc && pool->sys_allocator.sys_free) {
        pool->sys_allocator.sys_free(ptr, size, pool->sys_allocator.user_data);
        return;
    }
    if (pool->flags & MP_FLAG_HUGE_PAGES) {
        munmap(ptr, size);
        return;
    }
    if (pool->flags & MP_FLAG_GUARD_PAGES) {
        long pg = sysconf(_SC_PAGESIZE);
        size_t page_sz = (pg > 0) ? (size_t)pg : 4096;
        size_t aligned_payload = (size + page_sz - 1) & ~(page_sz - 1);
        size_t total_map = page_sz + aligned_payload + page_sz;
        uint8_t* raw_map = (uint8_t*)ptr - page_sz;
        munmap(raw_map, total_map);
        return;
    }
    free(ptr);
}

/* Bitwise Utilities for TLSF */
/**
 * @brief Finds the most significant set bit (fls - find last set) for TLSF first-level mapping.
 * @param val Value to inspect
 * @return Index of highest set bit (0-31), or -1 if val is 0
 */
static inline int tlsf_fls(size_t val) {
    if (val == 0) return -1;
    return 31 - __builtin_clz((uint32_t)val);
}

/**
 * @brief Finds the least significant set bit (ffs - find first set) for TLSF second-level mapping.
 * @param val Value to inspect
 * @return Index of lowest set bit (0-31), or -1 if val is 0
 */
static inline int tlsf_ffs(uint32_t val) {
    if (val == 0) return -1;
    return __builtin_ctz(val);
}

/**
 * @brief Maps a size to TLSF first-level and second-level indices for insertion.
 * @param size Size to map
 * @param fl Output first-level index
 * @param sl Output second-level index
 */
static void tlsf_mapping_insert(size_t size, int* fl, int* sl) {
    if (size < (1 << TLSF_SL_SHIFT)) {
        *fl = 0;
        *sl = (int)size;
    } else {
        *fl = tlsf_fls(size);
        *sl = (int)((size >> (*fl - TLSF_SL_SHIFT)) ^ (1 << TLSF_SL_SHIFT));
    }
}

/**
 * @brief Maps a size to TLSF first-level and second-level indices for search/query.
 * Rounds up the size to the nearest representable block boundary.
 * @param size Size to map
 * @param fl Output first-level index
 * @param sl Output second-level index
 */
static void tlsf_mapping_search(size_t size, int* fl, int* sl) {
    if (size >= (1 << TLSF_SL_SHIFT)) {
        size_t round = (1 << (*fl = tlsf_fls(size) - TLSF_SL_SHIFT)) - 1;
        size += round;
    }
    tlsf_mapping_insert(size, fl, sl);
}

/* --- Slab Allocator Implementation --- */
/**
 * @brief Initializes all Slab size classes for a newly created memory pool.
 * @param pool Pointer to the memory pool
 * @return true on success
 */
static bool slab_init(memory_pool_t* pool) {
    for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
        pool->slab_classes[i].slot_size = pool->use_custom_slab_sizes ? pool->custom_slab_sizes[i] : kSlabSizes[i];
        pthread_mutex_init(&pool->slab_classes[i].lock, NULL);
        pool->slab_classes[i].partial_pages = NULL;
        pool->slab_classes[i].full_pages = NULL;
    }
    return true;
}

/**
 * @brief Creates a new Slab page for a given size class, initializing the free list.
 * @param pool Pointer to the memory pool
 * @param class_idx Slab size class index (0-6)
 * @return Pointer to the new Slab page metadata, or NULL on failure
 */
static mp_slab_page_t* slab_create_page(memory_pool_t* pool, uint8_t class_idx) {
    size_t slot_payload_size = pool->slab_classes[class_idx].slot_size;
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

/**
 * @brief Allocates a small object from the Slab allocator for the given size class.
 * @param pool Pointer to the memory pool
 * @param class_idx Slab size class index
 * @param req_size Requested payload size
 * @return Pointer to the payload, or NULL on failure
 */
static void* slab_alloc(memory_pool_t* pool, uint8_t class_idx, size_t req_size) {
    mp_slab_class_t* sc = &pool->slab_classes[class_idx];
    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_lock(&sc->lock);
    }

    mp_slab_page_t* page = sc->partial_pages;

    if (!page) {
        page = slab_create_page(pool, class_idx);
        if (!page) {
            if (pool->flags & MP_FLAG_THREAD_SAFE) {
                pthread_mutex_unlock(&sc->lock);
            }
            return NULL;
        }

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

    pool->stats.slab_allocated_bytes += sc->slot_size;

    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_unlock(&sc->lock);
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

    return payload;
}

/**
 * @brief Returns a previously allocated Slab slot back to its page free list.
 * @param pool Pointer to the memory pool
 * @param header Block header of the allocation being freed
 */
static void slab_free(memory_pool_t* pool, mp_block_header_t* header) {
    uint8_t class_idx = header->slab_class;
    mp_slab_class_t* sc = &pool->slab_classes[class_idx];

    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_lock(&sc->lock);
    }

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

    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_unlock(&sc->lock);
    }
}

/**
 * @brief Refills the thread-local cache with Slab slots for lock-free fast-path allocation.
 * @param pool Pointer to the memory pool
 * @param class_idx Slab size class index to refill
 */
static inline void tls_cache_refill(memory_pool_t* pool, uint8_t class_idx) {
    for (int i = 0; i < 32; i++) {
        void* ptr = slab_alloc(pool, class_idx, pool->slab_classes[class_idx].slot_size);
        if (!ptr) break;
        mp_block_header_t* header = (mp_block_header_t*)((uint8_t*)ptr - sizeof(mp_block_header_t));
        mp_slab_slot_t* slot = (mp_slab_slot_t*)header->raw_base;
        slot->next = tls_cache.slots[class_idx];
        tls_cache.slots[class_idx] = slot;
        tls_cache.counts[class_idx]++;
    }
}

/* --- Per-CPU Lock-Free Freelist Implementation --- */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>

#define MP_PERCPU_MAX_BATCH 16

/**
 * @brief Initializes the per-CPU lock-free freelist arrays.
 * @param pool Pointer to the memory pool
 */
static void percpu_init(memory_pool_t* pool) {
    if (!pool || pool->percpu_freelists) return;
    pool->num_cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (pool->num_cpus <= 0) pool->num_cpus = 1;
    if (pool->num_cpus > 256) pool->num_cpus = 256;

    size_t sz = (size_t)pool->num_cpus * SLAB_CLASS_COUNT * sizeof(mp_percpu_freelist_entry_t);
    pool->percpu_freelists = (mp_percpu_freelist_entry_t*)calloc(1, sz);
    if (!pool->percpu_freelists) {
        pool->num_cpus = 0;
        return;
    }
}

/**
 * @brief Destroys the per-CPU lock-free freelist arrays.
 * @param pool Pointer to the memory pool
 */
static void percpu_destroy(memory_pool_t* pool) {
    if (!pool || !pool->percpu_freelists) return;
    free(pool->percpu_freelists);
    pool->percpu_freelists = NULL;
    pool->num_cpus = 0;
}

/**
 * @brief Returns the current CPU index for per-CPU freelist access.
 * @return CPU index in range [0, num_cpus)
 */
static inline int percpu_cpu_index(void) {
    int cpu = sched_getcpu();
    if (cpu < 0) cpu = 0;
    return cpu;
}

/**
 * @brief Lock-free pop from per-CPU freelist for a given slab class.
 * @param pool Pointer to the memory pool
 * @param cpu CPU index
 * @param class_idx Slab size class index
 * @return Pointer to slot, or NULL if empty
 */
static inline mp_slab_slot_t* percpu_pop(memory_pool_t* pool, int cpu, uint8_t class_idx) {
    if (!pool->percpu_freelists || cpu < 0 || cpu >= pool->num_cpus) return NULL;
    size_t idx = (size_t)cpu * SLAB_CLASS_COUNT + class_idx;
    mp_percpu_freelist_entry_t* entry = &((mp_percpu_freelist_entry_t*)pool->percpu_freelists)[idx];
    cmem_atomic_size_t* headp = &entry->head;
    size_t head = CMEM_ATOMIC_LOAD(headp, CMEM_ORDER_RELAXED);
    if (head == 0) return NULL;

    mp_slab_slot_t* slot = (mp_slab_slot_t*)head;
    mp_slab_slot_t* next = slot->next;
    if (!CMEM_ATOMIC_COMPARE_EXCHANGE(headp, &head, (size_t)next, CMEM_ORDER_RELAXED, CMEM_ORDER_RELAXED)) {
        return NULL;
    }
    entry->count--;
    return slot;
}

/**
 * @brief Lock-free push to per-CPU freelist for a given slab class.
 * @param pool Pointer to the memory pool
 * @param cpu CPU index
 * @param class_idx Slab size class index
 * @param slot Pointer to slot to push
 * @return true if pushed, false if freelist is full (caller should use normal free path)
 */
static inline bool percpu_push(memory_pool_t* pool, int cpu, uint8_t class_idx, mp_slab_slot_t* slot) {
    if (!pool->percpu_freelists || cpu < 0 || cpu >= pool->num_cpus) return false;
    size_t idx = (size_t)cpu * SLAB_CLASS_COUNT + class_idx;
    mp_percpu_freelist_entry_t* entry = &((mp_percpu_freelist_entry_t*)pool->percpu_freelists)[idx];
    if (entry->count >= MP_PERCPU_MAX_BATCH) return false;

    cmem_atomic_size_t* headp = &entry->head;
    size_t old_head;
    do {
        old_head = CMEM_ATOMIC_LOAD(headp, CMEM_ORDER_RELAXED);
        slot->next = (mp_slab_slot_t*)old_head;
    } while (!CMEM_ATOMIC_COMPARE_EXCHANGE(headp, &old_head, (size_t)slot, CMEM_ORDER_RELAXED, CMEM_ORDER_RELAXED));
    entry->count++;
    return true;
}

/**
 * @brief Batch refills per-CPU freelist from global Slab allocator.
 * @param pool Pointer to the memory pool
 * @param cpu CPU index
 * @param class_idx Slab size class index
 */
static inline void percpu_refill(memory_pool_t* pool, int cpu, uint8_t class_idx) {
    if (!pool->percpu_freelists || cpu < 0 || cpu >= pool->num_cpus) return;
    size_t idx = (size_t)cpu * SLAB_CLASS_COUNT + class_idx;
    mp_percpu_freelist_entry_t* entry = &((mp_percpu_freelist_entry_t*)pool->percpu_freelists)[idx];
    if (entry->count > MP_PERCPU_MAX_BATCH / 2) return;

    mp_slab_slot_t* slots[MP_PERCPU_MAX_BATCH];
    int got = 0;
    for (int i = 0; i < MP_PERCPU_MAX_BATCH; i++) {
        void* ptr = slab_alloc(pool, class_idx, pool->slab_classes[class_idx].slot_size);
        if (!ptr) break;
        mp_block_header_t* header = (mp_block_header_t*)((uint8_t*)ptr - sizeof(mp_block_header_t));
        slots[got++] = (mp_slab_slot_t*)header->raw_base;
    }
    if (got == 0) return;

    mp_slab_slot_t* head = slots[0];
    mp_slab_slot_t* tail = head;
    for (int i = 1; i < got; i++) {
        tail->next = slots[i];
        tail = slots[i];
    }
    tail->next = (mp_slab_slot_t*)CMEM_ATOMIC_LOAD(&entry->head, CMEM_ORDER_RELAXED);
    CMEM_ATOMIC_STORE(&entry->head, (size_t)head, CMEM_ORDER_RELAXED);
    entry->count += (uint16_t)got;
}

/* ========================================================================== */
/*  Per-CPU Lock-Free Freelist Public API                                       */
/* ========================================================================== */
/**
 * @brief Enables or disables the per-CPU lock-free freelist optimization.
 * @param pool Pointer to the memory pool
 * @param enable true to enable, false to disable
 */
void mp_set_percpu_freelist(memory_pool_t* pool, bool enable) {
    if (!pool) return;
    pool_lock(pool);
    if (enable) {
        pool->flags = (mp_flags_t)(pool->flags | MP_FLAG_PERCPU_FREELIST);
        if (!pool->percpu_freelists) percpu_init(pool);
    } else {
        pool->flags = (mp_flags_t)(pool->flags & ~MP_FLAG_PERCPU_FREELIST);
        percpu_destroy(pool);
    }
    pool_unlock(pool);
}

/**
 * @brief Returns whether the per-CPU lock-free freelist is enabled.
 * @param pool Pointer to the memory pool
 * @return true if enabled, false otherwise
 */
bool mp_get_percpu_freelist(memory_pool_t* pool) {
    if (!pool) return false;
    pool_rdlock(pool);
    bool enabled = (pool->flags & MP_FLAG_PERCPU_FREELIST) != 0;
    pool_rdunlock(pool);
    return enabled;
}

/**
 * @brief Returns the number of CPUs detected for per-CPU freelist partitioning.
 * @param pool Pointer to the memory pool
 * @return Number of CPUs, or 0 if per-CPU freelist is not initialized
 */
int mp_get_percpu_cpu_count(memory_pool_t* pool) {
    if (!pool) return 0;
    pool_rdlock(pool);
    int count = pool->num_cpus;
    pool_rdunlock(pool);
    return count;
}

/* --- TLSF Implementation --- */
/**
 * @brief Creates a new TLSF pool, optionally using pre-allocated custom memory.
 * @param pool Pointer to the parent memory pool
 * @param size Size of the TLSF memory region
 * @param custom_mem Optional pre-allocated memory buffer, or NULL to allocate from system
 * @return Pointer to the new TLSF pool, or NULL on failure
 */
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

/**
 * @brief Inserts a free block into the TLSF free-list bitmap structure.
 * @param tpool Pointer to the TLSF pool
 * @param block Pointer to the block to insert
 */
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

/**
 * @brief Removes a free block from the TLSF free-list bitmap structure.
 * @param tpool Pointer to the TLSF pool
 * @param block Pointer to the block to remove
 */
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

/**
 * @brief Finds the best-fit free block in the TLSF pool for the requested size.
 * Uses two-level bitmap search for O(1) lookup.
 * @param tpool Pointer to the TLSF pool
 * @param total_needed Total size needed (including headers)
 * @return Pointer to the best-fit block, or NULL if none available
 */
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

/**
 * @brief Allocates a medium-sized object from the TLSF allocator (512B ~ 4MB).
 * @param pool Pointer to the memory pool
 * @param req_size Requested payload size
 * @return Pointer to the payload, or NULL on failure
 */
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

/**
 * @brief Returns a TLSF-allocated block back to the free pool, coalescing with neighbors.
 * @param pool Pointer to the memory pool
 * @param header Block header of the allocation being freed
 */
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

/**
 * @brief Attempts to expand a TLSF block in-place by absorbing the next free block.
 * Avoids memcpy overhead if expansion is possible.
 * @param pool Pointer to the memory pool
 * @param header Block header of the allocation to expand
 * @param new_size New requested payload size
 * @return true if expansion succeeded in-place, false otherwise
 */
static bool tlsf_try_inplace_expand(memory_pool_t* pool, mp_block_header_t* header, size_t new_size) {
    if (header->alloc_type != ALLOC_TYPE_TLSF) return false;
    tlsf_block_t* block = (tlsf_block_t*)header->raw_base;
    tlsf_pool_t* tpool = (tlsf_pool_t*)header->subpool;
    if (!block || !tpool) return false;

    size_t current_block_size = block->size_and_flags & BLOCK_SIZE_MASK;
    size_t total_needed = sizeof(tlsf_block_t) + sizeof(mp_block_header_t) + new_size + ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0);
    total_needed = (total_needed + 7) & ~7;
    if (total_needed < TLSF_MIN_BLOCK_SIZE) total_needed = TLSF_MIN_BLOCK_SIZE;

    tlsf_block_t* next_phys = (tlsf_block_t*)((uint8_t*)block + current_block_size);
    if (!(next_phys->size_and_flags & BLOCK_STATE_FREE)) return false;

    size_t next_size = next_phys->size_and_flags & BLOCK_SIZE_MASK;
    if (current_block_size + next_size < total_needed) return false;

    tlsf_remove_free_block(tpool, next_phys);
    size_t combined_size = current_block_size + next_size;
    size_t remaining = combined_size - total_needed;

    if (remaining >= TLSF_MIN_BLOCK_SIZE + sizeof(tlsf_block_t)) {
        block->size_and_flags = total_needed | (block->size_and_flags & BLOCK_STATE_PREV_FREE);

        tlsf_block_t* split_block = (tlsf_block_t*)((uint8_t*)block + total_needed);
        split_block->size_and_flags = remaining | BLOCK_STATE_FREE;
        split_block->prev_physical = block;

        tlsf_block_t* far_phys = (tlsf_block_t*)((uint8_t*)split_block + remaining);
        far_phys->prev_physical = split_block;
        far_phys->size_and_flags |= BLOCK_STATE_PREV_FREE;

        tlsf_insert_free_block(tpool, split_block);
    } else {
        block->size_and_flags = combined_size | (block->size_and_flags & BLOCK_STATE_PREV_FREE);
        tlsf_block_t* far_phys = (tlsf_block_t*)((uint8_t*)block + combined_size);
        far_phys->prev_physical = block;
        far_phys->size_and_flags &= ~BLOCK_STATE_PREV_FREE;
    }

    size_t new_usable = (block->size_and_flags & BLOCK_SIZE_MASK) - sizeof(tlsf_block_t) - sizeof(mp_block_header_t);
    pool->stats.tlsf_allocated_bytes += (new_usable - header->usable_size);
    header->requested_size = new_size;
    header->usable_size = new_usable;

    void* payload = (void*)((uint8_t*)header + sizeof(mp_block_header_t));
    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
        uint8_t* canary = (uint8_t*)payload + new_size;
        *canary = MP_CANARY_BYTE;
    }

    return true;
}

/* --- Lock-Free Ring Buffer Allocator Implementation --- */
/**
 * @brief Ring buffer allocator structure (DPDK-style single-producer single-consumer).
 */
struct cmem_ring_buffer {
    size_t slot_size;
    size_t capacity;
    size_t mask;
    cmem_atomic_size_t head;
    cmem_atomic_size_t tail;
    void** slots;
    void* buffer;
};

/**
 * @brief Creates a lock-free ring buffer allocator with power-of-two capacity.
 * @param slot_size Size of each slot in bytes
 * @param capacity Number of slots (rounded up to next power of two)
 * @return Pointer to the ring buffer, or NULL on failure
 */
cmem_ring_buffer_t* mp_ring_create(size_t slot_size, size_t capacity) {
    if (slot_size == 0 || capacity == 0) return NULL;
    size_t real_cap = 1;
    while (real_cap < capacity) real_cap <<= 1;

    cmem_ring_buffer_t* ring = (cmem_ring_buffer_t*)calloc(1, sizeof(cmem_ring_buffer_t));
    if (!ring) return NULL;

    ring->slot_size = slot_size;
    ring->capacity = real_cap;
    ring->mask = real_cap - 1;
    CMEM_ATOMIC_INIT(&ring->head, 0);
    CMEM_ATOMIC_INIT(&ring->tail, real_cap);

    ring->slots = (void**)calloc(real_cap, sizeof(void*));
    size_t total_buf = slot_size * real_cap;
    if (posix_memalign(&ring->buffer, 64, total_buf) != 0) {
        free(ring->slots);
        free(ring);
        return NULL;
    }

    uint8_t* base = (uint8_t*)ring->buffer;
    for (size_t i = 0; i < real_cap; i++) {
        ring->slots[i] = base + i * slot_size;
    }

    return ring;
}

/**
 * @brief Allocates a slot from the lock-free ring buffer (producer path).
 * @param ring Pointer to the ring buffer
 * @return Pointer to the slot payload, or NULL if full
 */
void* mp_ring_alloc(cmem_ring_buffer_t* ring) {
    if (!ring) return NULL;
    size_t head = CMEM_ATOMIC_FETCH_ADD(&ring->head, 1, CMEM_ORDER_RELAXED);
    size_t tail = CMEM_ATOMIC_LOAD(&ring->tail, CMEM_ORDER_ACQUIRE);

    if (head >= tail) {
        return NULL;
    }

    return ring->slots[head & ring->mask];
}

/**
 * @brief Returns a slot to the lock-free ring buffer (consumer path).
 * @param ring Pointer to the ring buffer
 * @param ptr Pointer to the slot to return
 * @return true on success, false on invalid input
 */
bool mp_ring_free(cmem_ring_buffer_t* ring, void* ptr) {
    if (!ring || !ptr) return false;
    size_t tail = CMEM_ATOMIC_FETCH_ADD(&ring->tail, 1, CMEM_ORDER_RELEASE);
    ring->slots[tail & ring->mask] = ptr;
    return true;
}

/**
 * @brief Destroys the ring buffer and frees all associated memory.
 * @param ring Pointer to the ring buffer
 */
void mp_ring_destroy(cmem_ring_buffer_t* ring) {
    if (!ring) return;
    if (ring->slots) free(ring->slots);
    if (ring->buffer) free(ring->buffer);
    free(ring);
}

/* --- Structured Event Log Ring Buffer Implementation --- */
/**
 * @brief Structured event log structure with embedded lock-free ring buffer.
 */
struct mp_event_log {
    cmem_ring_buffer_t* ring;
    size_t capacity;
    cmem_atomic_size_t count;
};

/**
 * @brief Creates a structured event log with a lock-free ring buffer.
 * @param capacity Number of entries in the ring buffer (must be power of two)
 * @return Pointer to the event log, or NULL on failure
 */
mp_event_log_t* mp_event_log_create(size_t capacity) {
    mp_event_log_t* log = (mp_event_log_t*)calloc(1, sizeof(mp_event_log_t));
    if (!log) return NULL;

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
 * @brief Destroys the event log and frees all associated memory.
 * @param log Pointer to the event log
 */
void mp_event_log_destroy(mp_event_log_t* log) {
    if (!log) return;
    if (log->ring) mp_ring_destroy(log->ring);
    free(log);
}

/**
 * @brief Records an event into the structured event log ring buffer.
 * @param log Pointer to the event log
 * @param event_type Event type
 * @param ptr Pointer involved in the event
 * @param size Size of the allocation
 * @return true on success, false if ring buffer is full
 */
bool mp_event_log_record(mp_event_log_t* log, mp_event_type_t event_type, void* ptr, size_t size) {
    if (!log || !log->ring) return false;

    mp_event_log_entry_t* entry = (mp_event_log_entry_t*)mp_ring_alloc(log->ring);
    if (!entry) return false;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    entry->timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    entry->event_type = event_type;
    entry->size = size;
    entry->ptr = (uintptr_t)ptr;

    CMEM_ATOMIC_FETCH_ADD(&log->count, 1, CMEM_ORDER_RELAXED);
    return true;
}

/**
 * @brief Consumes and returns the next event from the ring buffer.
 * @param log Pointer to the event log
 * @param entry Output event entry
 * @return true if an event was consumed, false if buffer is empty
 */
bool mp_event_log_consume(mp_event_log_t* log, mp_event_log_entry_t* entry) {
    if (!log || !log->ring || !entry) return false;

    mp_event_log_entry_t* slot = (mp_event_log_entry_t*)mp_ring_alloc(log->ring);
    if (!slot) return false;

    *entry = *slot;
    CMEM_ATOMIC_FETCH_SUB(&log->count, 1, CMEM_ORDER_RELAXED);
    return true;
}

/**
 * @brief Returns the number of unread events in the ring buffer.
 * @param log Pointer to the event log
 * @return Number of pending events
 */
size_t mp_event_log_pending(mp_event_log_t* log) {
    if (!log) return 0;
    return (size_t)CMEM_ATOMIC_LOAD(&log->count, CMEM_ORDER_ACQUIRE);
}

/**
 * @brief Clears all pending events from the ring buffer.
 * @param log Pointer to the event log
 */
void mp_event_log_clear(mp_event_log_t* log) {
    if (!log) return;
    CMEM_ATOMIC_STORE(&log->count, 0, CMEM_ORDER_RELAXED);
    mp_ring_destroy(log->ring);
    log->ring = mp_ring_create(sizeof(mp_event_log_entry_t), log->capacity);
}

/**
 * @brief Exports allocation events in pprof-compatible text format.
 * @param pool Pointer to the memory pool
 * @param out_buf Output buffer for pprof text
 * @param max_len Maximum length of the output buffer
 * @return Number of bytes written to out_buf
 */
size_t mp_export_pprof(memory_pool_t* pool, char* out_buf, size_t max_len) {
    if (!pool || !out_buf || max_len == 0) return 0;

    size_t total_alloc = pool->stats.total_alloc_ops;
    size_t active = pool->stats.active_allocations;
    size_t active_bytes = pool->stats.active_bytes;
    size_t peak_bytes = pool->stats.peak_bytes;

    int n = snprintf(out_buf, max_len,
        "heap: %zu %zu\n"
        "alloc_objects: total %zu\n"
        "alloc_space: total %zu\n"
        "inuse_objects: %zu\n"
        "inuse_space: %zu\n"
        "peak_space: %zu\n",
        active_bytes, active,
        total_alloc, total_alloc > 0 ? pool->stats.total_alloc_ops * sizeof(void*) : 0,
        active, active_bytes,
        peak_bytes);

    if (n < 0 || (size_t)n >= max_len) return (size_t)n < 0 ? 0 : max_len;
    return (size_t)n;
}

/* --- 0-Overhead Typed Object Pool Implementation --- */
/**
 * @brief Typed object pool structure for fixed-size object allocation.
 */
struct mp_typed_pool {
    size_t elem_size;
    size_t capacity;
    size_t active_count;
    void* free_list;
    void* raw_buf;
};

/**
 * @brief Creates a 0-overhead typed object pool for fixed-size elements.
 * @param elem_size Size of each element (rounded up to 8 bytes, minimum sizeof(void*))
 * @param capacity Maximum number of elements in the pool
 * @return Pointer to the typed pool, or NULL on failure
 */
mp_typed_pool_t* mp_typed_pool_create(size_t elem_size, size_t capacity) {
    if (elem_size == 0 || capacity == 0) return NULL;
    size_t real_elem_sz = (elem_size < sizeof(void*)) ? sizeof(void*) : elem_size;
    real_elem_sz = (real_elem_sz + 7) & ~((size_t)7);

    mp_typed_pool_t* tpool = (mp_typed_pool_t*)calloc(1, sizeof(mp_typed_pool_t));
    if (!tpool) return NULL;

    tpool->elem_size = real_elem_sz;
    tpool->capacity = capacity;
    tpool->active_count = 0;

    size_t total_sz = real_elem_sz * capacity;
    if (posix_memalign(&tpool->raw_buf, 64, total_sz) != 0) {
        free(tpool);
        return NULL;
    }

    uint8_t* base = (uint8_t*)tpool->raw_buf;
    tpool->free_list = base;

    for (size_t i = 0; i < capacity - 1; i++) {
        void** curr = (void**)(base + i * real_elem_sz);
        *curr = base + (i + 1) * real_elem_sz;
    }
    void** last = (void**)(base + (capacity - 1) * real_elem_sz);
    *last = NULL;

    return tpool;
}

/**
 * @brief Allocates a fixed-size object from the typed pool (zero header overhead).
 * @param tpool Pointer to the typed pool
 * @return Pointer to the object, or NULL if pool is exhausted
 */
void* mp_typed_alloc(mp_typed_pool_t* tpool) {
    if (!tpool || !tpool->free_list) return NULL;

    void* ptr = tpool->free_list;
    tpool->free_list = *(void**)ptr;
    tpool->active_count++;
    return ptr;
}

/**
 * @brief Returns a fixed-size object back to the typed pool free list.
 * @param tpool Pointer to the typed pool
 * @param ptr Pointer to the object to free
 */
void mp_typed_free(mp_typed_pool_t* tpool, void* ptr) {
    if (!tpool || !ptr) return;

    *(void**)ptr = tpool->free_list;
    tpool->free_list = ptr;
    if (tpool->active_count > 0) tpool->active_count--;
}

/**
 * @brief Destroys the typed pool and frees the underlying buffer.
 * @param tpool Pointer to the typed pool
 */
void mp_typed_pool_destroy(mp_typed_pool_t* tpool) {
    if (!tpool) return;
    if (tpool->raw_buf) free(tpool->raw_buf);
    free(tpool);
}

/* --- Public API Implementation --- */
/**
 * @brief Creates a new memory pool instance with the given initial capacity and flags.
 * @param initial_capacity Initial memory capacity in bytes (0 for default)
 * @param flags Configuration flags (thread safety, canary, etc.)
 * @return Pointer to the new memory pool, or NULL on failure
 */
memory_pool_t* mp_create(size_t initial_capacity, mp_flags_t flags) {
    return mp_create_custom(initial_capacity, flags, NULL);
}

/**
 * @brief Creates a POSIX shared memory pool in /dev/shm for zero-copy inter-process communication.
 * @param shm_name Name of the shared memory object (e.g. "/my_pool")
 * @param capacity Capacity in bytes
 * @param flags Configuration flags
 * @return Pointer to the new shared memory pool, or NULL on failure
 */
memory_pool_t* mp_create_shared(const char* shm_name, size_t capacity, mp_flags_t flags) {
    if (!shm_name || capacity < 64 * 1024) capacity = 1024 * 1024;

    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) return NULL;

    if (ftruncate(shm_fd, capacity) == -1) {
        close(shm_fd);
        return NULL;
    }

    void* shm_ptr = mmap(NULL, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);

    if (shm_ptr == MAP_FAILED) return NULL;

    memory_pool_t* pool = mp_create_from_buffer(shm_ptr, capacity, (mp_flags_t)(flags | MP_FLAG_SHARED_MEMORY));
    if (pool) {
        snprintf(pool->arena_name, sizeof(pool->arena_name), "SharedIPC[%s]", shm_name);
        if (flags & MP_FLAG_THREAD_SAFE) {
            pthread_mutexattr_t mattr;
            pthread_mutexattr_init(&mattr);
            pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
            pthread_mutex_init(&pool->lock, &mattr);
            pthread_mutexattr_destroy(&mattr);
        }
    }
    return pool;
}

/**
 * @brief Destroys a shared memory pool and unlinks the POSIX shared memory segment.
 * @param pool Pointer to the shared memory pool
 * @param shm_name Name of the shared memory object to unlink
 */
void mp_destroy_shared(memory_pool_t* pool, const char* shm_name) {
    if (!pool) return;
    size_t sz = pool->stats.total_pool_size;
    void* shm_ptr = (void*)pool;
    munmap(shm_ptr, sz);
    if (shm_name) {
        shm_unlink(shm_name);
    }
}

/**
 * @brief Creates a child memory pool linked to a parent pool for hierarchical arena management.
 * @param parent Pointer to the parent memory pool (can be NULL)
 * @param initial_capacity Initial capacity for the child pool
 * @param flags Configuration flags
 * @param arena_name Human-readable name for the child arena
 * @return Pointer to the new child memory pool, or NULL on failure
 */
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

/**
 * @brief Parses the CMEM_CONF environment variable to enable runtime configuration flags.
 * Supports: canary=1, zero=1, tls=1, track=1, poison=1, aligned=1, guard=1, hugepages=1
 * @param default_flags Default flags to merge with parsed flags
 * @return Merged flags value
 */
mp_flags_t mp_parse_env_flags(mp_flags_t default_flags) {
    const char* env_conf = getenv("CMEM_CONF");
    if (!env_conf || strlen(env_conf) == 0) return default_flags;

    mp_flags_t flags = default_flags;

    if (strstr(env_conf, "canary=1") || strstr(env_conf, "canary=on")) flags = (mp_flags_t)(flags | MP_FLAG_DEBUG_CANARY);
    if (strstr(env_conf, "zero=1") || strstr(env_conf, "zero=on")) flags = (mp_flags_t)(flags | MP_FLAG_ZERO_ON_ALLOC);
    if (strstr(env_conf, "tls=1") || strstr(env_conf, "tls=on")) flags = (mp_flags_t)(flags | MP_FLAG_THREAD_LOCAL_CACHE);
    if (strstr(env_conf, "track=1") || strstr(env_conf, "track=on")) flags = (mp_flags_t)(flags | MP_FLAG_TRACK_LOCATIONS);
    if (strstr(env_conf, "poison=1") || strstr(env_conf, "poison=on")) flags = (mp_flags_t)(flags | MP_FLAG_POISON_ON_FREE);
    if (strstr(env_conf, "aligned=1") || strstr(env_conf, "aligned=on")) flags = (mp_flags_t)(flags | MP_FLAG_CACHE_ALIGNED);
    if (strstr(env_conf, "guard=1") || strstr(env_conf, "guard=on")) flags = (mp_flags_t)(flags | MP_FLAG_GUARD_PAGES);
    if (strstr(env_conf, "hugepages=1") || strstr(env_conf, "hugepages=on")) flags = (mp_flags_t)(flags | MP_FLAG_HUGE_PAGES);

    return flags;
}

/**
 * @brief Creates a memory pool with a custom backing allocator vtable.
 * @param initial_capacity Initial memory capacity in bytes
 * @param flags Configuration flags
 * @param sys_allocator Custom system allocator function table, or NULL for default
 * @return Pointer to the new memory pool, or NULL on failure
 */
memory_pool_t* mp_create_custom(size_t initial_capacity, mp_flags_t flags, const mp_sys_allocator_t* sys_allocator) {
    flags = mp_parse_env_flags(flags);

    memory_pool_t* pool = (memory_pool_t*)calloc(1, sizeof(memory_pool_t));
    if (!pool) return NULL;

    pool->flags = flags;
    snprintf(pool->arena_name, sizeof(pool->arena_name), "RootArena");
    clock_gettime(CLOCK_MONOTONIC, &pool->window_start_time);
    if (sys_allocator) {
        pool->has_custom_sys_alloc = true;
        pool->sys_allocator = *sys_allocator;
    }

    if (flags & MP_FLAG_THREAD_SAFE) {
        pthread_rwlock_init(&pool->rwlock, NULL);
        pthread_mutex_init(&pool->lock, NULL);
    }

    slab_init(pool);

    if (flags & MP_FLAG_PERCPU_FREELIST) {
        percpu_init(pool);
    }

    if (initial_capacity > 0) {
        pool->tlsf_root = tlsf_create_pool_custom(pool, initial_capacity, NULL);
        if (pool->tlsf_root) {
            pool->stats.total_pool_size += initial_capacity + sizeof(tlsf_pool_t);
        }
    }

    return pool;
}

/**
 * @brief Creates a memory pool inside a pre-allocated static buffer (zero OS malloc dependency).
 * @param buffer Pre-allocated buffer memory
 * @param buffer_size Size of the buffer in bytes
 * @param flags Configuration flags
 * @return Pointer to the new memory pool, or NULL on failure
 */
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

    if (flags & MP_FLAG_PERCPU_FREELIST) {
        percpu_init(pool);
    }

    uint8_t* remain_mem = (uint8_t*)aligned_addr + sizeof(memory_pool_t);
    size_t remain_sz = buffer_size - align_offset - sizeof(memory_pool_t) - sizeof(tlsf_pool_t);

    pool->tlsf_root = tlsf_create_pool_custom(pool, remain_sz, remain_mem);
    if (!pool->tlsf_root) return NULL;

    pool->stats.total_pool_size = buffer_size;
    return pool;
}

/**
 * @brief Sets a human-readable name for the memory pool arena.
 * @param pool Pointer to the memory pool
 * @param name Null-terminated name string
 */
void mp_set_name(memory_pool_t* pool, const char* name) {
    if (!pool || !name) return;
    pool_lock(pool);
    snprintf(pool->arena_name, sizeof(pool->arena_name), "%s", name);
    pool_unlock(pool);
}

/**
 * @brief Gets the human-readable name of the memory pool arena.
 * @param pool Pointer to the memory pool
 * @return Pointer to the name string, or NULL if pool is invalid
 */
const char* mp_get_name(memory_pool_t* pool) {
    if (!pool) return NULL;
    return pool->arena_name;
}

/**
 * @brief Gets the parent pool pointer if this pool is a child arena.
 * @param pool Pointer to the memory pool
 * @return Pointer to the parent pool, or NULL if this is a root pool
 */
memory_pool_t* mp_get_parent(memory_pool_t* pool) {
    if (!pool) return NULL;
    return pool->parent;
}

/**
 * @brief Gets the count of direct child arenas linked to this pool.
 * @param pool Pointer to the memory pool
 * @return Number of direct children, or 0 if pool is invalid
 */
size_t mp_get_child_count(memory_pool_t* pool) {
    if (!pool) return 0;
    pool_rdlock(pool);
    size_t count = 0;
    memory_pool_t* child = pool->first_child;
    while (child) {
        count++;
        child = child->next_sibling;
    }
    pool_rdunlock(pool);
    return count;
}

/**
 * @brief Calculates the memory pool pressure ratio relative to its limit or total size.
 * @param pool Pointer to the memory pool
 * @return Pressure ratio between 0.0 (0%) and 1.0 (100%)
 */
double mp_pressure(memory_pool_t* pool) {
    if (!pool) return 0.0;
    pool_rdlock(pool);
    double ratio = 0.0;
    if (pool->stats.max_memory_limit > 0) {
        ratio = (double)pool->stats.active_bytes / (double)pool->stats.max_memory_limit;
    } else if (pool->stats.total_pool_size > 0) {
        ratio = (double)pool->stats.active_bytes / (double)pool->stats.total_pool_size;
    }
    pool_rdunlock(pool);
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    return ratio;
}

/**
 * @brief Returns the total bytes that could be reclaimed by trimming fully-free Slab pages.
 * @param pool Pointer to the memory pool
 * @return Number of reclaimable bytes
 */
size_t mp_freeable(memory_pool_t* pool) {
    if (!pool) return 0;
    pool_rdlock(pool);
    size_t freeable_bytes = 0;

    for (int c = 0; c < SLAB_CLASS_COUNT; c++) {
        mp_slab_class_t* sc = &pool->slab_classes[c];
        mp_slab_page_t* curr = sc->partial_pages;
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
 * @brief Returns the estimated physical RSS resident memory size of the pool.
 * @param pool Pointer to the memory pool
 * @return Total reserved bytes from the OS
 */
size_t mp_resident(memory_pool_t* pool) {
    if (!pool) return 0;
    pool_rdlock(pool);
    size_t res = pool->stats.total_pool_size;
    pool_rdunlock(pool);
    return res;
}

/* ========================================================================== */
/*  Runtime Config Hot-Reload                                                  */
/* ========================================================================== */
/**
 * @brief Re-parses the CMEM_CONF environment variable and applies safe runtime flag changes.
 * @param pool Pointer to the memory pool
 * @return Merged flags value after applying environment changes
 */
mp_flags_t mp_reparse_env_flags(memory_pool_t* pool) {
    if (!pool) return 0;
    mp_flags_t new_flags = mp_parse_env_flags(pool->flags);
    if (new_flags == pool->flags) return pool->flags;

    pool_lock(pool);
    pool->flags = new_flags;
    pool->env_flags_generation++;
    pool_unlock(pool);

    return pool->flags;
}

/**
 * @brief Returns the current environment configuration generation counter.
 * @param pool Pointer to the memory pool
 * @return Current generation counter, or 0 if pool is invalid
 */
uint64_t mp_get_env_generation(memory_pool_t* pool) {
    if (!pool) return 0;
    return pool->env_flags_generation;
}

/* ========================================================================== */
/*  Auto-Compaction Trigger                                                    */
/* ========================================================================== */
/**
 * @brief Enables automatic compaction triggered by pool pressure or fragmentation.
 * @param pool Pointer to the memory pool
 * @param enable true to enable auto-compaction, false to disable
 * @param pressure_threshold Pressure ratio (0.0-1.0) above which compaction is triggered
 * @param fragmentation_threshold Fragmentation ratio (0.0-1.0) above which compaction is triggered
 */
void mp_set_auto_compact(memory_pool_t* pool, bool enable, double pressure_threshold, double fragmentation_threshold) {
    if (!pool) return;
    pool_lock(pool);
    pool->auto_compact_enabled = enable;
    pool->auto_compact_pressure_threshold = pressure_threshold;
    pool->auto_compact_fragmentation_threshold = fragmentation_threshold;
    pool_unlock(pool);
}

/**
 * @brief Checks if auto-compaction is needed and triggers it if so.
 * @param pool Pointer to the memory pool
 * @return true if compaction was performed, false otherwise
 */
bool mp_auto_compact_check(memory_pool_t* pool) {
    if (!pool || !pool->auto_compact_enabled) return false;

    pool_lock(pool);
    double pressure = (double)pool->stats.active_bytes / (double)(pool->stats.max_memory_limit > 0 ? pool->stats.max_memory_limit : pool->stats.total_pool_size);
    double frag = pool->stats.fragmentation_ratio;
    pool_unlock(pool);

    if (pressure > pool->auto_compact_pressure_threshold || frag > pool->auto_compact_fragmentation_threshold) {
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

/* ========================================================================== */
/*  Per-Arena Memory Quota                                                     */
/* ========================================================================== */
/**
 * @brief Sets a per-arena memory quota with an over-limit callback.
 * @param pool Pointer to the memory pool
 * @param quota_bytes Maximum allowed active bytes for this arena (0 for unlimited)
 * @param cb Callback invoked when quota is exceeded
 * @param user_data Optional user data passed to the callback
 */
void mp_set_arena_quota(memory_pool_t* pool, size_t quota_bytes, mp_watermark_callback_t cb, void* user_data) {
    if (!pool) return;
    pool_lock(pool);
    pool->arena_quota_limit = quota_bytes;
    pool->arena_quota_cb = cb;
    pool->arena_quota_user_data = user_data;
    pool_unlock(pool);
}

/**
 * @brief Checks if the arena is within its quota limit.
 * @param pool Pointer to the memory pool
 * @return true if within quota or no quota set, false if over quota
 */
bool mp_check_arena_quota(memory_pool_t* pool) {
    if (!pool || pool->arena_quota_limit == 0) return true;
    pool_rdlock(pool);
    bool ok = pool->stats.active_bytes <= pool->arena_quota_limit;
    pool_rdunlock(pool);
    return ok;
}

/* ========================================================================== */
/*  Allocation Latency Statistics                                              */
/* ========================================================================== */
/**
 * @brief Records an allocation latency sample in nanoseconds.
 * @param pool Pointer to the memory pool
 * @param latency_ns Latency in nanoseconds
 */
void mp_record_latency(memory_pool_t* pool, uint64_t latency_ns) {
    if (!pool) return;
    pool_lock(pool);
    pool->alloc_latency_sum_ns += latency_ns;
    pool->alloc_latency_count++;

    size_t idx = 0;
    uint64_t v = latency_ns;
    while (v >= 1024 && idx < 31) { v >>= 1; idx++; }
    pool->alloc_latency_histogram[idx]++;
    pool_unlock(pool);
}

/**
 * @brief Calculates the P99 allocation latency from the histogram.
 * @param pool Pointer to the memory pool
 * @return P99 latency in nanoseconds, or 0 if no samples
 */
uint64_t mp_get_latency_p99(memory_pool_t* pool) {
    if (!pool || pool->alloc_latency_count == 0) return 0;
    pool_rdlock(pool);
    size_t total = pool->alloc_latency_count;
    size_t target = (total * 99) / 100;
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
 * @brief Returns the average allocation latency in nanoseconds.
 * @param pool Pointer to the memory pool
 * @return Average latency in nanoseconds, or 0 if no samples
 */
uint64_t mp_get_latency_avg(memory_pool_t* pool) {
    if (!pool || pool->alloc_latency_count == 0) return 0;
    pool_rdlock(pool);
    uint64_t avg = pool->alloc_latency_sum_ns / pool->alloc_latency_count;
    pool_rdunlock(pool);
    return avg;
}

/**
 * @brief Resets the allocation latency statistics histogram.
 * @param pool Pointer to the memory pool
 */
void mp_reset_latency_stats(memory_pool_t* pool) {
    if (!pool) return;
    pool_lock(pool);
    memset(pool->alloc_latency_histogram, 0, sizeof(pool->alloc_latency_histogram));
    pool->alloc_latency_count = 0;
    pool->alloc_latency_sum_ns = 0;
    pool_unlock(pool);
}

/**
 * @brief Resets cumulative performance metrics and peak memory statistics.
 * @param pool Pointer to the memory pool
 */
void mp_reset_stats(memory_pool_t* pool) {
    if (!pool) return;
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
 * @brief Returns the optimal size class for a requested byte size.
 * For sizes <= 512B, returns the next Slab size class (8, 16, 32, 64, 128, 256, 512).
 * For larger sizes, returns the size aligned to 8 bytes.
 * @param size Requested size in bytes
 * @return Preferred/aligned size
 */
size_t mp_preferred_size(size_t size) {
    if (size == 0) return 0;
    if (size <= SLAB_MAX_SIZE) {
        for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
            if (kSlabSizes[i] >= size) {
                return kSlabSizes[i];
            }
        }
    }
    return (size + 7) & ~7;
}

/**
 * @brief Returns the optimal size class for a requested byte size using a pool's custom Slab table.
 * @param pool Pointer to the memory pool
 * @param size Requested size in bytes
 * @return Preferred/aligned size based on the pool's configured Slab classes
 */
size_t mp_preferred_size_for_pool(memory_pool_t* pool, size_t size) {
    if (!pool || size == 0) return 0;
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
    return (size + 7) & ~7;
}

/**
 * @brief Configures a custom Slab class size table for the memory pool.
 * @param pool Pointer to the memory pool
 * @param sizes Array of custom slab class sizes in bytes
 * @param count Number of custom sizes (must be <= SLAB_CLASS_COUNT)
 * @return true on success, false on invalid input
 */
bool mp_set_slab_classes(memory_pool_t* pool, const size_t* sizes, size_t count) {
    if (!pool || !sizes || count == 0 || count > SLAB_CLASS_COUNT) return false;

    for (size_t i = 0; i < count; i++) {
        if (sizes[i] == 0 || (i > 0 && sizes[i] <= sizes[i - 1])) return false;
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
 * @brief Retrieves the number of active Slab size classes for a pool.
 * @param pool Pointer to the memory pool
 * @return Number of slab classes, or SLAB_CLASS_COUNT if using defaults
 */
size_t mp_get_slab_class_count(memory_pool_t* pool) {
    if (!pool) return 0;
    pool_rdlock(pool);
    size_t count = pool->use_custom_slab_sizes ? SLAB_CLASS_COUNT : SLAB_CLASS_COUNT;
    pool_rdunlock(pool);
    return count;
}

/**
 * @brief Retrieves the configured Slab class sizes for a pool.
 * @param pool Pointer to the memory pool
 * @param out_sizes Output buffer to store slab class sizes
 * @param max_count Maximum number of sizes to retrieve
 * @return Number of sizes written to out_sizes
 */
size_t mp_get_slab_classes(memory_pool_t* pool, size_t* out_sizes, size_t max_count) {
    if (!pool || !out_sizes || max_count == 0) return 0;
    pool_rdlock(pool);
    size_t count = (max_count < SLAB_CLASS_COUNT) ? max_count : SLAB_CLASS_COUNT;
    for (size_t i = 0; i < count; i++) {
        out_sizes[i] = pool->slab_classes[i].slot_size;
    }
    pool_rdunlock(pool);
    return count;
}

/**
 * @brief Destroys the memory pool and recursively destroys all linked child arenas.
 * Releases all system memory, Slab pages, TLSF pools, and synchronization primitives.
 * @param pool Pointer to the memory pool
 */
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
 * @brief Resets the memory pool and all linked child arenas to an empty state.
 * All allocations are logically freed in O(1) time; underlying memory is retained for reuse.
 * @param pool Pointer to the memory pool
 */
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
        size_t slot_payload_size = pool->slab_classes[c].slot_size;
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

/**
 * @brief Sets a hard maximum memory budget limit on the pool.
 * @param pool Pointer to the memory pool
 * @param max_bytes Maximum allowed active bytes (0 for unlimited)
 */
void mp_set_memory_limit(memory_pool_t* pool, size_t max_bytes) {
    if (!pool) return;
    pool_lock(pool);
    pool->stats.max_memory_limit = max_bytes;
    pool_unlock(pool);
}

/**
 * @brief Compacts the memory pool by releasing completely free Slab pages back to the OS.
 * @param pool Pointer to the memory pool
 * @return Number of bytes freed back to the OS
 */
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

/**
 * @brief Purges unused Slab pages using Linux madvise MADV_DONTNEED to reduce physical RSS.
 * @param pool Pointer to the memory pool
 * @return Number of bytes purged
 */
size_t mp_purge_lazy(memory_pool_t* pool) {
    if (!pool || (pool->flags & MP_FLAG_STATIC_BUFFER)) return 0;
    pool_lock(pool);

    size_t purged_bytes = 0;
    long page_sz = sysconf(_SC_PAGESIZE);
    if (page_sz <= 0) page_sz = 4096;

    for (int c = 0; c < SLAB_CLASS_COUNT; c++) {
        mp_slab_class_t* sc = &pool->slab_classes[c];
        mp_slab_page_t* curr = sc->partial_pages;
        while (curr) {
            if (curr->free_count == curr->total_slots && curr->page_raw_mem) {
                uintptr_t start = (uintptr_t)curr->page_raw_mem + sizeof(mp_slab_page_t);
                uintptr_t aligned_start = (start + page_sz - 1) & ~((uintptr_t)page_sz - 1);
                uintptr_t end = (uintptr_t)curr->page_raw_mem + SLAB_PAGE_SIZE;
                uintptr_t aligned_end = end & ~((uintptr_t)page_sz - 1);

                if (aligned_end > aligned_start) {
                    size_t purge_sz = aligned_end - aligned_start;
#ifdef MADV_DONTNEED
                    madvise((void*)aligned_start, purge_sz, MADV_DONTNEED);
                    purged_bytes += purge_sz;
#else
                    (void)purge_sz;
#endif
                }
            }
            curr = curr->next;
        }
    }

    pool_unlock(pool);
    printf("[CMEM PERF] Lazy RSS physical memory purge completed: %zu bytes released to Linux kernel\n", purged_bytes);
    return purged_bytes;
}

/**
 * @brief Portable wrapper for madvise / VirtualAlloc memory advice across Linux and Windows.
 * @param pool Pointer to the memory pool
 * @param addr Start address of the memory region
 * @param length Length of the memory region in bytes
 * @param advice Advice value (e.g. MADV_DONTNEED on Linux)
 * @return 0 on success, -1 on failure
 */
int mp_madvise(memory_pool_t* pool, void* addr, size_t length, int advice) {
    if (!addr || length == 0) return -1;
    (void)pool;

#ifdef _WIN32
    (void)advice;
    VirtualAlloc(addr, length, MEM_RESET, PAGE_READWRITE);
    return 0;
#else
    long pg = sysconf(_SC_PAGESIZE);
    size_t page_sz = (pg > 0) ? (size_t)pg : 4096;

    uintptr_t start = (uintptr_t)addr;
    uintptr_t aligned_start = (start + page_sz - 1) & ~(page_sz - 1);
    uintptr_t end = start + length;
    uintptr_t aligned_end = end & ~(page_sz - 1);

    if (aligned_end <= aligned_start) {
        return 0;
    }

    size_t aligned_len = aligned_end - aligned_start;

#ifdef MADV_DONTNEED
    return madvise((void*)aligned_start, aligned_len, advice);
#else
    (void)advice;
    return 0;
#endif
#endif
}

/**
 * @brief Trims and reclaims unused memory capacity back to the OS, recursively for child arenas.
 * @param pool Pointer to the memory pool
 * @param pad Minimum number of bytes to keep reserved
 * @return Total bytes reclaimed across all arenas
 */
size_t mp_trim(memory_pool_t* pool, size_t pad) {
    if (!pool) return 0;

    size_t total_reclaimed = 0;

    total_reclaimed += mp_compact(pool);
    total_reclaimed += mp_purge_lazy(pool);

    pool_lock(pool);
    memory_pool_t* child = pool->first_child;
    while (child) {
        memory_pool_t* next = child->next_sibling;
        pool_unlock(pool);
        total_reclaimed += mp_trim(child, pad);
        pool_lock(pool);
        child = next;
    }
    pool_unlock(pool);

    return total_reclaimed;
}

/**
 * @brief Registers an event callback for real-time profiling and debugging.
 * @param pool Pointer to the memory pool
 * @param callback Event callback function pointer
 * @param user_data Optional user data passed to the callback
 */
void mp_set_event_callback(memory_pool_t* pool, mp_event_callback_t callback, void* user_data) {
    if (!pool) return;
    pool_lock(pool);
    pool->event_cb = callback;
    pool->event_user_data = user_data;
    pool_unlock(pool);
}

/**
 * @brief Configures high and low watermark threshold alert callbacks.
 * @param pool Pointer to the memory pool
 * @param high_ratio High watermark ratio (0.0-1.0) that triggers the callback
 * @param low_ratio Low watermark ratio (0.0-1.0) that clears the high state
 * @param cb Watermark callback function pointer
 * @param user_data Optional user data passed to the callback
 */
void mp_set_watermark_callback(memory_pool_t* pool, double high_ratio, double low_ratio, mp_watermark_callback_t cb, void* user_data) {
    if (!pool) return;
    pool_lock(pool);
    pool->high_watermark_ratio = high_ratio;
    pool->low_watermark_ratio = low_ratio;
    pool->watermark_cb = cb;
    pool->watermark_user_data = user_data;
    pool->in_high_watermark_state = false;
    pool_unlock(pool);
}

/**
 * @brief Binds memory pool backing allocations to a specific Linux NUMA CPU node.
 * @param pool Pointer to the memory pool
 * @param numa_node NUMA node ID (-1 for default)
 * @return true on success
 */
bool mp_set_numa_node(memory_pool_t* pool, int numa_node) {
    if (!pool) return false;
    pool_lock(pool);
    pool->numa_node = numa_node;
    pool_unlock(pool);
    printf("[CMEM NUMA] Memory Pool [%s] bound to NUMA CPU Node #%d\n", pool->arena_name, numa_node);
    return true;
}

/**
 * @brief Enables an emergency fallback reserve cushion for critical OOM scenarios.
 * @param pool Pointer to the memory pool
 * @param reserve_bytes Size of the emergency reserve buffer in bytes
 * @return true on success
 */
bool mp_enable_emergency_reserve(memory_pool_t* pool, size_t reserve_bytes) {
    if (!pool || reserve_bytes == 0) return false;
    pool_lock(pool);
    if (pool->emergency_buf) free(pool->emergency_buf);

    pool->emergency_buf = malloc(reserve_bytes);
    if (!pool->emergency_buf) {
        pool_unlock(pool);
        return false;
    }
    pool->emergency_size = reserve_bytes;
    pool->emergency_used = 0;
    pool->in_emergency_state = false;
    pool_unlock(pool);
    printf("[CMEM RELIABILITY] Emergency OOM reserve buffer (%zu bytes) configured for [%s]\n", reserve_bytes, pool->arena_name);
    return true;
}

static inline void check_watermark_after_change(memory_pool_t* pool) {
    if (!pool->watermark_cb || pool->stats.max_memory_limit == 0) return;

    size_t limit = pool->stats.max_memory_limit;
    size_t active = pool->stats.active_bytes;

    if (!pool->in_high_watermark_state && pool->high_watermark_ratio > 0.0) {
        size_t high_thresh = (size_t)(pool->high_watermark_ratio * limit);
        if (active >= high_thresh) {
            pool->in_high_watermark_state = true;
            pool->watermark_cb(pool, true, active, limit, pool->watermark_user_data);
        }
    } else if (pool->in_high_watermark_state && pool->low_watermark_ratio > 0.0) {
        size_t low_thresh = (size_t)(pool->low_watermark_ratio * limit);
        if (active <= low_thresh) {
            pool->in_high_watermark_state = false;
            pool->watermark_cb(pool, false, active, limit, pool->watermark_user_data);
        }
    }
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

/**
 * @brief Allocates memory block with source location tracking for leak diagnostics.
 * @param pool Pointer to the memory pool
 * @param size Size in bytes
 * @param file Source file name (usually __FILE__)
 * @param line Source line number (usually __LINE__)
 * @param func Source function name (usually __func__)
 * @return Pointer to the allocated payload, or NULL on failure
 */
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

/**
 * @brief Allocates zeroed memory block with source location tracking.
 * @param pool Pointer to the memory pool
 * @param num Number of elements
 * @param size Size of each element in bytes
 * @param file Source file name
 * @param line Source line number
 * @param func Source function name
 * @return Pointer to the allocated payload, or NULL on failure
 */
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

/**
 * @brief Reallocates memory block with source location tracking.
 * @param pool Pointer to the memory pool
 * @param ptr Existing allocation pointer (or NULL for new allocation)
 * @param new_size New requested size in bytes
 * @param file Source file name
 * @param line Source line number
 * @param func Source function name
 * @return Pointer to the reallocated payload, or NULL on failure
 */
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

/**
 * @brief Maps a byte size to a Slab class index for small-object allocation.
 * @param pool Pointer to the memory pool
 * @param size Requested size in bytes
 * @return Slab class index, or SLAB_CLASS_COUNT if no class fits
 */
static inline uint8_t get_slab_class_index(memory_pool_t* pool, size_t size) {
    for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
        if (size <= pool->slab_classes[i].slot_size) {
            return (uint8_t)i;
        }
    }
    return (uint8_t)SLAB_CLASS_COUNT;
}

/**
 * @brief Core allocation dispatch function handling TLS cache, Slab, TLSF, and OS fallback paths.
 * @param pool Pointer to the memory pool
 * @param size Requested size in bytes
 * @return Pointer to the payload, or NULL on failure
 */
static void* mp_alloc_internal(memory_pool_t* pool, size_t size) {
    if (!pool || size == 0) return NULL;

    if ((pool->flags & MP_FLAG_PERCPU_FREELIST) && size <= SLAB_MAX_SIZE) {
        uint8_t class_idx = get_slab_class_index(pool, size);
        int cpu = percpu_cpu_index();
        mp_slab_slot_t* slot = percpu_pop(pool, cpu, class_idx);
        if (!slot) {
            percpu_refill(pool, cpu, class_idx);
            slot = percpu_pop(pool, cpu, class_idx);
        }
        if (slot) {
            mp_block_header_t* header = (mp_block_header_t*)slot;
            header->magic = MP_MAGIC_HEAD;
            header->alloc_type = ALLOC_TYPE_SLAB;
            header->slab_class = class_idx;
            header->flags = 0;
            header->requested_size = size;
            header->usable_size = pool->slab_classes[class_idx].slot_size;
            header->raw_base = slot;
            header->alloc_file = NULL;
            header->alloc_line = 0;
            header->alloc_func = NULL;
            header->backtrace_depth = 0;

            void* payload = (void*)((uint8_t*)header + sizeof(mp_block_header_t));
            if (pool->flags & MP_FLAG_DEBUG_CANARY) {
                uint8_t* canary = (uint8_t*)payload + size;
                *canary = MP_CANARY_BYTE;
            }
            if (pool->flags & MP_FLAG_ZERO_ON_ALLOC) memset(payload, 0, size);

            if (!(pool->flags & MP_FLAG_THREAD_SAFE)) {
                active_list_add(pool, header);
                pool->stats.active_bytes += size;
                if (pool->stats.active_bytes > pool->stats.peak_bytes) {
                    pool->stats.peak_bytes = pool->stats.active_bytes;
                }
                pool->stats.active_allocations++;
                pool->stats.total_alloc_ops++;
            } else {
                pool_lock(pool);
                active_list_add(pool, header);
                pool->stats.active_bytes += size;
                if (pool->stats.active_bytes > pool->stats.peak_bytes) {
                    pool->stats.peak_bytes = pool->stats.active_bytes;
                }
                pool->stats.active_allocations++;
                pool->stats.total_alloc_ops++;
                pool_unlock(pool);
            }
            return payload;
        }
    }

    if ((pool->flags & MP_FLAG_THREAD_LOCAL_CACHE) && size <= SLAB_MAX_SIZE) {
        uint8_t class_idx = get_slab_class_index(pool, size);
        if (tls_cache.counts[class_idx] == 0) {
            tls_cache_refill(pool, class_idx);
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
            header->usable_size = pool->slab_classes[class_idx].slot_size;
            header->raw_base = slot;
            header->alloc_file = NULL;
            header->alloc_line = 0;
            header->alloc_func = NULL;
            header->backtrace_depth = 0;

            void* payload = (void*)((uint8_t*)header + sizeof(mp_block_header_t));
            if (pool->flags & MP_FLAG_DEBUG_CANARY) {
                uint8_t* canary = (uint8_t*)payload + size;
                *canary = MP_CANARY_BYTE;
            }
            if (pool->flags & MP_FLAG_ZERO_ON_ALLOC) memset(payload, 0, size);

            if (!(pool->flags & MP_FLAG_THREAD_SAFE)) {
                active_list_add(pool, header);
                pool->stats.active_bytes += size;
                if (pool->stats.active_bytes > pool->stats.peak_bytes) {
                    pool->stats.peak_bytes = pool->stats.active_bytes;
                }
                pool->stats.active_allocations++;
                pool->stats.total_alloc_ops++;
            } else {
                pool_lock(pool);
                active_list_add(pool, header);
                pool->stats.active_bytes += size;
                if (pool->stats.active_bytes > pool->stats.peak_bytes) {
                    pool->stats.peak_bytes = pool->stats.active_bytes;
                }
                pool->stats.active_allocations++;
                pool->stats.total_alloc_ops++;
                pool_unlock(pool);
            }
            return payload;
        }
    }

    pool_lock(pool);
    if (pool->stats.max_memory_limit > 0 && pool->stats.active_bytes + size > pool->stats.max_memory_limit) {
        size_t emerg_total = sizeof(mp_block_header_t) + size + ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0);
        if (pool->emergency_buf && (pool->emergency_used + emerg_total) <= pool->emergency_size) {
            if (!pool->in_emergency_state) {
                pool->in_emergency_state = true;
                fprintf(stderr, "[CMEM CRITICAL] System OOM limit reached! Activating emergency fallback memory reserve buffer (%zu bytes)\n", pool->emergency_size);
            }
            uint8_t* raw = (uint8_t*)pool->emergency_buf + pool->emergency_used;
            pool->emergency_used += emerg_total;

            mp_block_header_t* header = (mp_block_header_t*)raw;
            header->magic = MP_MAGIC_HEAD;
            header->alloc_type = ALLOC_TYPE_EMERGENCY;
            header->slab_class = 0;
            header->flags = 0;
            header->requested_size = size;
            header->usable_size = size;
            header->raw_base = raw;
            header->alloc_file = NULL;
            header->alloc_line = 0;
            header->alloc_func = NULL;
            header->backtrace_depth = 0;

            void* emergency_ptr = (void*)(raw + sizeof(mp_block_header_t));
            if (pool->flags & MP_FLAG_DEBUG_CANARY) {
                uint8_t* canary = (uint8_t*)emergency_ptr + size;
                *canary = MP_CANARY_BYTE;
            }
            active_list_add(pool, header);
            pool->stats.active_bytes += size;
            pool->stats.active_allocations++;
            pool->stats.total_alloc_ops++;
            pool_unlock(pool);
            trigger_event(pool, MP_EVENT_OOM, emergency_ptr, size);
            return emergency_ptr;
        }
        trigger_event(pool, MP_EVENT_OOM, NULL, size);
        pool_unlock(pool);
        return NULL;
    }

    void* ptr = NULL;

    if ((pool->flags & MP_FLAG_STATIC_BUFFER) == 0 && size <= SLAB_MAX_SIZE) {
        uint8_t class_idx = get_slab_class_index(pool, size);
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

        int bucket = get_slab_class_index(pool, size);
        if (bucket < CMEM_HISTOGRAM_BUCKETS) {
            pool->stats.size_histogram[bucket]++;
        }

        if (pool->watermark_cb) check_watermark_after_change(pool);
        if (pool->event_cb) trigger_event(pool, MP_EVENT_ALLOC, ptr, size);
    }

    pool_unlock(pool);
    return ptr;
}

/**
 * @brief Allocates a memory block from the pool.
 * If MP_FLAG_CACHE_ALIGNED is set, delegates to mp_aligned_alloc with 64-byte alignment.
 * @param pool Pointer to the memory pool
 * @param size Requested size in bytes
 * @return Pointer to the payload, or NULL on failure
 */
void* mp_alloc(memory_pool_t* pool, size_t size) {
    if (!pool || size == 0) return NULL;
    if (pool->flags & MP_FLAG_CACHE_ALIGNED) {
        return mp_aligned_alloc(pool, 64, size);
    }
    return mp_alloc_internal(pool, size);
}

/**
 * @brief Allocates multiple memory blocks of the same size in a single operation.
 * @param pool Pointer to the memory pool
 * @param size Size of each block in bytes
 * @param out_ptrs Output array to store allocated pointers
 * @param count Maximum number of blocks to allocate
 * @return Number of blocks successfully allocated
 */
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

/**
 * @brief Frees multiple memory blocks in a single operation.
 * @param pool Pointer to the memory pool
 * @param ptrs Array of pointers to free
 * @param count Number of pointers in the array
 */
void mp_free_batch(memory_pool_t* pool, void** ptrs, size_t count) {
    if (!pool || !ptrs || count == 0) return;
    for (size_t i = 0; i < count; i++) {
        if (ptrs[i]) {
            mp_free(pool, ptrs[i]);
            ptrs[i] = NULL;
        }
    }
}

/**
 * @brief Allocates and zero-initializes memory for an array of elements.
 * @param pool Pointer to the memory pool
 * @param num Number of elements
 * @param size Size of each element in bytes
 * @return Pointer to the allocated payload, or NULL on failure
 */
void* mp_calloc(memory_pool_t* pool, size_t num, size_t size) {
    size_t total_size = num * size;
    void* ptr = mp_alloc(pool, total_size);
    if (ptr && !(pool->flags & MP_FLAG_ZERO_ON_ALLOC)) {
        memset(ptr, 0, total_size);
    }
    return ptr;
}

/**
 * @brief Frees a memory block back to the pool, performing canary checks and poison fill if enabled.
 * @param pool Pointer to the memory pool
 * @param ptr Pointer to the payload to free
 */
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

        if (pool->flags & MP_FLAG_PERCPU_FREELIST) {
            int cpu = percpu_cpu_index();
            mp_slab_slot_t* slot = (mp_slab_slot_t*)header->raw_base;
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
    check_watermark_after_change(pool);
    trigger_event(pool, MP_EVENT_FREE, ptr, header->requested_size);

    if (header->alloc_type == ALLOC_TYPE_SLAB) {
        slab_free(pool, header);
    } else if (header->alloc_type == ALLOC_TYPE_TLSF) {
        tlsf_free(pool, header);
    } else if (header->alloc_type == ALLOC_TYPE_OS) {
        pool->stats.os_allocated_bytes -= header->requested_size;
        pool->stats.total_pool_size -= (header->requested_size + sizeof(mp_block_header_t) + ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0));
        sys_mem_free(pool, header->raw_base, header->requested_size);
    } else if (header->alloc_type == ALLOC_TYPE_EMERGENCY) {
        // Allocated inside pool->emergency_buf; reclaimed automatically on mp_destroy
    }

    pool_unlock(pool);
}

/**
 * @brief Reallocates a memory block to a new size.
 * Attempts in-place expansion for TLSF blocks to avoid memcpy.
 * @param pool Pointer to the memory pool
 * @param ptr Existing allocation pointer (or NULL for new allocation)
 * @param new_size New requested size in bytes
 * @return Pointer to the reallocated payload, or NULL on failure
 */
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
        if (pool->flags & MP_FLAG_DEBUG_CANARY) {
            uint8_t* canary = (uint8_t*)ptr + new_size;
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

    void* new_ptr = mp_alloc(pool, new_size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, header->requested_size);
        mp_free(pool, ptr);
    }
    return new_ptr;
}

/**
 * @brief Overflow-safe reallocarray: reallocates an array with nmemb elements of size.
 * @param pool Pointer to the memory pool
 * @param ptr Existing allocation pointer
 * @param nmemb Number of elements
 * @param size Size of each element in bytes
 * @return Pointer to the reallocated payload, or NULL on overflow/failure
 */
void* mp_reallocarray_loc(memory_pool_t* pool, void* ptr, size_t nmemb, size_t size, const char* file, int line, const char* func) {
    if (nmemb != 0 && size > SIZE_MAX / nmemb) {
        return NULL;
    }
    return mp_realloc_loc(pool, ptr, nmemb * size, file, line, func);
}

/**
 * @brief Overflow-safe reallocarray without location tracking.
 * @param pool Pointer to the memory pool
 * @param ptr Existing allocation pointer
 * @param nmemb Number of elements
 * @param size Size of each element in bytes
 * @return Pointer to the reallocated payload, or NULL on overflow/failure
 */
void* mp_reallocarray(memory_pool_t* pool, void* ptr, size_t nmemb, size_t size) {
    if (nmemb != 0 && size > SIZE_MAX / nmemb) {
        return NULL;
    }
    return mp_realloc(pool, ptr, nmemb * size);
}

/**
 * @brief Duplicates a null-terminated string into the memory pool with location tracking.
 * @param pool Pointer to the memory pool
 * @param str Source string to duplicate
 * @param file Source file name
 * @param line Source line number
 * @param func Source function name
 * @return Pointer to the duplicated string, or NULL on failure
 */
char* mp_strdup_loc(memory_pool_t* pool, const char* str, const char* file, int line, const char* func) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = (char*)mp_alloc_loc(pool, len + 1, file, line, func);
    if (dup) {
        memcpy(dup, str, len + 1);
    }
    return dup;
}

/**
 * @brief Duplicates a null-terminated string into the memory pool.
 * @param pool Pointer to the memory pool
 * @param str Source string to duplicate
 * @return Pointer to the duplicated string, or NULL on failure
 */
char* mp_strdup(memory_pool_t* pool, const char* str) {
    return mp_strdup_loc(pool, str, NULL, 0, NULL);
}

/**
 * @brief Duplicates a binary memory region into the pool with location tracking.
 * @param pool Pointer to the memory pool
 * @param src Source memory region
 * @param n Number of bytes to copy
 * @param file Source file name
 * @param line Source line number
 * @param func Source function name
 * @return Pointer to the duplicated memory, or NULL on failure
 */
void* mp_memdup_loc(memory_pool_t* pool, const void* src, size_t n, const char* file, int line, const char* func) {
    if (!src || n == 0) return NULL;
    void* dup = mp_alloc_loc(pool, n, file, line, func);
    if (dup) {
        memcpy(dup, src, n);
    }
    return dup;
}

/**
 * @brief Duplicates a binary memory region into the pool.
 * @param pool Pointer to the memory pool
 * @param src Source memory region
 * @param n Number of bytes to copy
 * @return Pointer to the duplicated memory, or NULL on failure
 */
void* mp_memdup(memory_pool_t* pool, const void* src, size_t n) {
    return mp_memdup_loc(pool, src, n, NULL, 0, NULL);
}

/**
 * @brief Formats a string and allocates the result in the pool with location tracking.
 * @param pool Pointer to the memory pool
 * @param file Source file name
 * @param line Source line number
 * @param func Source function name
 * @param fmt Printf-style format string
 * @return Pointer to the formatted string, or NULL on failure
 */
char* mp_asprintf_loc(memory_pool_t* pool, const char* file, int line, const char* func, const char* fmt, ...) {
    if (!fmt) return NULL;
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

    char* buf = (char*)mp_alloc_loc(pool, (size_t)len + 1, file, line, func);
    if (buf) {
        vsnprintf(buf, (size_t)len + 1, fmt, args_copy);
    }
    va_end(args_copy);
    return buf;
}

/**
 * @brief Formats a string and allocates the result in the pool.
 * @param pool Pointer to the memory pool
 * @param fmt Printf-style format string
 * @return Pointer to the formatted string, or NULL on failure
 */
char* mp_asprintf(memory_pool_t* pool, const char* fmt, ...) {
    if (!fmt) return NULL;
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

    char* buf = (char*)mp_alloc(pool, (size_t)len + 1);
    if (buf) {
        vsnprintf(buf, (size_t)len + 1, fmt, args_copy);
    }
    va_end(args_copy);
    return buf;
}

/**
 * @brief Allocates memory with a specific byte alignment requirement.
 * @param pool Pointer to the memory pool
 * @param alignment Byte alignment (must be power of two, minimum sizeof(void*))
 * @param size Requested payload size in bytes
 * @return Pointer to the aligned payload, or NULL on failure
 */
void* mp_aligned_alloc(memory_pool_t* pool, size_t alignment, size_t size) {
    if ((alignment & (alignment - 1)) != 0 || alignment < sizeof(void*)) {
        return NULL;
    }

    size_t total_size = size + alignment + sizeof(mp_block_header_t) + ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0);
    void* raw_ptr = mp_alloc_internal(pool, total_size);
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
    } else {
        new_header->requested_size = size;
    }

    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
        uint8_t* canary = (uint8_t*)aligned_addr + size;
        *canary = MP_CANARY_BYTE;
    }

    return (void*)aligned_addr;
}

/**
 * @brief Returns the usable allocated capacity of a pointer block.
 * @param pool Pointer to the memory pool
 * @param ptr Pointer to the payload
 * @return Usable size in bytes, or 0 if invalid
 */
size_t mp_usable_size(memory_pool_t* pool, void* ptr) {
    if (!pool || !ptr || !mp_ptr_valid(pool, ptr)) return 0;
    mp_block_header_t* header = (mp_block_header_t*)((uint8_t*)ptr - sizeof(mp_block_header_t));
    return header->usable_size;
}

/**
 * @brief Returns the requested payload size of an allocated pointer block.
 * @param pool Pointer to the memory pool
 * @param ptr Pointer to the payload
 * @return Requested size in bytes, or 0 if invalid
 */
size_t mp_alloc_size(memory_pool_t* pool, void* ptr) {
    if (!pool || !ptr || !mp_ptr_valid(pool, ptr)) return 0;
    mp_block_header_t* header = (mp_block_header_t*)((uint8_t*)ptr - sizeof(mp_block_header_t));
    return header->requested_size;
}

/**
 * @brief Validates if a pointer belongs to an active allocation in the memory pool.
 * @param pool Pointer to the memory pool
 * @param ptr Pointer to validate
 * @return true if the pointer is valid and active, false otherwise
 */
bool mp_ptr_valid(memory_pool_t* pool, void* ptr) {
    if (!pool || !ptr) return false;
    pool_rdlock(pool);
    mp_block_header_t* header = (mp_block_header_t*)((uint8_t*)ptr - sizeof(mp_block_header_t));
    if (header->magic != MP_MAGIC_HEAD) {
        pool_rdunlock(pool);
        return false;
    }
    bool found = false;
    mp_block_header_t* curr = pool->active_head;
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

/* --- Heap Integrity, Leak Analysis & HTML Dashboard --- */

/**
 * @brief Audits heap integrity by checking header magics and canary redzones of all active allocations.
 * @param pool Pointer to the memory pool
 * @return true if heap is healthy, false if corruption detected
 */
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

/**
 * @brief Generates a detailed memory leak analysis report with file/line locations and callstacks.
 * @param pool Pointer to the memory pool
 * @param report_buf Output buffer for the report text
 * @param max_len Maximum length of the output buffer
 * @return Number of bytes written to report_buf
 */
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

/**
 * @brief Exports the memory leak analysis report to a text file.
 * @param pool Pointer to the memory pool
 * @param filepath Path to the output text file
 * @return true on success
 */
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

/**
 * @brief Exports an interactive visual HTML profiler dashboard to a file.
 * @param pool Pointer to the memory pool
 * @param filepath Path to the output HTML file
 * @return true on success
 */
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

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t total_pool_size;
    uint64_t active_bytes;
    uint64_t active_allocations;
} cmem_snapshot_header_t;

typedef struct {
    uint64_t address;
    uint64_t requested_size;
    uint8_t  alloc_type;
    uint32_t alloc_line;
    char     alloc_file[64];
    char     alloc_func[64];
} cmem_snapshot_record_t;

/**
 * @brief Exports a binary post-mortem memory crash snapshot dump to file.
 * @param pool Pointer to the memory pool
 * @param filepath Path to the output binary snapshot file
 * @return true on success
 */
bool mp_export_binary_snapshot(memory_pool_t* pool, const char* filepath) {
    if (!pool || !filepath) return false;
    FILE* f = fopen(filepath, "wb");
    if (!f) return false;

    pool_lock(pool);

    cmem_snapshot_header_t hdr;
    hdr.magic = 0x434D454D;
    hdr.version = 1;
    hdr.total_pool_size = (uint64_t)pool->stats.total_pool_size;
    hdr.active_bytes = (uint64_t)pool->stats.active_bytes;
    hdr.active_allocations = (uint64_t)pool->stats.active_allocations;

    fwrite(&hdr, sizeof(hdr), 1, f);

    mp_block_header_t* curr = pool->active_head;
    while (curr) {
        cmem_snapshot_record_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.address = (uint64_t)(uintptr_t)((uint8_t*)curr + sizeof(mp_block_header_t));
        rec.requested_size = (uint64_t)curr->requested_size;
        rec.alloc_type = curr->alloc_type;
        rec.alloc_line = (uint32_t)curr->alloc_line;
        if (curr->alloc_file) snprintf(rec.alloc_file, sizeof(rec.alloc_file), "%s", curr->alloc_file);
        if (curr->alloc_func) snprintf(rec.alloc_func, sizeof(rec.alloc_func), "%s", curr->alloc_func);

        fwrite(&rec, sizeof(rec), 1, f);
        curr = curr->next;
    }

    pool_unlock(pool);
    fclose(f);

    printf("[CMEM DIAGNOSTICS] Binary Crash Snapshot Dump exported to: %s\n", filepath);
    return true;
}

/**
 * @brief Parses a binary post-mortem memory snapshot file into readable text report.
 * @param filepath Path to the binary snapshot file
 * @param out_report Output buffer for the text report
 * @param max_len Maximum length of the output buffer
 * @return true on success
 */
bool mp_parse_binary_snapshot(const char* filepath, char* out_report, size_t max_len) {
    if (!filepath || !out_report || max_len == 0) return false;
    FILE* f = fopen(filepath, "rb");
    if (!f) return false;

    cmem_snapshot_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != 0x434D454D) {
        fclose(f);
        return false;
    }

    size_t offset = 0;
    offset += snprintf(out_report + offset, max_len - offset,
        "=================== CMEM BINARY SNAPSHOT DUMP PARSER ===================\n"
        "  Format Version     : %u\n"
        "  Total Pool Size    : %" PRIu64 " bytes\n"
        "  Active Payload B   : %" PRIu64 " bytes\n"
        "  Active Allocations : %" PRIu64 " blocks\n"
        "========================================================================\n",
        hdr.version, hdr.total_pool_size, hdr.active_bytes, hdr.active_allocations
    );

    cmem_snapshot_record_t rec;
    size_t idx = 1;

    while (fread(&rec, sizeof(rec), 1, f) == 1 && offset < max_len) {
        const char* tier_str = (rec.alloc_type == ALLOC_TYPE_SLAB) ? "SLAB" :
                               ((rec.alloc_type == ALLOC_TYPE_TLSF) ? "TLSF" : "DIRECT OS");
        offset += snprintf(out_report + offset, max_len - offset,
            "[Record #%zu] Addr: 0x%" PRIx64 " | Size: %" PRIu64 " B | Tier: %s | Location: %s:%u (%s)\n",
            idx++, rec.address, rec.requested_size, tier_str,
            rec.alloc_file[0] ? rec.alloc_file : "unknown",
            rec.alloc_line,
            rec.alloc_func[0] ? rec.alloc_func : "unknown"
        );
    }

    fclose(f);
    return true;
}

/**
 * @brief Compares two binary snapshot files and generates an incremental leak diff report.
 * @param snapshot_a_path Path to the baseline snapshot
 * @param snapshot_b_path Path to the target snapshot
 * @param out_report Output buffer for the diff report
 * @param max_len Maximum length of the output buffer
 * @return true on success
 */
bool mp_diff_snapshots(const char* snapshot_a_path, const char* snapshot_b_path, char* out_report, size_t max_len) {
    if (!snapshot_a_path || !snapshot_b_path || !out_report || max_len == 0) return false;

    FILE* fa = fopen(snapshot_a_path, "rb");
    FILE* fb = fopen(snapshot_b_path, "rb");
    if (!fa || !fb) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return false;
    }

    cmem_snapshot_header_t hdra, hdrb;
    if (fread(&hdra, sizeof(hdra), 1, fa) != 1 || hdra.magic != 0x434D454D ||
        fread(&hdrb, sizeof(hdrb), 1, fb) != 1 || hdrb.magic != 0x434D454D) {
        fclose(fa);
        fclose(fb);
        return false;
    }

    size_t count_a = (size_t)hdra.active_allocations;
    cmem_snapshot_record_t* recs_a = NULL;
    if (count_a > 0) {
        recs_a = (cmem_snapshot_record_t*)calloc(count_a, sizeof(cmem_snapshot_record_t));
        if (recs_a) {
            fread(recs_a, sizeof(cmem_snapshot_record_t), count_a, fa);
        }
    }
    fclose(fa);

    size_t offset = 0;
    size_t diff_count = 0;
    size_t diff_bytes = 0;

    offset += snprintf(out_report + offset, max_len - offset,
        "=================== CMEM INCREMENTAL SNAPSHOT DIFF REPORT ===================\n"
        "  Baseline Snapshot A : %s (%u blocks)\n"
        "  Target Snapshot B   : %s (%u blocks)\n"
        "=============================================================================\n",
        snapshot_a_path, (unsigned)hdra.active_allocations,
        snapshot_b_path, (unsigned)hdrb.active_allocations
    );

    cmem_snapshot_record_t recb;
    while (fread(&recb, sizeof(recb), 1, fb) == 1) {
        bool found_in_a = false;
        for (size_t i = 0; i < count_a; i++) {
            if (recs_a && recs_a[i].address == recb.address && recs_a[i].requested_size == recb.requested_size) {
                found_in_a = true;
                break;
            }
        }
        if (!found_in_a) {
            diff_count++;
            diff_bytes += (size_t)recb.requested_size;
            if (offset < max_len) {
                const char* tier_str = (recb.alloc_type == ALLOC_TYPE_SLAB) ? "SLAB" :
                                       ((recb.alloc_type == ALLOC_TYPE_TLSF) ? "TLSF" : "DIRECT OS");
                offset += snprintf(out_report + offset, max_len - offset,
                    "[Incremental Leak #%zu] Addr: 0x%" PRIx64 " | Size: %" PRIu64 " B | Tier: %s | Location: %s:%u (%s)\n",
                    diff_count, recb.address, recb.requested_size, tier_str,
                    recb.alloc_file[0] ? recb.alloc_file : "unknown",
                    recb.alloc_line,
                    recb.alloc_func[0] ? recb.alloc_func : "unknown"
                );
            }
        }
    }

    fclose(fb);
    if (recs_a) free(recs_a);

    if (offset < max_len) {
        offset += snprintf(out_report + offset, max_len - offset,
            "-----------------------------------------------------------------------------\n"
            "  Net Incremental Leaked Allocations : %zu blocks\n"
            "  Net Incremental Leaked Bytes       : %zu bytes\n"
            "=============================================================================\n",
            diff_count, diff_bytes
        );
    }

    return true;
}

/**
 * @brief Retrieves current statistical metrics of the memory pool.
 * @param pool Pointer to the memory pool
 * @param stats Output statistics structure
 */
void mp_get_stats(memory_pool_t* pool, mp_stats_t* stats) {
    if (!pool || !stats) return;
    pool_rdlock(pool);
    *stats = pool->stats;
    size_t total_sys = pool->stats.total_pool_size > 0 ? pool->stats.total_pool_size : 1;
    stats->fragmentation_ratio = 1.0 - ((double)pool->stats.active_bytes / (double)total_sys);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (pool->window_start_time.tv_sec == 0) {
        pool->window_start_time = now;
    }
    double elapsed = (now.tv_sec - pool->window_start_time.tv_sec) + (now.tv_nsec - pool->window_start_time.tv_nsec) / 1e9;
    size_t ops = pool->stats.total_alloc_ops;
    size_t active = pool->stats.active_bytes;
    if (elapsed > 0.000001 && ops > 0) {
        stats->alloc_qps = (double)ops / elapsed;
        stats->bandwidth_mbps = ((double)active / (1024.0 * 1024.0)) / elapsed;
    } else {
        stats->alloc_qps = (double)ops;
        stats->bandwidth_mbps = (double)active / (1024.0 * 1024.0);
    }
    pool_rdunlock(pool);
}

/**
 * @brief Prints detailed summary and health status of the memory pool to stdout.
 * @param pool Pointer to the memory pool
 */
void mp_dump_info(memory_pool_t* pool) {
    if (!pool) return;
    mp_stats_t stats;
    mp_get_stats(pool, &stats);

    printf("\n================ CMEM DIAGNOSTICS DUMP [%s] ================\n", pool->arena_name);
    printf("  Total System Reserved Memory: %zu bytes (%.2f KB)\n", stats.total_pool_size, stats.total_pool_size / 1024.0);
    printf("  Current Active Allocations  : %zu blocks, %zu bytes (%.2f KB)\n", stats.active_allocations, stats.active_bytes, stats.active_bytes / 1024.0);
    printf("  Peak Memory Allocation      : %zu bytes (%.2f KB)\n", stats.peak_bytes, stats.peak_bytes / 1024.0);
    printf("  Max Memory Budget Limit     : %zu bytes (%s)\n", stats.max_memory_limit, stats.max_memory_limit > 0 ? "Enforced" : "Unlimited");
    printf("  Estimated Fragmentation     : %.2f%%\n", stats.fragmentation_ratio * 100.0);
    printf("  Real-time Alloc Rate (QPS)  : %.2f ops/sec\n", stats.alloc_qps);
    printf("  Real-time Bandwidth         : %.2f MB/sec\n", stats.bandwidth_mbps);
    printf("  Cumulative Stats            : %zu Allocations, %zu Frees\n", stats.total_alloc_ops, stats.total_free_ops);
    printf("  Allocation Tier Breakdown   :\n");
    printf("    - Slab Pool (Small <=512B): %zu bytes\n", stats.slab_allocated_bytes);
    printf("    - TLSF Pool (Med <=4MB)   : %zu bytes\n", stats.tlsf_allocated_bytes);
    printf("    - Direct OS (Large >4MB)  : %zu bytes\n", stats.os_allocated_bytes);
    printf("==============================================================\n\n");
}

/**
 * @brief Prints ASCII allocation size distribution histogram chart to stdout.
 * @param pool Pointer to the memory pool
 */
void mp_dump_histogram(memory_pool_t* pool) {
    if (!pool) return;
    mp_stats_t stats;
    mp_get_stats(pool, &stats);

    static const char* labels[CMEM_HISTOGRAM_BUCKETS] = {
        "<= 16 B       ", "17 B - 32 B    ", "33 B - 64 B    ", "65 B - 128 B   ",
        "129 B - 256 B  ", "257 B - 512 B  ", "513 B - 1 KB   ", "1 KB - 2 KB    ",
        "2 KB - 4 KB    ", "4 KB - 8 KB    ", "8 KB - 16 KB   ", "16 KB - 32 KB  ",
        "32 KB - 64 KB  ", "64 KB - 512 KB ", "512 KB - 4 MB  ", "> 4 MB         "
    };

    size_t max_count = 0;
    for (int i = 0; i < CMEM_HISTOGRAM_BUCKETS; i++) {
        if (stats.size_histogram[i] > max_count) max_count = stats.size_histogram[i];
    }

    printf("\n================ ALLOCATION SIZE HISTOGRAM [%s] ================\n", pool->arena_name);
    for (int i = 0; i < CMEM_HISTOGRAM_BUCKETS; i++) {
        if (stats.size_histogram[i] == 0) continue;
        int bar_len = (max_count > 0) ? (int)((stats.size_histogram[i] * 20) / max_count) : 0;
        char bar_str[21];
        memset(bar_str, '*', bar_len);
        bar_str[bar_len] = '\0';

        printf("  Bucket %-2d [%s] : %-8zu [%-20s]\n", i, labels[i], stats.size_histogram[i], bar_str);
    }
    printf("=========================================================================\n\n");
}

/**
 * @brief Recursively prints a memory pool arena tree node and its children.
 * @param pool Pointer to the memory pool node to print
 * @param indent Current indentation level
 */
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

/**
 * @brief Dumps memory pool tree hierarchy to stdout.
 * @param pool Pointer to the root memory pool
 */
void mp_dump_tree_info(memory_pool_t* pool) {
    if (!pool) return;
    pool_lock(pool);
    printf("\n================ CMEM ARENA TREE DUMP ================\n");
    print_arena_node(pool, 0);
    printf("======================================================\n\n");
    pool_unlock(pool);
}

/**
 * @brief Dumps memory pool stats into JSON format buffer for telemetry monitoring.
 * @param pool Pointer to the memory pool
 * @param buf Output buffer for JSON text
 * @param max_len Maximum length of the output buffer
 * @return Number of bytes written to buf
 */
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
        "  \"max_memory_limit\": %zu,\n"
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
        stats.max_memory_limit,
        stats.active_allocations, stats.total_alloc_ops, stats.total_free_ops,
        stats.slab_allocated_bytes, stats.tlsf_allocated_bytes, stats.os_allocated_bytes,
        stats.fragmentation_ratio
    );

    return (len > 0 && (size_t)len < max_len) ? (size_t)len : max_len - 1;
}

/**
 * @brief Formats and dumps memory pool metrics into Prometheus text exposition format.
 * @param pool Pointer to the memory pool
 * @param out_buf Output buffer for Prometheus metrics text
 * @param max_len Maximum length of the output buffer
 * @return Number of bytes written to out_buf
 */
size_t mp_export_prometheus_metrics(memory_pool_t* pool, char* out_buf, size_t max_len) {
    if (!pool || !out_buf || max_len == 0) return 0;
    mp_stats_t stats;
    mp_get_stats(pool, &stats);

    int len = snprintf(out_buf, max_len,
        "# HELP cmem_total_pool_bytes Total reserved bytes from OS.\n"
        "# TYPE cmem_total_pool_bytes gauge\n"
        "cmem_total_pool_bytes{arena=\"%s\"} %zu\n\n"
        "# HELP cmem_active_bytes Currently allocated active payload bytes.\n"
        "# TYPE cmem_active_bytes gauge\n"
        "cmem_active_bytes{arena=\"%s\"} %zu\n\n"
        "# HELP cmem_active_allocations Active outstanding allocation count.\n"
        "# TYPE cmem_active_allocations gauge\n"
        "cmem_active_allocations{arena=\"%s\"} %zu\n\n"
        "# HELP cmem_alloc_ops_total Cumulative count of allocation operations.\n"
        "# TYPE cmem_alloc_ops_total counter\n"
        "cmem_alloc_ops_total{arena=\"%s\"} %zu\n\n"
        "# HELP cmem_alloc_qps Real-time allocation QPS rate.\n"
        "# TYPE cmem_alloc_qps gauge\n"
        "cmem_alloc_qps{arena=\"%s\"} %.2f\n\n"
        "# HELP cmem_bandwidth_mbps Real-time allocation bandwidth throughput in MB/s.\n"
        "# TYPE cmem_bandwidth_mbps gauge\n"
        "cmem_bandwidth_mbps{arena=\"%s\"} %.2f\n\n"
        "# HELP cmem_fragmentation_ratio Memory fragmentation ratio (0.0 to 1.0).\n"
        "# TYPE cmem_fragmentation_ratio gauge\n"
        "cmem_fragmentation_ratio{arena=\"%s\"} %.4f\n",
        pool->arena_name, stats.total_pool_size,
        pool->arena_name, stats.active_bytes,
        pool->arena_name, stats.active_allocations,
        pool->arena_name, stats.total_alloc_ops,
        pool->arena_name, stats.alloc_qps,
        pool->arena_name, stats.bandwidth_mbps,
        pool->arena_name, stats.fragmentation_ratio
    );

    return (len > 0 && (size_t)len < max_len) ? (size_t)len : max_len - 1;
}

/**
 * @brief Checks if there are any un-freed memory allocations and prints a leak report if found.
 * @param pool Pointer to the memory pool
 * @return true if no leaks detected, false otherwise
 */
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

/* --- Game & Graphics Pipeline Dual Ping-Pong Frame Arena --- */
/**
 * @brief Frame arena structure for double-buffered per-frame allocations.
 */
struct cmem_frame_arena {
    memory_pool_t* pool_a;
    memory_pool_t* pool_b;
    memory_pool_t* active_pool;
    size_t frame_index;
};

/**
 * @brief Creates a game & graphics dual ping-pong frame arena allocator.
 * @param frame_capacity Capacity per frame buffer in bytes
 * @return Pointer to the frame arena, or NULL on failure
 */
cmem_frame_arena_t* mp_frame_arena_create(size_t frame_capacity) {
    cmem_frame_arena_t* farena = (cmem_frame_arena_t*)malloc(sizeof(cmem_frame_arena_t));
    if (!farena) return NULL;

    size_t cap = frame_capacity > 0 ? frame_capacity : 1024 * 1024;
    farena->pool_a = mp_create(cap, MP_FLAG_DEFAULT);
    farena->pool_b = mp_create(cap, MP_FLAG_DEFAULT);
    if (!farena->pool_a || !farena->pool_b) {
        if (farena->pool_a) mp_destroy(farena->pool_a);
        if (farena->pool_b) mp_destroy(farena->pool_b);
        free(farena);
        return NULL;
    }

    farena->active_pool = farena->pool_a;
    farena->frame_index = 0;
    return farena;
}

/**
 * @brief Allocates temporary memory for the current frame from the active buffer.
 * @param farena Pointer to the frame arena
 * @param size Requested size in bytes
 * @return Pointer to the payload, or NULL on failure
 */
void* mp_frame_alloc(cmem_frame_arena_t* farena, size_t size) {
    if (!farena || !farena->active_pool) return NULL;
    return mp_alloc(farena->active_pool, size);
}

/**
 * @brief Ends the current frame and swaps the active ping-pong buffer with O(1) reset.
 * @param farena Pointer to the frame arena
 */
void mp_frame_end(cmem_frame_arena_t* farena) {
    if (!farena) return;
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
 * @brief Destroys the frame arena allocator instance and frees both buffers.
 * @param farena Pointer to the frame arena
 */
void mp_frame_arena_destroy(cmem_frame_arena_t* farena) {
    if (!farena) return;
    if (farena->pool_a) mp_destroy(farena->pool_a);
    if (farena->pool_b) mp_destroy(farena->pool_b);
    free(farena);
}

