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
 * -------------------------------------------------------------------------
 */
#ifdef __cplusplus
#include <atomic>
typedef std::atomic<size_t> cmem_atomic_size_t;
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
#include <sys/mman.h>
#include <unistd.h>

#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
#endif

#define MP_PERCPU_MAX_BATCH 16

typedef struct {
    size_t alloc_bytes;
    size_t alloc_count;
} mp_thread_quota_t;

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

/* -------------------------------------------------------------------------
 * Shared constants
 * -------------------------------------------------------------------------
 */
#define MP_MAGIC_HEAD 0x4D504F4F
#define MP_CANARY_BYTE 0xDE
#define MP_POISON_BYTE 0xDD

#define SLAB_CLASS_COUNT 7
#define SLAB_MAX_SIZE 512
#define SLAB_PAGE_SIZE (64 * 1024)
#define TLS_CACHE_MAX_SLOTS 256
#define MAX_BACKTRACE_FRAMES 8

/* TLSF Allocator Constants */
#define TLSF_SL_SHIFT 4
#define TLSF_SL_COUNT (1 << TLSF_SL_SHIFT)
#define TLSF_FL_MAX 30
#define TLSF_MIN_BLOCK_SIZE 32
#define TLSF_MAX_SIZE (4 * 1024 * 1024)

#define BLOCK_STATE_FREE 0x1
#define BLOCK_STATE_PREV_FREE 0x2
#define BLOCK_SIZE_MASK (~(size_t)(BLOCK_STATE_FREE | BLOCK_STATE_PREV_FREE))

/* -------------------------------------------------------------------------
 * Shared internal types
 * -------------------------------------------------------------------------
 */
typedef struct mp_block_header {
    uint32_t magic;
    uint8_t  alloc_type;
    uint8_t  slab_class;
    uint16_t flags;
    size_t   requested_size;
    size_t   usable_size;
    void    *raw_base;
    void    *subpool;

    const char *alloc_file;
    int         alloc_line;
    const char *alloc_func;
    void       *backtrace_addrs[MAX_BACKTRACE_FRAMES];
    int         backtrace_depth;

    struct mp_block_header *prev;
    struct mp_block_header *next;
} mp_block_header_t;

typedef struct mp_slab_slot {
    struct mp_slab_slot *next;
} mp_slab_slot_t;

typedef struct mp_slab_page {
    uint8_t              class_index;
    uint16_t             free_count;
    uint16_t             total_slots;
    mp_slab_slot_t      *free_list;
    struct mp_slab_page *next;
    struct mp_slab_page *prev;
    void                *page_raw_mem;
    bool                 is_hot;
} mp_slab_page_t;

typedef struct {
    size_t          slot_size;
    pthread_mutex_t lock;
    mp_slab_page_t *partial_pages;
    mp_slab_page_t *full_pages;
    mp_slab_page_t *hot_pages;
    mp_slab_page_t *cold_pages;
} mp_slab_class_t;

typedef struct {
    mp_slab_slot_t *slots[SLAB_CLASS_COUNT];
    uint16_t        counts[SLAB_CLASS_COUNT];
} thread_cache_t;

extern thread_cache_t    tls_cache;
extern mp_thread_quota_t thread_quota;

typedef struct tlsf_block {
    size_t             size_and_flags;
    struct tlsf_block *prev_physical;
    struct tlsf_block *next_free;
    struct tlsf_block *prev_free;
} tlsf_block_t;

typedef struct tlsf_pool {
    uint32_t          fl_bitmap;
    uint32_t          sl_bitmap[TLSF_FL_MAX];
    tlsf_block_t     *blocks[TLSF_FL_MAX][TLSF_SL_COUNT];
    void             *raw_area;
    size_t            raw_size;
    struct tlsf_pool *next;
} tlsf_pool_t;

typedef struct {
    cmem_atomic_size_t head;
    uint16_t           count;
} mp_percpu_freelist_entry_t;

typedef struct mp_typed_pool {
    size_t elem_size;
    size_t capacity;
    size_t active_count;
    void  *free_list;
    void  *raw_buf;
} mp_typed_pool_t;

struct memory_pool {
    mp_flags_t       flags;
    pthread_rwlock_t rwlock;
    pthread_mutex_t  lock;
    char             arena_name[64];

    struct memory_pool *parent;
    struct memory_pool *first_child;
    struct memory_pool *next_sibling;

