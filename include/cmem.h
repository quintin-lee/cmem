/**
 * @file cmem.h
 * @brief cmem - High-Performance Universal Tiered Memory Manager in C.
 *
 * Features:
 *  - Tiered allocation: Slab (Small), TLSF (Medium), Direct OS (Large)
 *  - Emergency OOM Fallback Memory Reserve Cushion (mp_enable_emergency_reserve)
 *  - Linux NUMA Node CPU Memory Affinity Binding (mp_set_numa_node)
 *  - Game & Graphics Pipeline Dual Ping-Pong Frame Arena (mp_frame_arena_create / mp_frame_alloc / mp_frame_end)
 *  - Incremental Memory Leak Diff Analysis Tool (mp_diff_snapshots)
 *  - High/Low Watermark Threshold Alert Callbacks (mp_set_watermark_callback)
 *  - Linux madvise MADV_DONTNEED / MADV_FREE Lazy RSS Physical Memory Purging (mp_purge_lazy)
 *  - Prometheus / OpenTelemetry Standard Metrics Exporter (mp_export_prometheus_metrics)
 *  - 0-Overhead Typed Object Pool Allocator (mp_typed_pool_create / mp_typed_alloc / mp_typed_free)
 *  - CMEM_CONF Environment Variable Runtime Auto-Tuning (mp_parse_env_flags)
 *  - DPDK-Style Lock-Free Atomic Ring Buffer Allocator (mp_ring_create / mp_ring_alloc / mp_ring_free)
 *  - POSIX Shared Memory IPC Arenas for Zero-Copy Inter-Process Communication (mp_create_shared)
 *  - Linux HugePages (2MB / 1GB) Support for TLB Performance Acceleration (MP_FLAG_HUGE_PAGES)
 *  - Post-Mortem Binary Crash Memory Snapshot Dump & Parser (mp_export_binary_snapshot / mp_parse_binary_snapshot)
 *  - Real-Time Allocation QPS & Bandwidth Throughput Meter (alloc_qps, bandwidth_mbps)
 *  - O(1) Allocation and Free performance
 *  - Thread-Local Caching (Lock-Free fast path for small objects)
 *  - Cache Line 64B Alignment & False Sharing Elimination (MP_FLAG_CACHE_ALIGNED)
 *  - Page-Level Guard Pages Protection via PROT_NONE (MP_FLAG_GUARD_PAGES)
 *  - Allocation Size Histogram & Distribution Diagnostics (mp_dump_histogram)
 *  - High-Throughput Batch Allocation & Free (mp_alloc_batch / mp_free_batch)
 *  - Memory Compaction & OS Page Trimming (mp_compact)
 *  - Memory Budget Limits & OOM Protection (mp_set_memory_limit)
 *  - Arena Fast Reset (O(1) batch deallocation for request-scoped lifetime)
 *  - Hierarchical Child Arenas (Parent-Child nested memory contexts)
 *  - Static Buffer Mode (Zero OS malloc dependency for embedded / bare-metal)
 *  - Custom Backing Allocator injection (Shared Memory, HugePages)
 *  - Real-time Profiling & Event Callback Hooks
 *  - Advanced Memory Diagnostics & Leak Analysis
 */

#ifndef CMEM_H
#define CMEM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CMEM_HISTOGRAM_BUCKETS 16

/**
 * @brief Configuration flags for memory pool behavior.
 *
 * These flags control allocation strategy, debugging, thread safety,
 * and platform-specific optimizations. Multiple flags can be ORed together.
 */
typedef enum {
    MP_FLAG_DEFAULT            = 0,                    /**< Default configuration, no special features enabled */
    MP_FLAG_THREAD_SAFE        = (1 << 0),             /**< Enable thread safety via pthread mutex/rwlock */
    MP_FLAG_DEBUG_CANARY       = (1 << 1),             /**< Add magic canary bytes for buffer overflow checks */
    MP_FLAG_ZERO_ON_ALLOC      = (1 << 2),             /**< Automatically zero memory upon allocation */
    MP_FLAG_THREAD_LOCAL_CACHE = (1 << 3),             /**< Enable thread-local cache for lock-free small allocs */
    MP_FLAG_STATIC_BUFFER      = (1 << 4),             /**< Static buffer mode (no OS memory allocation/free) */
    MP_FLAG_TRACK_LOCATIONS    = (1 << 5),             /**< Record file, line, function & backtrace for allocs */
    MP_FLAG_POISON_ON_FREE     = (1 << 6),             /**< Poison freed memory with 0xDD byte pattern (UAF protection) */
    MP_FLAG_CACHE_ALIGNED      = (1 << 7),             /**< Force 64-byte Cache Line alignment to prevent False Sharing */
    MP_FLAG_GUARD_PAGES        = (1 << 8),             /**< Add PROT_NONE Guard Pages to trap out-of-bounds page faults */
    MP_FLAG_SHARED_MEMORY      = (1 << 9),             /**< POSIX Shared Memory IPC Mode (/dev/shm zero-copy) */
    MP_FLAG_HUGE_PAGES         = (1 << 10)             /**< Use Linux HugePages (2MB/1GB MAP_HUGETLB) for TLB performance */
} mp_flags_t;

/**
 * @brief Profiling & Debug Event Types.
 *
 * Used with mp_set_event_callback() to receive notifications about
 * allocation lifecycle events and error conditions.
 */
typedef enum {
    MP_EVENT_ALLOC = 1,          /**< Memory block allocated */
    MP_EVENT_FREE,               /**< Memory block freed */
    MP_EVENT_REALLOC,            /**< Memory block reallocated */
    MP_EVENT_CANARY_CORRUPTION,  /**< Buffer overflow detected via canary check */
    MP_EVENT_DOUBLE_FREE,        /**< Double-free or invalid free detected */
    MP_EVENT_RESET,              /**< Memory pool reset */
    MP_EVENT_COMPACT,            /**< Memory pool compaction */
    MP_EVENT_OOM                 /**< Out-of-memory condition reached */
} mp_event_type_t;

