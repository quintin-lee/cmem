/**
 * @file cmem.h
 * @brief cmem - High-Performance Universal Tiered Memory Manager in C.
 *
 * Features:
 *  - Tiered allocation: Slab (Small), TLSF (Medium), Direct OS (Large)
 *  - Emergency OOM Fallback Memory Reserve Cushion (mp_enable_emergency_reserve)
 *  - Linux NUMA Node CPU Memory Affinity Binding (mp_set_numa_node)
 *  - Game & Graphics Pipeline Dual Ping-Pong Frame Arena (mp_frame_arena_create / mp_frame_alloc /
 * mp_frame_end)
 *  - Incremental Memory Leak Diff Analysis Tool (mp_diff_snapshots)
 *  - High/Low Watermark Threshold Alert Callbacks (mp_set_watermark_callback)
 *  - Linux madvise MADV_DONTNEED / MADV_FREE Lazy RSS Physical Memory Purging (mp_purge_lazy)
 *  - Prometheus / OpenTelemetry Standard Metrics Exporter (mp_export_prometheus_metrics)
 *  - 0-Overhead Typed Object Pool Allocator (mp_typed_pool_create / mp_typed_alloc / mp_typed_free)
 *  - CMEM_CONF Environment Variable Runtime Auto-Tuning (mp_parse_env_flags)
 *  - Runtime Config Hot-Reload without pool recreation (mp_reparse_env_flags)
 *  - Auto-Compaction Trigger based on pressure/fragmentation thresholds (mp_set_auto_compact)
 *  - Per-Arena Memory Quota with over-limit callbacks (mp_set_arena_quota)
 *  - Allocation Latency P99 Statistics Tracking (mp_record_latency / mp_get_latency_p99)
 *  - Configurable Slab Class Size Table (mp_set_slab_classes)
 *  - Structured Event Log Ring Buffer & pprof Export (mp_event_log_create / mp_export_pprof)
 *  - Per-CPU Lock-Free Freelist for low-contention fast path (MP_FLAG_PERCPU_FREELIST)
 *  - Graceful Degradation: fallback malloc, GC callback, eviction callback
 *  - Memory Error Recovery: dirty pool marking, bad-block isolation (mp_isolate_bad_block)
 *  - Thread-Level Quota & Circuit Breaker (mp_set_thread_quota / mp_set_circuit_breaker)
 *  - ABI Versioning & Container cgroup Awareness (mp_abi_version / mp_set_cgroup_aware)
 *  - Hot/Cold Page Separation for TLB Optimization (MP_FLAG_HOT_COLD_SEPARATION)
 *  - Encrypted Memory Support: mlock, MADV_DONTDUMP, secure zero (MP_FLAG_ENCRYPTED_MEMORY)
 *  - AddressSanitizer Integration Layer (MP_FLAG_ASAN_INTEGRATION)
 *  - Online Pool Expansion without service interruption (mp_expand_pool)
 *  - DPDK-Style Lock-Free Atomic Ring Buffer Allocator (mp_ring_create / mp_ring_alloc /
 * mp_ring_free)
 *  - POSIX Shared Memory IPC Arenas for Zero-Copy Inter-Process Communication (mp_create_shared)
 *  - Linux HugePages (2MB / 1GB) Support for TLB Performance Acceleration (MP_FLAG_HUGE_PAGES)
 *  - Post-Mortem Binary Crash Memory Snapshot Dump & Parser (mp_export_binary_snapshot /
 * mp_parse_binary_snapshot)
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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
    MP_FLAG_DEFAULT = 0,              /**< Default configuration, no special features enabled */
    MP_FLAG_THREAD_SAFE = (1 << 0),   /**< Enable thread safety via pthread mutex/rwlock */
    MP_FLAG_DEBUG_CANARY = (1 << 1),  /**< Add magic canary bytes for buffer overflow checks */
    MP_FLAG_ZERO_ON_ALLOC = (1 << 2), /**< Automatically zero memory upon allocation */
    MP_FLAG_THREAD_LOCAL_CACHE =
        (1 << 3),                       /**< Enable thread-local cache for lock-free small allocs */
    MP_FLAG_STATIC_BUFFER = (1 << 4),   /**< Static buffer mode (no OS memory allocation/free) */
    MP_FLAG_TRACK_LOCATIONS = (1 << 5), /**< Record file, line, function & backtrace for allocs */
    MP_FLAG_POISON_ON_FREE =
        (1 << 6), /**< Poison freed memory with 0xDD byte pattern (UAF protection) */
    MP_FLAG_CACHE_ALIGNED =
        (1 << 7), /**< Force 64-byte Cache Line alignment to prevent False Sharing */
    MP_FLAG_GUARD_PAGES =
        (1 << 8), /**< Add PROT_NONE Guard Pages to trap out-of-bounds page faults */
    MP_FLAG_SHARED_MEMORY = (1 << 9), /**< POSIX Shared Memory IPC Mode (/dev/shm zero-copy) */
    MP_FLAG_HUGE_PAGES =
        (1 << 10), /**< Use Linux HugePages (2MB/1GB MAP_HUGETLB) for TLB performance */
    MP_FLAG_PERCPU_FREELIST =
        (1 << 11), /**< Enable per-CPU lock-free freelist for low-contention fast path */
    MP_FLAG_HOT_COLD_SEPARATION =
        (1 << 12), /**< Enable Hot/Cold page separation for TLB optimization */
    MP_FLAG_ENCRYPTED_MEMORY =
        (1 << 13), /**< Enable encrypted memory with mlock and MADV_DONTDUMP */
    MP_FLAG_ASAN_INTEGRATION = (1 << 14), /**< Enable AddressSanitizer integration layer */
    MP_FLAG_REPORT_LEAKS_ON_DESTROY =
        (1 << 15), /**< Automatically report leaks to stderr on mp_destroy() */
    MP_FLAG_AUTO_NUMA =
        (1 << 16), /**< Auto-bind allocations to the calling thread's NUMA node (Linux) */
    MP_FLAG_MULTI_ARENA = (1 << 17), /**< Enable multi-arena thread-to-arena partitioning mode */
    MP_FLAG_FAST_PATH = (1 << 18),   /**< Skip header audit fields and active-list
                                         tracking; prefer raw allocation speed over
                                         double-free/leak-address diagnostics */
} mp_flags_t;

/**
 * @brief Profiling & Debug Event Types.
 *
 * Used with mp_set_event_callback() to receive notifications about
 * allocation lifecycle events and error conditions.
 */
typedef enum {
    MP_EVENT_ALLOC = 1,         /**< Memory block allocated */
    MP_EVENT_FREE,              /**< Memory block freed */
    MP_EVENT_REALLOC,           /**< Memory block reallocated */
    MP_EVENT_CANARY_CORRUPTION, /**< Buffer overflow detected via canary check */
    MP_EVENT_DOUBLE_FREE,       /**< Double-free or invalid free detected */
    MP_EVENT_RESET,             /**< Memory pool reset */
    MP_EVENT_COMPACT,           /**< Memory pool compaction */
    MP_EVENT_OOM,               /**< Out-of-memory condition reached */
    MP_EVENT_DIRTY              /**< Pool marked dirty due to memory corruption */
} mp_event_type_t;