    bool               has_custom_sys_alloc;
    mp_sys_allocator_t sys_allocator;

    mp_event_callback_t event_cb;
    void               *event_user_data;

    mp_watermark_callback_t watermark_cb;
    double                  high_watermark_ratio;
    double                  low_watermark_ratio;
    bool                    in_high_watermark_state;
    void                   *watermark_user_data;

    int numa_node;

    void  *emergency_buf;
    size_t emergency_size;
    size_t emergency_used;
    bool   in_emergency_state;

    mp_stats_t         stats;
    mp_block_header_t *active_head;
    uint64_t           window_alloc_ops;
    uint64_t           window_alloc_bytes;
    struct timespec    window_start_time;

    mp_slab_class_t slab_classes[SLAB_CLASS_COUNT];
    bool            use_custom_slab_sizes;
    size_t          custom_slab_sizes[SLAB_CLASS_COUNT];

    tlsf_pool_t *tlsf_root;

    uint64_t env_flags_generation;

    bool            auto_compact_enabled;
    double          auto_compact_pressure_threshold;
    double          auto_compact_fragmentation_threshold;
    struct timespec last_auto_compact_time;

    size_t   alloc_latency_histogram[32];
    size_t   alloc_latency_count;
    uint64_t alloc_latency_sum_ns;

    size_t                  arena_quota_limit;
    mp_watermark_callback_t arena_quota_cb;
    void                   *arena_quota_user_data;

    int                         num_cpus;
    mp_percpu_freelist_entry_t *percpu_freelists;

    mp_watermark_callback_t gc_cb;
    void                   *gc_user_data;
    mp_watermark_callback_t eviction_cb;
    void                   *eviction_user_data;
    bool                    fallback_to_sys_alloc_on_oom;

    bool                    is_dirty;
    mp_watermark_callback_t error_recovery_cb;
    void                   *error_recovery_user_data;

    size_t thread_quota_bytes;
    bool   circuit_breaker_enabled;
    bool   circuit_breaker_tripped;

    uint32_t abi_version;

    bool   cgroup_aware;
    size_t cgroup_mem_limit;
};

struct cmem_ring_buffer {
    size_t             slot_size;
    size_t             capacity;
    size_t             mask;
    cmem_atomic_size_t head;
    cmem_atomic_size_t tail;
    void             **slots;
    void              *buffer;
};

struct mp_event_log {
    cmem_ring_buffer_t *ring;
    size_t              capacity;
    cmem_atomic_size_t  count;
};

struct cmem_frame_arena {
    memory_pool_t *pool_a;
    memory_pool_t *pool_b;
    memory_pool_t *active_pool;
    size_t         frame_index;
};

/* -------------------------------------------------------------------------
 * Shared data
 * -------------------------------------------------------------------------
 */
extern const size_t kSlabSizes[SLAB_CLASS_COUNT];

/* -------------------------------------------------------------------------
 * Inline helpers (available to all modules)
 * -------------------------------------------------------------------------
 */
static inline void pool_rdlock(memory_pool_t *pool)
{
    if (pool && (pool->flags & MP_FLAG_THREAD_SAFE)) {
        pthread_rwlock_rdlock(&pool->rwlock);
    }
}

static inline void pool_rdunlock(memory_pool_t *pool)
{
    if (pool && (pool->flags & MP_FLAG_THREAD_SAFE)) {
        pthread_rwlock_unlock(&pool->rwlock);
    }
}

static inline void pool_wrlock(memory_pool_t *pool)
{
    if (pool && (pool->flags & MP_FLAG_THREAD_SAFE)) {
        pthread_rwlock_wrlock(&pool->rwlock);
    }
}

static inline void pool_wrunlock(memory_pool_t *pool)
{
    if (pool && (pool->flags & MP_FLAG_THREAD_SAFE)) {
        pthread_rwlock_unlock(&pool->rwlock);
    }
}

static inline void pool_lock(memory_pool_t *pool)
{
    pool_wrlock(pool);
}

static inline void pool_unlock(memory_pool_t *pool)
{
    pool_wrunlock(pool);
}

static inline void trigger_event(memory_pool_t *pool, mp_event_type_t ev, void *ptr, size_t size)
{
    if (pool->event_cb) {
        pool->event_cb(pool, ev, ptr, size, pool->event_user_data);
    }
}

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
        return 5;
    }
    if (size <= 512) {
        return 6;
    }
    return SLAB_CLASS_COUNT;
}

