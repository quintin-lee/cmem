/**
 * @file memory_pool.h
 * @brief High-Performance Universal Tiered Memory Manager in C.
 * 
 * Features:
 *  - Tiered allocation: Slab (Small), TLSF (Medium), Direct OS (Large)
 *  - O(1) Allocation and Free performance
 *  - Thread-Local Caching (Lock-Free fast path for small objects)
 *  - Arena Fast Reset (O(1) batch deallocation for request-scoped lifetime)
 *  - Static Buffer Mode (Zero OS malloc dependency for embedded / bare-metal)
 *  - Custom Backing Allocator injection (Shared Memory, HugePages)
 *  - Real-time Profiling & Event Callback Hooks
 *  - Advanced Memory Diagnostics:
 *      * Leak source tracking (File, Line, Function & Backtrace)
 *      * Heap integrity auditing (Canary redzone verification)
 *      * Memory poisoning (Use-After-Free protection)
 *      * Structured leak analysis report export
 */

#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Configuration flags for memory pool behavior.
 */
typedef enum {
    MP_FLAG_DEFAULT            = 0,
    MP_FLAG_THREAD_SAFE        = (1 << 0), /**< Enable thread safety via mutex locks */
    MP_FLAG_DEBUG_CANARY       = (1 << 1), /**< Add magic canary bytes for buffer overflow checks */
    MP_FLAG_ZERO_ON_ALLOC      = (1 << 2), /**< Automatically zero memory upon allocation */
    MP_FLAG_THREAD_LOCAL_CACHE = (1 << 3), /**< Enable thread-local cache for lock-free small allocs */
    MP_FLAG_STATIC_BUFFER      = (1 << 4), /**< Static buffer mode (no OS memory allocation/free) */
    MP_FLAG_TRACK_LOCATIONS    = (1 << 5), /**< Record file, line, function & backtrace for allocs */
    MP_FLAG_POISON_ON_FREE     = (1 << 6)  /**< Poison freed memory with 0xDD byte pattern (UAF protection) */
} mp_flags_t;

/**
 * Profiling & Debug Event Types.
 */
typedef enum {
    MP_EVENT_ALLOC = 1,
    MP_EVENT_FREE,
    MP_EVENT_REALLOC,
    MP_EVENT_CANARY_CORRUPTION,
    MP_EVENT_DOUBLE_FREE,
    MP_EVENT_RESET
} mp_event_type_t;

typedef struct memory_pool memory_pool_t;

/**
 * Event Callback function pointer for telemetry profiling.
 */
typedef void (*mp_event_callback_t)(memory_pool_t* pool, mp_event_type_t event, void* ptr, size_t size, void* user_data);

/**
 * Custom Backing Allocator function table for system memory injection.
 */
typedef struct {
    void* (*sys_alloc)(size_t size, void* user_data);
    void  (*sys_free)(void* ptr, size_t size, void* user_data);
    void* user_data;
} mp_sys_allocator_t;

/**
 * Statistics snapshot of the memory pool.
 */
typedef struct {
    size_t total_pool_size;       /**< Total bytes reserved/allocated from system */
    size_t active_bytes;          /**< Total active payload bytes requested by user */
    size_t peak_bytes;            /**< Peak active payload bytes */
    size_t active_allocations;    /**< Count of currently outstanding allocations */
    size_t total_alloc_ops;       /**< Cumulative allocation count */
    size_t total_free_ops;        /**< Cumulative free count */
    size_t slab_allocated_bytes;  /**< Payload bytes in small-object Slab allocator */
    size_t tlsf_allocated_bytes;  /**< Payload bytes in medium-object TLSF allocator */
    size_t os_allocated_bytes;    /**< Payload bytes in direct OS fallback allocator */
    double fragmentation_ratio;   /**< Estimated memory fragmentation ratio (0.0 to 1.0) */
} mp_stats_t;

/**
 * @brief Creates a new memory pool instance using default OS memory.
 * @param initial_capacity Initial memory pool capacity in bytes (0 for default 4MB).
 * @param flags Bitwise OR of mp_flags_t.
 * @return Pointer to created memory_pool_t, or NULL on failure.
 */
memory_pool_t* mp_create(size_t initial_capacity, mp_flags_t flags);

/**
 * @brief Creates a memory pool instance using a custom backing allocator (e.g. Shared Memory / HugePages).
 * @param initial_capacity Initial capacity in bytes.
 * @param flags Bitwise OR of mp_flags_t.
 * @param sys_allocator Custom backing allocator vtable.
 * @return Pointer to created memory_pool_t, or NULL on failure.
 */
memory_pool_t* mp_create_custom(size_t initial_capacity, mp_flags_t flags, const mp_sys_allocator_t* sys_allocator);