/**
 * @brief Opaque handle to a memory pool instance.
 */
typedef struct memory_pool memory_pool_t;

/**
 * @brief Opaque handle to a lock-free ring buffer allocator.
 */
typedef struct cmem_ring_buffer cmem_ring_buffer_t;

/**
 * @brief Opaque handle to a typed object pool allocator.
 */
typedef struct mp_typed_pool mp_typed_pool_t;

/**
 * @brief Opaque handle to a game/graphics frame arena allocator.
 */
typedef struct cmem_frame_arena cmem_frame_arena_t;

/**
 * @brief Event Callback function pointer for telemetry profiling.
 *
 * @param pool Pointer to the memory pool where the event occurred
 * @param event Type of event (alloc, free, realloc, etc.)
 * @param ptr Pointer involved in the event (NULL for some events)
 * @param size Size of the allocation in bytes
 * @param user_data Optional user data passed to the callback
 */
typedef void (*mp_event_callback_t)(memory_pool_t* pool, mp_event_type_t event, void* ptr, size_t size, void* user_data);

/**
 * @brief Watermark Alert Callback function pointer.
 *
 * Called when the pool crosses a high or low watermark threshold.
 *
 * @param pool Pointer to the memory pool
 * @param is_high_watermark true if high watermark hit, false if low watermark recovery
 * @param current_bytes Current active bytes in the pool
 * @param limit_bytes The memory limit being compared against
 * @param user_data Optional user data passed to the callback
 */
typedef void (*mp_watermark_callback_t)(memory_pool_t* pool, bool is_high_watermark, size_t current_bytes, size_t limit_bytes, void* user_data);

/**
 * @brief Custom Backing Allocator function table for system memory injection.
 *
 * Allows users to provide their own memory allocation backend instead of
 * the default system malloc/mmap.
 */
typedef struct {
    void* (*sys_alloc)(size_t size, void* user_data);  /**< Allocation function */
    void  (*sys_free)(void* ptr, size_t size, void* user_data); /**< Free function */
    void* user_data;                                    /**< User data passed to alloc/free */
} mp_sys_allocator_t;

/**
 * @brief Statistics snapshot of the memory pool.
 *
 * Captured at a point in time by mp_get_stats().
 */
typedef struct {
    size_t total_pool_size;       /**< Total bytes reserved/allocated from system */
    size_t active_bytes;          /**< Total active payload bytes requested by user */
    size_t peak_bytes;            /**< Peak active payload bytes */
    size_t max_memory_limit;      /**< Maximum memory limit budget in bytes (0 for unlimited) */
    size_t active_allocations;    /**< Count of currently outstanding allocations */
    size_t total_alloc_ops;       /**< Cumulative allocation count */
    size_t total_free_ops;        /**< Cumulative free count */
    size_t slab_allocated_bytes;  /**< Payload bytes in small-object Slab allocator */
    size_t tlsf_allocated_bytes;  /**< Payload bytes in medium-object TLSF allocator */
    size_t os_allocated_bytes;    /**< Payload bytes in direct OS fallback allocator */
    double fragmentation_ratio;   /**< Estimated memory fragmentation ratio (0.0 to 1.0) */
    double alloc_qps;             /**< Real-time allocation operations per second (QPS) */
    double bandwidth_mbps;        /**< Real-time allocation bandwidth throughput (MB/s) */
    size_t size_histogram[CMEM_HISTOGRAM_BUCKETS]; /**< Allocation size distribution histogram */
} mp_stats_t;

/* ========================================================================== */
/*  Advanced Feature API Declarations                                         */
/* ========================================================================== */

/**
 * @brief Enables an emergency fallback reserve cushion for critical OOM scenarios.
 *
 * When the main pool exceeds its memory limit, allocations can fall back to
 * this reserved buffer for critical operations like logging or monitoring.
 *
 * @param pool Pointer to the memory pool
 * @param reserve_bytes Size of the emergency reserve buffer in bytes
 * @return true on success, false on failure
 */
bool mp_enable_emergency_reserve(memory_pool_t* pool, size_t reserve_bytes);

/**
 * @brief Binds memory pool backing allocations to a specific Linux NUMA CPU node.
 *
 * All subsequent system allocations from this pool will be bound to the
 * specified NUMA node using mbind().
 *
 * @param pool Pointer to the memory pool
 * @param numa_node NUMA node ID (-1 for default/disabled)
 * @return true on success, false on failure
 */
bool mp_set_numa_node(memory_pool_t* pool, int numa_node);

/**
 * @brief Parses CMEM_CONF environment variable string and returns merged configuration flags.
 *
 * Supported keys: canary=1/on, zero=1/on, tls=1/on, track=1/on,
 *                 poison=1/on, aligned=1/on, guard=1/on, hugepages=1/on
 *
 * @param default_flags Default flags to merge with parsed flags
 * @return Merged flags value
 */
mp_flags_t mp_parse_env_flags(mp_flags_t default_flags);

/**
 * @brief Creates a game & graphics dual ping-pong frame arena allocator.
 *
 * Frame arenas use double-buffering to provide O(1) per-frame reset,
 * ideal for per-frame allocations in rendering or physics pipelines.
 *
 * @param frame_capacity Capacity per frame buffer in bytes
 * @return Pointer to the frame arena, or NULL on failure
 */
cmem_frame_arena_t* mp_frame_arena_create(size_t frame_capacity);

/**
 * @brief Allocates temporary memory for the current frame.
 *
 * Allocation occurs from the currently active ping-pong buffer.
 *
 * @param farena Pointer to the frame arena
 * @param size Requested size in bytes
 * @return Pointer to the payload, or NULL on failure
 */