/**
 * @brief Allocation tier types for diagnostic reporting.
 */
typedef enum {
    ALLOC_TYPE_SLAB = 1,     /**< Small-object Slab allocator (<= 512B) */
    ALLOC_TYPE_TLSF = 2,     /**< Medium-object TLSF allocator (512B ~ 4MB) */
    ALLOC_TYPE_OS = 3,       /**< Direct OS fallback allocator (> 4MB) */
    ALLOC_TYPE_EMERGENCY = 4 /**< Emergency reserve buffer */
} mp_alloc_type_t;

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
typedef void (*mp_event_callback_t)(
    memory_pool_t *pool, mp_event_type_t event, void *ptr, size_t size, void *user_data);

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
typedef void (*mp_watermark_callback_t)(memory_pool_t *pool,
                                        bool is_high_watermark,
                                        size_t current_bytes,
                                        size_t limit_bytes,
                                        void *user_data);

/**
 * @brief Custom Backing Allocator function table for system memory injection.
 *
 * Allows users to provide their own memory allocation backend instead of
 * the default system malloc/mmap.
 */
typedef struct {
    void *(*sys_alloc)(size_t size, void *user_data);          /**< Allocation function */
    void (*sys_free)(void *ptr, size_t size, void *user_data); /**< Free function */
    void *user_data; /**< User data passed to alloc/free */
} mp_sys_allocator_t;

/**
 * @brief Statistics snapshot of the memory pool.
 *
 * Captured at a point in time by mp_get_stats().
 */
typedef struct {
    size_t total_pool_size;      /**< Total bytes reserved/allocated from system */
    size_t active_bytes;         /**< Total active payload bytes requested by user */
    size_t peak_bytes;           /**< Peak active payload bytes */
    size_t max_memory_limit;     /**< Maximum memory limit budget in bytes (0 for unlimited) */
    size_t active_allocations;   /**< Count of currently outstanding allocations */
    size_t total_alloc_ops;      /**< Cumulative allocation count */
    size_t total_free_ops;       /**< Cumulative free count */
    size_t slab_allocated_bytes; /**< Payload bytes in small-object Slab allocator */
    size_t tlsf_allocated_bytes; /**< Payload bytes in medium-object TLSF allocator */
    size_t os_allocated_bytes;   /**< Payload bytes in direct OS fallback allocator */
    double fragmentation_ratio;  /**< Estimated memory fragmentation ratio (0.0 to 1.0) */
    double alloc_qps;            /**< Real-time allocation operations per second (QPS) */
    double bandwidth_mbps;       /**< Real-time allocation bandwidth throughput (MB/s) */
    size_t size_histogram[CMEM_HISTOGRAM_BUCKETS]; /**< Allocation size distribution histogram */
} mp_stats_t;

/**
 * @brief Per-allocation metadata returned by mp_get_allocation_info().
 */
typedef struct {
    void *ptr;                  /**< Payload pointer */
    size_t requested_size;      /**< Originally requested payload size */
    size_t usable_size;         /**< Actual usable capacity of this block */
    mp_alloc_type_t alloc_type; /**< Allocation tier (Slab/TLSF/OS/Emergency) */
    uint8_t slab_class;         /**< Slab class index (0 for non-Slab) */
    void *raw_base;             /**< Raw base address from system/slab allocation */
    const char *alloc_file;     /**< Source file where allocated (NULL if untracked) */
    int alloc_line;             /**< Source line number (-1 if untracked) */
    const char *alloc_func;     /**< Source function name (NULL if untracked) */
    void *backtrace_addrs[8];   /**< Captured backtrace addresses (0 if untracked) */
    int backtrace_depth;        /**< Number of backtrace frames captured */
} mp_allocation_info_t;

/**
 * @brief Leak severity classification.
 */

/**
 * @brief Memory region descriptor returned by mp_enumerate_regions().
 */
typedef struct {
    void *base;           /**< Base address of the region */
    size_t size;          /**< Size of the region in bytes */
    mp_alloc_type_t type; /**< Region type */
    uint8_t slab_class;   /**< Slab class index (0 for non-Slab) */
    bool is_hot;          /**< Hot page flag (for Slab pages) */
} mp_region_info_t;

/* Platform detection */
#if defined(_WIN32) || defined(_WIN64)
#define CMEM_PLATFORM_WINDOWS 1
#elif defined(__APPLE__) && defined(__MACH__)
#define CMEM_PLATFORM_MACOS 1
#elif defined(__linux__)
#define CMEM_PLATFORM_LINUX 1
#elif defined(__ANDROID__)
#define CMEM_PLATFORM_ANDROID 1
#elif defined(__FreeBSD__)
#define CMEM_PLATFORM_FREEBSD 1
#endif

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
bool mp_enable_emergency_reserve(memory_pool_t *pool, size_t reserve_bytes);

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
bool mp_set_numa_node(memory_pool_t *pool, int numa_node);

/**
 * @brief Opaque handle to a compressed block stored inside a pool.
 *
 * Zero is always an invalid handle.  Handles remain valid until the
 * block is explicitly freed (mp_free_compressed) or evicted by budget
 * pressure; stale handles are rejected safely by every API.
 */
typedef uint64_t compressed_handle_t;

/**
 * @brief Compresses data and transfers its ownership to the pool.
 *
 * The buffer at @p data is compressed into the pool's compression area
 * and then released via mp_free().  On success the caller must no longer
 * use @p data.  Returns 0 (and leaves @p data untouched) when the data
 * does not compress, the pool has no compression facility, or no budget
 * slot can be made available.
 *
 * @param pool Pool to store the compressed block in
 * @param data Buffer to compress (ownership transferred on success)
 * @param size Size of @p data in bytes
 * @return Handle, or 0 on failure (original data retained)
 */
compressed_handle_t mp_compress_block(memory_pool_t *pool, void *data, size_t size);

/**
 * @brief Decompresses a block back into a fresh pool allocation.
 *
 * Safe to call multiple times; each call returns an independent copy.
 * @return Pointer to the decompressed data, or NULL on invalid/stale
 *         handles or corrupted data.
 */
void *mp_decompress_block(memory_pool_t *pool, compressed_handle_t handle);

/**
 * @brief Frees a compressed block and invalidates its handle.
 * @return true on success, false if the handle was invalid or stale.
 */
bool mp_free_compressed(memory_pool_t *pool, compressed_handle_t handle);

