# cmem - Universal High-Performance Tiered Memory Manager

> 🌐 **Languages**: [English](README.md) | [中文](README.zh.md)

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![CI Build & Test](https://github.com/quintin-lee/cmem/actions/workflows/ci.yml/badge.svg)](https://github.com/quintin-lee/cmem/actions/workflows/ci.yml)
[![Code Coverage](https://codecov.io/gh/quintin-lee/cmem/branch/master/graph/badge.svg)](https://codecov.io/gh/quintin-lee/cmem)
[![C11](https://img.shields.io/badge/C-C11-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B17)
[![clang-format](https://img.shields.io/badge/Code%20Style-clang--format-green.svg)](https://clang.llvm.org/docs/ClangFormat.html)
[![ASan+UBSan](https://img.shields.io/badge/Sanitizer-ASan%20%2B%20UBSan-red.svg)](https://clang.llvm.org/docs/AddressSanitizer.html)
[![Valgrind](https://img.shields.io/badge/Valgrind-Tested-orange.svg)](https://valgrind.org/)
[![Security](https://img.shields.io/badge/Security-PRIVATE%20REPORTING-green.svg)](https://github.com/quintin-lee/cmem/security/advisories)
[![ThreadSanitizer](https://img.shields.io/badge/Sanitizer-ThreadSanitizer-blue.svg)](https://clang.llvm.org/docs/ThreadSanitizer.html)

**cmem** is a universal, high-performance tiered memory management tool designed with **C11 / C++17**. It combines the core strengths of industrial-grade allocators such as **Slab** and **TLSF**, providing comprehensive memory diagnostics, introspection queries, cascading tree-shaped Arena organization, cross-platform OS page reclamation, and C++17 PMR container adaptation.

---

## 🚀 Quick Start

### Installation

```bash
git clone https://github.com/quintin-lee/cmem.git
cd cmem
make install PREFIX=/usr/local
```

Or use CMake:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /usr/local
```

### Basic Usage

```c
#include "cmem.h"

int main() {
    memory_pool_t* pool = mp_create(64 * 1024 * 1024, MP_FLAG_THREAD_SAFE | MP_FLAG_THREAD_LOCAL_CACHE);
    
    void* p = mp_alloc(pool, 128);
    if (p) {
        memset(p, 0, 128);
        mp_free(pool, p);
    }
    
    mp_destroy(pool);
    return 0;
}
```

### C++17 PMR Usage

```cpp
#include "cmem_pmr.hpp"

int main() {
    cmem::MemoryPool pool(64 * 1024 * 1024);
    std::vector<int, cmem::allocator<int>> vec(pool.get_resource());
    
    vec.push_back(42);
    return 0;
}
```

---

## 🌟 Core Architecture (Architecture)

**cmem** adopts a modern **Tiered Hybrid Architecture** that ensures $O(1)$ allocation time complexity while greatly reducing memory fragmentation and significantly improving L1/L2 Cache locality:

```text
                          +-------------------------------+
                          |   User Request (mp_alloc)    |
                          +-------------------------------+
                                          |
                   +----------------------+----------------------+
                   | (<= 512 Bytes)       | (512B ~ 4MB)         | (> 4MB)
                   v                      v                      v
          +------------------+   +-------------------+   +--------------------+
          |   Slab Pool      |   |   TLSF Allocator  |   | Direct OS Fallback |
          | (Fixed Blocks)   |   | (Two-Level Fit)   |   |  (System Malloc)   |
          +------------------+   +-------------------+   +--------------------+
          | O(1) Fast Alloc  |   | O(1) Bitmap Search|   | Huge Allocations   |
          | Zero Extra Frag  |   | Immediate Merge   |   | Dynamic Page Track |
          +------------------+   +-------------------+   +--------------------+
```

---

## 🔥 Feature Highlights

### 1. ⚡ High-Performance Tiered Allocator
- **Slab Allocator (<= 512B)**: Fixed-size Class allocator for small objects (8B ~ 512B) with $O(1)$ alloc/free and zero external fragmentation.
- **TLSF Allocator (512B ~ 4MB)**: Two-Level Segregated Fit with $O(1)$ bitmap search and **in-place expansion** to avoid unnecessary `memcpy`.
- **Direct OS Fallback (> 4MB)**: Automatically falls back to system memory mapping with Guard Pages and HugePages acceleration.

### 2. 🔍 Memory Introspection APIs
- **`mp_usable_size`**: Query actual usable byte capacity of an allocated block.
- **`mp_alloc_size`**: Query the user-requested byte size.
- **`mp_ptr_valid`**: Quickly validate if a pointer is an active allocation.
- **`mp_preferred_size`**: Calculate the best matching Size Class alignment.

### 3. ♻️ Memory Compaction & OS Page Reclamation
- **`mp_trim`**: Deep physical page compaction and reclamation with optional padding.
- **`mp_madvise`**: Cross-platform memory advice wrapper (`madvise` on Linux, `VirtualAlloc(MEM_RESET)` on Windows).
- **`mp_compact` / `mp_purge_lazy`**: Compact memory and lazily purge physical pages.
- **`mp_resident`**: Get current RSS resident memory size.
- **`mp_freeable`**: Get reclaimable idle page bytes.

### 4. 🔒 High-Concurrency Locking (RWLock & Fine-Grained Locks)
- **Read-Write Lock**: All introspection APIs use read locks for zero-lock-contention profiling.
- **Fine-Grained Slab Class Locks**: Each Slab size Class has its own `pthread_mutex_t`.
- **Thread-Local Cache**: Lock-free fast path for small object allocations.

### 5. 🚀 C++17 PMR & STL Container Adapters
- Include `#include "cmem_pmr.hpp"` for `cmem::pmr_resource` adapter.
- Provides `cmem::MemoryPool` RAII wrapper and `cmem::allocator<T>` STL allocator.

### 6. 🛡️ Safety & Security Hardening
- **`mp_reallocarray`**: Overflow-safe reallocation with `nmemb * size` validation.
- **Redzone Canary**: `0xAB` tail bytes for real-time overflow detection.
- **UAF Poisoning**: `0xDD` poison pattern on free.
- **Guard Pages**: `PROT_NONE` at page boundaries.
- **Cache Line Alignment**: 64B alignment to eliminate false sharing.

### 7. 🛠️ Convenience Helpers
- **`mp_strdup`**: Deep-copy null-terminated strings into the pool.
- **`mp_memdup`**: Deep-copy N-byte binary regions.
- **`mp_asprintf`**: Formatted string allocation.

### 8. 🌳 Tree-Shaped Memory Arena Navigation
- Nested parent-child pools (`mp_create_child`) with recursive destroy/reset.
- Metadata APIs: `mp_set_name`, `mp_get_name`, `mp_get_parent`, `mp_get_child_count`.

### 9. 📊 Diagnostics & Monitoring
- **Interactive HTML Report**: One-file HTML dashboard with memory visualization.
- **Prometheus Exporter**: Standard exposition format metrics.
- **Binary Snapshots**: Post-mortem crash dumps with incremental diff leak detection.
- **Real-time QPS & Bandwidth**: Throughput metering in `mp_stats_t`.
- **Size Histogram**: ASCII allocation size distribution.

### 10. 🎮 Game/Graphics Frame Arena
- **Dual Ping-Pong Frame Arena**: $O(1)$ frame-level batch reset, zero-lock contention.

### 11. 🎯 0-Overhead Typed Object Pool
- Zero-header-overhead fixed-size object pooling for high-frequency allocations.

### 12. ⚡ Lock-Free Ring Buffer Allocator
- DPDK-style atomic ring buffer for single-producer/single-consumer scenarios.

### 13. 🔗 POSIX Shared Memory IPC
- Zero-copy inter-process communication via `/dev/shm`.

### 14. 📦 Linux HugePages Support
- 2MB/1GB HugeTLB pages for reduced TLB misses.

### 15. 🚨 Emergency OOM Fallback Reserve
- Reserve cushion for critical operations during OOM.

### 16. 🧭 Linux NUMA Node Affinity
- Bind pool backing memory to specific NUMA nodes.

### 17. ⚙️ Runtime Config Hot-Reload
- **`mp_reparse_env_flags(pool)`**: Hot-reload `CMEM_CONF` without recreating the pool.
- **`mp_get_env_generation(pool)`**: Detect runtime config changes via generation counter.

### 18. 🗜️ Auto-Compaction Trigger
- **`mp_set_auto_compact(pool, enable, pressure_threshold, fragmentation_threshold)`**: Configure automatic compaction based on pool pressure or fragmentation.
- **`mp_auto_compact_check(pool)`**: Internal check and trigger `mp_compact()` when needed.

### 19. 📊 Allocation Latency P99 Statistics
- **`mp_record_latency(pool, latency_ns)`**: Record allocation latency samples (nanosecond histogram).
- **`mp_get_latency_p99(pool)`** / **`mp_get_latency_avg(pool)`**: Query P99 and average allocation latency.
- **`mp_reset_latency_stats(pool)`**: Reset latency histogram.

### 20. 🎛️ Configurable Slab Class Table
- **`mp_set_slab_classes(pool, sizes, count)`**: Replace default Slab size class table.
- **`mp_get_slab_classes(pool, out_sizes, max_count)`**: Query current Slab class configuration.
- **`mp_preferred_size_for_pool(pool, size)`**: Calculate best alignment based on pool's custom Slab table.

### 21. 📝 Structured Event Log & pprof Export
- **`mp_event_log_create(capacity)`** / **`mp_event_log_record(...)`** / **`mp_event_log_consume(...)`**: Lock-free Ring Buffer based structured event log for post-mortem replay.
- **`mp_export_pprof(pool, buf, max_len)`**: Export pprof-compatible text format for flame graph generation.

### 22. 🚀 Per-CPU Lock-Free Freelist
- **`MP_FLAG_PERCPU_FREELIST`**: Enable per-CPU lock-free freelist for small object allocations to reduce lock contention.
- **`mp_set_percpu_freelist(pool, enable)`** / **`mp_get_percpu_freelist(pool)`** / **`mp_get_percpu_cpu_count(pool)`**: Configure and query Per-CPU freelist.

### 23. 🛡️ Memory Error Recovery
- **`mp_mark_pool_dirty(pool)`** / **`mp_clear_pool_dirty(pool)`** / **`mp_is_pool_dirty(pool)`**: Mark/clear/query dirty pool state. New allocations are rejected after canary corruption or double free.
- **`mp_set_error_recovery_callback(pool, cb, udata)`**: Register memory error recovery callback.
- **`mp_isolate_bad_block(pool, ptr)`**: Isolate bad blocks by removing from active tracking.

### 24. 🎯 Thread-Level Quota & Circuit Breaker
- **`mp_set_thread_quota(pool, quota_bytes)`**: Set per-thread memory quota to prevent a single thread from exhausting the pool.
- **`mp_set_circuit_breaker(pool, enable)`** / **`mp_is_circuit_breaker_tripped(pool)`**: Enable/query thread-level circuit breaker.
- **`mp_get_thread_allocated_bytes(pool)`** / **`mp_reset_thread_quota(pool)`**: Query/reset current thread allocated bytes.

### 25. 🧊 Hot/Cold Page Separation
- **`MP_FLAG_HOT_COLD_SEPARATION`**: Enable hot/cold page physical separation to improve TLB hit rate.
- **`mp_mark_page_hot(pool, page_raw_mem)`** / **`mp_mark_page_cold(pool, page_raw_mem)`**: Mark page temperature attributes.
- **`mp_get_hot_page_count(pool)`** / **`mp_get_cold_page_count(pool)`**: Query hot/cold page counts.
- **`mp_separate_hot_cold_pages(pool)`**: Execute hot/cold page physical separation.

### 26. 🔒 Encrypted Memory Support
- **`MP_FLAG_ENCRYPTED_MEMORY`**: Enable encrypted memory mode with `mlock` + `madvise(MADV_DONTDUMP)`.
- **`mp_lock_memory(pool, addr, length)`** / **`mp_unlock_memory(pool, addr, length)`**: Lock/unlock memory pages to prevent swap.
- **`mp_protect_from_dump(pool, addr, length)`**: Exclude memory from core dumps.
- **`mp_secure_zero(pool, ptr, length)`**: Volatile secure zero to prevent data remanence.
- **`mp_set_encrypted_memory(pool, enable)`**: One-click encrypted memory mode.

### 27. 🛡️ AddressSanitizer Integration
- **`MP_FLAG_ASAN_INTEGRATION`**: Enable ASan-compatible mode.
- **`mp_asan_is_enabled()`**: Detect if ASan is active.
- **`mp_asan_report_error(pool, ptr, size, is_write)`**: Report custom memory errors to ASan.
- **`mp_asan_check_memory(pool, ptr, size)`**: Check memory region for ASan errors.
- **`mp_set_asan_integration(pool, enable)`**: Enable/disable ASan integration.

### 28. 🚀 Online Pool Expansion
- **`mp_expand_pool(pool, additional_bytes)`**: Add capacity without service interruption by linking new TLSF pools.
- **`mp_can_expand(pool)`**: Check if pool supports expansion.
- **`mp_get_expandable_size(pool)`**: Query remaining expandable capacity.

---

## 📦 Headers

| Language | Header | Namespace / Standard |
| :--- | :--- | :--- |
| **C** | `#include "cmem.h"` | C11 standard C API |
| **C++ Wrapper** | `#include "cmem.hpp"` | `cmem::` namespace |
| **C++17 PMR** | `#include "cmem_pmr.hpp"` | `cmem::pmr_resource` (`std::pmr`) |
| **Global Override** | `#include "cmem_override.h"` | Redirect `malloc`/`free` |

---

## 💻 Quick Start

### C Example (Introspection & Reclamation)

```c
#include "cmem.h"
#include <stdio.h>
#include <assert.h>

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEBUG_CANARY | MP_FLAG_ZERO_ON_ALLOC);
    mp_set_name(pool, "MyAppRootPool");

    void* ptr = mp_alloc(pool, 120);
    printf("Is pointer valid: %s\n", mp_ptr_valid(pool, ptr) ? "true" : "false");
    printf("Requested size: %zu bytes, Usable size: %zu bytes\n",
           mp_alloc_size(pool, ptr), mp_usable_size(pool, ptr));

    char* str = mp_strdup(pool, "Hello cmem memory manager!");
    char* formatted = mp_asprintf(pool, "Arena [%s] active allocations: %zu", mp_get_name(pool), (size_t)2);
    printf("%s\n", formatted);

    mp_free(pool, ptr);
    size_t reclaimed = mp_trim(pool, 0);
    printf("Reclaimed %zu bytes back to OS\n", reclaimed);

    mp_destroy(pool);
    return 0;
}
```

### C++17 PMR Polymorphic Containers

```cpp
#include "cmem.hpp"
#include "cmem_pmr.hpp"
#include <vector>
#include <string>
#include <iostream>

int main() {
    cmem::MemoryPool pool(1024 * 1024, MP_FLAG_THREAD_SAFE);
    cmem::pmr_resource res(pool.get());

    std::pmr::vector<std::pmr::string> vec(&res);
    vec.push_back(std::pmr::string("Polymorphic", &res));
    vec.push_back(std::pmr::string("Memory Resource", &res));
    vec.push_back(std::pmr::string("Integration", &res));

    for (const auto& str : vec) {
        std::cout << str << " ";
    }
    std::cout << std::endl;

    return 0;
}
```

### C++ STL-Compatible Allocator

```cpp
#include "cmem.hpp"
#include <vector>
#include <unordered_map>
#include <string>
#include <cassert>

int main() {
    cmem::MemoryPool pool(1024 * 1024, MP_FLAG_THREAD_SAFE);

    cmem::allocator<int> vec_alloc(pool);
    std::vector<int, cmem::allocator<int>> vec(vec_alloc);
    for (int i = 0; i < 100; i++) vec.push_back(i * 10);
    assert(vec[50] == 500);

    using MapAlloc = cmem::allocator<std::pair<const int, std::string>>;
    std::unordered_map<int, std::string, std::hash<int>, std::equal_to<int>, MapAlloc> map(10, std::hash<int>(), std::equal_to<int>(), MapAlloc(pool));
    map[1] = "cmem";
    map[2] = "High-Performance";
    assert(map[1] == "cmem");

    pool.check_leaks();
    return 0;
}
```

### Advanced: Frame Arena (Game/Graphics Pipeline)

```c
#include "cmem.h"
#include <stdio.h>
#include <string.h>

int main() {
    cmem_frame_arena_t* farena = mp_frame_arena_create(512 * 1024);

    void* frame1_ptr = mp_frame_alloc(farena, 1024);
    strcpy((char*)frame1_ptr, "RenderMeshFrame1");
    mp_frame_end(farena);

    void* frame2_ptr = mp_frame_alloc(farena, 2048);
    strcpy((char*)frame2_ptr, "RenderMeshFrame2");
    mp_frame_end(farena);

    mp_frame_arena_destroy(farena);
    printf("Frame Arena Ping-Pong completed!\n");
    return 0;
}
```

### Advanced: Typed Object Pool (Zero-Overhead)

```c
#include "cmem.h"
#include <stdio.h>
#include <assert.h>

typedef struct { int id; char name[32]; double value; } game_entity_t;

int main() {
    mp_typed_pool_t* tpool = mp_typed_pool_create(sizeof(game_entity_t), 128);

    game_entity_t* e1 = (game_entity_t*)mp_typed_alloc(tpool);
    game_entity_t* e2 = (game_entity_t*)mp_typed_alloc(tpool);
    assert(e1 != e2);

    e1->id = 42;
    strcpy(e1->name, "Player");
    e1->value = 100.0;

    printf("Entity: id=%d, name=%s, value=%.1f\n", e1->id, e1->name, e1->value);

    mp_typed_free(tpool, e1);
    mp_typed_free(tpool, e2);
    mp_typed_pool_destroy(tpool);
    return 0;
}
```

### Advanced: Lock-Free Ring Buffer

```c
#include "cmem.h"
#include <stdio.h>
#include <string.h>

int main() {
    cmem_ring_buffer_t* ring = mp_ring_create(128, 64);

    void* slot1 = mp_ring_alloc(ring);
    void* slot2 = mp_ring_alloc(ring);
    strcpy((char*)slot1, "Lock-Free Payload 1");
    strcpy((char*)slot2, "Lock-Free Payload 2");

    printf("Slot 1: %s\n", (char*)slot1);
    printf("Slot 2: %s\n", (char*)slot2);

    mp_ring_free(ring, slot1);
    mp_ring_free(ring, slot2);
    mp_ring_destroy(ring);
    return 0;
}
```

### Advanced: Shared Memory IPC

```c
#include "cmem.h"
#include <stdio.h>
#include <string.h>

int main() {
    memory_pool_t* pool = mp_create_shared("/my_ipc_pool", 512 * 1024, MP_FLAG_THREAD_SAFE);
    void* p = mp_alloc(pool, 1024);
    strcpy((char*)p, "Zero-Copy IPC Message from Process A");

    // Process B can open the same shared memory pool
    // memory_pool_t* pool_b = mp_create_shared("/my_ipc_pool", 512 * 1024, MP_FLAG_THREAD_SAFE);
    // printf("Process B reads: %s\n", (char*)p);

    mp_free(pool, p);
    mp_destroy_shared(pool, "/my_ipc_pool");
    return 0;
}
```

### Advanced: Leak Analysis & HTML Report

```c
#include "cmem.h"
#include <stdio.h>

void leaky_function(memory_pool_t* pool) {
    mp_alloc_loc(pool, 256, __FILE__, __LINE__, __func__); // intentional leak
    void* p = mp_alloc_loc(pool, 128, __FILE__, __LINE__, __func__);
    mp_free(pool, p);
}

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_TRACK_LOCATIONS | MP_FLAG_DEBUG_CANARY);
    leaky_function(pool);

    bool healthy = mp_audit_heap(pool);
    printf("Heap Audit: %s\n", healthy ? "HEALTHY" : "CORRUPTED");

    char report[8192];
    mp_analyze_leaks(pool, report, sizeof(report));
    printf("%s\n", report);

    mp_export_html_report(pool, "memory_profile.html");
    mp_export_binary_snapshot(pool, "snapshot_before.cmem_dump");

    mp_destroy(pool);
    return 0;
}
```

### Advanced: Prometheus Metrics Export

```c
#include "cmem.h"
#include <stdio.h>

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);

    mp_alloc(pool, 1024);
    mp_alloc(pool, 2048);

    char prom_buf[4096];
    size_t len = mp_export_prometheus_metrics(pool, prom_buf, sizeof(prom_buf));

    printf("Prometheus Metrics (%zu bytes):\n%s\n", len, prom_buf);

    mp_destroy(pool);
    return 0;
}
```

### Advanced: Environment Variable Auto-Tuning

```bash
# Enable security features without code changes
export CMEM_CONF="canary=1,poison=on,aligned=1,guard=1"
./your_application
```

```c
#include "cmem.h"

int main() {
    mp_flags_t flags = mp_parse_env_flags(MP_FLAG_DEFAULT);
    memory_pool_t* pool = mp_create(0, flags);
    // pool now has Canary + Poison + Cache Aligned + Guard Pages
    return 0;
}
```

### Advanced: Runtime Config Hot-Reload

```c
#include "cmem.h"

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);

    // Later: hot-reload config from environment
    mp_flags_t new_flags = mp_reparse_env_flags(pool);
    printf("Config generation: %llu\n", (unsigned long long)mp_get_env_generation(pool));

    mp_destroy(pool);
    return 0;
}
```

### Advanced: Graceful Degradation (OOM Fallback)

```c
#include "cmem.h"

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);
    mp_set_memory_limit(pool, 1024); // very small limit

    // Enable fallback to system malloc on OOM
    mp_set_fallback_on_oom(pool, true);

    // Register GC callback to free non-critical cache before OOM rejection
    mp_set_gc_callback(pool, [](memory_pool_t* p, bool is_high, size_t current, size_t limit, void* ud) {
        printf("GC callback: freeing non-critical cache\n");
        free_non_critical_cache();
    }, NULL);

    void* p = mp_alloc(pool, 2048); // triggers fallback
    if (p) {
        printf("Fallback allocation succeeded\n");
        mp_free(pool, p);
    }

    mp_destroy(pool);
    return 0;
}
```

### Advanced: Memory Error Recovery

```c
#include "cmem.h"

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEBUG_CANARY);

    // Register error recovery callback
    mp_set_error_recovery_callback(pool, [](memory_pool_t* p, bool is_high, size_t current, size_t limit, void* ud) {
        printf("Memory error recovery triggered\n");
        // Isolate bad blocks, mark pool dirty, etc.
    }, NULL);

    // If canary corruption or double free is detected, pool is marked dirty
    // New allocations will be rejected until recovery

    mp_destroy(pool);
    return 0;
}
```

### Advanced: Thread Quota & Circuit Breaker

```c
#include "cmem.h"

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_THREAD_SAFE);

    // Set per-thread quota: 8MB per thread
    mp_set_thread_quota(pool, 8 * 1024 * 1024);

    // Enable circuit breaker
    mp_set_circuit_breaker(pool, true);

    // Allocate from multiple threads...
    // When a thread exceeds its quota, circuit breaker trips and rejects further allocations

    mp_destroy(pool);
    return 0;
}
```

### Advanced: Hot/Cold Page Separation

```c
#include "cmem.h"

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_HOT_COLD_SEPARATION);

    // Mark frequently accessed pages as hot
    // Mark rarely accessed pages as cold
    // Separate hot and cold pages into distinct memory regions

    printf("Hot pages: %zu\n", mp_get_hot_page_count(pool));
    printf("Cold pages: %zu\n", mp_get_cold_page_count(pool));

    mp_separate_hot_cold_pages(pool);

    mp_destroy(pool);
    return 0;
}
```

### Advanced: Encrypted Memory

```c
#include "cmem.h"

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_ENCRYPTED_MEMORY);

    // Lock sensitive data in RAM
    mp_lock_memory(pool, ptr, len);

    // Protect from core dumps
    mp_protect_from_dump(pool, ptr, len);

    // Secure zero when done
    mp_secure_zero(pool, ptr, len);

    mp_unlock_memory(pool, ptr, len);

    mp_destroy(pool);
    return 0;
}
```

---

## 🚀 Build & Test

```bash
# Build static library and run C unit tests (with ASan/UBSan)
make test

# Build and run C++17 PMR and STL allocator tests
make test_cpp

# Build and run performance benchmarks
make bench

# Build and run all examples
make examples

# CMake build
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure

# ThreadSanitizer build (detect data races)
make clean
CC=clang CFLAGS="-fsanitize=thread -g -O1" make test

# Docker reproducible build (Ubuntu 22.04)
make docker-build
```

### Versioning & Tagging

The `VERSION` file at the repository root is the single source of truth for the version number.

```bash
# Bump patch version and create tag (e.g. 1.0.0 -> 1.0.1)
make tag --bump=patch

# Bump minor version (e.g. 1.0.0 -> 1.1.0)
make tag --bump=minor

# Bump major version (e.g. 1.0.0 -> 2.0.0)
make tag --bump=major

# Set explicit version
make tag 1.2.3

# Dry run to preview changes
make tag --dry-run --bump patch
```

The `tag.sh` script automatically:
1. Updates the `VERSION` file
2. Updates `CHANGELOG.md` (promotes `[Unreleased]` to `[x.y.z] - <date>` and inserts a fresh `[Unreleased]` section)
3. Commits the change with `chore(version): 🧹 bump version to x.y.z`
4. Creates a git tag `v<x.y.z>`

### Docker

The project includes a `Dockerfile` for reproducible builds:

```bash
make docker-build
# or manually:
docker build --rm -t cmem-build .
docker run -it cmem-build /bin/bash
```

This builds and tests cmem in a clean Ubuntu 22.04 environment with all dependencies pre-installed.

---

## 📊 Performance Benchmarks

> **Note**: Results vary by platform, compiler, and CPU. The following are representative measurements on a modern x86_64 Linux system with `MP_FLAG_THREAD_LOCAL_CACHE` enabled.

| Scenario | System Malloc | cmem | Result |
| :--- | :--- | :--- | :--- |
| Small Objects (32-256B x 1M ops) | 2.25 Mops/sec | 1.21 Mops/sec | Overhead from safety features |
| Medium Objects (1KB-64KB x 100K ops) | ~0.5 Mops/sec | ~0.4 Mops/sec | TLSF path with headers |
| Arena Reset (500 allocs x 1000 rounds) | 118.6 ms | 52.9 ms | **~2.2x faster** |
| Batch Free (256 slots) | O(n) individual free | O(1) batch free | **Significant for bulk cleanup** |

### Key Takeaways

- **cmem is not a drop-in replacement for raw throughput**: It prioritizes memory safety, introspection, and debug features over raw allocator speed. For latency-sensitive paths, use `MP_FLAG_PERCPU_FREELIST` or `MP_FLAG_THREAD_LOCAL_CACHE`.
- **Batch operations excel**: `mp_reset()` and `mp_free_batch()` provide O(1) arena teardown vs O(n) individual frees.
- **TLSF in-place realloc**: When adjacent blocks are free, `mp_realloc()` avoids memcpy entirely.
- **Production tuning**: Disable `MP_FLAG_DEBUG_CANARY` and `MP_FLAG_POISON_ON_FREE` in Release builds to reduce overhead.

---

## 📁 Project Structure

```
cmem/
├── include/
│   ├── cmem.h              # C11 public API
│   ├── cmem.hpp            # C++11 RAII wrapper + STL allocator
│   ├── cmem_pmr.hpp        # C++17 std::pmr::memory_resource adapter
│   └── cmem_override.h     # Global malloc/free symbol interception
├── src/
│   └── cmem.c              # Core implementation
├── tests/
│   ├── test_main.c         # C unit tests (60+ test cases)
│   └── test_cpp.cpp        # C++ PMR + STL allocator tests
├── benchmarks/
│   └── bench_main.c        # Performance benchmarks
├── examples/
│   ├── example_basic.c
│   ├── example_embedded.c
│   ├── example_leak_analysis.c
│   └── example_arena_tree.c
├── docs/
│   ├── index.md            # Documentation index
│   ├── en/                 # English documentation
│   └── zh/                 # Chinese documentation
├── scripts/
│   └── tag.sh            # Shell script for version bumping and git tagging
├── CMakeLists.txt
├── Makefile
├── LICENSE
└── README.md
```

---

## 🔒 ABI Stability & Versioning

### ABI Version

The current ABI version is `1`. Use `mp_abi_version()` to query at runtime.

### Stability Promise

cmem follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html):

| Change Type | ABI Break? | Semantic Version |
| :--- | :--- | :--- |
| Bug fix (no API change) | No | Patch |
| New API (backward compatible) | No | Minor |
| New API (backward incompatible) | Yes | Major |
| Internal refactoring | No | Patch |
| Struct layout change | Yes | Major |
| Flag enum value change | Yes | Major |

**API Stability**: Public APIs in `include/cmem.h` and `include/cmem.hpp` are stable within a major version. New symbols may be added in minor versions but never removed or renamed without a major version bump.

**ABI Compatibility**: The ABI version (`mp_abi_version()`) is incremented only when binary compatibility is broken. Applications linking against `libcmem.so` should check this version at startup if they need to support multiple cmem versions.

### Compatibility Rules

- New fields are appended only at the end of public structs.
- `mp_flags_t` enum values are append-only; do not reuse or renumber existing values.
- Old clients can detect newer pool versions via `mp_abi_version()` and degrade gracefully.

---

## 🖥️ Platform Support

| Platform | Status | Notes |
| :--- | :--- | :--- |
| **Linux (glibc)** | ✅ Full | NUMA, HugePages, Shared Memory, madvise, mlock |
| **Linux (musl)** | ⚠️ Partial | Basic allocator works; NUMA/HugePages may need porting |
| **macOS** | ⚠️ Partial | No NUMA/HugePages/Shared Memory; madvise limited |
| **Windows (MSVC)** | ⚠️ Partial | Ported `mmap`/`madvise` to `VirtualAlloc`; POSIX shared memory not available |
| **FreeBSD** | ⚠️ Basic | May work with minor `#ifdef` adjustments |
| **Android** | ⚠️ Basic | Bionic libc; test before production |

### Compiler Support

| Compiler | Minimum | Recommended | Sanitizers |
| :--- | :--- | :--- | :--- |
| GCC | 7.0 | 13.0+ | ASan, UBSan, TSan |
| Clang | 6.0 | 17.0+ | ASan, UBSan, TSan |
| MSVC | 2017 | 2022+ | ASan (VS 2019+) |

### Runtime Analysis

- **AddressSanitizer (ASan)**: Fully supported via `-fsanitize=address,undefined`
- **ThreadSanitizer (TSan)**: Fully supported via `-fsanitize=thread` for data race detection
- **Valgrind**: Tested on Linux glibc; use `--leak-check=full` for leak detection

---

## 📄 License

MIT License. See [LICENSE](LICENSE) for details.

---

**cmem** — Making memory management simpler, safer, and faster. 🚀