void* mp_frame_alloc(cmem_frame_arena_t* farena, size_t size);

/**
 * @brief Ends the current frame and swaps ping-pong buffers with O(1) cursor reset.
 *
 * The previously active buffer is reset, making it ready for the next frame.
 *
 * @param farena Pointer to the frame arena
 */
void mp_frame_end(cmem_frame_arena_t* farena);

/**
 * @brief Destroys the frame arena allocator instance.
 *
 * @param farena Pointer to the frame arena
 */
void mp_frame_arena_destroy(cmem_frame_arena_t* farena);

/**
 * @brief Creates a 0-overhead fixed-size object pool allocator.
 *
 * The typed pool uses a free-list with no per-object header overhead,
 * making it ideal for high-frequency allocation of fixed-size objects.
 *
 * @param elem_size Size of each element in bytes (rounded up to 8 bytes, min sizeof(void*))
 * @param capacity Maximum number of elements in the pool
 * @return Pointer to the typed pool, or NULL on failure
 */
mp_typed_pool_t* mp_typed_pool_create(size_t elem_size, size_t capacity);

/**
 * @brief Allocates an object pointer from the typed object pool with 0 header overhead.
 *
 * @param tpool Pointer to the typed pool
 * @return Pointer to the object, or NULL if pool is exhausted
 */
void* mp_typed_alloc(mp_typed_pool_t* tpool);

/**
 * @brief Returns an object pointer back to the typed object pool.
 *
 * @param tpool Pointer to the typed pool
 * @param ptr Pointer to the object to free
 */
void mp_typed_free(mp_typed_pool_t* tpool, void* ptr);

/**
 * @brief Destroys the typed object pool instance.
 *
 * @param tpool Pointer to the typed pool
 */
void mp_typed_pool_destroy(mp_typed_pool_t* tpool);

/**
 * @brief Creates a new cmem memory pool instance using default OS memory.
 *
 * This is the standard pool creation function. It internally calls mp_create_custom()
 * with a NULL system allocator.
 *
 * @param initial_capacity Initial memory capacity in bytes (0 for default ~4MB TLSF pool)
 * @param flags Configuration flags (thread safety, canary, etc.)
 * @return Pointer to the new memory pool, or NULL on failure
 */
memory_pool_t* mp_create(size_t initial_capacity, mp_flags_t flags);

/**
 * @brief Creates a lock-free DPDK-style Ring Buffer Allocator.
 *
 * Single-producer single-consumer lock-free ring buffer with atomic head/tail indices.
 * Capacity is rounded up to the next power of two.
 *
 * @param slot_size Size of each slot in bytes
 * @param capacity Number of slots (rounded up to next power of two)
 * @return Pointer to the ring buffer, or NULL on failure
 */
cmem_ring_buffer_t* mp_ring_create(size_t slot_size, size_t capacity);

/**
 * @brief Allocates a slot pointer from the lock-free ring buffer.
 *
 * Producer path using atomic fetch-add on head index.
 *
 * @param ring Pointer to the ring buffer
 * @return Pointer to the slot payload, or NULL if full
 */
void* mp_ring_alloc(cmem_ring_buffer_t* ring);

/**
 * @brief Returns a slot pointer back to the lock-free ring buffer.
 *
 * Consumer path using atomic fetch-add on tail index.
 *
 * @param ring Pointer to the ring buffer
 * @param ptr Pointer to the slot to return
 * @return true on success, false on invalid input
 */
bool mp_ring_free(cmem_ring_buffer_t* ring, void* ptr);

/**
 * @brief Destroys the lock-free ring buffer allocator instance.
 *
 * @param ring Pointer to the ring buffer
 */
void mp_ring_destroy(cmem_ring_buffer_t* ring);

/**
 * @brief Creates a POSIX shared memory pool in /dev/shm for zero-copy Inter-Process Communication (IPC).
 *
 * The shared memory pool uses mmap(MAP_SHARED) and can be opened by
 * multiple processes using the same shm_name.
 *
 * @param shm_name Name of the shared memory object (e.g. "/my_pool")
 * @param capacity Capacity in bytes (minimum 64KB, defaults to 1MB)
 * @param flags Configuration flags
 * @return Pointer to the new shared memory pool, or NULL on failure
 */
memory_pool_t* mp_create_shared(const char* shm_name, size_t capacity, mp_flags_t flags);

/**
 * @brief Destroys a shared memory pool and unlinks the POSIX shared memory segment.
 *
 * @param pool Pointer to the shared memory pool
 * @param shm_name Name of the shared memory object to unlink
 */
void mp_destroy_shared(memory_pool_t* pool, const char* shm_name);

/**
 * @brief Creates a child memory pool linked to a parent pool.
 *
 * Child pools form a tree structure with their parent. Destroying or resetting
 * a parent recursively affects all linked children.
 *
 * @param parent Pointer to the parent memory pool (can be NULL)
 * @param initial_capacity Initial capacity for the child pool
 * @param flags Configuration flags
 * @param arena_name Human-readable name for the child arena
 * @return Pointer to the new child memory pool, or NULL on failure
 */
memory_pool_t* mp_create_child(memory_pool_t* parent, size_t initial_capacity, mp_flags_t flags, const char* arena_name);

/**
 * @brief Sets a human-readable name for the memory pool arena.
 *
 * @param pool Pointer to the memory pool
 * @param name Null-terminated name string (max 63 chars)
 */
void mp_set_name(memory_pool_t* pool, const char* name);

/**
 * @brief Gets the human-readable name of the memory pool arena.
 *
 * @param pool Pointer to the memory pool
 * @return Pointer to the name string, or NULL if pool is invalid
 */
const char* mp_get_name(memory_pool_t* pool);