/**
 * @brief Sets the maximum bytes the compression area may occupy.
 *
 * A budget of 0 disables further compression (existing handles remain
 * valid and decompressible).  When the budget is exceeded, the oldest
 * block is evicted to make room.
 *
 * @note The compression area is grown once, on the first use of
 *       compression (either this call or mp_compress_block) and sized to
 *       the budget in effect at that moment.  Raising the budget after
 *       the area exists raises the eviction threshold only; it does not
 *       enlarge the area.
 * @return true on success.
 */
bool mp_set_compressed_budget(memory_pool_t *pool, size_t max_bytes);

/**
 * @brief Reports compression-area usage.  Any out parameter may be NULL.
 * @return true on success.
 */
bool mp_get_compressed_stats(memory_pool_t *pool,
                             size_t *used,
                             size_t *budget,
                             size_t *block_count);

/**
 * @brief Returns the number of NUMA nodes detected on this system.
 *
 * Always returns at least 1 (single-node / non-NUMA systems report 1).
 * On platforms without NUMA support this returns 1.
 *
 * @return Number of NUMA nodes, or 1 when unknown/unsupported.
 */
int mp_numa_node_count(void);

/**
 * @brief Returns the NUMA node ID owning the given CPU index.
 *
 * @param cpu CPU index (>= 0)
 * @return NUMA node ID, or 0 when the CPU is unknown/unsupported.
 */
int mp_cpu_to_node(int cpu);

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
 * @brief Creates a new cmem memory pool instance using default OS memory.
 *
 * This is the standard pool creation function. It internally calls mp_create_custom()
 * with a NULL system allocator.
 *
 * @param initial_capacity Initial memory capacity in bytes (0 for default ~4MB TLSF pool)
 * @param flags Configuration flags (thread safety, canary, etc.)
 * @return Pointer to the new memory pool, or NULL on failure
 */
memory_pool_t *mp_create(size_t initial_capacity, mp_flags_t flags);

/**
 * @brief Retrieves or initializes the global cmem memory pool instance.
 *
 * Creates a 4MB thread-safe pool with thread-local cache on first call.
 * Subsequent calls return the same pool instance.
 *
 * @return Pointer to the global cmem memory pool, or NULL on failure
 */
memory_pool_t *mp_get_global_pool(void);

/**
 * @brief Sets the global cmem memory pool instance.
 *
 * Replaces the default global pool with a user-provided pool.
 * Useful for custom global pool initialization.
 *
 * @param pool Pointer to the memory pool to set as global (NULL resets to default)
 */
void mp_set_global_pool(memory_pool_t *pool);

/**
 * @brief Creates a POSIX shared memory pool in /dev/shm for zero-copy Inter-Process
 * Communication (IPC).
 *
 * The shared memory pool uses mmap(MAP_SHARED) and can be opened by
 * multiple processes using the same shm_name.
 *
 * @param shm_name Name of the shared memory object (e.g. "/my_pool")
 * @param capacity Capacity in bytes (minimum 64KB, defaults to 1MB)
 * @param flags Configuration flags
 * @return Pointer to the new shared memory pool, or NULL on failure
 */
memory_pool_t *mp_create_shared(const char *shm_name, size_t capacity, mp_flags_t flags);

/**
 * @brief Destroys a shared memory pool and unlinks the POSIX shared memory segment.
 *
 * @param pool Pointer to the shared memory pool
 * @param shm_name Name of the shared memory object to unlink
 */
void mp_destroy_shared(memory_pool_t *pool, const char *shm_name);

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
memory_pool_t *mp_create_child(memory_pool_t *parent,
                               size_t initial_capacity,
                               mp_flags_t flags,
                               const char *arena_name);

/**
 * @brief Sets a human-readable name for the memory pool arena.
 *
 * @param pool Pointer to the memory pool
 * @param name Null-terminated name string (max 63 chars)
 */
void mp_set_name(memory_pool_t *pool, const char *name);

/**
 * @brief Gets the human-readable name of the memory pool arena.
 *
 * @param pool Pointer to the memory pool
 * @return Pointer to the name string, or NULL if pool is invalid
 */
const char *mp_get_name(memory_pool_t *pool);

/**
 * @brief Gets the parent pool pointer if this pool is a child arena.
 *
 * @param pool Pointer to the memory pool
 * @return Pointer to the parent pool, or NULL if this is a root pool
 */
memory_pool_t *mp_get_parent(memory_pool_t *pool);

/**
 * @brief Gets the count of direct child arenas linked to this pool.
 *
 * @param pool Pointer to the memory pool
 * @return Number of direct children, or 0 if pool is invalid
 */
size_t mp_get_child_count(memory_pool_t *pool);

/**
 * @brief Enables multi-arena mode for a memory pool and sets up thread-to-arena partitioning.
 *
 * Spawns num_arenas internal child arenas. Threads calling mp_alloc() are bound
 * to specific arenas (via round-robin or CPU affinity), minimizing lock contention
 * and enabling NUMA node alignment on Linux.
 *
 * @param pool Memory pool to upgrade to multi-arena mode
 * @param num_arenas Number of sub-arenas to create (0 to auto-detect CPU/NUMA count)
 * @return True on success, false on failure
 */
bool mp_enable_multi_arena(memory_pool_t *pool, int num_arenas);

/**
 * @brief Binds the calling thread to a specific arena index within a multi-arena pool.
 *
 * @param pool Multi-arena memory pool
 * @param arena_index Arena index (0 <= arena_index < num_arenas)
 * @return True on success, false on failure
 */
bool mp_bind_thread_to_arena(memory_pool_t *pool, int arena_index);

/**
 * @brief Gets the sub-arena assigned to the calling thread for a multi-arena pool.
 *
 * @param pool Multi-arena memory pool
 * @return Pointer to the bound sub-arena (or pool if multi-arena is disabled)
 */
memory_pool_t *mp_get_thread_arena(memory_pool_t *pool);

/**
 * @brief Gets the total count of sub-arenas in a multi-arena pool.
 *
 * @param pool Memory pool
 * @return Number of sub-arenas, or 0 if multi-arena mode is not active
 */
int mp_get_arena_count(memory_pool_t *pool);

/**
 * @brief Gets a pointer to the sub-arena at the specified index.
 *
 * @param pool Multi-arena memory pool
 * @param arena_index Arena index (0 <= arena_index < num_arenas)
 * @return Pointer to the sub-arena, or NULL if invalid
 */
memory_pool_t *mp_get_arena(memory_pool_t *pool, int arena_index);

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
double mp_pressure(memory_pool_t *pool);

/**
 * @brief Returns the total bytes that could be reclaimed by trimming fully-free Slab pages.
 *
 * @param pool Pointer to the memory pool
 * @return Number of reclaimable bytes
 */
size_t mp_freeable(memory_pool_t *pool);

/**
 * @brief Returns the estimated physical RSS resident memory size of the pool.
 *
 * @param pool Pointer to the memory pool
 * @return Total reserved bytes from the OS
 */
size_t mp_resident(memory_pool_t *pool);

