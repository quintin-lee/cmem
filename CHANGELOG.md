# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed
- **Slab Allocator Thread Safety & Lifetime Fixes**:
  - Eliminated mid-free `cmem_munmap` call in `slab_free_nolock` to prevent use-after-free race conditions when unmapping 64KB slab pages during concurrent allocations/frees.
  - Added POSIX `pthread_key` thread-exit destructor (`tls_cache_dtor`) to automatically flush thread-local cached slots (`tls_cache`) back to `owner_pool` on thread termination.
  - Updated `tls_cache_validate_owner()` to flush previous pool slots before reassigning thread cache ownership.
  - Zeroed out `header->prev` and `header->next` in `active_list_remove()` to prevent dangling linked-list pointers.
  - Included `empty_pages` list cleanup in `mp_destroy()` and re-initialization in `mp_reset()`.

### Added
- **Expanded Boundary Tests**:
  - Added `test_boundary_cross_allocator`, `test_boundary_zero_size_all_tiers`, and `test_boundary_max_size` unit test cases.
- **Diagnostic Parser Bounds Checks**:
  - Hardened snapshot parser `cmem-analyze-parser.c` with total pool size and allocation record limit validation.

## [0.1.0] - 2026-08-05

### Added
- **Modular Public Subsystem Headers**:
  - `include/cmem_ring.h`: SPSC lock-free ring buffer allocator and event log ring buffer APIs
  - `include/cmem_tlsf.h`: Two-Level Segregated Fit (TLSF) allocator structures
  - `include/cmem_diag.h`: Heap integrity auditing (`mp_audit_heap`) and memory leak analysis (`mp_analyze_leaks`, `mp_export_leak_report`, `mp_export_html_report`)
  - `include/cmem_snapshot.h`: Post-mortem binary crash dump snapshot exporter, parser, and incremental diff tool
  - `include/cmem_metrics.h`: Prometheus exporter, JSON stats dump, latency histogram, P99/avg latency metrics, and pprof profiling
  - `include/cmem_arena.h`: Hierarchical child pools, multi-arena partitioning, and per-arena memory quota management
  - `include/cmem_frame.h`: Game & graphics dual ping-pong frame arena allocator
  - `include/cmem_typed_pool.h`: Zero-overhead type-safe object pool allocator (`MP_TYPED_POOL_DEFINE`)
- **Decoupled Diagnostics Subsystem**:
  - `cmem_core` (`libcmem_core.a` / `libcmem_core.so`) for zero-diagnostic lightweight embedded deployment
  - `cmem_diag` (`libcmem_diag.a` / `libcmem_diag.so`) for standalone observability and heap inspection
  - `CMEM_ENABLE_DIAGNOSTICS` build option in CMake and Makefile
- **Fast Path Inline Allocation APIs**:
  - `mp_alloc_fast` and `mp_free_fast` inline functions for ultra-low latency allocations (up to 2.8x speedup over standard allocations)
- **Library Versioning & Shared Targets**:
  - Versioned static archives (`libcmem-0.1.0.a`, `libcmem_core-0.1.0.a`, `libcmem_diag-0.1.0.a`)
  - Versioned shared libraries (`libcmem.so.0.1.0`, `libcmem_core.so.0.1.0`, `libcmem_diag.so.0.1.0`) with `SOVERSION` symlinks across Make and CMake
- **Project Infrastructure & Tooling**:
  - `VERSION` file as the single source of truth for version number
  - `MP_FLAG_AUTO_NUMA` pool flag with thread-local-first NUMA binding and node topology query APIs (`mp_numa_node_count`, `mp_cpu_to_node`)
  - In-pool compressed storage with built-in LZ4 codec, handle table, and LRU eviction (`mp_compress_block`, `mp_decompress_block`, `mp_free_compressed`)
  - Diagnostic CLI utilities (`tools/cmem-inspect` for real-time inspection, `tools/cmem-analyze` for offline crash dump diffing)
  - C++17 PMR adapter and STL allocator support (`include/cmem_pmr.hpp`, `include/cmem.hpp`)
  - Windows / MSVC cross-platform support with `VirtualAlloc` system backend

### Changed
- **Header Refactoring**:
  - Refactored `include/cmem.h` into a lightweight master header that includes all subsystem headers by default for 100% backward compatibility
- **Diagnostic Decoupling**:
  - Refactored `mp_destroy()` to avoid implicit leak checks when diagnostics are disabled (`CMEM_DISABLE_DIAGNOSTICS`)
- **Benchmark Improvements**:
  - Corrected Benchmark 10 compressed storage handle tracking and output ratio calculations
  - Improved throughput metric formatting (MB/s, Mops/sec, Kops/sec) and added accurate slower-than-malloc ratio indicators

### Fixed
- **Sanitizer & Build Fixes**:
  - Resolved ASan preloading link order conflicts in Makefile using dynamic executable-scoped `RUN_ASAN`
  - Fixed clang-tidy warnings (`readability-magic-numbers`, `bugprone-macro-parentheses`, parameter name consistency) across all public headers
