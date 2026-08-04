/**
 * @file cmem.c
 * @brief cmem - Universal Tiered Memory Manager Implementation (Slab + TLSF + OS + Child Arenas +
 * Diagnostics).
 */

#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include "cmem.h"
#include "cmem_internal.h"
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
#else
#if defined(__has_include)
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define CMEM_HAS_EXECINFO 1
#endif
#else
#include <execinfo.h>
#define CMEM_HAS_EXECINFO 1
#endif
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define MP_MAGIC_HEAD 0x4D504F4F // "MPOO" in ASCII
#define MP_CANARY_BYTE 0xDE
#define MP_POISON_BYTE 0xDD

#define SLAB_CLASS_COUNT 7
#define SLAB_MAX_SIZE 512
#define SLAB_PAGE_SIZE (64 * 1024) // 64 KB per slab page
#define TLS_CACHE_MAX_SLOTS 256
#define MAX_BACKTRACE_FRAMES 8

/* TLSF Allocator Constants */
#define TLSF_SL_SHIFT 4
#define TLSF_SL_COUNT (1 << TLSF_SL_SHIFT) // 16 subdivisions per FL
#define TLSF_FL_MAX 30                     // Up to 1GB
#define TLSF_MIN_BLOCK_SIZE 32
#define TLSF_MAX_SIZE (4 * 1024 * 1024) // 4 MB threshold for TLSF vs Direct OS

#define BLOCK_STATE_FREE 0x1
#define BLOCK_STATE_PREV_FREE 0x2
#define BLOCK_SIZE_MASK (~(size_t)(BLOCK_STATE_FREE | BLOCK_STATE_PREV_FREE))

/* Header prepended to every user payload */

/* --- Slab Structs --- */

/* --- TLS Cache Struct for Lock-Free Small Allocations --- */

#ifdef __cplusplus
#define MP_THREAD_LOCAL thread_local
#else
#define MP_THREAD_LOCAL _Thread_local
#endif

/* --- TLSF Structs --- */

/* --- Per-CPU Lock-Free Freelist Entry --- */

/* --- Main Memory Pool Struct --- */

/* Lock Utilities */

/**
 * @brief Acquires a read lock on the memory pool for thread-safe concurrent reads.
 * @param pool Pointer to the memory pool
 */

/**
 * @brief Releases the read lock on the memory pool.
 * @param pool Pointer to the memory pool
 */

/**
 * @brief Acquires a write lock on the memory pool for exclusive access.
 * @param pool Pointer to the memory pool
 */

/**
 * @brief Releases the write lock on the memory pool.
 * @param pool Pointer to the memory pool
 */

/**
 * @brief Acquires the write lock (alias for pool_wrlock).
 * @param pool Pointer to the memory pool
 */

/**
 * @brief Releases the write lock (alias for pool_wrunlock).
 * @param pool Pointer to the memory pool
 */

/* Event Profiling Dispatcher */
/**
 * @brief Dispatches a profiling/debug event to the registered callback if present.
 * @param pool Pointer to the memory pool
 * @param ev Event type (alloc, free, realloc, etc.)
 * @param ptr Pointer involved in the event
 * @param size Size of the allocation
 */

/* Backing Memory Allocator Helpers */

/* Bitwise Utilities for TLSF */

/* --- Slab Allocator Implementation --- */

/* --- Per-CPU Lock-Free Freelist Implementation --- */
#include <sched.h>

#define MP_PERCPU_MAX_BATCH 16

/**
 * @brief Lock-free push to per-CPU freelist for a given slab class.
 * @param pool Pointer to the memory pool
 * @param cpu CPU index
 * @param class_idx Slab size class index
 * @param slot Pointer to slot to push
 * @return true if pushed, false if freelist is full (caller should use normal free path)
 */

/* ========================================================================== */
/*  Per-CPU Lock-Free Freelist Public API                                       */
/* ========================================================================== */

/* ========================================================================== */
/*  Online Pool Expansion                                                     */
/* ========================================================================== */

/* ========================================================================== */
/*  Hot/Cold Page Separation                                                   */
/* ========================================================================== */

/* --- TLSF Implementation --- */

/* ========================================================================== */
/*  Memory Error Recovery Public API                                           */
/* ========================================================================== */

/**
 * @brief Registers a callback for memory error recovery.
 * @param pool Pointer to the memory pool
 * @param cb Error recovery callback function pointer
 * @param user_data Optional user data passed to the callback
 */