/**
 * @brief Resets cumulative performance metrics and peak memory statistics.
 *
 * This does not free any allocations; it only resets counters like
 * total_alloc_ops, total_free_ops, and the allocation histogram.
 *
 * @param pool Pointer to the memory pool
 */
void mp_reset_stats(memory_pool_t *pool);

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
 * @brief Returns the optimal size class for a requested byte size using a pool's custom Slab
 * table.
 *
 * @param pool Pointer to the memory pool
 * @param size Requested size in bytes
 * @return Preferred/aligned size based on the pool's configured Slab classes
 */
size_t mp_preferred_size_for_pool(memory_pool_t *pool, size_t size);

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
bool mp_set_slab_classes(memory_pool_t *pool, const size_t *sizes, size_t count);

/**
 * @brief Retrieves the number of active Slab size classes for a pool.
 *
 * @param pool Pointer to the memory pool
 * @return Number of slab classes, or SLAB_CLASS_COUNT if using defaults
 */
size_t mp_get_slab_class_count(memory_pool_t *pool);

/**
 * @brief Retrieves the configured Slab class sizes for a pool.
 *
 * @param pool Pointer to the memory pool
 * @param out_sizes Output buffer to store slab class sizes
 * @param max_count Maximum number of sizes to retrieve
 * @return Number of sizes written to out_sizes
 */
size_t mp_get_slab_classes(memory_pool_t *pool, size_t *out_sizes, size_t max_count);

/* ========================================================================== */
/*  Hot/Cold Page Separation                                                   */
/* ========================================================================== */

/**
 * @brief Marks a Slab page as hot for TLB optimization.
 *
 * Hot pages are kept physically separate from cold pages to improve
 * Translation Lookaside Buffer (TLB) hit rates.
 *
 * @param pool Pointer to the memory pool
 * @param page_raw_mem Raw memory pointer of the Slab page
 * @return true on success, false if page not found
 */
bool mp_mark_page_hot(memory_pool_t *pool, void *page_raw_mem);

/**
 * @brief Marks a Slab page as cold for TLB optimization.
 *
 * Cold pages contain infrequently accessed data and are separated from hot pages.
 *
 * @param pool Pointer to the memory pool
 * @param page_raw_mem Raw memory pointer of the Slab page
 * @return true on success, false if page not found
 */
bool mp_mark_page_cold(memory_pool_t *pool, void *page_raw_mem);

/**
 * @brief Returns the number of hot pages across all Slab classes.
 *
 * @param pool Pointer to the memory pool
 * @return Number of hot pages
 */
size_t mp_get_hot_page_count(memory_pool_t *pool);

/**
 * @brief Returns the number of cold pages across all Slab classes.
 *
 * @param pool Pointer to the memory pool
 * @return Number of cold pages
 */
size_t mp_get_cold_page_count(memory_pool_t *pool);

/**
 * @brief Separates hot and cold pages into distinct memory regions.
 *
 * This physically relocates cold pages to a separate memory region
 * to improve TLB locality for hot pages.
 *
 * @param pool Pointer to the memory pool
 * @return Number of pages separated, or 0 on failure
 */
size_t mp_separate_hot_cold_pages(memory_pool_t *pool);

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
memory_pool_t *mp_create_custom(size_t initial_capacity,
                                mp_flags_t flags,
                                const mp_sys_allocator_t *sys_allocator);

/**
 * @brief Creates a memory pool inside a pre-allocated static buffer (zero OS malloc
 * dependency).
 *
 * Useful for embedded systems, kernel modules, or any scenario where
 * dynamic system allocation is not available or desired.
 *
 * @param buffer Pre-allocated buffer memory (must be 8-byte aligned)
 * @param buffer_size Size of the buffer in bytes
 * @param flags Configuration flags
 * @return Pointer to the new memory pool, or NULL on failure
 */
memory_pool_t *mp_create_from_buffer(void *buffer, size_t buffer_size, mp_flags_t flags);

/**
 * @brief Destroys the memory pool and recursively destroys all linked child arenas.
 *
 * Releases all system memory, Slab pages, TLSF pools, and synchronization primitives.
 * After calling this, the pool pointer must not be used again.
 *
 * @param pool Pointer to the memory pool
 */
void mp_destroy(memory_pool_t *pool);

/**
 * @brief Resets the memory pool and all linked child arenas to an empty state.
 *
 * All allocations are logically freed in O(1) time; underlying memory is
 * retained for reuse. This is much faster than freeing individual blocks.
 *
 * @param pool Pointer to the memory pool
 */
void mp_reset(memory_pool_t *pool);

/**
 * @brief Sets a hard maximum memory budget limit on the pool.
 *
 * When active_bytes exceeds this limit, further allocations will attempt
 * to use the emergency reserve (if enabled) or return NULL.
 *
 * @param pool Pointer to the memory pool
 * @param max_bytes Maximum allowed active bytes (0 for unlimited)
 */
void mp_set_memory_limit(memory_pool_t *pool, size_t max_bytes);

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
void mp_set_watermark_callback(memory_pool_t *pool,
                               double high_ratio,
                               double low_ratio,
                               mp_watermark_callback_t cb,
                               void *user_data);

/**
 * @brief Compacts the memory pool by releasing completely free Slab pages back to the OS.
 *
 * Only pages that are entirely unused are returned to the system.
 * When idle page reclamation is enabled, long-idle pages are also returned.
 *
 * @param pool Pointer to the memory pool
 * @return Number of bytes freed back to the OS
 */
size_t mp_compact(memory_pool_t *pool);

/**
 * @brief Configures idle page reclamation parameters.
 *
 * When enabled, completely-free or long-idle Slab pages are eligible for
 * reclamation via mp_compact or mp_reclaim_idle_pages.
 *
 * @param pool Pointer to the memory pool
 * @param enable true to enable idle page reclamation
 * @param timeout_ms Idle timeout in milliseconds (0 = reclaim immediately)
 * @param min_pages Minimum number of idle pages before reclamation triggers
 */
void mp_set_idle_page_reclaim(memory_pool_t *pool,
                              bool enable,
                              uint64_t timeout_ms,
                              size_t min_pages);

/**
 * @brief Reclaims idle Slab pages that have exceeded the configured timeout.
 *
 * Scans all Slab classes for fully free or long-idle pages and returns them
 * to the OS. This is a more aggressive variant of mp_compact that respects
 * the idle timeout configuration.
 *
 * @param pool Pointer to the memory pool
 * @return Number of bytes freed back to the OS
 */
size_t mp_reclaim_idle_pages(memory_pool_t *pool);

/**
 * @brief Returns the number of idle Slab pages eligible for reclamation.
 *
 * Counts pages that are either completely free or have been idle longer than
 * the configured timeout.
 *
 * @param pool Pointer to the memory pool
 * @return Number of idle pages
 */
size_t mp_get_idle_page_count(memory_pool_t *pool);