/**
 * @brief Gets the parent pool pointer if this pool is a child arena.
 *
 * @param pool Pointer to the memory pool
 * @return Pointer to the parent pool, or NULL if this is a root pool
 */
memory_pool_t* mp_get_parent(memory_pool_t* pool);

/**
 * @brief Gets the count of direct child arenas linked to this pool.
 *
 * @param pool Pointer to the memory pool
 * @return Number of direct children, or 0 if pool is invalid
 */
size_t mp_get_child_count(memory_pool_t* pool);

/**
 * @brief Calculates the memory pool pressure ratio relative to its limit or total size.
 *
 * Returns a value between 0.0 (0% used) and 1.0 (100% used).
 * If max_memory_limit is set, pressure is calculated against that limit.
 * Otherwise, it is calculated against total_pool_size.
 *
 * @param pool Pointer to the memory pool
 * @return Pressure ratio between 0.0 and 1.0
 */
double mp_pressure(memory_pool_t* pool);

/**
 * @brief Returns the total bytes that could be reclaimed by trimming fully-free Slab pages.
 *
 * @param pool Pointer to the memory pool
 * @return Number of reclaimable bytes
 */
size_t mp_freeable(memory_pool_t* pool);

/**
 * @brief Returns the estimated physical RSS resident memory size of the pool.
 *
 * @param pool Pointer to the memory pool
 * @return Total reserved bytes from the OS
 */
size_t mp_resident(memory_pool_t* pool);

/**
 * @brief Resets cumulative performance metrics and peak memory statistics.
 *
 * This does not free any allocations; it only resets counters like
 * total_alloc_ops, total_free_ops, and the allocation histogram.
 *
 * @param pool Pointer to the memory pool
 */
void mp_reset_stats(memory_pool_t* pool);

/**
 * @brief Returns the optimal size class for a requested byte size.
 *
 * For sizes <= 512B, returns the next Slab size class (8, 16, 32, 64, 128, 256, 512).
 * For larger sizes, returns the size aligned to 8 bytes.
 *
 * @param size Requested size in bytes
 * @return Preferred/aligned size
 */
size_t mp_preferred_size(size_t size);

/**
 * @brief Returns the optimal size class for a requested byte size using a pool's custom Slab table.
 *
 * @param pool Pointer to the memory pool
 * @param size Requested size in bytes
 * @return Preferred/aligned size based on the pool's configured Slab classes
 */
size_t mp_preferred_size_for_pool(memory_pool_t* pool, size_t size);

/**
 * @brief Configures a custom Slab class size table for the memory pool.
 *
 * Replaces the default Slab size classes (8, 16, 32, 64, 128, 256, 512) with
 * user-provided sizes. The number of classes must not exceed SLAB_CLASS_COUNT.
 * Sizes must be in ascending order and powers of two for optimal performance.
 *
 * @param pool Pointer to the memory pool
 * @param sizes Array of custom slab class sizes in bytes
 * @param count Number of custom sizes (must be <= SLAB_CLASS_COUNT)
 * @return true on success, false on invalid input
 */
bool mp_set_slab_classes(memory_pool_t* pool, const size_t* sizes, size_t count);

/**
 * @brief Retrieves the number of active Slab size classes for a pool.
 *
 * @param pool Pointer to the memory pool
 * @return Number of slab classes, or SLAB_CLASS_COUNT if using defaults
 */
size_t mp_get_slab_class_count(memory_pool_t* pool);

/**
 * @brief Retrieves the configured Slab class sizes for a pool.
 *
 * @param pool Pointer to the memory pool
 * @param out_sizes Output buffer to store slab class sizes
 * @param max_count Maximum number of sizes to retrieve
 * @return Number of sizes written to out_sizes
 */
size_t mp_get_slab_classes(memory_pool_t* pool, size_t* out_sizes, size_t max_count);

/**
 * @brief Creates a memory pool instance using a custom backing allocator.
 *
 * This is the core pool creation function used by mp_create() and mp_create_shared().
 * If sys_allocator is NULL, the default system allocator is used.
 *
 * @param initial_capacity Initial memory capacity in bytes
 * @param flags Configuration flags
 * @param sys_allocator Custom system allocator function table, or NULL for default
 * @return Pointer to the new memory pool, or NULL on failure
 */
memory_pool_t* mp_create_custom(size_t initial_capacity, mp_flags_t flags, const mp_sys_allocator_t* sys_allocator);

/**
 * @brief Creates a memory pool inside a pre-allocated static buffer (zero OS malloc dependency).
 *
 * Useful for embedded systems, kernel modules, or any scenario where
 * dynamic system allocation is not available or desired.
 *
 * @param buffer Pre-allocated buffer memory (must be 8-byte aligned)
 * @param buffer_size Size of the buffer in bytes
 * @param flags Configuration flags
 * @return Pointer to the new memory pool, or NULL on failure
 */
memory_pool_t* mp_create_from_buffer(void* buffer, size_t buffer_size, mp_flags_t flags);

/**
 * @brief Destroys the memory pool and recursively destroys all linked child arenas.
 *
 * Releases all system memory, Slab pages, TLSF pools, and synchronization primitives.
 * After calling this, the pool pointer must not be used again.
 *
 * @param pool Pointer to the memory pool
 */
void mp_destroy(memory_pool_t* pool);

/**
 * @brief Resets the memory pool and all linked child arenas to an empty state.
 *
 * All allocations are logically freed in O(1) time; underlying memory is
 * retained for reuse. This is much faster than freeing individual blocks.
 *
 * @param pool Pointer to the memory pool
 */
void mp_reset(memory_pool_t* pool);

/**
 * @brief Sets a hard maximum memory budget limit on the pool.
 *
 * When active_bytes exceeds this limit, further allocations will attempt
 * to use the emergency reserve (if enabled) or return NULL.
 *
 * @param pool Pointer to the memory pool
 * @param max_bytes Maximum allowed active bytes (0 for unlimited)
 */
