/**
 * @file cmem_internal.h
 * @brief Internal shared declarations for cmem modules.
 *
 * This header is included by all cmem implementation files to share
 * type definitions, inline functions, and macros.
 * It is NOT part of the public API and must not be included by users.
 */

#ifndef CMEM_INTERNAL_H
#define CMEM_INTERNAL_H

#include "cmem.h"

/* -------------------------------------------------------------------------
 * Platform / C++ compatibility
 *
 * Portable abstractions over C11 <stdatomic.h> and C++ <atomic>.  All cmem
 * modules use these wrappers (never the raw <atomic> / <stdatomic.h> API
 * directly) so that the same source compiles cleanly as both C11 and C++17.
 * The `cmem_atomic_size_t` typedef hides the platform's atomic counter type,
 * and the CMEM_ATOMIC_* macros map to the explicit-memory-order functions so
 * lock-free algorithms can specify their intended ordering at each site.
 * -------------------------------------------------------------------------
 */
#ifdef __cplusplus
#include <atomic>
typedef std::atomic<size_t> cmem_atomic_size_t; /* Atomic size counter (C++ std::atomic) */
#define CMEM_ATOMIC_FETCH_ADD(obj, arg, order) std::atomic_fetch_add_explicit(obj, arg, order)
#define CMEM_ATOMIC_FETCH_SUB(obj, arg, order) std::atomic_fetch_sub_explicit(obj, arg, order)
#define CMEM_ATOMIC_LOAD(obj, order) std::atomic_load_explicit(obj, order)
#define CMEM_ATOMIC_STORE(obj, val, order) std::atomic_store_explicit(obj, val, order)
#define CMEM_ATOMIC_COMPARE_EXCHANGE(obj, expected, desired, succ, fail)                           \
    std::atomic_compare_exchange_weak_explicit(obj, expected, desired, succ, fail)
#define CMEM_ATOMIC_INIT(obj, val) std::atomic_init(obj, val)
#define CMEM_ORDER_RELAXED std::memory_order_relaxed
#define CMEM_ORDER_ACQUIRE std::memory_order_acquire
#define CMEM_ORDER_RELEASE std::memory_order_release
#define MP_THREAD_LOCAL thread_local
#elif defined(_WIN32)
#include <windows.h>
typedef volatile LONG_PTR cmem_atomic_size_t;
#define CMEM_ATOMIC_FETCH_ADD(obj, arg, order)                                                     \
    InterlockedExchangeAdd64((LONG64 volatile *)(obj), (LONG64)(arg))
#define CMEM_ATOMIC_FETCH_SUB(obj, arg, order)                                                     \
    InterlockedExchangeAdd64((LONG64 volatile *)(obj), -(LONG64)(arg))
#define CMEM_ATOMIC_LOAD(obj, order) InterlockedCompareExchange64((LONG64 volatile *)(obj), 0, 0)
#define CMEM_ATOMIC_STORE(obj, val, order)                                                         \
    InterlockedExchange64((LONG64 volatile *)(obj), (LONG64)(val))
#define CMEM_ATOMIC_COMPARE_EXCHANGE(obj, expected, desired, succ, fail)                           \
    (InterlockedCompareExchange64(                                                                 \
         (LONG64 volatile *)(obj), (LONG64)(desired), (LONG64)(*expected)) == (LONG64)(*expected))
#define CMEM_ATOMIC_INIT(obj, val) (*(volatile LONG_PTR *)(obj) = (LONG_PTR)(val))
#define CMEM_ORDER_RELAXED 0
#define CMEM_ORDER_ACQUIRE 0
#define CMEM_ORDER_RELEASE 0
#define MP_THREAD_LOCAL __declspec(thread)
#else
#include <errno.h>
#include <stdatomic.h>
typedef atomic_size_t cmem_atomic_size_t;
#define CMEM_ATOMIC_FETCH_ADD(obj, arg, order) atomic_fetch_add_explicit(obj, arg, order)
#define CMEM_ATOMIC_FETCH_SUB(obj, arg, order) atomic_fetch_sub_explicit(obj, arg, order)
#define CMEM_ATOMIC_LOAD(obj, order) atomic_load_explicit(obj, order)
#define CMEM_ATOMIC_STORE(obj, val, order) atomic_store_explicit(obj, val, order)
#define CMEM_ATOMIC_COMPARE_EXCHANGE(obj, expected, desired, succ, fail)                           \
    atomic_compare_exchange_weak_explicit(obj, expected, desired, succ, fail)