/**
 * @brief Purges unused Slab pages using Linux madvise MADV_DONTNEED to reduce physical RSS.
 *
 * This advises the kernel that the specified pages are not needed, allowing
 * it to reclaim physical memory without removing the pages from the pool.
 *
 * @param pool Pointer to the memory pool
 * @return Number of bytes purged
 */
size_t mp_purge_lazy(memory_pool_t *pool);

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
int mp_madvise(memory_pool_t *pool, void *addr, size_t length, int advice);

/**
 * @brief Trims and reclaims unused memory capacity back to the OS, recursively for child
 * arenas.
 *
 * Combines mp_compact() and mp_purge_lazy() for maximum memory reclamation.
 *
 * @param pool Pointer to the memory pool
 * @param pad Minimum number of bytes to keep reserved
 * @return Total bytes reclaimed across all arenas
 */
size_t mp_trim(memory_pool_t *pool, size_t pad);

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
void mp_set_event_callback(memory_pool_t *pool, mp_event_callback_t callback, void *user_data);

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
void *mp_alloc_loc(memory_pool_t *pool, size_t size, const char *file, int line, const char *func);

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
void *mp_calloc_loc(
    memory_pool_t *pool, size_t num, size_t size, const char *file, int line, const char *func);

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
void *mp_realloc_loc(
    memory_pool_t *pool, void *ptr, size_t new_size, const char *file, int line, const char *func);

/**
 * @brief Allocates a memory block from the pool.
 *
 * If MP_FLAG_CACHE_ALIGNED is set, this delegates to mp_aligned_alloc with 64-byte alignment.
 *
 * @param pool Pointer to the memory pool
 * @param size Requested size in bytes
 * @return Pointer to the payload, or NULL on failure
 */
void *mp_alloc(memory_pool_t *pool, size_t size);

/**
 * @brief Allocates and zero-initializes memory for an array of elements.
 *
 * @param pool Pointer to the memory pool
 * @param num Number of elements
 * @param size Size of each element in bytes
 * @return Pointer to the allocated payload, or NULL on failure
 */
void *mp_calloc(memory_pool_t *pool, size_t num, size_t size);

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
void *mp_realloc(memory_pool_t *pool, void *ptr, size_t new_size);

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
void *mp_reallocarray(memory_pool_t *pool, void *ptr, size_t nmemb, size_t size);

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
void *mp_reallocarray_loc(memory_pool_t *pool,
                          void *ptr,
                          size_t nmemb,
                          size_t size,
                          const char *file,
                          int line,
                          const char *func);

/**
 * @brief Reallocate an array of memory blocks in batch.
 *
 * For each pointer, attempts in-place expansion when possible (same as mp_realloc).
 * When in-place expansion is not feasible, performs alloc+memcpy+free per element.
 * Successfully reallocated pointers are updated in-place in the ptrs array;
 * failed entries are set to NULL.
 *
 * @param pool     Pointer to the memory pool
 * @param ptrs     Array of pointers to reallocate (updated in-place on success)
 * @param new_sizes Array of new sizes in bytes (one per pointer)
 * @param count    Number of entries in the arrays
 * @return Number of successfully reallocated blocks
 */
size_t mp_realloc_batch(memory_pool_t *pool, void **ptrs, size_t *new_sizes, size_t count);

/**
 * @brief Allocates memory with a specific byte alignment requirement.
 *
 * @param pool Pointer to the memory pool
 * @param alignment Byte alignment (must be power of two, minimum sizeof(void*))
 * @param size Requested payload size in bytes
 * @return Pointer to the aligned payload, or NULL on failure
 */
void *mp_aligned_alloc(memory_pool_t *pool, size_t alignment, size_t size);

/**
 * @brief Frees a memory block back to the pool, performing canary checks and poison fill if
 * enabled.
 *
 * @param pool Pointer to the memory pool
 * @param ptr Pointer to the payload to free
 */
void mp_free(memory_pool_t *pool, void *ptr);

#ifndef MP_THREAD_LOCAL
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
#define MP_THREAD_LOCAL _Thread_local
#elif defined(_MSC_VER)
#define MP_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define MP_THREAD_LOCAL __thread
#else
#define MP_THREAD_LOCAL
#endif
#endif

typedef struct mp_slab_slot {
    struct mp_slab_slot *next;
} mp_slab_slot_t;

#define CMEM_SLAB_CLASS_COUNT 7

/* Per-thread TLSF cache: covers fl indices 6-13 (64B-8KB blocks). */
#define TLSF_CACHE_MIN_FL 6 /* Minimum fl index covered by cache (64B)    */
#define TLSF_CACHE_SIZES 8
#define TLSF_CACHE_MAX_SLOTS 8

/* Per-thread TLSF arena for lock-free hot-path allocations (512B–4MB). */
#define TLSF_ARENA_DEFAULT_SIZE (256 * 1024) /*< 256 KB per thread              */
#define TLSF_ARENA_MIN_SIZE (64 * 1024)      /*< Minimum arena size in bytes    */

/* Per-thread TLSF free-block cache entry. */
typedef struct tlsf_cache_entry {
    void *block;                   /**< Cached tlsf_block_t pointer              */
    void *tpool;                   /**< tlsf_pool_t that owns this block         */
    struct tlsf_cache_entry *next; /**< Next entry in the per-size linked list */
} tlsf_cache_entry_t;

typedef struct {
    memory_pool_t *owner_pool;
    mp_slab_slot_t *slots[CMEM_SLAB_CLASS_COUNT];
    uint16_t counts[CMEM_SLAB_CLASS_COUNT];
    memory_pool_t *bound_arena; /**< Multi-arena bound child (NULL = not bound) */
    /* Per-size TLSF free-block caches. Each slot holds a linked list of
     * tlsf_cache_entry_t allocated from tlsf_entries[]. */
    tlsf_cache_entry_t *tlsf_slots[TLSF_CACHE_SIZES];
    uint8_t tlsf_counts[TLSF_CACHE_SIZES];
    /* Embedded entry storage: TLSF_CACHE_SIZES * TLSF_CACHE_MAX_SLOTS entries. */
    tlsf_cache_entry_t tlsf_entries[TLSF_CACHE_SIZES * TLSF_CACHE_MAX_SLOTS];

    /* Per-thread TLSF arena (lock-free hot path for sizes 64B–4MB). */
    void *tlsf_arena_raw_mem; /**< Raw memory from sys_mem_alloc (for munmap)  */
    void *tlsf_arena_free;    /**< Head of address-ordered free list            */
    size_t tlsf_arena_used;   /**< Bytes currently allocated from arena          */
    size_t tlsf_arena_total;  /**< Total arena capacity (raw_size - block_hdr)   */
} thread_cache_t;