void mp_set_memory_limit(memory_pool_t* pool, size_t max_bytes);

/**
 * @brief Configures high and low watermark threshold alert callbacks.
 *
 * The callback is triggered when active_bytes crosses the high threshold
 * and again when it falls below the low threshold.
 *
 * @param pool Pointer to the memory pool
 * @param high_ratio High watermark ratio (0.0-1.0) that triggers the callback
 * @param low_ratio Low watermark ratio (0.0-1.0) that clears the high state
 * @param cb Watermark callback function pointer
 * @param user_data Optional user data passed to the callback
 */
void mp_set_watermark_callback(memory_pool_t* pool, double high_ratio, double low_ratio, mp_watermark_callback_t cb, void* user_data);

/**
 * @brief Compacts the memory pool by releasing completely free Slab pages back to the OS.
 *
 * Only pages that are entirely unused are returned to the system.
 *
 * @param pool Pointer to the memory pool
 * @return Number of bytes freed back to the OS
 */
size_t mp_compact(memory_pool_t* pool);

/**
 * @brief Purges unused Slab pages using Linux madvise MADV_DONTNEED to reduce physical RSS.
 *
 * This advises the kernel that the specified pages are not needed, allowing
 * it to reclaim physical memory without removing the pages from the pool.
 *
 * @param pool Pointer to the memory pool
 * @return Number of bytes purged
 */
size_t mp_purge_lazy(memory_pool_t* pool);

/**
 * @brief Portable wrapper for madvise / VirtualAlloc memory advice across Linux and Windows.
 *
 * On Linux, this calls madvise(). On Windows, it uses VirtualAlloc(MEM_RESET).
 *
 * @param pool Pointer to the memory pool
 * @param addr Start address of the memory region
 * @param length Length of the memory region in bytes
 * @param advice Advice value (e.g. MADV_DONTNEED on Linux)
 * @return 0 on success, -1 on failure
 */
int mp_madvise(memory_pool_t* pool, void* addr, size_t length, int advice);

/**
 * @brief Trims and reclaims unused memory capacity back to the OS, recursively for child arenas.
 *
 * Combines mp_compact() and mp_purge_lazy() for maximum memory reclamation.
 *
 * @param pool Pointer to the memory pool
 * @param pad Minimum number of bytes to keep reserved
 * @return Total bytes reclaimed across all arenas
 */
size_t mp_trim(memory_pool_t* pool, size_t pad);

/**
 * @brief Registers an event callback for real-time profiling and debugging.
 *
 * The callback will be invoked for events like alloc, free, realloc,
 * canary corruption, double free, reset, compact, and OOM.
 *
 * @param pool Pointer to the memory pool
 * @param callback Event callback function pointer
 * @param user_data Optional user data passed to the callback
 */
void mp_set_event_callback(memory_pool_t* pool, mp_event_callback_t callback, void* user_data);

/**
 * @brief Allocates memory block with source location tracking for leak diagnostics.
 *
 * This variant records the source file, line, and function name for each allocation.
 * Useful when MP_ENABLE_LOCATION_MACROS is not defined.
 *
 * @param pool Pointer to the memory pool
 * @param size Size in bytes
 * @param file Source file name (usually __FILE__)
 * @param line Source line number (usually __LINE__)
 * @param func Source function name (usually __func__)
 * @return Pointer to the allocated payload, or NULL on failure
 */
void* mp_alloc_loc(memory_pool_t* pool, size_t size, const char* file, int line, const char* func);

/**
 * @brief Allocates zeroed memory block with source location tracking.
 *
 * @param pool Pointer to the memory pool
 * @param num Number of elements
 * @param size Size of each element in bytes
 * @param file Source file name
 * @param line Source line number
 * @param func Source function name
 * @return Pointer to the allocated payload, or NULL on failure
 */
void* mp_calloc_loc(memory_pool_t* pool, size_t num, size_t size, const char* file, int line, const char* func);

/**
 * @brief Reallocates memory block with source location tracking.
 *
 * @param pool Pointer to the memory pool
 * @param ptr Existing allocation pointer (or NULL for new allocation)
 * @param new_size New requested size in bytes
 * @param file Source file name
 * @param line Source line number
 * @param func Source function name
 * @return Pointer to the reallocated payload, or NULL on failure
 */
void* mp_realloc_loc(memory_pool_t* pool, void* ptr, size_t new_size, const char* file, int line, const char* func);

/**
 * @brief Allocates a memory block from the pool.
 *
 * If MP_FLAG_CACHE_ALIGNED is set, this delegates to mp_aligned_alloc with 64-byte alignment.
 *
 * @param pool Pointer to the memory pool
 * @param size Requested size in bytes
 * @return Pointer to the payload, or NULL on failure
 */
void* mp_alloc(memory_pool_t* pool, size_t size);

/**
 * @brief Allocates and zero-initializes memory for an array of elements.
 *
 * @param pool Pointer to the memory pool
 * @param num Number of elements
 * @param size Size of each element in bytes
 * @return Pointer to the allocated payload, or NULL on failure
 */
void* mp_calloc(memory_pool_t* pool, size_t num, size_t size);

/**
 * @brief Reallocates a memory block to a new size.
 *
 * Attempts in-place expansion for TLSF blocks to avoid memcpy overhead.
 *
 * @param pool Pointer to the memory pool
 * @param ptr Existing allocation pointer (or NULL for new allocation)
 * @param new_size New requested size in bytes
 * @return Pointer to the reallocated payload, or NULL on failure
 */
void* mp_realloc(memory_pool_t* pool, void* ptr, size_t new_size);

