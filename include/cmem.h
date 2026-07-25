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
    MP_FLAG_POISON_ON_FREE     = (1 << 6), /**< Poison freed memory with 0xDD byte pattern (UAF protection) */
    MP_FLAG_CACHE_ALIGNED      = (1 << 7), /**< Force 64-byte Cache Line alignment to prevent False Sharing */
    MP_FLAG_GUARD_PAGES        = (1 << 8), /**< Add PROT_NONE Guard Pages to trap out-of-bounds page faults */
    MP_FLAG_SHARED_MEMORY      = (1 << 9), /**< POSIX Shared Memory IPC Mode (/dev/shm zero-copy) */
    MP_FLAG_HUGE_PAGES         = (1 << 10) /**< Use Linux HugePages (2MB/1GB MAP_HUGETLB) for TLB performance */
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
    MP_EVENT_RESET,
    MP_EVENT_COMPACT,
    MP_EVENT_OOM
} mp_event_type_t;

typedef struct memory_pool memory_pool_t;
typedef struct cmem_ring_buffer cmem_ring_buffer_t;
typedef struct mp_typed_pool mp_typed_pool_t;
typedef struct cmem_frame_arena cmem_frame_arena_t;

/**
 * Event Callback function pointer for telemetry profiling.
 */
typedef void (*mp_event_callback_t)(memory_pool_t* pool, mp_event_type_t event, void* ptr, size_t size, void* user_data);

/**
 * Watermark Alert Callback function pointer.
 */
typedef void (*mp_watermark_callback_t)(memory_pool_t* pool, bool is_high_watermark, size_t current_bytes, size_t limit_bytes, void* user_data);

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

/**
 * @brief Enables an emergency fallback reserve cushion for critical OOM scenarios.
 */
bool mp_enable_emergency_reserve(memory_pool_t* pool, size_t reserve_bytes);

/**
 * @brief Binds memory pool backing allocations to a specific Linux NUMA CPU node.
 */
bool mp_set_numa_node(memory_pool_t* pool, int numa_node);

/**
 * @brief Parses CMEM_CONF environment variable string and returns merged configuration flags.
 */
mp_flags_t mp_parse_env_flags(mp_flags_t default_flags);

/**
 * @brief Creates a game & graphics dual ping-pong frame arena allocator.
 */
cmem_frame_arena_t* mp_frame_arena_create(size_t frame_capacity);

/**
 * @brief Allocates temporary memory for the current frame.
 */
void* mp_frame_alloc(cmem_frame_arena_t* farena, size_t size);

/**
 * @brief Ends the current frame and swaps ping-pong buffers with O(1) cursor reset.
 */
void mp_frame_end(cmem_frame_arena_t* farena);

/**
 * @brief Destroys the frame arena allocator instance.
 */
void mp_frame_arena_destroy(cmem_frame_arena_t* farena);

/**
 * @brief Creates a 0-overhead fixed-size object pool allocator.
 */
mp_typed_pool_t* mp_typed_pool_create(size_t elem_size, size_t capacity);

/**
 * @brief Allocates an object pointer from the typed object pool with 0 header overhead.
 */
void* mp_typed_alloc(mp_typed_pool_t* tpool);

/**
 * @brief Returns an object pointer back to the typed object pool.
 */
void mp_typed_free(mp_typed_pool_t* tpool, void* ptr);

/**
 * @brief Destroys the typed object pool instance.
 */
void mp_typed_pool_destroy(mp_typed_pool_t* tpool);

/**
 * @brief Creates a new cmem memory pool instance using default OS memory.
 */
memory_pool_t* mp_create(size_t initial_capacity, mp_flags_t flags);

/**
 * @brief Creates a lock-free DPDK-style Ring Buffer Allocator.
 */
cmem_ring_buffer_t* mp_ring_create(size_t slot_size, size_t capacity);

/**
 * @brief Allocates a slot pointer from the lock-free ring buffer.
 */
void* mp_ring_alloc(cmem_ring_buffer_t* ring);

/**
 * @brief Returns a slot pointer back to the lock-free ring buffer.
 */
bool mp_ring_free(cmem_ring_buffer_t* ring, void* ptr);

/**
 * @brief Destroys the lock-free ring buffer allocator instance.
 */
void mp_ring_destroy(cmem_ring_buffer_t* ring);

/**
 * @brief Creates a POSIX shared memory pool in /dev/shm for zero-copy Inter-Process Communication (IPC).
 */
memory_pool_t* mp_create_shared(const char* shm_name, size_t capacity, mp_flags_t flags);

/**
 * @brief Destroys a shared memory pool and unlinks the POSIX shared memory segment.
 */
void mp_destroy_shared(memory_pool_t* pool, const char* shm_name);

/**
 * @brief Creates a child memory pool linked to a parent pool.
 */
memory_pool_t* mp_create_child(memory_pool_t* parent, size_t initial_capacity, mp_flags_t flags, const char* arena_name);

/**
 * @brief Creates a memory pool instance using a custom backing allocator.
 */