typedef struct mp_block_header {
    uint32_t magic;
    uint8_t alloc_type;
    uint8_t slab_class;
    uint16_t flags;
    size_t requested_size;
    size_t usable_size;
    void *raw_base;
    void *subpool;
    const char *alloc_file;
    int alloc_line;
    const char *alloc_func;
    void *backtrace_addrs[8];
    int backtrace_depth;
    struct mp_block_header *prev;
    struct mp_block_header *next;
} mp_block_header_t;

extern MP_THREAD_LOCAL thread_cache_t tls_cache;
extern const uint8_t cmem_size_to_class[513];

/**
 * @brief Ultra-fast inline small-object allocator for MP_FLAG_FAST_PATH pools.
 */
static inline void *mp_alloc_fast(memory_pool_t *pool, size_t size)
{
    if (__builtin_expect(pool != NULL && size > 0 && size <= 512, 1)) {
        mp_flags_t flags = *(const mp_flags_t *)pool;
        if (__builtin_expect((flags & MP_FLAG_FAST_PATH) != 0, 1)) {
            uint8_t class_idx = cmem_size_to_class[size];
            if (__builtin_expect(tls_cache.counts[class_idx] > 0, 1)) {
                mp_slab_slot_t *slot = tls_cache.slots[class_idx];
                tls_cache.slots[class_idx] = slot->next;
                tls_cache.counts[class_idx]--;

                mp_block_header_t *header = (mp_block_header_t *)slot;
                header->alloc_type = (uint8_t)ALLOC_TYPE_SLAB;
                header->slab_class = class_idx;
                header->raw_base = slot;
                header->subpool = pool;

                return (void *)((uint8_t *)header + sizeof(mp_block_header_t));
            }
        }
    }
    return mp_alloc(pool, size);
}

/**
 * @brief Ultra-fast inline small-object deallocator for MP_FLAG_FAST_PATH pools.
 */
static inline void mp_free_fast(memory_pool_t *pool, void *ptr)
{
    if (__builtin_expect(ptr != NULL && pool != NULL, 1)) {
        mp_flags_t flags = *(const mp_flags_t *)pool;
        if (__builtin_expect((flags & MP_FLAG_FAST_PATH) != 0, 1)) {
            mp_block_header_t *header =
                (mp_block_header_t *)((uint8_t *)ptr - sizeof(mp_block_header_t));
            if (__builtin_expect(header->alloc_type == (uint8_t)ALLOC_TYPE_SLAB, 1)) {
                uint8_t class_idx = header->slab_class;
                if (__builtin_expect(tls_cache.counts[class_idx] < 256, 1)) {
                    mp_slab_slot_t *slot = (mp_slab_slot_t *)header->raw_base;
                    slot->next = tls_cache.slots[class_idx];
                    tls_cache.slots[class_idx] = slot;
                    tls_cache.counts[class_idx]++;
                    return;
                }
            }
        }
    }
    mp_free(pool, ptr);
}

/**
 * @brief Duplicates a null-terminated string into the memory pool.
 *
 * @param pool Pointer to the memory pool
 * @param str Source string to duplicate
 * @return Pointer to the duplicated string, or NULL on failure
 */
char *mp_strdup(memory_pool_t *pool, const char *str);

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
char *
mp_strdup_loc(memory_pool_t *pool, const char *str, const char *file, int line, const char *func);

/**
 * @brief Duplicates a binary memory region into the pool.
 *
 * @param pool Pointer to the memory pool
 * @param src Source memory region
 * @param n Number of bytes to copy
 * @return Pointer to the duplicated memory, or NULL on failure
 */
void *mp_memdup(memory_pool_t *pool, const void *src, size_t n);

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
void *mp_memdup_loc(
    memory_pool_t *pool, const void *src, size_t n, const char *file, int line, const char *func);

/**
 * @brief Formats a string and allocates the result in the pool.
 *
 * @param pool Pointer to the memory pool
 * @param fmt Printf-style format string
 * @return Pointer to the formatted string, or NULL on failure
 */
char *mp_asprintf(memory_pool_t *pool, const char *fmt, ...);

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
char *mp_asprintf_loc(
    memory_pool_t *pool, const char *file, int line, const char *func, const char *fmt, ...);

/**
 * @brief Returns the usable allocated capacity of a pointer block.
 *
 * Validates the pointer before dereferencing to avoid undefined behavior.
 *
 * @param pool Pointer to the memory pool
 * @param ptr Pointer to the payload
 * @return Usable size in bytes, or 0 if invalid
 */
size_t mp_usable_size(memory_pool_t *pool, void *ptr);

/**
 * @brief Returns the requested payload size of an allocated pointer block.
 *
 * Validates the pointer before dereferencing to avoid undefined behavior.
 *
 * @param pool Pointer to the memory pool
 * @param ptr Pointer to the payload
 * @return Requested size in bytes, or 0 if invalid
 */
size_t mp_alloc_size(memory_pool_t *pool, void *ptr);

/**
 * @brief Validates if a pointer belongs to an active allocation in the memory pool.
 *
 * Checks the block header magic and walks the active allocation list.
 *
 * @param pool Pointer to the memory pool
 * @param ptr Pointer to validate
 * @return true if the pointer is valid and active, false otherwise
 */
bool mp_ptr_valid(memory_pool_t *pool, void *ptr);

/**
 * @brief Batch allocates multiple memory blocks of the same size in a single operation.
 *
 * @param pool Pointer to the memory pool
 * @param size Size of each block in bytes
 * @param out_ptrs Output array to store allocated pointers
 * @param count Maximum number of blocks to allocate
 * @return Number of blocks successfully allocated
 */
size_t mp_alloc_batch(memory_pool_t *pool, size_t size, void **out_ptrs, size_t count);

/**
 * @brief Batch frees multiple memory blocks in a single operation.
 *
 * @param pool Pointer to the memory pool
 * @param ptrs Array of pointers to free
 * @param count Number of pointers in the array
 */
void mp_free_batch(memory_pool_t *pool, void **ptrs, size_t count);

/**
 * @brief Audits heap integrity by checking header magics and canary redzones of all active
 * allocations.
 *
 * @param pool Pointer to the memory pool
 * @return true if heap is healthy, false if corruption detected
 */

/* ========================================================================== */
/*  Advanced Diagnostic Utilities                                              */
/* ========================================================================== */

/**
 * @brief Retrieves detailed metadata for a single allocation.
 *
 * This API allows upper-layer applications to inspect the internal state
 * of an allocated block, including its tier, size, source location, and
 * captured backtrace (if MP_FLAG_TRACK_LOCATIONS was enabled).
 *
 * @param pool Pointer to the memory pool
 * @param ptr Payload pointer returned by mp_alloc/mp_calloc/mp_realloc
 * @param info Output structure filled with allocation metadata
 * @return true if ptr is valid and info was filled, false otherwise
 */
bool mp_get_allocation_info(memory_pool_t *pool, void *ptr, mp_allocation_info_t *info);

