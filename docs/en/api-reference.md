# cmem API Reference Documentation

This document provides detailed descriptions, parameters, return values, and usage examples for all public APIs of cmem.

## Table of Contents

1. [Memory Pool Lifecycle](#1-memory-pool-lifecycle)
2. [Memory Allocation and Deallocation](#2-memory-allocation-and-deallocation)
3. [Introspection Queries and Metadata](#3-introspection-queries-and-metadata)
4. [Convenience Helper Functions](#4-convenience-helper-functions)
5. [Memory Compaction and Reclamation](#5-memory-compaction-and-reclamation)
6. [Statistics, Diagnostics, and Monitoring](#6-statistics-diagnostics-and-monitoring)
7. [Leak Detection and Heap Auditing](#7-leak-detection-and-heap-auditing)
8. [Advanced Feature APIs](#8-advanced-feature-apis)
9. [Configuration Flags](#9-configuration-flags)
10. [C++ API](#10-c-api)
11. [Event Types](#11-event-types)

---

## 1. Memory Pool Lifecycle

### mp_create

```c
memory_pool_t* mp_create(size_t initial_capacity, mp_flags_t flags);
```

Creates and initializes a standard memory pool instance.

**Parameters:**
- `initial_capacity`: Initial memory capacity in bytes; 0 means use the default value
- `flags`: Configuration flags, combine multiple flags with `|`

**Return Value:**
- Success: Returns a `memory_pool_t*` pointer
- Failure: Returns `NULL`

**Example:**
```c
memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_THREAD_SAFE | MP_FLAG_DEBUG_CANARY);
```

---

### mp_create_child

```c
memory_pool_t* mp_create_child(memory_pool_t* parent, size_t capacity, mp_flags_t flags, const char* name);
```

Creates a child Arena nested under a parent memory pool.

**Parameters:**
- `parent`: Pointer to the parent memory pool
- `capacity`: Initial capacity of the child Arena
- `flags`: Configuration flags
- `name`: Name of the child Arena (used for diagnostic output)

**Return Value:**
- Success: Returns a pointer to the child Arena
- Failure: Returns `NULL`

**Features:**
- Destroying/resetting a child Arena recursively affects all descendants
- Supports unlimited nesting levels

---

### mp_create_from_buffer

```c
memory_pool_t* mp_create_from_buffer(void* buffer, size_t buffer_size, mp_flags_t flags);
```

Creates a memory pool within a pre-allocated static buffer (zero OS malloc dependency).

**Parameters:**
- `buffer`: Pre-allocated buffer (must be 8-byte aligned)
- `buffer_size`: Size of the buffer
- `flags`: Configuration flags

**Return Value:**
- Success: Returns a memory pool pointer
- Failure: Returns `NULL`

**Applicable Scenarios:**
- Embedded systems
- Kernel modules
- Environments without dynamic memory allocation

---

### mp_create_shared

```c
memory_pool_t* mp_create_shared(const char* shm_name, size_t capacity, mp_flags_t flags);
```

Creates a POSIX shared memory pool (`/dev/shm` zero-copy IPC).

**Parameters:**
- `shm_name`: Shared memory object name (e.g., `"/my_ipc_pool"`)
- `capacity`: Capacity in bytes
- `flags`: Configuration flags

**Return Value:**
- Success: Returns a memory pool pointer
- Failure: Returns `NULL`

---

### mp_create_custom

```c
memory_pool_t* mp_create_custom(size_t initial_capacity, mp_flags_t flags, const mp_sys_allocator_t* sys_allocator);
```

Creates a memory pool using a custom system allocator.

**Parameters:**
- `initial_capacity`: Initial capacity
- `flags`: Configuration flags
- `sys_allocator`: Custom allocator function table; `NULL` uses the default system allocator

**Return Value:**
- Success: Returns a memory pool pointer
- Failure: Returns `NULL`

---

### mp_destroy

```c
void mp_destroy(memory_pool_t* pool);
```

Destroys the memory pool and recursively destroys all child Contexts.

**Parameters:**
- `pool`: Memory pool pointer

**Note:**
- The `pool` pointer must not be used after destruction
- Automatically releases all Slab Pages, TLSF Pools, and synchronization primitives

---

### mp_destroy_shared

```c
void mp_destroy_shared(memory_pool_t* pool, const char* shm_name);
```

Destroys a shared memory pool and unlinks the POSIX shared memory segment.

**Parameters:**
- `pool`: Memory pool pointer
- `shm_name`: Shared memory name

---

### mp_reset

```c
void mp_reset(memory_pool_t* pool);
```

$O(1)$ batch reset of the memory pool and all its child Contexts.

**Features:**
- Logically releases all allocated blocks
- Underlying memory is retained for reuse
- Much faster than calling `mp_free` individually

---

## 2. Memory Allocation and Deallocation

### mp_alloc

```c
void* mp_alloc(memory_pool_t* pool, size_t size);
```

Allocates a memory block of the specified number of bytes from the memory pool.

**Parameters:**
- `pool`: Memory pool pointer
- `size`: Number of bytes requested

**Return Value:**
- Success: Returns a pointer to the payload
- Failure: Returns `NULL`

**Allocation Strategy:**
- `size <= 512B`: Slab Pool
- `512B < size <= 4MB`: TLSF Allocator
- `size > 4MB`: Direct OS Fallback

---

### mp_calloc

```c
void* mp_calloc(memory_pool_t* pool, size_t num, size_t size);
```

Allocates a memory block and automatically fills it with zero data.

**Parameters:**
- `pool`: Memory pool pointer
- `num`: Number of elements
- `size`: Size of each element

**Return Value:**
- Success: Returns a pointer to zeroed memory
- Failure: Returns `NULL`

---

### mp_realloc

```c
void* mp_realloc(memory_pool_t* pool, void* ptr, size_t new_size);
```

Reallocates a memory block to a new size.

**Parameters:**
- `pool`: Memory pool pointer
- `ptr`: Existing allocation pointer (`NULL` means new allocation)
- `new_size`: New requested size

**Return Value:**
- Success: Returns a new pointer
- Failure: Returns `NULL` (original pointer remains unchanged)

**Optimization:**
- TLSF blocks attempt In-Place Expansion
- Avoids unnecessary `memcpy`

---

### mp_reallocarray

```c
void* mp_reallocarray(memory_pool_t* pool, void* ptr, size_t nmemb, size_t size);
```

Overflow-safe array reallocation.

**Safety Features:**
- Explicitly checks `nmemb * size > SIZE_MAX` for multiplication overflow
- Prevents allocation underflow caused by integer overflow

---

### mp_aligned_alloc

```c
void* mp_aligned_alloc(memory_pool_t* pool, size_t alignment, size_t size);
```

Allocates a memory block aligned to the specified boundary.

**Parameters:**
- `pool`: Memory pool pointer
- `alignment`: Alignment in bytes (must be a power of 2, minimum `sizeof(void*)`)
- `size`: Number of bytes requested

**Return Value:**
- Success: Returns an aligned pointer
- Failure: Returns `NULL`

---

### mp_free

```c
void* mp_free(memory_pool_t* pool, void* ptr);
```

Frees a memory block and returns it to the memory pool.

**Parameters:**
- `pool`: Memory pool pointer
- `ptr`: Pointer to free (`NULL` returns immediately)

**Debug Checks (if enabled):**
- Canary out-of-bounds verification
- Magic number verification
- Double-free detection

---

### mp_alloc_batch

```c
size_t mp_alloc_batch(memory_pool_t* pool, size_t size, void** out_ptrs, size_t count);
```

High-throughput single-call batch allocation of N memory blocks.

**Parameters:**
- `pool`: Memory pool pointer
- `size`: Size of each block
- `out_ptrs`: Output pointer array
- `count`: Number of blocks requested

**Return Value:**
- Actual number of blocks successfully allocated (may be less than count)

---

### mp_free_batch

```c
void mp_free_batch(memory_pool_t* pool, void** ptrs, size_t count);
```

Single-call batch deallocation of N memory blocks.

**Parameters:**
- `pool`: Memory pool pointer
- `ptrs`: Pointer array
- `count`: Number of blocks

---

## 3. Introspection Queries and Metadata

### mp_usable_size

```c
size_t mp_usable_size(memory_pool_t* pool, void* ptr);
```

Queries the actual usable byte capacity of an allocated memory block.

**Return Value:**
- Valid pointer: Returns the number of usable bytes
- Invalid pointer: Returns 0

---

### mp_alloc_size

```c
size_t mp_alloc_size(memory_pool_t* pool, void* ptr);
```

Queries the user-requested byte size of an allocated memory block.

**Return Value:**
- Valid pointer: Returns the requested size
- Invalid pointer: Returns 0

---

### mp_ptr_valid

```c
bool mp_ptr_valid(memory_pool_t* pool, void* ptr);
```

Validates whether a pointer is a valid active pointer in the current pool.

**Return Value:**
- `true`: Pointer is valid and in an active allocation state
- `false`: Pointer is invalid or has been freed

---

### mp_preferred_size

```c
size_t mp_preferred_size(size_t size);
```

Calculates the best-matching Size Class aligned size.

**Example:**
```c
mp_preferred_size(12) -> 16
mp_preferred_size(40) -> 64
mp_preferred_size(600) -> 608 (TLSF aligned)
```

---

### mp_set_name / mp_get_name

```c
void mp_set_name(memory_pool_t* pool, const char* name);
const char* mp_get_name(memory_pool_t* pool);
```

Sets/gets the human-readable name of the memory pool (used for diagnostic output).

---

### mp_get_parent / mp_get_child_count

```c
memory_pool_t* mp_get_parent(memory_pool_t* pool);
size_t mp_get_child_count(memory_pool_t* pool);
```

Gets the parent Arena pointer and the number of child Arenas.

---

### mp_get_allocation_info

```c
bool mp_get_allocation_info(memory_pool_t* pool, void* ptr, mp_allocation_info_t* info);
```

Retrieves detailed metadata for a single allocation including tier, size, source location, and backtrace.

**Parameters:**
- `pool`: Memory pool pointer
- `ptr`: Payload pointer returned by mp_alloc/calloc/realloc
- `info`: Output structure filled with allocation metadata

**Return Value:**
- `true` if ptr is valid and info was filled, `false` otherwise

---

### mp_enumerate_regions

```c
size_t mp_enumerate_regions(memory_pool_t* pool, mp_region_info_t* regions, size_t max_regions);
```

Enumerates all memory regions backing the pool (Slab pages, TLSF pools, OS fallback mappings).

**Parameters:**
- `pool`: Memory pool pointer
- `regions`: Output array of mp_region_info_t
- `max_regions`: Maximum number of entries the array can hold

**Return Value:**
- Number of regions written to the array

---

## 4. Convenience Helper Functions

### mp_strdup

```c
char* mp_strdup(memory_pool_t* pool, const char* str);
```

Deep copies a null-terminated string into the memory pool.

**Return Value:**
- Success: Returns a pointer to the new string
- Failure: Returns `NULL`

---

### mp_memdup

```c
void* mp_memdup(memory_pool_t* pool, const void* src, size_t n);
```

Deep copies N bytes of binary data into the memory pool.

---

### mp_asprintf

```c
char* mp_asprintf(memory_pool_t* pool, const char* fmt, ...);
```

Formats a string and allocates storage for it in the memory pool.

**Example:**
```c
char* msg = mp_asprintf(pool, "Arena [%s] active: %zu", mp_get_name(pool), count);
```

---

## 5. Memory Compaction and Reclamation

### mp_trim

```c
size_t mp_trim(memory_pool_t* pool, size_t pad);
```

Deep memory compaction and page return.

**Parameters:**
- `pad`: Minimum bytes to retain

**Return Value:**
- Actual number of bytes reclaimed

**Behavior:**
- Releases fully free Slab Pages
- Calls `mp_compact` + `mp_purge_lazy`

---

### mp_compact

```c
size_t mp_compact(memory_pool_t* pool);
```

Compacts the memory pool, releasing free Slab Pages back to the OS.

---

### mp_purge_lazy

```c
size_t mp_purge_lazy(memory_pool_t* pool);
```

Deferred physical memory page RSS cleanup (Linux `MADV_DONTNEED` / `MADV_FREE`).

---

### mp_madvise

```c
int mp_madvise(memory_pool_t* pool, void* addr, size_t length, int advice);
```

Cross-platform memory advice wrapper.

**Platform Differences:**
- Linux: Calls `madvise()`
- Windows: Calls `VirtualAlloc(MEM_RESET)`

---

### mp_resident

```c
size_t mp_resident(memory_pool_t* pool);
```

Gets the current system RSS physical resident byte count of the memory pool.

---

### mp_freeable

```c
size_t mp_freeable(memory_pool_t* pool);
```

Gets the number of free page bytes in the current pool that can be reclaimed by `mp_trim`.

---

## 6. Statistics, Diagnostics, and Monitoring

### mp_get_stats

```c
void mp_get_stats(memory_pool_t* pool, mp_stats_t* stats);
```

Gets a statistics snapshot.

---

### mp_pressure

```c
double mp_pressure(memory_pool_t* pool);
```

Gets the memory pool usage pressure ratio [0.0, 1.0].

**Calculation:**
```
pressure = active_bytes / max(limit, total_pool_size)
```

---

### mp_reset_stats

```c
void mp_reset_stats(memory_pool_t* pool);
```

Resets cumulative QPS, operation counts, and Peak values.

---

### mp_dump_info

```c
void mp_dump_info(memory_pool_t* pool);
```

Prints detailed summary and health status to stdout.

---

### mp_dump_tree_info

```c
void mp_dump_tree_info(memory_pool_t* pool);
```

Prints the memory pool tree hierarchy structure.

**Output Example:**
```
|- [Arena: RootArena] Active Bytes: 128 B, Active Allocations: 1
  |- [Arena: Child1] Active Bytes: 256 B, Active Allocations: 1
```

---

### mp_dump_histogram

```c
void mp_dump_histogram(memory_pool_t* pool);
```

Prints an ASCII allocation size distribution histogram.

---

### mp_dump_json_stats

```c
size_t mp_dump_json_stats(memory_pool_t* pool, char* buf, size_t max_len);
```

Exports statistics in JSON format.

---

### mp_export_html_report

```c
bool mp_export_html_report(memory_pool_t* pool, const char* filepath);
```

Exports an interactive visual HTML profiling and leak dashboard report.

---

### mp_export_prometheus_metrics

```c
size_t mp_export_prometheus_metrics(memory_pool_t* pool, char* out_buf, size_t max_len);
```

Exports metrics in Prometheus / OpenTelemetry standard format.

**Output Example:**
```
# HELP cmem_active_bytes Active memory bytes
# TYPE cmem_active_bytes gauge
cmem_active_bytes{arena="RootArena"} 3072
```

---

### mp_export_binary_snapshot

```c
bool mp_export_binary_snapshot(memory_pool_t* pool, const char* filepath);
```

Exports a binary crash-time memory snapshot.

---

### mp_parse_binary_snapshot

```c
bool mp_parse_binary_snapshot(const char* filepath, char* out_report, size_t max_len);
```

Parses a binary snapshot into a readable text report.

---

### mp_diff_snapshots

```c
bool mp_diff_snapshots(const char* snapshot_a_path, const char* snapshot_b_path, char* out_report, size_t max_len);
```

Compares two memory snapshots and generates an incremental leak difference report.

---

## 7. Diagnostic CLI Tools

### cmem-inspect

Real-time in-process diagnostic CLI that links against `libcmem`.

```bash
cmem-inspect <subcommand> [options]
```

Subcommands:
- `leaks` — leak analysis
- `audit` — heap audit
- `stats` — pool statistics
- `tree` — arena tree dump
- `histogram` — allocation size histogram
- `snapshot` — export binary snapshot
- `diff` — diff two binary snapshots
- `html` — export HTML report

Common options:
- `--json` — output in JSON format
- `--output <path>` — write output to file
- `--quiet` — suppress non-essential output

### cmem-analyze

Standalone offline analyzer for `.cmem_dump` binary snapshots.

```bash
cmem-analyze <subcommand> [options] <input>
```

Subcommands:
- `report` — full analysis report
- `top` — top allocations by size/count
- `summary` — concise summary statistics
- `validate` — validate snapshot integrity
- `diff` — diff two snapshots

Common options:
- `--json` — output in JSON format
- `--html` — generate HTML report
- `--output <path>` — write output to file
- `--quiet` — suppress non-essential output
- `--top <n>` — limit top N results

Build both tools with `make tools`.

---

## 7. Leak Detection and Heap Auditing

### mp_audit_heap

```c
bool mp_audit_heap(memory_pool_t* pool);
```

Traverses heap memory and actively audits Redzone Canary out-of-bounds.

**Return Value:**
- `true`: Heap is healthy
- `false`: Corruption detected

---

### mp_analyze_leaks

```c
size_t mp_analyze_leaks(memory_pool_t* pool, char* report_buf, size_t max_len);
```

Generates a leak report containing code locations (file:line) and call stacks.

**Return Value:**
- Number of bytes written to report_buf

---

### mp_export_leak_report

```c
bool mp_export_leak_report(memory_pool_t* pool, const char* filepath);
```

Exports a leak analysis report to a text file.

---

### mp_check_leaks

```c
bool mp_check_leaks(memory_pool_t* pool);
```

Checks whether there are any unfreed memory allocations.

**Return Value:**
- `true`: No leaks
- `false`: Leaks exist

---

## 8. Advanced Feature APIs

### Runtime Configuration Hot Reload

```c
mp_flags_t mp_reparse_env_flags(memory_pool_t* pool);
uint64_t mp_get_env_generation(memory_pool_t* pool);
```

Re-parses the `CMEM_CONF` environment variable at runtime.

---

### Auto-Compact Triggering

```c
void mp_set_auto_compact(memory_pool_t* pool, bool enable, double pressure_threshold, double fragmentation_threshold);
bool mp_auto_compact_check(memory_pool_t* pool);
```

Configures and triggers auto-compaction based on pressure/fragmentation.

---

### Arena Quota

```c
void mp_set_arena_quota(memory_pool_t* pool, size_t quota_bytes, mp_watermark_callback_t cb, void* user_data);
bool mp_check_arena_quota(memory_pool_t* pool);
```

Sets a per-Arena memory quota and overflow callback.

---

### Latency Statistics

```c
void mp_record_latency(memory_pool_t* pool, uint64_t latency_ns);
uint64_t mp_get_latency_p99(memory_pool_t* pool);
uint64_t mp_get_latency_avg(memory_pool_t* pool);
void mp_reset_latency_stats(memory_pool_t* pool);
```

Records and queries allocation latency statistics.

---

### Configurable Slab Class

```c
bool mp_set_slab_classes(memory_pool_t* pool, const size_t* sizes, size_t count);
size_t mp_get_slab_classes(memory_pool_t* pool, size_t* out_sizes, size_t max_count);
size_t mp_get_slab_class_count(memory_pool_t* pool);
size_t mp_preferred_size_for_pool(memory_pool_t* pool, size_t size);
```

Custom Slab size class table.

---

### Event Log

```c
mp_event_log_t* mp_event_log_create(size_t capacity);
bool mp_event_log_record(mp_event_log_t* log, mp_event_type_t event_type, void* ptr, size_t size);
bool mp_event_log_consume(mp_event_log_t* log, mp_event_log_entry_t* entry);
size_t mp_event_log_pending(mp_event_log_t* log);
void mp_event_log_clear(mp_event_log_t* log);
void mp_event_log_destroy(mp_event_log_t* log);
```

Structured event log Ring Buffer.

---

### pprof Export

```c
size_t mp_export_pprof(memory_pool_t* pool, char* out_buf, size_t max_len);
```

Exports pprof-compatible text format.

---

### Per-CPU Freelist

```c
void mp_set_percpu_freelist(memory_pool_t* pool, bool enable);
bool mp_get_percpu_freelist(memory_pool_t* pool);
int mp_get_percpu_cpu_count(memory_pool_t* pool);
```

Per-CPU lock-free freelist configuration.

---

### Graceful Degradation

```c
void mp_set_fallback_on_oom(memory_pool_t* pool, bool enable);
void mp_set_gc_callback(memory_pool_t* pool, mp_watermark_callback_t cb, void* user_data);
void mp_set_eviction_callback(memory_pool_t* pool, mp_watermark_callback_t cb, void* user_data);
```

OOM fallback, GC callback, and eviction callback.

---

### Memory Error Recovery

```c
void mp_mark_pool_dirty(memory_pool_t* pool);
void mp_clear_pool_dirty(memory_pool_t* pool);
bool mp_is_pool_dirty(memory_pool_t* pool);
void mp_set_error_recovery_callback(memory_pool_t* pool, mp_watermark_callback_t cb, void* user_data);
bool mp_isolate_bad_block(memory_pool_t* pool, void* ptr);
```

Dirty pool marking, error recovery callback, and bad block isolation.

---

### Thread-Level Quota and Circuit Breaker

```c
void mp_set_thread_quota(memory_pool_t* pool, size_t quota_bytes);
void mp_set_circuit_breaker(memory_pool_t* pool, bool enable);
bool mp_is_circuit_breaker_tripped(memory_pool_t* pool);
size_t mp_get_thread_allocated_bytes(memory_pool_t* pool);
void mp_reset_thread_quota(memory_pool_t* pool);
```

---

### ABI Version and cgroup Awareness

```c
uint32_t mp_abi_version(void);
void mp_set_cgroup_aware(memory_pool_t* pool, bool enable);
size_t mp_get_cgroup_mem_limit(memory_pool_t* pool);
```

---

### Hot/Cold Page Separation

```c
bool mp_mark_page_hot(memory_pool_t* pool, void* page_raw_mem);
bool mp_mark_page_cold(memory_pool_t* pool, void* page_raw_mem);
size_t mp_get_hot_page_count(memory_pool_t* pool);
size_t mp_get_cold_page_count(memory_pool_t* pool);
size_t mp_separate_hot_cold_pages(memory_pool_t* pool);
```

---

### Encrypted Memory

```c
int mp_lock_memory(memory_pool_t* pool, void* addr, size_t length);
int mp_unlock_memory(memory_pool_t* pool, void* addr, size_t length);
int mp_protect_from_dump(memory_pool_t* pool, void* addr, size_t length);
void mp_secure_zero(memory_pool_t* pool, void* ptr, size_t length);
void mp_set_encrypted_memory(memory_pool_t* pool, bool enable);
```

---

### ASan Integration

```c
bool mp_asan_is_enabled(void);
void mp_asan_report_error(memory_pool_t* pool, void* ptr, size_t size, bool is_write);
bool mp_asan_check_memory(memory_pool_t* pool, void* ptr, size_t size);
void mp_set_asan_integration(memory_pool_t* pool, bool enable);
```

---

### Online Expansion

```c
bool mp_expand_pool(memory_pool_t* pool, size_t additional_bytes);
bool mp_can_expand(memory_pool_t* pool);
size_t mp_get_expandable_size(memory_pool_t* pool);
```

---

### Frame Arena

```c
cmem_frame_arena_t* mp_frame_arena_create(size_t capacity);
void* mp_frame_alloc(cmem_frame_arena_t* farena, size_t size);
void mp_frame_end(cmem_frame_arena_t* farena);
void mp_frame_arena_destroy(cmem_frame_arena_t* farena);
```

Double-buffered Ping-Pong frame arena with O(1) reset.

---

### Typed Object Pool

```c
mp_typed_pool_t* mp_typed_pool_create(size_t elem_size, size_t capacity);
void* mp_typed_alloc(mp_typed_pool_t* tpool);
void mp_typed_free(mp_typed_pool_t* tpool, void* ptr);
void mp_typed_pool_destroy(mp_typed_pool_t* tpool);
```

Zero-overhead fixed-size object pool.

---

### Ring Buffer

```c
cmem_ring_buffer_t* mp_ring_create(size_t slot_size, size_t capacity);
void* mp_ring_alloc(cmem_ring_buffer_t* ring);
bool mp_ring_free(cmem_ring_buffer_t* ring, void* ptr);
void mp_ring_destroy(cmem_ring_buffer_t* ring);
```

DPDK-style lock-free ring buffer.

---

### Watermark Callback

```c
void mp_set_watermark_callback(memory_pool_t* pool, double high_ratio, double low_ratio, mp_watermark_callback_t cb, void* user_data);
```

Configures high/low watermark threshold alert callbacks.

---

### Memory Limit

```c
void mp_set_memory_limit(memory_pool_t* pool, size_t max_bytes);
```

Sets a hard maximum budget limit on the memory pool.

---

### Event Callback

```c
void mp_set_event_callback(memory_pool_t* pool, mp_event_callback_t callback, void* user_data);
```

Registers an event callback function for real-time profiling and debugging.

---

### Emergency OOM Reserve

```c
bool mp_enable_emergency_reserve(memory_pool_t* pool, size_t reserve_bytes);
```

Enables an emergency OOM fallback internal memory reserve.

---

### NUMA Binding

```c
bool mp_set_numa_node(memory_pool_t* pool, int numa_node);
```

Binds the memory pool's underlying allocation to the specified Linux NUMA CPU node.

---

### Environment Variable Parsing

```c
mp_flags_t mp_parse_env_flags(mp_flags_t default_flags);
```

Parses the `CMEM_CONF` environment variable.

**Supported Environment Variables:**
```bash
export CMEM_CONF="canary=1,poison=on,aligned=1,guard=1,tls=1,track=1,hugepages=1"
```

---

## 9. Configuration Flags

```c
typedef enum {
    MP_FLAG_DEFAULT            = 0,                    // Default configuration
    MP_FLAG_THREAD_SAFE        = (1 << 0),             // Enable thread safety (mutex)
    MP_FLAG_DEBUG_CANARY       = (1 << 1),             // Redzone Canary out-of-bounds detection
    MP_FLAG_ZERO_ON_ALLOC      = (1 << 2),             // Auto-zero on allocation
    MP_FLAG_THREAD_LOCAL_CACHE = (1 << 3),             // Thread-local cache (Lock-Free small objects)
    MP_FLAG_STATIC_BUFFER      = (1 << 4),             // Static buffer mode (no OS malloc)
    MP_FLAG_TRACK_LOCATIONS    = (1 << 5),             // Record file/line/function/backtrace
    MP_FLAG_POISON_ON_FREE     = (1 << 6),             // Fill with 0xDD on free (UAF detection)
    MP_FLAG_CACHE_ALIGNED      = (1 << 7),             // Force 64B Cache Line alignment
    MP_FLAG_GUARD_PAGES        = (1 << 8),             // Page-level Guard Pages (PROT_NONE)
    MP_FLAG_SHARED_MEMORY      = (1 << 9),             // POSIX shared memory IPC mode
    MP_FLAG_HUGE_PAGES         = (1 << 10),            // Linux HugePages (2MB/1GB)
    MP_FLAG_PERCPU_FREELIST    = (1 << 11),            // Per-CPU lock-free freelist
    MP_FLAG_HOT_COLD_SEPARATION = (1 << 12),           // Hot/Cold page separation (TLB optimization)
    MP_FLAG_ENCRYPTED_MEMORY   = (1 << 13),            // Encrypted memory (mlock + MADV_DONTDUMP)
    MP_FLAG_ASAN_INTEGRATION   = (1 << 14)             // AddressSanitizer integration layer
} mp_flags_t;
```

---

## 10. C++ API

### cmem::MemoryPool

```cpp
namespace cmem {

class MemoryPool {
public:
    explicit MemoryPool(size_t initial_capacity = 0, mp_flags_t flags = MP_FLAG_DEFAULT);
    MemoryPool(const std::string& shm_name, size_t capacity, mp_flags_t flags);
    MemoryPool(MemoryPool& parent, size_t capacity, mp_flags_t flags, const std::string& name);
    ~MemoryPool();

    void* alloc(size_t size);
    void* alloc_loc(size_t size, const char* file, int line, const char* func);
    void* calloc(size_t num, size_t size);
    void* realloc(void* ptr, size_t new_size);
    void* aligned_alloc(size_t alignment, size_t size);
    void free(void* ptr);

    size_t alloc_batch(size_t size, void** out_ptrs, size_t count);
    void free_batch(void** ptrs, size_t count);

    void reset();
    size_t compact();
    void set_memory_limit(size_t max_bytes);

    bool audit_heap();
    std::string analyze_leaks();
    bool export_leak_report(const std::string& path);
    bool export_html_report(const std::string& path);
    bool export_binary_snapshot(const std::string& path);
    static std::string parse_binary_snapshot(const std::string& path);

    mp_stats_t get_stats() const;
    void dump_info() const;
    void dump_tree_info() const;
    void dump_histogram() const;

    bool check_leaks() const;
    bool get_allocation_info(void *ptr, mp_allocation_info_t *info) const;
    size_t enumerate_regions(mp_region_info_t *regions, size_t max_regions) const;
    memory_pool_t* get() const;
    memory_pool_t* release();
};

}
```

### cmem::allocator<T>

```cpp
template<typename T>
class allocator {
public:
    using value_type = T;
    allocator(memory_pool_t* pool);
    template<typename U> allocator(const allocator<U>& other);

    T* allocate(std::size_t n);
    void deallocate(T* p, std::size_t n);
    bool operator==(const allocator& other) const;
    bool operator!=(const allocator& other) const;
};
```

### cmem::pmr_resource

```cpp
class pmr_resource : public std::pmr::memory_resource {
public:
    explicit pmr_resource(memory_pool_t* pool);
    memory_pool_t* pool() const;

protected:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;
};
```

---

## 11. Event Types

```c
typedef enum {
    MP_EVENT_ALLOC = 1,              // Memory block allocated
    MP_EVENT_FREE,                   // Memory block freed
    MP_EVENT_REALLOC,                // Memory block reallocated
    MP_EVENT_CANARY_CORRUPTION,      // Buffer overflow detected via canary check
    MP_EVENT_DOUBLE_FREE,            // Double-free or invalid free detected
    MP_EVENT_RESET,                  // Memory pool reset
    MP_EVENT_COMPACT,               // Memory pool compaction
    MP_EVENT_OOM,                    // Out-of-memory condition reached
    MP_EVENT_DIRTY                   // Pool marked dirty due to memory corruption
} mp_event_type_t;
```

---

## Appendix: Quick Reference Card

### C Language

```c
#include "cmem.h"

// Create
memory_pool_t* pool = mp_create(1024*1024, MP_FLAG_THREAD_SAFE);

// Allocate/Free
void* p = mp_alloc(pool, 128);
mp_free(pool, p);

// Introspection
size_t usable = mp_usable_size(pool, p);
bool valid = mp_ptr_valid(pool, p);

// Destroy
mp_destroy(pool);
```

### C++17

```cpp
#include "cmem.hpp"
#include "cmem_pmr.hpp"

// RAII
cmem::MemoryPool pool(1024*1024, MP_FLAG_THREAD_SAFE);

// STL container
cmem::allocator<int> alloc(pool.get());
std::vector<int, cmem::allocator<int>> vec(alloc);

// PMR
cmem::pmr_resource res(pool.get());
std::pmr::vector<std::pmr::string> vec(&res);
```