/**
 * @brief Overflow-safe reallocarray without location tracking.
 *
 * Checks for nmemb * size overflow before allocation.
 *
 * @param pool Pointer to the memory pool
 * @param ptr Existing allocation pointer
 * @param nmemb Number of elements
 * @param size Size of each element in bytes
 * @return Pointer to the reallocated payload, or NULL on overflow/failure
 */
void* mp_reallocarray(memory_pool_t* pool, void* ptr, size_t nmemb, size_t size);

/**
 * @brief Overflow-safe reallocarray with source location tracking.
 *
 * @param pool Pointer to the memory pool
 * @param ptr Existing allocation pointer
 * @param nmemb Number of elements
 * @param size Size of each element in bytes
 * @param file Source file name
 * @param line Source line number
 * @param func Source function name
 * @return Pointer to the reallocated payload, or NULL on overflow/failure
 */
void* mp_reallocarray_loc(memory_pool_t* pool, void* ptr, size_t nmemb, size_t size, const char* file, int line, const char* func);

/**
 * @brief Allocates memory with a specific byte alignment requirement.
 *
 * @param pool Pointer to the memory pool
 * @param alignment Byte alignment (must be power of two, minimum sizeof(void*))
 * @param size Requested payload size in bytes
 * @return Pointer to the aligned payload, or NULL on failure
 */
void* mp_aligned_alloc(memory_pool_t* pool, size_t alignment, size_t size);

/**
 * @brief Frees a memory block back to the pool, performing canary checks and poison fill if enabled.
 *
 * @param pool Pointer to the memory pool
 * @param ptr Pointer to the payload to free
 */
void mp_free(memory_pool_t* pool, void* ptr);

/**
 * @brief Duplicates a null-terminated string into the memory pool.
 *
 * @param pool Pointer to the memory pool
 * @param str Source string to duplicate
 * @return Pointer to the duplicated string, or NULL on failure
 */
char* mp_strdup(memory_pool_t* pool, const char* str);

/**
 * @brief Duplicates a null-terminated string into the memory pool with location tracking.
 *
 * @param pool Pointer to the memory pool
 * @param str Source string to duplicate
 * @param file Source file name
 * @param line Source line number
 * @param func Source function name
 * @return Pointer to the duplicated string, or NULL on failure
 */
char* mp_strdup_loc(memory_pool_t* pool, const char* str, const char* file, int line, const char* func);

/**
 * @brief Duplicates a binary memory region into the pool.
 *
 * @param pool Pointer to the memory pool
 * @param src Source memory region
 * @param n Number of bytes to copy
 * @return Pointer to the duplicated memory, or NULL on failure
 */
void* mp_memdup(memory_pool_t* pool, const void* src, size_t n);

/**
 * @brief Duplicates a binary memory region into the pool with location tracking.
 *
 * @param pool Pointer to the memory pool
 * @param src Source memory region
 * @param n Number of bytes to copy
 * @param file Source file name
 * @param line Source line number
 * @param func Source function name
 * @return Pointer to the duplicated memory, or NULL on failure
 */
void* mp_memdup_loc(memory_pool_t* pool, const void* src, size_t n, const char* file, int line, const char* func);

/**
 * @brief Formats a string and allocates the result in the pool.
 *
 * @param pool Pointer to the memory pool
 * @param fmt Printf-style format string
 * @return Pointer to the formatted string, or NULL on failure
 */
char* mp_asprintf(memory_pool_t* pool, const char* fmt, ...);

/**
 * @brief Formats a string and allocates the result in the pool with location tracking.
 *
 * @param pool Pointer to the memory pool
 * @param file Source file name
 * @param line Source line number
 * @param func Source function name
 * @param fmt Printf-style format string
 * @return Pointer to the formatted string, or NULL on failure
 */
char* mp_asprintf_loc(memory_pool_t* pool, const char* file, int line, const char* func, const char* fmt, ...);

/**
 * @brief Returns the usable allocated capacity of a pointer block.
 *
 * Validates the pointer before dereferencing to avoid undefined behavior.
 *
 * @param pool Pointer to the memory pool
 * @param ptr Pointer to the payload
 * @return Usable size in bytes, or 0 if invalid
 */
size_t mp_usable_size(memory_pool_t* pool, void* ptr);

/**
 * @brief Returns the requested payload size of an allocated pointer block.
 *
 * Validates the pointer before dereferencing to avoid undefined behavior.
 *
 * @param pool Pointer to the memory pool
 * @param ptr Pointer to the payload
 * @return Requested size in bytes, or 0 if invalid
 */
size_t mp_alloc_size(memory_pool_t* pool, void* ptr);

/**
 * @brief Validates if a pointer belongs to an active allocation in the memory pool.
 *
 * Checks the block header magic and walks the active allocation list.
 *
 * @param pool Pointer to the memory pool
 * @param ptr Pointer to validate
 * @return true if the pointer is valid and active, false otherwise
 */
bool mp_ptr_valid(memory_pool_t* pool, void* ptr);

/**
 * @brief Batch allocates multiple memory blocks of the same size in a single operation.
 *
 * @param pool Pointer to the memory pool
 * @param size Size of each block in bytes
 * @param out_ptrs Output array to store allocated pointers
 * @param count Maximum number of blocks to allocate
 * @return Number of blocks successfully allocated
 */
size_t mp_alloc_batch(memory_pool_t* pool, size_t size, void** out_ptrs, size_t count);

/**
 * @brief Batch frees multiple memory blocks in a single operation.
 *
 * @param pool Pointer to the memory pool
 * @param ptrs Array of pointers to free
 * @param count Number of pointers in the array
 */
void mp_free_batch(memory_pool_t* pool, void** ptrs, size_t count);

/**
 * @brief Audits heap integrity by checking header magics and canary redzones of all active allocations.
 *
 * @param pool Pointer to the memory pool
 * @return true if heap is healthy, false if corruption detected
 */
bool mp_audit_heap(memory_pool_t* pool);