/**
 * @brief Creates a memory pool inside a pre-allocated static buffer (Zero OS malloc dependency).
 * @param buffer Pointer to contiguous static memory buffer.
 * @param buffer_size Size of the buffer in bytes.
 * @param flags Bitwise OR of mp_flags_t.
 * @return Pointer to created memory_pool_t inside the buffer, or NULL on failure.
 */
memory_pool_t* mp_create_from_buffer(void* buffer, size_t buffer_size, mp_flags_t flags);

/**
 * @brief Destroys the memory pool and frees all associated system resources.
 * @param pool Memory pool pointer.
 */
void mp_destroy(memory_pool_t* pool);

/**
 * @brief Resets the memory pool, instantly clearing all active allocations while preserving reserved memory blocks.
 * @param pool Memory pool pointer.
 */
void mp_reset(memory_pool_t* pool);

/**
 * @brief Registers an event callback function for real-time profiling and debugging.
 * @param pool Memory pool pointer.
 * @param callback Callback function pointer.
 * @param user_data User context pointer passed to callback.
 */
void mp_set_event_callback(memory_pool_t* pool, mp_event_callback_t callback, void* user_data);

/**
 * @brief Allocates memory block with location tracking.
 */
void* mp_alloc_loc(memory_pool_t* pool, size_t size, const char* file, int line, const char* func);

/**
 * @brief Allocates zeroed memory block with location tracking.
 */
void* mp_calloc_loc(memory_pool_t* pool, size_t num, size_t size, const char* file, int line, const char* func);

/**
 * @brief Reallocates memory block with location tracking.
 */
void* mp_realloc_loc(memory_pool_t* pool, void* ptr, size_t new_size, const char* file, int line, const char* func);

/**
 * @brief Standard allocation wrappers.
 */
void* mp_alloc(memory_pool_t* pool, size_t size);
void* mp_calloc(memory_pool_t* pool, size_t num, size_t size);
void* mp_realloc(memory_pool_t* pool, void* ptr, size_t new_size);
void* mp_aligned_alloc(memory_pool_t* pool, size_t alignment, size_t size);
void mp_free(memory_pool_t* pool, void* ptr);

/**
 * @brief Audits heap integrity by checking header magics and canary redzones of all active allocations.
 * @param pool Memory pool pointer.
 * @return True if heap is healthy, False if buffer overflow or corruption detected.
 */
bool mp_audit_heap(memory_pool_t* pool);

/**
 * @brief Generates a detailed memory leak analysis report including file, line, function and backtrace for each leak.
 * @param pool Memory pool pointer.
 * @param report_buf Output character buffer.
 * @param max_len Maximum buffer length.
 * @return Length of report text.
 */
size_t mp_analyze_leaks(memory_pool_t* pool, char* report_buf, size_t max_len);

/**
 * @brief Exports memory leak analysis report to a text file.
 * @param pool Memory pool pointer.
 * @param filepath Output file path.
 * @return True on success, False on failure.
 */
bool mp_export_leak_report(memory_pool_t* pool, const char* filepath);

/**
 * @brief Retrieves current statistical metrics of the memory pool.
 * @param pool Memory pool pointer.
 * @param stats Pointer to output mp_stats_t struct.
 */
void mp_get_stats(memory_pool_t* pool, mp_stats_t* stats);

/**
 * @brief Prints detailed summary and health status of the memory pool to stdout.
 * @param pool Memory pool pointer.
 */
void mp_dump_info(memory_pool_t* pool);

/**
 * @brief Dumps memory pool stats into JSON format buffer for telemetry monitoring.
 * @param pool Memory pool pointer.
 * @param buf Output character buffer.
 * @param max_len Maximum length of buffer.
 * @return Number of characters written.
 */
size_t mp_dump_json_stats(memory_pool_t* pool, char* buf, size_t max_len);

/**
 * @brief Checks if there are any un-freed memory allocations (memory leaks).
 * @param pool Memory pool pointer.
 * @return True if clean (no leaks), False if memory leaks exist.
 */
bool mp_check_leaks(memory_pool_t* pool);

#ifdef MP_ENABLE_LOCATION_MACROS
#define mp_alloc(pool, sz) mp_alloc_loc(pool, sz, __FILE__, __LINE__, __func__)
#define mp_calloc(pool, num, sz) mp_calloc_loc(pool, num, sz, __FILE__, __LINE__, __func__)
#define mp_realloc(pool, ptr, sz) mp_realloc_loc(pool, ptr, sz, __FILE__, __LINE__, __func__)
#endif

#ifdef __cplusplus
}
#endif

#endif // MEMORY_POOL_H
