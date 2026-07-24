/**
 * @file memory_pool.h
 * @brief High-Performance Universal Tiered Memory Manager in C.
 * 
 * Features:
 *  - Tiered allocation: Slab (Small), TLSF (Medium), Direct OS (Large)
 *  - O(1) Allocation and Free performance
 *  - Dynamic expansion & thread-safety options
 *  - Memory diagnostic tools: Leak detection, memory stats, redzone canary
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
    MP_FLAG_DEFAULT       = 0,
    MP_FLAG_THREAD_SAFE   = (1 << 0), /**< Enable thread safety via mutex locks */
    MP_FLAG_DEBUG_CANARY  = (1 << 1), /**< Add magic canary bytes for buffer overflow checks */
    MP_FLAG_ZERO_ON_ALLOC = (1 << 2)  /**< Automatically zero memory upon allocation */
} mp_flags_t;

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
} mp_stats_t;

typedef struct memory_pool memory_pool_t;

/**
 * @brief Creates a new memory pool instance.
 * @param initial_capacity Initial memory pool capacity in bytes (0 for default 4MB).
 * @param flags Bitwise OR of mp_flags_t.
 * @return Pointer to created memory_pool_t, or NULL on failure.
 */
memory_pool_t* mp_create(size_t initial_capacity, mp_flags_t flags);

/**
 * @brief Destroys the memory pool and frees all associated system resources.
 * @param pool Memory pool pointer.
 */
void mp_destroy(memory_pool_t* pool);

/**
 * @brief Allocates memory block of specified size.
 * @param pool Memory pool pointer.
 * @param size Allocation size in bytes.
 * @return Pointer to allocated memory block, or NULL on failure.
 */
void* mp_alloc(memory_pool_t* pool, size_t size);

/**
 * @brief Allocates memory for an array of num elements of size bytes each and clears it to zero.
 * @param pool Memory pool pointer.
 * @param num Number of elements.
 * @param size Size of each element.
 * @return Pointer to allocated memory block, or NULL on failure.
 */
void* mp_calloc(memory_pool_t* pool, size_t num, size_t size);

/**
 * @brief Reallocates memory block to a new size.
 * @param pool Memory pool pointer.
 * @param ptr Pointer to existing memory block (or NULL).
 * @param new_size New allocation size in bytes.
 * @return Pointer to reallocated memory block, or NULL on failure.
 */
void* mp_realloc(memory_pool_t* pool, void* ptr, size_t new_size);

/**
 * @brief Allocates aligned memory block.
 * @param pool Memory pool pointer.
 * @param alignment Alignment boundary (must be power of two and multiple of sizeof(void*)).
 * @param size Allocation size in bytes.
 * @return Pointer to aligned allocated memory block, or NULL on failure.
 */
void* mp_aligned_alloc(memory_pool_t* pool, size_t alignment, size_t size);

/**
 * @brief Frees memory block back to the memory pool.
 * @param pool Memory pool pointer.
 * @param ptr Pointer to memory block previously allocated by mp_alloc/calloc/realloc/aligned_alloc.
 */
void mp_free(memory_pool_t* pool, void* ptr);

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
 * @brief Checks if there are any un-freed memory allocations (memory leaks).
 * @param pool Memory pool pointer.
 * @return True if clean (no leaks), False if memory leaks exist.
 */
bool mp_check_leaks(memory_pool_t* pool);

#ifdef __cplusplus
}
#endif

#endif // MEMORY_POOL_H