/**
 * @brief Generates a detailed memory leak analysis report with file/line locations and callstacks.
 *
 * @param pool Pointer to the memory pool
 * @param report_buf Output buffer for the report text
 * @param max_len Maximum length of the output buffer
 * @return Number of bytes written to report_buf
 */
size_t mp_analyze_leaks(memory_pool_t* pool, char* report_buf, size_t max_len);

/**
 * @brief Exports the memory leak analysis report to a text file.
 *
 * @param pool Pointer to the memory pool
 * @param filepath Path to the output text file
 * @return true on success
 */
bool mp_export_leak_report(memory_pool_t* pool, const char* filepath);

/**
 * @brief Exports an interactive visual HTML profiler dashboard to a file.
 *
 * The dashboard includes memory usage cards, tier distribution bars,
 * and a table of active allocations with source locations.
 *
 * @param pool Pointer to the memory pool
 * @param filepath Path to the output HTML file
 * @return true on success
 */
bool mp_export_html_report(memory_pool_t* pool, const char* filepath);

/**
 * @brief Formats and dumps memory pool metrics into Prometheus text exposition format.
 *
 * Output follows the Prometheus exposition format with HELP/TYPE headers.
 *
 * @param pool Pointer to the memory pool
 * @param out_buf Output buffer for Prometheus metrics text
 * @param max_len Maximum length of the output buffer
 * @return Number of bytes written to out_buf
 */
size_t mp_export_prometheus_metrics(memory_pool_t* pool, char* out_buf, size_t max_len);

/**
 * @brief Exports a binary post-mortem memory crash snapshot dump to file.
 *
 * The snapshot can later be parsed with mp_parse_binary_snapshot()
 * or compared with mp_diff_snapshots().
 *
 * @param pool Pointer to the memory pool
 * @param filepath Path to the output binary snapshot file
 * @return true on success
 */
bool mp_export_binary_snapshot(memory_pool_t* pool, const char* filepath);

/**
 * @brief Parses a binary post-mortem memory snapshot file into readable text report.
 *
 * @param filepath Path to the binary snapshot file
 * @param out_report Output buffer for the text report
 * @param max_len Maximum length of the output buffer
 * @return true on success
 */
bool mp_parse_binary_snapshot(const char* filepath, char* out_report, size_t max_len);

/**
 * @brief Compares two binary snapshot files and generates an incremental leak diff report.
 *
 * Identifies allocations present in snapshot B but not in snapshot A.
 *
 * @param snapshot_a_path Path to the baseline snapshot
 * @param snapshot_b_path Path to the target snapshot
 * @param out_report Output buffer for the diff report
 * @param max_len Maximum length of the output buffer
 * @return true on success
 */
bool mp_diff_snapshots(const char* snapshot_a_path, const char* snapshot_b_path, char* out_report, size_t max_len);

/**
 * @brief Prints ASCII allocation size distribution histogram chart to stdout.
 *
 * @param pool Pointer to the memory pool
 */
void mp_dump_histogram(memory_pool_t* pool);

/**
 * @brief Retrieves current statistical metrics of the memory pool.
 *
 * @param pool Pointer to the memory pool
 * @param stats Output statistics structure
 */
void mp_get_stats(memory_pool_t* pool, mp_stats_t* stats);

/**
 * @brief Prints detailed summary and health status of the memory pool to stdout.
 *
 * @param pool Pointer to the memory pool
 */
void mp_dump_info(memory_pool_t* pool);

/**
 * @brief Dumps memory pool tree hierarchy to stdout.
 *
 * Recursively prints all child arenas with their active bytes and allocation counts.
 *
 * @param pool Pointer to the root memory pool
 */
void mp_dump_tree_info(memory_pool_t* pool);

/**
 * @brief Dumps memory pool stats into JSON format buffer for telemetry monitoring.
 *
 * @param pool Pointer to the memory pool
 * @param buf Output buffer for JSON text
 * @param max_len Maximum length of the output buffer
 * @return Number of bytes written to buf
 */
size_t mp_dump_json_stats(memory_pool_t* pool, char* buf, size_t max_len);

/**
 * @brief Checks if there are any un-freed memory allocations and prints a leak report if found.
 *
 * @param pool Pointer to the memory pool
 * @return true if no leaks detected, false otherwise
 */
bool mp_check_leaks(memory_pool_t* pool);

/* ========================================================================== */
/*  Runtime Config Hot-Reload                                                  */
/* ========================================================================== */

/**
 * @brief Re-parses the CMEM_CONF environment variable and applies safe runtime flag changes.
 *
 * This allows runtime reconfiguration without recreating the pool.
 * Only flags that do not require pool re-initialization are applied.
 *
 * Supported keys: canary=1/on, zero=1/on, tls=1/on, track=1/on,
 *                 poison=1/on, aligned=1/on, guard=1/on, hugepages=1/on
 *
 * @param pool Pointer to the memory pool
 * @return Merged flags value after applying environment changes
 */
mp_flags_t mp_reparse_env_flags(memory_pool_t* pool);

/**
 * @brief Returns the current environment configuration generation counter.
 *
 * Incremented each time mp_reparse_env_flags() successfully applies changes.
 * Useful for detecting when runtime config has been updated.
 *
 * @param pool Pointer to the memory pool
 * @return Current generation counter, or 0 if pool is invalid
 */
uint64_t mp_get_env_generation(memory_pool_t* pool);

/* ========================================================================== */
/*  Auto-Compaction Trigger                                                    */
/* ========================================================================== */

/**
 * @brief Enables automatic compaction triggered by pool pressure or fragmentation.
 *
 * When enabled, mp_alloc() and mp_free() will periodically check if compaction
 * is needed based on the configured thresholds.
 *
 * @param pool Pointer to the memory pool
 * @param enable true to enable auto-compaction, false to disable
 * @param pressure_threshold Pressure ratio (0.0-1.0) above which compaction is triggered
 * @param fragmentation_threshold Fragmentation ratio (0.0-1.0) above which compaction is triggered
 */