#define CMEM_ATOMIC_INIT(obj, val) atomic_init(obj, val)
#define CMEM_ORDER_RELAXED memory_order_relaxed
#define CMEM_ORDER_ACQUIRE memory_order_acquire
#define CMEM_ORDER_RELEASE memory_order_release
#define MP_THREAD_LOCAL _Thread_local
#endif

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
#endif

#define MP_PERCPU_MAX_BATCH 16

/**
 * @brief Per-thread allocation accounting for thread-level memory quotas.
 *
 * A thread-local record of how many bytes and allocation operations the
 * current thread has consumed.  Used by the thread-quota feature to cap an
 * individual thread's impact on the shared pool before hitting global OOM.
 */
typedef struct {
    size_t alloc_bytes; /**< Total bytes still attributed to this thread   */
    size_t alloc_count; /**< Number of live allocations held by this thread */
} mp_thread_quota_t;

/**
 * @brief File header of a binary memory snapshot (.cmem_dump).
 *
 * Written at the front of every exported snapshot so a later
 * mp_parse_binary_snapshot() can validate magic/version and know the pool
 * dimensions before reading the per-allocation records.
 */
typedef struct {
    uint32_t magic;              /**< Magic bytes identifying a cmem snapshot      */
    uint32_t version;            /**< Snapshot format version                      */
    uint64_t total_pool_size;    /**< Total backing size of the pool at dump time  */
    uint64_t active_bytes;       /**< Sum of bytes in live allocations             */
    uint64_t active_allocations; /**< Count of live allocations recorded           */
} cmem_snapshot_header_t;

/**
 * @brief A single allocation record inside a binary snapshot.
 *
 * Pairs each tracked allocation with its requested size, allocation type,
 * and source location so post-mortem tools can reproduce a leak report.
 */
typedef struct {
    uint64_t address;        /**< Address of the allocation (null if freed)  */
    uint64_t requested_size; /**< User-requested size in bytes               */
    uint8_t alloc_type;      /**< Which tier served it (slab/TLSF/direct)    */
    uint32_t alloc_line;     /**< __LINE__ of the allocation site            */
    char alloc_file[64];     /**< __FILE__ of the allocation site            */
    char alloc_func[64];     /**< __func__ of the allocation site            */
} cmem_snapshot_record_t;

/* -------------------------------------------------------------------------
 * Shared constants
 * -------------------------------------------------------------------------
 */
#define MP_MAGIC_HEAD 0x4D504F4F       /* Magic value ("MPOO") stamped on block headers          */
#define MP_CANARY_BYTE 0xDE            /* Fill byte for canary regions (buffer-overflow guard)  */
#define MP_POISON_BYTE 0xDD            /* Fill byte for freed payloads (use-after-free guard)   */
#define CMEM_FREED_MAGIC 0xDEADBEEFu   /* Magic stamped on a block header once it is freed      */
#define CMEM_SNAPSHOT_MAGIC 0x434D454D /* Magic bytes ("CMEM") in binary snapshot header  */
/* Upper bound on allocation records accepted from a binary snapshot file.  Guards an
 * untrusted file from forcing an oversized calloc() and keeps validation realistic. */
#define CMEM_MAX_SNAPSHOT_RECORDS (8ul * 1024ul * 1024ul)

/* NUMA memory policy: MPOL_BIND (value 2).  Used with the mbind() syscall to
 * pin freshly mapped pages to a specific NUMA node. */
#ifndef CMEM_MPOL_BIND
#define CMEM_MPOL_BIND 2
#endif

/* Lazily-probed NUMA topology.  cpu_to_node[cpu] holds the node owning that
 * CPU; NULL when topology detection is unavailable (single-node fallback). */
typedef struct cmem_numa_topology {
    int node_count;   /* Number of NUMA nodes (>= 1)                          */
    int cpu_count;    /* Number of CPUs covered by cpu_to_node[]              */
    int *cpu_to_node; /* cpu -> owning node map, NULL if not probed           */
} cmem_numa_topology_t;

/* Slab allocator tuning. Small allocations (<= SLAB_MAX_SIZE) are served from
 * SLAB_CLASS_COUNT fixed bucket sizes carved out of SLAB_PAGE_SIZE pages. */
#define SLAB_CLASS_COUNT 7         /* Number of distinct small-object size classes    */
#define SLAB_MAX_SIZE 512          /* Largest object routed to a slab class (bytes)   */
#define SLAB_PAGE_SIZE (64 * 1024) /* Bytes carved per slab page (64 KB)              */
#define TLS_CACHE_MAX_SLOTS 256    /* Max slots stashed in a thread-local cache       */
#define MAX_BACKTRACE_FRAMES 8     /* Max callstack frames captured per allocation    */