void mp_set_error_recovery_callback(memory_pool_t *pool,
                                    mp_watermark_callback_t cb,
                                    void *user_data)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    pool->error_recovery_cb = cb;
    pool->error_recovery_user_data = user_data;
    pool_unlock(pool);
}

/**
 * @brief Isolates a bad memory block by marking it as freed and removing it from active tracking.
 * @param pool Pointer to the memory pool
 * @param ptr Pointer to the bad block payload
 * @return true if block was isolated, false if invalid
 */

/* ========================================================================== */
/*  Thread-Level Quota & Circuit Breaker Public API                            */
/* ========================================================================== */

/* ========================================================================== */
/*  ABI Versioning & Container cgroup Awareness Public API                     */
/* ========================================================================== */

/* ========================================================================== */
/*  Encrypted Memory Support                                                   */
/* ========================================================================== */

/* ========================================================================== */
/*  AddressSanitizer Integration Layer                                         */
/* ========================================================================== */
#ifdef __has_attribute
#if __has_attribute(no_sanitize)
#define MP_ASAN_NO_SANITIZE __attribute__((no_sanitize("address")))
#else
#define MP_ASAN_NO_SANITIZE
#endif
#else
#define MP_ASAN_NO_SANITIZE
#endif

/* --- TLSF Implementation --- */

/* --- Lock-Free Ring Buffer Allocator Implementation --- */
/**
 * @brief Ring buffer allocator structure (DPDK-style single-producer single-consumer).
 */

/* --- Structured Event Log Ring Buffer Implementation --- */
/**
 * @brief Structured event log structure with embedded lock-free ring buffer.
 */

/* --- 0-Overhead Typed Object Pool Implementation --- */
/**
 * @brief Typed object pool structure for fixed-size object allocation.
 */

/* --- Public API Implementation --- */

/**
 * @brief Creates a child memory pool linked to a parent pool for hierarchical arena management.
 * @param parent Pointer to the parent memory pool (can be NULL)
 * @param initial_capacity Initial capacity for the child pool
 * @param flags Configuration flags
 * @param arena_name Human-readable name for the child arena
 * @return Pointer to the new child memory pool, or NULL on failure
 */
memory_pool_t *mp_create_child(memory_pool_t *parent,
                               size_t initial_capacity,
                               mp_flags_t flags,
                               const char *arena_name)
{
    memory_pool_t *child =
        mp_create_custom(initial_capacity, flags, parent ? &parent->sys_allocator : NULL);
    if (!child) {
        return NULL;
    }

    if (parent && parent->has_custom_sys_alloc) {
        child->has_custom_sys_alloc = true;
        child->sys_allocator = parent->sys_allocator;
    }

    child->parent = parent;
    if (arena_name) {
        (void)snprintf(child->arena_name, sizeof(child->arena_name), "%s", arena_name);
    } else {
        (void)snprintf(child->arena_name, sizeof(child->arena_name), "ChildArena");
    }

    if (parent) {
        pool_lock(parent);
        child->next_sibling = parent->first_child;
        parent->first_child = child;
        pool_unlock(parent);
    }
    return child;
}

/**
 * @brief Creates a memory pool with a custom backing allocator vtable.
 * @param initial_capacity Initial memory capacity in bytes
 * @param flags Configuration flags
 * @param sys_allocator Custom system allocator function table, or NULL for default
 * @return Pointer to the new memory pool, or NULL on failure
 */
memory_pool_t *
mp_create_custom(size_t initial_capacity, // NOLINT(bugprone-easily-swappable-parameters)
                 mp_flags_t flags,
                 const mp_sys_allocator_t *sys_allocator)
{
    flags = mp_parse_env_flags(flags);

    memory_pool_t *pool = (memory_pool_t *)calloc(1, sizeof(memory_pool_t));
    if (!pool) {
        return NULL;
    }

    pool->flags = flags;
    (void)snprintf(pool->arena_name, sizeof(pool->arena_name), "RootArena");
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

/* ========================================================================== */
/*  Runtime Config Hot-Reload                                                  */
/* ========================================================================== */

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
void mp_set_auto_compact(memory_pool_t *pool,

                         bool enable,
                         double pressure_threshold, // NOLINT(bugprone-easily-swappable-parameters)

                         double fragmentation_threshold)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    pool->auto_compact_enabled = enable;
    pool->auto_compact_pressure_threshold = pressure_threshold;
    pool->auto_compact_fragmentation_threshold = fragmentation_threshold;
    pool_unlock(pool);
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
void mp_set_arena_quota(memory_pool_t *pool,
                        size_t quota_bytes,
                        mp_watermark_callback_t cb,
                        void *user_data)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    pool->arena_quota_limit = quota_bytes;
    pool->arena_quota_cb = cb;
    pool->arena_quota_user_data = user_data;
    pool_unlock(pool);
}