/**
 * @brief Enumerates all memory regions backing the pool.
 *
 * Fills the caller-provided array with descriptors for all underlying
 * memory regions (Slab pages, TLSF pools, OS fallback mappings).
 *
 * @param pool Pointer to the memory pool
 * @param regions Output array of mp_region_info_t
 * @param max_regions Maximum number of entries the array can hold
 * @return Number of regions written to the array
 */
size_t mp_enumerate_regions(memory_pool_t *pool, mp_region_info_t *regions, size_t max_regions);

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
mp_flags_t mp_reparse_env_flags(memory_pool_t *pool);

/**
 * @brief Returns the current environment configuration generation counter.
 *
 * Incremented each time mp_reparse_env_flags() successfully applies changes.
 * Useful for detecting when runtime config has been updated.
 *
 * @param pool Pointer to the memory pool
 * @return Current generation counter, or 0 if pool is invalid
 */
uint64_t mp_get_env_generation(memory_pool_t *pool);

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
 * @param fragmentation_threshold Fragmentation ratio (0.0-1.0) above which compaction is
 * triggered
 */
void mp_set_auto_compact(memory_pool_t *pool,
                         bool enable,
                         double pressure_threshold,
                         double fragmentation_threshold);

/**
 * @brief Checks if auto-compaction is needed and triggers it if so.
 *
 * This is called internally by mp_alloc() and mp_free().
 *
 * @param pool Pointer to the memory pool
 * @return true if compaction was performed, false otherwise
 */
bool mp_auto_compact_check(memory_pool_t *pool);

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
void mp_record_latency(memory_pool_t *pool, uint64_t latency_ns);

/**
 * @brief Calculates the P99 allocation latency from the histogram.
 *
 * @param pool Pointer to the memory pool
 * @return P99 latency in nanoseconds, or 0 if no samples
 */
uint64_t mp_get_latency_p99(memory_pool_t *pool);

/**
 * @brief Returns the average allocation latency in nanoseconds.
 *
 * @param pool Pointer to the memory pool
 * @return Average latency in nanoseconds, or 0 if no samples
 */
uint64_t mp_get_latency_avg(memory_pool_t *pool);

/**
 * @brief Resets the allocation latency statistics histogram.
 *
 * @param pool Pointer to the memory pool
 */
void mp_reset_latency_stats(memory_pool_t *pool);

/* ========================================================================== */
/*  Per-CPU Lock-Free Freelist                                                  */
/* ========================================================================== */

/**
 * @brief Enables or disables the per-CPU lock-free freelist optimization.
 *
 * When enabled, small-object allocations and frees use per-CPU lock-free
 * freelists to reduce contention on the global Slab class locks.
 *
 * @param pool Pointer to the memory pool
 * @param enable true to enable, false to disable
 */
void mp_set_percpu_freelist(memory_pool_t *pool, bool enable);

/**
 * @brief Returns whether the per-CPU lock-free freelist is enabled.
 *
 * @param pool Pointer to the memory pool
 * @return true if enabled, false otherwise
 */
bool mp_get_percpu_freelist(memory_pool_t *pool);

/**
 * @brief Returns the number of CPUs detected for per-CPU freelist partitioning.
 *
 * @param pool Pointer to the memory pool
 * @return Number of CPUs, or 0 if per-CPU freelist is not initialized
 */
int mp_get_percpu_cpu_count(memory_pool_t *pool);

/* ========================================================================== */
/*  Graceful Degradation                                                       */
/* ========================================================================== */

/**
 * @brief Configures graceful degradation behavior when the pool is over its memory limit.
 *
 * When enabled and the pool exceeds its limit, allocations fall back to system malloc
 * instead of returning NULL. These fallback allocations are tracked separately.
 *
 * @param pool Pointer to the memory pool
 * @param enable true to enable fallback to system malloc on OOM
 */
void mp_set_fallback_on_oom(memory_pool_t *pool, bool enable);

/**
 * @brief Registers a garbage collection callback invoked before OOM rejection.
 *
 * The callback can free non-critical cached data to make room for the allocation.
 *
 * @param pool Pointer to the memory pool
 * @param cb GC callback function pointer
 * @param user_data Optional user data passed to the callback
 */
void mp_set_gc_callback(memory_pool_t *pool, mp_watermark_callback_t cb, void *user_data);

/**
 * @brief Registers an eviction callback for low-priority object eviction under pressure.
 *
 * The callback should evict the specified number of bytes from non-critical caches.
 *
 * @param pool Pointer to the memory pool
 * @param cb Eviction callback function pointer
 * @param user_data Optional user data passed to the callback
 */
void mp_set_eviction_callback(memory_pool_t *pool, mp_watermark_callback_t cb, void *user_data);

/* ========================================================================== */
/*  Memory Error Recovery                                                      */
/* ========================================================================== */

/**
 * @brief Marks the memory pool as dirty after detecting a memory error.
 *
 * A dirty pool will reject new allocations unless fallback mode is enabled.
 *
 * @param pool Pointer to the memory pool
 */
void mp_mark_pool_dirty(memory_pool_t *pool);

/**
 * @brief Clears the dirty state of the memory pool after recovery.
 *
 * @param pool Pointer to the memory pool
 */
void mp_clear_pool_dirty(memory_pool_t *pool);

/**
 * @brief Checks if the memory pool is in a dirty (error) state.
 *
 * @param pool Pointer to the memory pool
 * @return true if pool is dirty, false otherwise
 */
bool mp_is_pool_dirty(memory_pool_t *pool);

/**
 * @brief Registers a callback for memory error recovery.
 *
 * The callback is invoked when canary corruption or double free is detected.
 *
 * @param pool Pointer to the memory pool
 * @param cb Error recovery callback function pointer
 * @param user_data Optional user data passed to the callback
 */
void mp_set_error_recovery_callback(memory_pool_t *pool,
                                    mp_watermark_callback_t cb,
                                    void *user_data);

/**
 * @brief Isolates a bad memory block by marking it as freed and removing it from active
 * tracking.
 *
 * @param pool Pointer to the memory pool
 * @param ptr Pointer to the bad block payload
 * @return true if block was isolated, false if invalid
 */
bool mp_isolate_bad_block(memory_pool_t *pool, void *ptr);

/* ========================================================================== */
/*  Thread-Level Quota & Circuit Breaker                                         */
/* ========================================================================== */

/**
 * @brief Sets a per-thread memory quota limit.
 *
 * When a thread's cumulative allocations exceed this limit, the circuit breaker
 * will trip and reject further allocations from that thread.
 *
 * @param pool Pointer to the memory pool
 * @param quota_bytes Maximum bytes a single thread can allocate (0 for unlimited)
 */
void mp_set_thread_quota(memory_pool_t *pool, size_t quota_bytes);

/**
 * @brief Enables or disables the circuit breaker for thread quota enforcement.
 *
 * @param pool Pointer to the memory pool
 * @param enable true to enable, false to disable
 */
void mp_set_circuit_breaker(memory_pool_t *pool, bool enable);