/* TLSF Allocator Constants: the two-level segregated fit index geometry. */
#define TLSF_SL_SHIFT 4                    /* Bits of second-level index (seg)                */
#define TLSF_SL_COUNT (1 << TLSF_SL_SHIFT) /* 16 subdivisions per first-level bucket  */
#define TLSF_FL_MAX 30                     /* Number of first-level log2-size buckets (<= 1GB) */
#define TLSF_MIN_BLOCK_SIZE 32             /* Smallest block the TLSF region will manage      */
#define TLSF_MAX_SIZE (4 * 1024 * 1024)    /* Upper byte bound routed through TLSF (4 MB) */

/* Block-header flag bits carved from the low two bits of each TLSF block's size field. */
#define BLOCK_STATE_FREE 0x1      /* Bit: this block is free      */
#define BLOCK_STATE_PREV_FREE 0x2 /* Bit: the physically-adjacent previous block is free */
#define BLOCK_SIZE_MASK                                                                            \
    (~(size_t)(BLOCK_STATE_FREE | BLOCK_STATE_PREV_FREE)) /* Mask to extract the real size */

/* -------------------------------------------------------------------------
 * Shared internal types
 * -------------------------------------------------------------------------
 */
/**
 * @brief cmem's own per-allocation metadata header.
 *
 * Every allocation cmem hands out points at a user payload whose address is
 * immediately preceded by one of these headers. It stores the mgmt info
 * needed for free(), realloc(), leak tracking, and introspection. This is a
 * cmem-internal structure distinct from the TLSF block header in cmem_tlsf.c.
 */
typedef struct mp_block_header {
    uint32_t magic;        /**< MP_MAGIC_HEAD to detect free/corruption         */
    uint8_t alloc_type;    /**< Allocation tier: slab / TLSF / direct OS        */
    uint8_t slab_class;    /**< Slab class index if served from a slab         */
    uint16_t flags;        /**< Per-block flags (aligned, poisoned, etc).      */
    size_t requested_size; /**< Size the caller actually asked for             */
    size_t usable_size;    /**< Padded size actually handed out                */
    void *raw_base;        /**< Start of backing memory (for munmap/free)      */
    void *subpool;         /**< Owning subpool/child arena, if any             */

    const char *alloc_file; /**< __FILE__ when location tracking is enabled     */
    int alloc_line;         /**< __LINE__ when location tracking is enabled     */
    const char *alloc_func; /**< __func__  when location tracking is enabled    */
    void *backtrace_addrs[MAX_BACKTRACE_FRAMES]; /**< Captured stack addresses */
    int backtrace_depth;                         /**< Number of valid backtrace frames */

    struct mp_block_header *prev; /**< Previous live block in the active list      */
    struct mp_block_header *next; /**< Next live block in the active list          */
} mp_block_header_t;

/**
 * @brief A free slot carved from a slab page.
 *
 * While a slot is free its first word is reused as the "next" freelist
 * pointer (intrusive singly linked list). When allocated, that space becomes
 * the user's payload. Only one mp_slab_slot_t per free object exists.
 */
typedef struct mp_slab_slot {
    struct mp_slab_slot *next; /**< Next free slot in the same class freelist */
} mp_slab_slot_t;

/**
 * @brief One full (or partially-used) 64 KB slab page.
 *
 * A page is carved into total_slots equal sized chunks of its class, links
 * into per-class lists, and tracks thermal/paging state.
 */
typedef struct mp_slab_page {
    uint8_t class_index;       /**< Slab size class this page serves         */
    uint16_t free_count;       /**< Number of slots still free                */
    uint16_t total_slots;      /**< Total slots carved from the page          */
    mp_slab_slot_t *free_list; /**< Head of the page's free-object list       */
    struct mp_slab_page *next; /**< Sibling in the class's page list          */
    struct mp_slab_page *prev; /**< Previous sibling in the page list         */
    void *page_raw_mem;        /**< Raw page base (for munmap and hot/cold)   */
    bool is_hot;               /**< True when marked hot for TLB optimization */
    int64_t idle_since_ts;     /**< Monotonic timestamp when page became fully idle (0 = not idle) */
} mp_slab_page_t;

/**
 * @brief One slab size class: a per-class freelist of carved pages.
 *
 * Each class owns a lock so that independent size classes can service
 * allocations concurrently without a global pool lock.
 */
typedef struct {
    size_t slot_size;              /**< Bytes per slot in this class                 */
    pthread_mutex_t lock;          /**< Guards the class page lists (fine-grained)   */
    mp_slab_page_t *partial_pages; /**< Pages with free slots (next refill source)  */
    mp_slab_page_t *full_pages;    /**< Completely full pages                         */
    mp_slab_page_t *hot_pages;     /**< Pages marked hot                              */
    mp_slab_page_t *cold_pages;    /**< Pages marked cold                             */
} mp_slab_class_t;