/* -------------------------------------------------------------------------
 * Cross-module function declarations
 * -------------------------------------------------------------------------
 */

/* Core utilities (cmem.c) */
extern void  active_list_add(memory_pool_t *pool, mp_block_header_t *header);
extern void  active_list_remove(memory_pool_t *pool, mp_block_header_t *header);
extern void *mp_alloc_internal(memory_pool_t *pool, size_t size);

/* Slab allocator (cmem_slab.c) */
extern bool            slab_init(memory_pool_t *pool);
extern mp_slab_page_t *slab_create_page(memory_pool_t *pool, uint8_t class_idx);
extern void           *slab_alloc(memory_pool_t *pool, uint8_t class_idx, size_t req_size);
extern void            slab_free(memory_pool_t *pool, mp_block_header_t *header);
extern void            tls_cache_refill(memory_pool_t *pool, uint8_t class_idx);
extern void            percpu_init(memory_pool_t *pool);
extern void            percpu_destroy(memory_pool_t *pool);
extern int             percpu_cpu_index(void);
extern mp_slab_slot_t *percpu_pop(memory_pool_t *pool, int cpu, uint8_t class_idx);
extern bool percpu_push(memory_pool_t *pool, int cpu, uint8_t class_idx, mp_slab_slot_t *slot);
extern void percpu_refill(memory_pool_t *pool, int cpu, uint8_t class_idx);

/* TLSF allocator (cmem_tlsf.c) */
extern tlsf_pool_t  *tlsf_create_pool_custom(memory_pool_t *pool, size_t size, void *custom_mem);
extern void          tlsf_insert_free_block(tlsf_pool_t *tpool, tlsf_block_t *block);
extern void          tlsf_remove_free_block(tlsf_pool_t *tpool, tlsf_block_t *block);
extern tlsf_block_t *tlsf_find_suitable_block(tlsf_pool_t *tpool, size_t total_needed);
extern void         *tlsf_alloc(memory_pool_t *pool, size_t req_size);
extern void          tlsf_free(memory_pool_t *pool, mp_block_header_t *header);
extern bool
tlsf_try_inplace_expand(memory_pool_t *pool, mp_block_header_t *header, size_t new_size);

/* System memory & platform (cmem_sys.c) */
extern int    cmem_sched_getcpu(void);
extern void   cmem_munmap(void *ptr, size_t size);
extern void  *cmem_aligned_malloc(size_t size, size_t alignment);
extern void   cmem_aligned_free(void *ptr);
extern void  *sys_mem_alloc(memory_pool_t *pool, size_t size, size_t alignment);
extern void   sys_mem_free(memory_pool_t *pool, void *ptr, size_t size);
extern bool   mp_expand_pool(memory_pool_t *pool, size_t additional_bytes);
extern bool   mp_can_expand(memory_pool_t *pool);
extern size_t mp_get_expandable_size(memory_pool_t *pool);

/* Diagnostics (cmem_diag.c) */
extern bool   mp_audit_heap(memory_pool_t *pool);
extern size_t mp_analyze_leaks(memory_pool_t *pool, char *report_buf, size_t max_len);
extern bool   mp_export_leak_report(memory_pool_t *pool, const char *filepath);
extern bool   mp_export_html_report(memory_pool_t *pool, const char *filepath);
extern bool   mp_export_binary_snapshot(memory_pool_t *pool, const char *filepath);
extern bool   mp_parse_binary_snapshot(const char *filepath, char *out_report, size_t max_len);
extern bool   mp_diff_snapshots(const char *snapshot_a_path,
                                const char *snapshot_b_path,
                                char       *out_report,
                                size_t      max_len);
extern void   mp_get_stats(memory_pool_t *pool, mp_stats_t *stats);
extern void   mp_dump_info(memory_pool_t *pool);
extern void   mp_dump_histogram(memory_pool_t *pool);
extern void   mp_dump_tree_info(memory_pool_t *pool);
extern size_t mp_dump_json_stats(memory_pool_t *pool, char *buf, size_t max_len);
extern size_t mp_export_prometheus_metrics(memory_pool_t *pool, char *out_buf, size_t max_len);
extern size_t mp_export_pprof(memory_pool_t *pool, char *out_buf, size_t max_len);
extern bool   mp_check_leaks(memory_pool_t *pool);