/* ========================================================================== */
/*  Allocation Latency Statistics                                              */
/* ========================================================================== */

/**
 * @brief Configures high and low watermark threshold alert callbacks.
 * @param pool Pointer to the memory pool
 * @param high_ratio High watermark ratio (0.0-1.0) that triggers the callback
 * @param low_ratio Low watermark ratio (0.0-1.0) that clears the high state
 * @param cb Watermark callback function pointer
 * @param user_data Optional user data passed to the callback
 */
void mp_set_watermark_callback(memory_pool_t *pool,

                               double high_ratio, // NOLINT(bugprone-easily-swappable-parameters)

                               double low_ratio,
                               mp_watermark_callback_t cb,
                               void *user_data)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    pool->high_watermark_ratio = high_ratio;
    pool->low_watermark_ratio = low_ratio;
    pool->watermark_cb = cb;
    pool->watermark_user_data = user_data;
    pool->in_high_watermark_state = false;
    pool_unlock(pool);
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
void *mp_calloc_loc(
    memory_pool_t *pool, size_t num, size_t size, const char *file, int line, const char *func)
{
    void *ptr = mp_calloc(pool, num, size);
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
 * @brief Reallocates memory block with source location tracking.
 * @param pool Pointer to the memory pool
 * @param ptr Existing allocation pointer (or NULL for new allocation)
 * @param new_size New requested size in bytes
 * @param file Source file name
 * @param line Source line number
 * @param func Source function name
 * @return Pointer to the reallocated payload, or NULL on failure
 */
void *mp_realloc_loc(
    memory_pool_t *pool, void *ptr, size_t new_size, const char *file, int line, const char *func)
{
    void *new_ptr = mp_realloc(pool, ptr, new_size);
    if (new_ptr) {
        mp_block_header_t *header =
            (mp_block_header_t *)((uint8_t *)new_ptr - sizeof(mp_block_header_t));
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
void *mp_reallocarray_loc(memory_pool_t *pool,
                          void *ptr,
                          size_t nmemb,
                          size_t size,
                          const char *file,
                          int line,
                          const char *func)
{
    if (nmemb != 0 && size > SIZE_MAX / nmemb) {
        return NULL;
    }
    return mp_realloc_loc(pool, ptr, nmemb * size, file, line, func);
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
char *mp_strdup_loc(memory_pool_t *pool,
                    const char *str, // NOLINT(bugprone-easily-swappable-parameters)
                    const char *file,
                    int line,
                    const char *func)
{
    if (!str) {
        return NULL;
    }
    size_t len = strlen(str);
    char *dup = (char *)mp_alloc_loc(pool, len + 1, file, line, func);
    if (dup) {
        memcpy(dup, str, len + 1);
    }
    return dup;
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
void *mp_memdup_loc(
    memory_pool_t *pool, const void *src, size_t n, const char *file, int line, const char *func)
{
    if (!src || n == 0) {
        return NULL;
    }
    void *dup = mp_alloc_loc(pool, n, file, line, func);
    if (dup) {
        memcpy(dup, src, n);
    }
    return dup;
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
char *mp_asprintf_loc(memory_pool_t *pool,
                      const char *file,
                      int line,
                      const char *func, // NOLINT(bugprone-easily-swappable-parameters)
                      const char *fmt,
                      ...)
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

    char *buf = (char *)mp_alloc_loc(pool, (size_t)len + 1, file, line, func);
    if (buf) {
        (void)vsnprintf(buf, (size_t)len + 1, fmt, args_copy);
    }
    va_end(args_copy);
    return buf;
}

/* --- Heap Integrity, Leak Analysis & HTML Dashboard --- */

/**
 * @brief Compares two binary snapshot files and generates an incremental leak diff report.
 * @param snapshot_a_path Path to the baseline snapshot
 * @param snapshot_b_path Path to the target snapshot
 * @param out_report Output buffer for the diff report
 * @param max_len Maximum length of the output buffer
 * @return true on success
 */
bool mp_diff_snapshots(const char *snapshot_a_path,
                       const char *snapshot_b_path,
                       char *out_report,
                       size_t max_len)
{
    if (!snapshot_a_path || !snapshot_b_path || !out_report || max_len == 0) {
        return false;
    }

    FILE *fa = fopen(snapshot_a_path, "rb");
    FILE *fb = fopen(snapshot_b_path, "rb");
    if (!fa || !fb) {
        if (fa) {
            (void)fclose(fa);
        }
        if (fb) {
            (void)fclose(fb);
        }
        return false;
    }

    cmem_snapshot_header_t hdra, hdrb;
    if (fread(&hdra, sizeof(hdra), 1, fa) != 1 || hdra.magic != CMEM_SNAPSHOT_MAGIC ||
        fread(&hdrb, sizeof(hdrb), 1, fb) != 1 || hdrb.magic != CMEM_SNAPSHOT_MAGIC) {
        (void)fclose(fa);
        (void)fclose(fb);
        return false;
    }

    size_t count_a = (size_t)hdra.active_allocations;
    cmem_snapshot_record_t *recs_a = NULL;
    if (count_a > CMEM_MAX_SNAPSHOT_RECORDS) {
        count_a = 0;
    }

    if (count_a > 0) {
        // NOLINTNEXTLINE(clang-analyzer-optin.taint.TaintedAlloc)
        recs_a = (cmem_snapshot_record_t *)calloc(count_a, sizeof(cmem_snapshot_record_t));
        if (recs_a) {
            if (fread(recs_a, sizeof(cmem_snapshot_record_t), count_a, fa) != count_a) {
                free(recs_a);
                recs_a = NULL;
                count_a = 0;
            }
        }
    }
    (void)fclose(fa);

    size_t offset = 0;
    size_t diff_count = 0;
    size_t diff_bytes = 0;

    offset += (size_t)snprintf(
        out_report + offset,
        max_len - offset,
        "=================== CMEM INCREMENTAL SNAPSHOT DIFF REPORT ===================\n"
        "  Baseline Snapshot A : %s (%u blocks)\n"
        "  Target Snapshot B   : %s (%u blocks)\n"
        "=============================================================================\n",
        snapshot_a_path,
        (unsigned)hdra.active_allocations,
        snapshot_b_path,
        (unsigned)hdrb.active_allocations);

    cmem_snapshot_record_t recb;
    while (fread(&recb, sizeof(recb), 1, fb) == 1) {
        bool found_in_a = false;
        for (size_t i = 0; i < count_a; i++) {
            if (recs_a && recs_a[i].address == recb.address &&
                recs_a[i].requested_size == recb.requested_size) {
                found_in_a = true;
                break;
            }
        }
        if (!found_in_a) {
            diff_count++;
            diff_bytes += (size_t)recb.requested_size;
            if (offset < max_len) {
                const char *tier_str =
                    (recb.alloc_type == ALLOC_TYPE_SLAB)
                        ? "SLAB"
                        : ((recb.alloc_type == ALLOC_TYPE_TLSF) ? "TLSF" : "DIRECT OS");
                offset +=
                    (size_t)snprintf(out_report + offset,
                                     max_len - offset,
                                     "[Incremental Leak #%zu] Addr: 0x%" PRIx64 " | Size: %" PRIu64
                                     " B | Tier: %s | Location: %s:%u (%s)\n",
                                     diff_count,
                                     recb.address,
                                     recb.requested_size,
                                     tier_str,
                                     recb.alloc_file[0] ? recb.alloc_file : "unknown",
                                     recb.alloc_line,
                                     recb.alloc_func[0] ? recb.alloc_func : "unknown");
            }
        }
    }

    (void)fclose(fb);
    if (recs_a) {
        free(recs_a);
    }

    if (offset < max_len) {
        (void)snprintf(
            out_report + offset,
            max_len - offset,
            "-----------------------------------------------------------------------------\n"
            "  Net Incremental Leaked Allocations : %zu blocks\n"
            "  Net Incremental Leaked Bytes       : %zu bytes\n"
            "=============================================================================\n",
            diff_count,
            diff_bytes);
    }

    return true;
}

/* --- Game & Graphics Pipeline Dual Ping-Pong Frame Arena --- */
/**
 * @brief Frame arena structure for double-buffered per-frame allocations.
 */