/**
 * @brief Thread-local small-object cache.
 *
 * When MP_FLAG_THREAD_LOCAL_CACHE is on, each thread caches a few slots per
 * slab class here to avoid taking the class lock for every allocation/free.
 */
typedef struct {
    mp_slab_slot_t *slots[SLAB_CLASS_COUNT]; /**< Per-class cache of free slots        */
    uint16_t counts[SLAB_CLASS_COUNT];       /**< Number of cached slots per class      */
} thread_cache_t;

extern MP_THREAD_LOCAL thread_cache_t tls_cache; /**< Per-thread slab cache (TLS)            */
extern MP_THREAD_LOCAL mp_thread_quota_t thread_quota; /**< Per-thread quota accounting (TLS) */

/**
 * @brief TLSF block header (cmem_tlsf.c).
 *
 * Each memory chunk in a TLSF arena carries this 5-word footer-free header.
 * The low two bits of `size_and_flags` encode the FREE / PREV_FREE flags.
 */
typedef struct tlsf_block {
    size_t size_and_flags;            /**< Block length + free flags (low bits)   */
    struct tlsf_block *prev_physical; /**< Physically previous block in the arena */
    struct tlsf_block *next_free;     /**< Next block in a free-size list          */
    struct tlsf_block *prev_free;     /**< Previous block in a free-size list      */
} tlsf_block_t;

/**
 * @brief A TLSF arena (physical memory region being managed).
 *
 * Two-level bitmap indexing: `fl_bitmap` scans first-level bins; each set bit
 * points to one `sl_bitmap` whose bits index the second-level free lists.
 */
typedef struct tlsf_pool {
    uint32_t fl_bitmap;                               /**< First-level free-bin bitmap           */
    uint32_t sl_bitmap[TLSF_FL_MAX];                  /**< Second-level bitmap per first-level   */
    tlsf_block_t *blocks[TLSF_FL_MAX][TLSF_SL_COUNT]; /**< Free-list heads per bin    */
    void *raw_area;                                   /**< Raw base of this TLSF region          */
    size_t raw_size;                                  /**< Size of the TLSF region               */
    struct tlsf_pool *next;                           /**< Next TLSF arena (linked for expansion) */
} tlsf_pool_t;

/**
 * @brief One per-CPU lock-free freelist entry.
 *
 * A single head/tail atomic counter pair; the "queue" actually stores two
 * cursors into a fixed pre-allocated array, allowing wait-free push/pop.
 */
typedef struct {
    cmem_atomic_size_t head; /**< Atomic head cursor of the per-CPU freelist */
    uint16_t count;          /**< Number of slots currently in the list      */
} mp_percpu_freelist_entry_t;

/**
 * @brief Zero-header-overhead fixed-size typed object pool.
 *
 * Manages a contiguous pre-allocated buffer of `elem_size` objects with an
 * intrusive freelist, adding no header to each element.
 */
typedef struct mp_typed_pool {
    size_t elem_size;    /**< Bytes per fixed-size element         */
    size_t capacity;     /**< Max number of elements              */
    size_t active_count; /**< Number of pool-allocated objects    */
    void *free_list;     /**< Head of the intrusive free list     */
    void *raw_buf;       /**< Underlying backing array            */
} mp_typed_pool_t;

/**
 * @brief Core cmem memory-pool state (opaque `memory_pool_t`).
 *
 * Every mp_create*() / mp_create_child() returns one of these. It bundles
 * configuration flags, locking primitives, statistics, the slab + TLSF + OS
 * backend state, child arena tree links, and all the feature toggles
 * (quotas, circuit breaker, hot/cold, ASAN integration, etc.).
 */
struct memory_pool {         // NOLINT(clang-analyzer-optin.performance.Padding)
    mp_flags_t flags;        /**< Configuration flags from mp_create()      */
    pthread_rwlock_t rwlock; /**< RW lock guarding reads vs structural writes */
    pthread_mutex_t lock;    /**< Mutex for the main allocation path           */
    char arena_name[64];     /**< Human-readable arena name                   */

    struct memory_pool *parent;       /**< Parent pool (NULL if root)                 */
    struct memory_pool *first_child;  /**< First child arena in a tree                */
    struct memory_pool *next_sibling; /**< Next child in the parent's arena list      */

    bool has_custom_sys_alloc;        /**< True when a custom sys allocator is installed */
    mp_sys_allocator_t sys_allocator; /**< Vtable for custom backing allocation     */