/* Events & callbacks (cmem_event.c) */
extern mp_event_log_t *mp_event_log_create(size_t capacity);
extern void            mp_event_log_destroy(mp_event_log_t *log);
extern bool
mp_event_log_record(mp_event_log_t *log, mp_event_type_t event_type, void *ptr, size_t size);
extern bool   mp_event_log_consume(mp_event_log_t *log, mp_event_log_entry_t *entry);
extern size_t mp_event_log_pending(mp_event_log_t *log);
extern void   mp_event_log_clear(mp_event_log_t *log);
extern void
mp_set_event_callback(memory_pool_t *pool, mp_event_callback_t callback, void *user_data);
extern void     mp_set_watermark_callback(memory_pool_t          *pool,
                                          double                  high_ratio,
                                          double                  low_ratio,
                                          mp_watermark_callback_t cb,
                                          void                   *user_data);
extern void     check_watermark_after_change(memory_pool_t *pool);
extern void     mp_set_arena_quota(memory_pool_t          *pool,
                                   size_t                  quota_bytes,
                                   mp_watermark_callback_t cb,
                                   void                   *user_data);
extern bool     mp_check_arena_quota(memory_pool_t *pool);
extern void     mp_set_thread_quota(memory_pool_t *pool, size_t quota_bytes);
extern size_t   mp_get_thread_allocated_bytes(memory_pool_t *pool);
extern void     mp_reset_thread_quota(memory_pool_t *pool);
extern bool     mp_is_circuit_breaker_tripped(memory_pool_t *pool);
extern void     mp_set_circuit_breaker(memory_pool_t *pool, bool enable);
extern void     mp_record_latency(memory_pool_t *pool, uint64_t latency_ns);
extern void     mp_reset_latency_stats(memory_pool_t *pool);
extern uint64_t mp_get_latency_avg(memory_pool_t *pool);
extern uint64_t mp_get_latency_p99(memory_pool_t *pool);
extern void     mp_set_auto_compact(memory_pool_t *pool,
                                    bool           enable,
                                    double         pressure_threshold,
                                    double         fragmentation_threshold);
extern bool     mp_auto_compact_check(memory_pool_t *pool);
extern void     mp_set_fallback_on_oom(memory_pool_t *pool, bool enable);
extern void mp_set_gc_callback(memory_pool_t *pool, mp_watermark_callback_t cb, void *user_data);
extern void
mp_set_eviction_callback(memory_pool_t *pool, mp_watermark_callback_t cb, void *user_data);
extern bool mp_enable_emergency_reserve(memory_pool_t *pool, size_t reserve_bytes);
extern void mp_mark_pool_dirty(memory_pool_t *pool);
extern void mp_clear_pool_dirty(memory_pool_t *pool);
extern bool mp_is_pool_dirty(memory_pool_t *pool);
extern void
mp_set_error_recovery_callback(memory_pool_t *pool, mp_watermark_callback_t cb, void *user_data);
extern bool       mp_isolate_bad_block(memory_pool_t *pool, void *ptr);
extern bool       mp_asan_is_enabled(void);
extern void       mp_asan_report_error(memory_pool_t *pool, void *ptr, size_t size, bool is_write);
extern bool       mp_asan_check_memory(memory_pool_t *pool, void *ptr, size_t size);
extern void       mp_set_asan_integration(memory_pool_t *pool, bool enable);
extern void      *mp_ring_alloc(cmem_ring_buffer_t *ring);
extern bool       mp_ring_free(cmem_ring_buffer_t *ring, void *ptr);
extern void       mp_ring_destroy(cmem_ring_buffer_t *ring);
extern mp_flags_t mp_reparse_env_flags(memory_pool_t *pool);
extern uint64_t   mp_get_env_generation(memory_pool_t *pool);
extern void      *mp_frame_alloc(cmem_frame_arena_t *farena, size_t size);
extern void       mp_frame_end(cmem_frame_arena_t *farena);
extern void       mp_frame_arena_destroy(cmem_frame_arena_t *farena);

/* -------------------------------------------------------------------------
 * Shared macros
 * -------------------------------------------------------------------------
 */
#define CMEM_MIN(a, b) ((a) < (b) ? (a) : (b))
#define CMEM_MAX(a, b) ((a) > (b) ? (a) : (b))

#endif /* CMEM_INTERNAL_H */