/**
 * @brief Returns the current thread's allocated bytes from the pool.
 *
 * @param pool Pointer to the memory pool
 * @return Number of bytes allocated by the current thread
 */
size_t mp_get_thread_allocated_bytes(memory_pool_t *pool);

/**
 * @brief Resets the current thread's allocation counter.
 *
 * @param pool Pointer to the memory pool
 */
void mp_reset_thread_quota(memory_pool_t *pool);

/**
 * @brief Returns whether the circuit breaker is tripped for the current thread.
 *
 * @param pool Pointer to the memory pool
 * @return true if tripped, false otherwise
 */
bool mp_is_circuit_breaker_tripped(memory_pool_t *pool);

/* ========================================================================== */
/*  ABI Versioning & Container cgroup Awareness                                  */
/* ========================================================================== */

/**
 * @brief Returns the ABI version of the cmem library.
 *
 * @return ABI version number
 */
uint32_t mp_abi_version(void);

/**
 * @brief Enables or disables container cgroup memory limit awareness.
 *
 * When enabled, the pool will read cgroup memory limits and adjust its
 * behavior accordingly.
 *
 * @param pool Pointer to the memory pool
 * @param enable true to enable, false to disable
 */
void mp_set_cgroup_aware(memory_pool_t *pool, bool enable);

/**
 * @brief Returns the detected cgroup memory limit in bytes.
 *
 * @param pool Pointer to the memory pool
 * @return Cgroup memory limit in bytes, or 0 if not available
 */
size_t mp_get_cgroup_mem_limit(memory_pool_t *pool);

/* ========================================================================== */
/*  Encrypted Memory Support                                                   */
/* ========================================================================== */

/**
 * @brief Locks a memory region into RAM to prevent swapping to disk.
 *
 * Uses mlock() to ensure sensitive data never leaves physical memory.
 *
 * @param pool Pointer to the memory pool
 * @param addr Start address of the memory region
 * @param length Length of the memory region in bytes
 * @return 0 on success, -1 on failure
 */
int mp_lock_memory(memory_pool_t *pool, void *addr, size_t length);

/**
 * @brief Unlocks a previously locked memory region.
 *
 * @param pool Pointer to the memory pool
 * @param addr Start address of the memory region
 * @param length Length of the memory region in bytes
 * @return 0 on success, -1 on failure
 */
int mp_unlock_memory(memory_pool_t *pool, void *addr, size_t length);

/**
 * @brief Protects a memory region from being included in core dumps.
 *
 * Uses madvise(MADV_DONTDUMP) on Linux to exclude memory from crash dumps.
 *
 * @param pool Pointer to the memory pool
 * @param addr Start address of the memory region
 * @param length Length of the memory region in bytes
 * @return 0 on success, -1 on failure
 */
int mp_protect_from_dump(memory_pool_t *pool, void *addr, size_t length);

/**
 * @brief Securely zeroes a memory region to prevent data remanence.
 *
 * Uses a volatile function pointer to prevent compiler optimization
 * and ensure the zeroing actually occurs.
 *
 * @param pool Pointer to the memory pool
 * @param ptr Pointer to the memory region
 * @param length Length of the memory region in bytes
 */
void mp_secure_zero(memory_pool_t *pool, void *ptr, size_t length);

/**
 * @brief Enables or disables encrypted memory mode for the pool.
 *
 * When enabled, all system allocations from this pool will be locked
 * and protected from core dumps.
 *
 * @param pool Pointer to the memory pool
 * @param enable true to enable encrypted memory mode
 */
void mp_set_encrypted_memory(memory_pool_t *pool, bool enable);

/* ========================================================================== */
/*  AddressSanitizer Integration Layer                                          */
/* ========================================================================== */

/**
 * @brief Checks if AddressSanitizer (ASan) is currently active.
 *
 * @return true if ASan is enabled, false otherwise
 */
bool mp_asan_is_enabled(void);

/**
 * @brief Reports a custom memory error to AddressSanitizer.
 *
 * @param pool Pointer to the memory pool
 * @param ptr Pointer to the memory location
 * @param size Size of the memory region
 * @param is_write true if the error is a write, false for read
 */
void mp_asan_report_error(memory_pool_t *pool, void *ptr, size_t size, bool is_write);

/**
 * @brief Checks a memory region for ASan errors.
 *
 * @param pool Pointer to the memory pool
 * @param ptr Pointer to the memory region
 * @param size Size of the memory region
 * @return true if the region is valid, false if ASan detects an error
 */
bool mp_asan_check_memory(memory_pool_t *pool, void *ptr, size_t size);

/**
 * @brief Enables or disables ASan-compatible mode for the pool.
 *
 * When enabled, cmem will coordinate with ASan's shadow memory
 * for improved error detection.
 *
 * @param pool Pointer to the memory pool
 * @param enable true to enable ASan integration
 */
void mp_set_asan_integration(memory_pool_t *pool, bool enable);

/* ========================================================================== */
/*  Online Pool Expansion                                                     */
/* ========================================================================== */

/**
 * @brief Expands the memory pool by adding additional capacity without service interruption.
 *
 * This creates a new TLSF pool and links it to the existing pool, allowing
 * concurrent allocations to continue during the expansion.
 *
 * @param pool Pointer to the memory pool
 * @param additional_bytes Number of additional bytes to add
 * @return true on success, false on failure
 */
bool mp_expand_pool(memory_pool_t *pool, size_t additional_bytes);

/**
 * @brief Checks if the pool can be expanded further.
 *
 * @param pool Pointer to the memory pool
 * @return true if expansion is possible, false otherwise
 */
bool mp_can_expand(memory_pool_t *pool);

/**
 * @brief Returns the total expandable capacity of the pool.
 *
 * @param pool Pointer to the memory pool
 * @return Total bytes that can still be added
 */
size_t mp_get_expandable_size(memory_pool_t *pool);

#include "cmem_arena.h"
#include "cmem_frame.h"
#include "cmem_ring.h"
#include "cmem_snapshot.h"
#include "cmem_tlsf.h"
#include "cmem_typed_pool.h"

#if !defined(CMEM_DISABLE_DIAGNOSTICS) &&                                                          \
    (!defined(CMEM_ENABLE_DIAGNOSTICS) || CMEM_ENABLE_DIAGNOSTICS == 1)
#include "cmem_diag.h"
#include "cmem_metrics.h"
#endif

#ifdef MP_ENABLE_LOCATION_MACROS
#define mp_alloc(pool, sz) mp_alloc_loc(pool, sz, __FILE__, __LINE__, __func__)
#define mp_calloc(pool, num, sz) mp_calloc_loc(pool, num, sz, __FILE__, __LINE__, __func__)
#define mp_realloc(pool, ptr, sz) mp_realloc_loc(pool, ptr, sz, __FILE__, __LINE__, __func__)
#endif

#ifdef __cplusplus
}
#endif

#endif // CMEM_H