    mp_event_callback_t event_cb; /**< User event callback (mp_set_event_callback) */
    void *event_user_data;        /**< User arg passed to event_cb             */

    mp_watermark_callback_t watermark_cb; /**< Watermark threshold callback            */
    double high_watermark_ratio;          /**< High threshold (0..1)            */
    double low_watermark_ratio;           /**< Low threshold (0..1)              */
    bool in_high_watermark_state;         /**< Currently above high watermark */
    void *watermark_user_data;            /**< Watermark callback arg         */

    int numa_node; /**< NUMA node affinity for backing pages (Linux) */

    void *emergency_buf;     /**< Pre-reserved crash cushion                 */
    size_t emergency_size;   /**< Size of the emergency reserve              */
    size_t emergency_used;   /**< Bytes currently drawn from the reserve     */
    bool in_emergency_state; /**< True while serving from the reserve     */

    mp_stats_t stats;                  /**< Public accounting stats                */
    mp_block_header_t *active_head;    /**< Head of the live-allocation linked list */
    uint64_t window_alloc_ops;         /**< Allocation ops in current sample window  */
    uint64_t window_alloc_bytes;       /**< Bytes allocated in current window      */
    struct timespec window_start_time; /**< Start of the sampling window          */

    mp_slab_class_t slab_classes[SLAB_CLASS_COUNT]; /**< One class per size bucket     */
    bool use_custom_slab_sizes;                     /**< Custom class table in effect */
    size_t custom_slab_sizes[SLAB_CLASS_COUNT];     /**< Custom class sizes        */

    tlsf_pool_t *tlsf_root; /**< First TLSF arena (chained for expansion) */

    uint64_t env_flags_generation; /**< Incremented on each CMEM_CONF reparse */

    bool auto_compact_enabled;                   /**< Auto-compact on/off              */
    double auto_compact_pressure_threshold;      /**< Pressure ratio that triggers     */
    double auto_compact_fragmentation_threshold; /**< Fragmentation trigger ratio  */
    struct timespec last_auto_compact_time;      /**< Last auto-compact timestamp  */

    size_t alloc_latency_histogram[32]; /**< Buckets for allocation-latency samples */
    size_t alloc_latency_count;         /**< Total latency samples                 */
    uint64_t alloc_latency_sum_ns;      /**< Accumulated latency (ns) for avg       */

    size_t arena_quota_limit;               /**< Per-arena byte cap (0=unlimited) */
    mp_watermark_callback_t arena_quota_cb; /**< Quota-exceeded callback          */
    void *arena_quota_user_data;            /**< Quota callback arg               */

    int num_cpus;                                 /**< CPU count for per-CPU freelists   */
    mp_percpu_freelist_entry_t *percpu_freelists; /**< Per-CPU free lists (lock-free)    */

    mp_watermark_callback_t gc_cb;       /**< GC callback invoked before OOM rejection   */
    void *gc_user_data;                  /**< GC callback arg                            */
    mp_watermark_callback_t eviction_cb; /**< Eviction callback for freeing lazily       */
    void *eviction_user_data;            /**< Eviction callback arg              */
    bool fallback_to_sys_alloc_on_oom;   /**< OOM -> sys malloc fallback */

    bool is_dirty;                             /**< Marked dirty after corruption       */
    mp_watermark_callback_t error_recovery_cb; /**< Corruption recovery callback         */
    void *error_recovery_user_data;            /**< Recovery callback arg               */

    size_t thread_quota_bytes;    /**< Per-thread byte cap (0 = disabled)           */
    bool circuit_breaker_enabled; /**< Thread-circuit-breaker feature on/off     */
    bool circuit_breaker_tripped; /**< Set once a thread exceeded its quota       */

    uint32_t abi_version; /**< Pool's ABI version for version-gated behavior */

    bool cgroup_aware;       /**< Tracked against a cgroup memory limit      */
    size_t cgroup_mem_limit; /**< Cgroup capped memory limit (if aware)      */

    bool idle_reclaim_enabled;         /**< Enable idle page reclamation               */
    uint64_t idle_reclaim_timeout_ms;  /**< Idle timeout before reclaim (ms)           */
    size_t idle_reclaim_min_pages;     /**< Min idle pages before reclaim triggers     */
};

/**
 * @brief Lock-free ring-buffer allocator (DPDK-style SPSC ring).
 *
 * head/tail are atomic cursors into a fixed-size slot array; MP_FLAG based
 * SPSC semantics make alloc/free wait-free in the uncontended case.
 */