memory_pool_t* mp_create_custom(size_t initial_capacity, mp_flags_t flags, const mp_sys_allocator_t* sys_allocator);

/**
 * @brief Creates a memory pool inside a pre-allocated static buffer (Zero OS malloc dependency).
 */
memory_pool_t* mp_create_from_buffer(void* buffer, size_t buffer_size, mp_flags_t flags);

/**
 * @brief Destroys the memory pool and recursively destroys all linked child arenas.
 */
void mp_destroy(memory_pool_t* pool);

/**
 * @brief Resets the memory pool and all linked child arenas.
 */
void mp_reset(memory_pool_t* pool);

/**
 * @brief Sets a hard maximum memory budget limit on the pool.
 */
void mp_set_memory_limit(memory_pool_t* pool, size_t max_bytes);

/**
 * @brief Configures high and low watermark threshold alert callbacks.
 */
void mp_set_watermark_callback(memory_pool_t* pool, double high_ratio, double low_ratio, mp_watermark_callback_t cb, void* user_data);

/**
 * @brief Compacts memory pool by releasing empty, unused system memory pages back to OS.
 */
size_t mp_compact(memory_pool_t* pool);

/**
 * @brief Purges unused memory pages using Linux madvise MADV_DONTNEED / MADV_FREE to reduce physical RSS.
 */
size_t mp_purge_lazy(memory_pool_t* pool);

/**
 * @brief Registers an event callback function for real-time profiling and debugging.
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
void* mp_reallocarray(memory_pool_t* pool, void* ptr, size_t nmemb, size_t size);
void* mp_reallocarray_loc(memory_pool_t* pool, void* ptr, size_t nmemb, size_t size, const char* file, int line, const char* func);
void* mp_aligned_alloc(memory_pool_t* pool, size_t alignment, size_t size);
void mp_free(memory_pool_t* pool, void* ptr);

/**
 * @brief Returns the usable allocated capacity of a pointer block.
 */
size_t mp_usable_size(memory_pool_t* pool, void* ptr);

/**
 * @brief Returns the requested payload size of an allocated pointer block.
 */
size_t mp_alloc_size(memory_pool_t* pool, void* ptr);

/**
 * @brief Validates if a pointer belongs to an active allocation in the memory pool.
 */
bool mp_ptr_valid(memory_pool_t* pool, void* ptr);

/**
 * @brief Batch allocates multiple memory blocks in a single operation.
 */
size_t mp_alloc_batch(memory_pool_t* pool, size_t size, void** out_ptrs, size_t count);

/**
 * @brief Batch frees multiple memory blocks in a single operation.
 */
void mp_free_batch(memory_pool_t* pool, void** ptrs, size_t count);

/**
 * @brief Audits heap integrity by checking header magics and canary redzones of all active allocations.
 */
bool mp_audit_heap(memory_pool_t* pool);

/**
 * @brief Generates a detailed memory leak analysis report.
 */
size_t mp_analyze_leaks(memory_pool_t* pool, char* report_buf, size_t max_len);

/**
 * @brief Exports memory leak analysis report to a text file.
 */
bool mp_export_leak_report(memory_pool_t* pool, const char* filepath);

/**
 * @brief Exports an interactive visual HTML report.
 */
bool mp_export_html_report(memory_pool_t* pool, const char* filepath);

/**
 * @brief Formats and dumps memory pool metrics into Prometheus text exposition format.
 */
size_t mp_export_prometheus_metrics(memory_pool_t* pool, char* out_buf, size_t max_len);

/**
 * @brief Exports a binary post-mortem memory crash snapshot dump to file.
 */
bool mp_export_binary_snapshot(memory_pool_t* pool, const char* filepath);

/**
 * @brief Parses a binary post-mortem memory snapshot file into readable text report.
 */
bool mp_parse_binary_snapshot(const char* filepath, char* out_report, size_t max_len);

/**
 * @brief Compares two binary snapshot files (A vs B) and generates an incremental leak diff report.
 */
bool mp_diff_snapshots(const char* snapshot_a_path, const char* snapshot_b_path, char* out_report, size_t max_len);

/**
 * @brief Prints ASCII allocation size distribution histogram chart.
 */
void mp_dump_histogram(memory_pool_t* pool);

/**
 * @brief Retrieves current statistical metrics of the memory pool.
 */
void mp_get_stats(memory_pool_t* pool, mp_stats_t* stats);

/**
 * @brief Prints detailed summary and health status of the memory pool to stdout.
 */
void mp_dump_info(memory_pool_t* pool);

/**
 * @brief Dumps memory pool tree hierarchy to stdout.
 */
void mp_dump_tree_info(memory_pool_t* pool);

/**
 * @brief Dumps memory pool stats into JSON format buffer for telemetry monitoring.
 */
size_t mp_dump_json_stats(memory_pool_t* pool, char* buf, size_t max_len);

/**
 * @brief Checks if there are any un-freed memory allocations (memory leaks).
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

#endif // CMEM_H