void mp_set_auto_compact(memory_pool_t* pool, bool enable, double pressure_threshold, double fragmentation_threshold);

/**
 * @brief Checks if auto-compaction is needed and triggers it if so.
 *
 * This is called internally by mp_alloc() and mp_free().
 *
 * @param pool Pointer to the memory pool
 * @return true if compaction was performed, false otherwise
 */
bool mp_auto_compact_check(memory_pool_t* pool);

/* ========================================================================== */
/*  Per-Arena Memory Quota                                                     */
/* ========================================================================== */

/**
 * @brief Sets a per-arena memory quota with an over-limit callback.
 *
 * When the arena's active bytes exceed the quota, the callback is invoked.
 * This is independent of the global mp_set_memory_limit().
 *
 * @param pool Pointer to the memory pool
 * @param quota_bytes Maximum allowed active bytes for this arena (0 for unlimited)
 * @param cb Callback invoked when quota is exceeded
 * @param user_data Optional user data passed to the callback
 */
void mp_set_arena_quota(memory_pool_t* pool, size_t quota_bytes, mp_watermark_callback_t cb, void* user_data);

/**
 * @brief Checks if the arena is within its quota limit.
 *
 * @param pool Pointer to the memory pool
 * @return true if within quota or no quota set, false if over quota
 */
bool mp_check_arena_quota(memory_pool_t* pool);

/* ========================================================================== */
/*  Allocation Latency Statistics                                              */
/* ========================================================================== */

/**
 * @brief Records an allocation latency sample in nanoseconds.
 *
 * This is called internally by the allocation fast-path.
 *
 * @param pool Pointer to the memory pool
 * @param latency_ns Latency in nanoseconds
 */
void mp_record_latency(memory_pool_t* pool, uint64_t latency_ns);

/**
 * @brief Calculates the P99 allocation latency from the histogram.
 *
 * @param pool Pointer to the memory pool
 * @return P99 latency in nanoseconds, or 0 if no samples
 */
uint64_t mp_get_latency_p99(memory_pool_t* pool);

/**
 * @brief Returns the average allocation latency in nanoseconds.
 *
 * @param pool Pointer to the memory pool
 * @return Average latency in nanoseconds, or 0 if no samples
 */
uint64_t mp_get_latency_avg(memory_pool_t* pool);

/**
 * @brief Resets the allocation latency statistics histogram.
 *
 * @param pool Pointer to the memory pool
 */
void mp_reset_latency_stats(memory_pool_t* pool);

/* ========================================================================== */
/*  Structured Event Log Ring Buffer & pprof Export                             */
/* ========================================================================== */

/**
 * @brief Structured event log record for lock-free ring buffer tracing.
 */
typedef struct {
    uint64_t timestamp_ns;      /**< Monotonic timestamp in nanoseconds */
    mp_event_type_t event_type; /**< Event type (alloc, free, realloc, etc.) */
    size_t size;                /**< Allocation size in bytes */
    uintptr_t ptr;              /**< Pointer involved in the event */
} mp_event_log_entry_t;

/**
 * @brief Opaque handle to a structured event log ring buffer.
 */
typedef struct mp_event_log mp_event_log_t;

/**
 * @brief Creates a structured event log with a lock-free ring buffer.
 *
 * @param capacity Number of entries in the ring buffer (must be power of two)
 * @return Pointer to the event log, or NULL on failure
 */
mp_event_log_t* mp_event_log_create(size_t capacity);

/**
 * @brief Destroys the event log and frees all associated memory.
 * @param log Pointer to the event log
 */
void mp_event_log_destroy(mp_event_log_t* log);

/**
 * @brief Records an event into the structured event log ring buffer.
 *
 * @param log Pointer to the event log
 * @param event_type Event type
 * @param ptr Pointer involved in the event
 * @param size Size of the allocation
 * @return true on success, false if ring buffer is full
 */
bool mp_event_log_record(mp_event_log_t* log, mp_event_type_t event_type, void* ptr, size_t size);

/**
 * @brief Consumes and returns the next event from the ring buffer.
 *
 * @param log Pointer to the event log
 * @param entry Output event entry
 * @return true if an event was consumed, false if buffer is empty
 */
bool mp_event_log_consume(mp_event_log_t* log, mp_event_log_entry_t* entry);

/**
 * @brief Returns the number of unread events in the ring buffer.
 *
 * @param log Pointer to the event log
 * @return Number of pending events
 */
size_t mp_event_log_pending(mp_event_log_t* log);

/**
 * @brief Clears all pending events from the ring buffer.
 *
 * @param log Pointer to the event log
 */
void mp_event_log_clear(mp_event_log_t* log);

/**
 * @brief Exports allocation events in pprof-compatible text format.
 *
 * Output can be processed by pprof tools for flame graph generation.
 *
 * @param pool Pointer to the memory pool
 * @param out_buf Output buffer for pprof text
 * @param max_len Maximum length of the output buffer
 * @return Number of bytes written to out_buf
 */
size_t mp_export_pprof(memory_pool_t* pool, char* out_buf, size_t max_len);

#ifdef MP_ENABLE_LOCATION_MACROS
#define mp_alloc(pool, sz) mp_alloc_loc(pool, sz, __FILE__, __LINE__, __func__)
#define mp_calloc(pool, num, sz) mp_calloc_loc(pool, num, sz, __FILE__, __LINE__, __func__)
#define mp_realloc(pool, ptr, sz) mp_realloc_loc(pool, ptr, sz, __FILE__, __LINE__, __func__)
#endif

#ifdef __cplusplus
}
#endif

#endif // CMEM_H