struct cmem_ring_buffer {
    size_t slot_size;        /**< Bytes per logical slot              */
    size_t capacity;         /**< Number of slots held                */
    size_t mask;             /**< capacity-1 (power-of-two wrap mask) */
    cmem_atomic_size_t head; /**< Atomic write cursor                 */
    cmem_atomic_size_t tail; /**< Atomic read cursor                  */
    void **slots;            /**< Slot payload pointers               */
    void *buffer;            /**< Backing memory for all slots        */
};

/**
 * @brief Structured event log built on a ring buffer.
 *
 * Records alloc/free/corruption events for post-mortem replay
 * (mp_event_log_create / record / consume).
 */
struct mp_event_log {
    cmem_ring_buffer_t *ring; /**< Ring buffer holding event entries   */
    size_t capacity;          /**< Max entries the log can hold     */
    cmem_atomic_size_t count; /**< Entries currently in the log       */
};

/**
 * @brief Dual ping-pong frame arena (game/graphics).
 *
 * Two child pools alternate each frame; when a frame ends the inactive pool
 * is reset in O(1), giving zero-lock frame-scoped batch allocation.
 */
struct cmem_frame_arena {
    memory_pool_t *pool_a;      /**< First backing pool                    */
    memory_pool_t *pool_b;      /**< Second (ping-pong) backing pool        */
    memory_pool_t *active_pool; /**< Pool serving the current frame         */
    size_t frame_index;         /**< Monotonic frame sequence               */
};

/* -------------------------------------------------------------------------
 * Shared data
 * -------------------------------------------------------------------------
 */
extern const size_t kSlabSizes[SLAB_CLASS_COUNT];

/* -------------------------------------------------------------------------
 * Inline helpers (available to all modules)
 *
 * Small, allocation-path helper wrappers shared by every cmem backend.
 * They centralize the thread-safety checks (MP_FLAG_THREAD_SAFE) so the
 * backend .c files never test the flag themselves.
 * -------------------------------------------------------------------------
 */
/** @brief Take the pool's reader lock if thread-safe mode is enabled. */
static inline void pool_rdlock(memory_pool_t *pool)
{
    if (pool && (pool->flags & MP_FLAG_THREAD_SAFE)) {
        pthread_rwlock_rdlock(&pool->rwlock);
    }
}

/** @brief Release the pool's reader lock (no-op in non-thread-safe mode). */
static inline void pool_rdunlock(memory_pool_t *pool)
{
    if (pool && (pool->flags & MP_FLAG_THREAD_SAFE)) {
        pthread_rwlock_unlock(&pool->rwlock);
    }
}

/** @brief Take the pool's writer lock (exclusive) if thread-safe mode is enabled. */
static inline void pool_wrlock(memory_pool_t *pool)
{
    if (pool && (pool->flags & MP_FLAG_THREAD_SAFE)) {
        pthread_rwlock_wrlock(&pool->rwlock);
    }
}

/** @brief Release the pool's writer lock (no-op in non-thread-safe mode). */
static inline void pool_wrunlock(memory_pool_t *pool)
{
    if (pool && (pool->flags & MP_FLAG_THREAD_SAFE)) {
        pthread_rwlock_unlock(&pool->rwlock);
    }
}

/** @brief Generic exclusive-pool lock: same as pool_wrlock(). */
static inline void pool_lock(memory_pool_t *pool)
{
    pool_wrlock(pool);
}

/** @brief Generic exclusive-pool unlock: same as pool_wrunlock(). */
static inline void pool_unlock(memory_pool_t *pool)
{
    pool_wrunlock(pool);
}

/** @brief Fire the user's event callback (if any) with the pool/user context. */
static inline void trigger_event(memory_pool_t *pool, mp_event_type_t ev, void *ptr, size_t size)
{
    if (pool->event_cb) {
        pool->event_cb(pool, ev, ptr, size, pool->event_user_data);
    }
}

/** @brief Return the default bucket index for the user's thread-local cache release. */
static inline uint8_t get_slab_class_index(memory_pool_t *pool, size_t size)
{
    if (pool->use_custom_slab_sizes) {
        for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
            if (size <= pool->custom_slab_sizes[i]) {
                return (uint8_t)i;
            }
        }
        return SLAB_CLASS_COUNT;
    }
    /* Default fixed bucket thresholds: 8,16,32,64,128,256,512 bytes. */
    if (size <= 8) {
        return 0;
    }
    if (size <= 16) {
        return 1;
    }
    if (size <= 32) {
        return 2;
    }
    if (size <= 64) {
        return 3;
    }
    if (size <= 128) {
        return 4;
    }
    if (size <= 256) {
        return 5; // NOLINT(readability-magic-numbers)
    }
    if (size <= 512) {
        return 6; // NOLINT(readability-magic-numbers)
    }
    return SLAB_CLASS_COUNT;
}

/* -------------------------------------------------------------------------
 * Cross-module function declarations
 *
 * Functions implemented in one backend .c file and called from others.
 * Grouped by owning module so the dependency direction stays explicit.
 * -------------------------------------------------------------------------
 */

/* Core utilities (cmem.c) */
extern void active_list_add(memory_pool_t *pool, mp_block_header_t *header);
extern void active_list_remove(memory_pool_t *pool, mp_block_header_t *header);
extern void *mp_alloc_internal(memory_pool_t *pool, size_t size);

/* Slab allocator (cmem_slab.c) */
extern bool slab_init(memory_pool_t *pool);
extern mp_slab_page_t *slab_create_page(memory_pool_t *pool, uint8_t class_idx);
extern void *slab_alloc(memory_pool_t *pool, uint8_t class_idx, size_t req_size);
extern void slab_free(memory_pool_t *pool, mp_block_header_t *header);
extern void tls_cache_refill(memory_pool_t *pool, uint8_t class_idx);
extern void percpu_init(memory_pool_t *pool);
extern void percpu_destroy(memory_pool_t *pool);
extern int percpu_cpu_index(void);
extern mp_slab_slot_t *percpu_pop(memory_pool_t *pool, int cpu, uint8_t class_idx);
extern bool percpu_push(memory_pool_t *pool, int cpu, uint8_t class_idx, mp_slab_slot_t *slot);
extern void percpu_refill(memory_pool_t *pool, int cpu, uint8_t class_idx);

/* TLSF allocator (cmem_tlsf.c) */
extern tlsf_pool_t *tlsf_create_pool_custom(memory_pool_t *pool, size_t size, void *custom_mem);
extern void tlsf_insert_free_block(tlsf_pool_t *tpool, tlsf_block_t *block);
extern void tlsf_remove_free_block(tlsf_pool_t *tpool, tlsf_block_t *block);
extern tlsf_block_t *tlsf_find_suitable_block(tlsf_pool_t *tpool, size_t total_needed);
extern void *tlsf_alloc(memory_pool_t *pool, size_t req_size);
extern void tlsf_free(memory_pool_t *pool, mp_block_header_t *header);
extern bool
tlsf_try_inplace_expand(memory_pool_t *pool, mp_block_header_t *header, size_t new_size);

/* System memory & platform (cmem_sys.c) */
extern int cmem_sched_getcpu(void);
extern int cmem_numa_current_node(void);
extern int cmem_numa_node_count(void);
extern int cmem_cpu_to_node(int cpu);
extern void cmem_munmap(void *ptr, size_t size);
extern void *cmem_aligned_malloc(size_t size, size_t alignment);
extern void cmem_aligned_free(void *ptr);
extern void *sys_mem_alloc(memory_pool_t *pool, size_t size, size_t alignment);
extern void sys_mem_free(memory_pool_t *pool, void *ptr, size_t size);
extern bool mp_expand_pool(memory_pool_t *pool, size_t additional_bytes);
extern bool mp_can_expand(memory_pool_t *pool);
extern size_t mp_get_expandable_size(memory_pool_t *pool);

/* Diagnostics (cmem_diag.c) */
extern bool mp_audit_heap(memory_pool_t *pool);
extern size_t mp_analyze_leaks(memory_pool_t *pool, char *report_buf, size_t max_len);
extern bool mp_export_leak_report(memory_pool_t *pool, const char *filepath);
extern bool mp_export_html_report(memory_pool_t *pool, const char *filepath);
extern bool mp_export_binary_snapshot(memory_pool_t *pool, const char *filepath);
extern bool mp_parse_binary_snapshot(const char *filepath, char *out_report, size_t max_len);
extern bool mp_diff_snapshots(const char *snapshot_a_path,
                              const char *snapshot_b_path,
                              char *out_report,
                              size_t max_len);
extern void mp_get_stats(memory_pool_t *pool, mp_stats_t *stats);
extern void mp_dump_info(memory_pool_t *pool);
extern void mp_dump_histogram(memory_pool_t *pool);
extern void mp_dump_tree_info(memory_pool_t *pool);
extern size_t mp_dump_json_stats(memory_pool_t *pool, char *buf, size_t max_len);
extern size_t mp_export_prometheus_metrics(memory_pool_t *pool, char *out_buf, size_t max_len);
extern size_t mp_export_pprof(memory_pool_t *pool, char *out_buf, size_t max_len);
extern bool mp_check_leaks(memory_pool_t *pool);

/* Events & callbacks (cmem_event.c) */
extern mp_event_log_t *mp_event_log_create(size_t capacity);
extern void mp_event_log_destroy(mp_event_log_t *log);
extern bool
mp_event_log_record(mp_event_log_t *log, mp_event_type_t event_type, void *ptr, size_t size);
extern bool mp_event_log_consume(mp_event_log_t *log, mp_event_log_entry_t *entry);
extern size_t mp_event_log_pending(mp_event_log_t *log);
extern void mp_event_log_clear(mp_event_log_t *log);
extern void
mp_set_event_callback(memory_pool_t *pool, mp_event_callback_t callback, void *user_data);
extern void mp_set_watermark_callback(memory_pool_t *pool,
                                      double high_ratio,
                                      double low_ratio,
                                      mp_watermark_callback_t cb,
                                      void *user_data);
extern void check_watermark_after_change(memory_pool_t *pool);
extern void mp_set_arena_quota(memory_pool_t *pool,
                               size_t quota_bytes,
                               mp_watermark_callback_t cb,
                               void *user_data);
extern bool mp_check_arena_quota(memory_pool_t *pool);
extern void mp_set_thread_quota(memory_pool_t *pool, size_t quota_bytes);
extern size_t mp_get_thread_allocated_bytes(memory_pool_t *pool);
extern void mp_reset_thread_quota(memory_pool_t *pool);
extern bool mp_is_circuit_breaker_tripped(memory_pool_t *pool);
extern void mp_set_circuit_breaker(memory_pool_t *pool, bool enable);
extern void mp_record_latency(memory_pool_t *pool, uint64_t latency_ns);
extern void mp_reset_latency_stats(memory_pool_t *pool);
extern uint64_t mp_get_latency_avg(memory_pool_t *pool);
extern uint64_t mp_get_latency_p99(memory_pool_t *pool);
extern void mp_set_auto_compact(memory_pool_t *pool,
                                bool enable,
                                double pressure_threshold,
                                double fragmentation_threshold);
extern bool mp_auto_compact_check(memory_pool_t *pool);
extern void mp_set_fallback_on_oom(memory_pool_t *pool, bool enable);
extern void mp_set_gc_callback(memory_pool_t *pool, mp_watermark_callback_t cb, void *user_data);
extern void
mp_set_eviction_callback(memory_pool_t *pool, mp_watermark_callback_t cb, void *user_data);
extern bool mp_enable_emergency_reserve(memory_pool_t *pool, size_t reserve_bytes);
extern void mp_mark_pool_dirty(memory_pool_t *pool);
extern void mp_clear_pool_dirty(memory_pool_t *pool);
extern bool mp_is_pool_dirty(memory_pool_t *pool);
extern void
mp_set_error_recovery_callback(memory_pool_t *pool, mp_watermark_callback_t cb, void *user_data);
extern bool mp_isolate_bad_block(memory_pool_t *pool, void *ptr);
extern bool mp_asan_is_enabled(void);
extern void mp_asan_report_error(memory_pool_t *pool, void *ptr, size_t size, bool is_write);
extern bool mp_asan_check_memory(memory_pool_t *pool, void *ptr, size_t size);
extern void mp_set_asan_integration(memory_pool_t *pool, bool enable);
extern void *mp_ring_alloc(cmem_ring_buffer_t *ring);
extern bool mp_ring_free(cmem_ring_buffer_t *ring, void *ptr);
extern void mp_ring_destroy(cmem_ring_buffer_t *ring);
extern mp_flags_t mp_reparse_env_flags(memory_pool_t *pool);
extern uint64_t mp_get_env_generation(memory_pool_t *pool);
extern void *mp_frame_alloc(cmem_frame_arena_t *farena, size_t size);
extern void mp_frame_end(cmem_frame_arena_t *farena);
extern void mp_frame_arena_destroy(cmem_frame_arena_t *farena);

/* -------------------------------------------------------------------------
 * Shared macros
 * -------------------------------------------------------------------------
 */
#define CMEM_MS_PER_SEC 1000           /**< Milliseconds per second                          */
#define CMEM_NSEC_PER_MSEC 1000000     /**< Nanoseconds per millisecond                      */
#define CMEM_MIN(a, b) ((a) < (b) ? (a) : (b)) /**< Type-generic minimum  */
#define CMEM_MAX(a, b) ((a) > (b) ? (a) : (b)) /**< Type-generic maximum  */

static inline int64_t cmem_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * CMEM_MS_PER_SEC + (int64_t)(ts.tv_nsec / CMEM_NSEC_PER_MSEC);
}

#endif /* CMEM_INTERNAL_H */
