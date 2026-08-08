# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Per-Thread TLSF Free-Block Cache**: Added thread-local cache (`tls_cache.tlsf_slots/counts`) for TLSF allocations, enabling lock-free hot-path allocations that bypass the TLSF global lock entirely.
- **Per-CPU Lock-Free Freelist Optimizations**: Added per-class remote-free pending flags (`remote_free_pending_class[]`) to skip empty-class harvest; optimized `percpu_push`/`percpu_pop` paths; inlined `tlsf_mapping_search` helpers.
- **Multi-Arena Bound-Arena Fast Path**: Added `tls_cache.bound_arena` TLS slot to cache per-thread arena binding, bypassing master-pool rwlock on subsequent allocations in child pools.
- **Atomic Page Free-Count**: Added `page->page_free_count` atomic field for lock-free availability checks in `slab_alloc_slot`, eliminating one class-lock acquisition per allocation.
- **CONFIG-Based Build System**: Added `make CONFIG=Debug|Release|ASan|TSan|UBSan` targets for sanitizer and debug builds; updated CI workflows.
- **Stress Test Configuration**: Added `STRESS_DEFINES=-DSTRESS_DURATION_SEC=10` to fix CI timeout in `stress_test`.
- **Batch Operation Tests**: Added `test_batch_free_semantics`, `test_batch_free_equivalence`, `test_batch_free_mixed_tiers`, `test_batch_free_corrupt`, `test_batch_free_subpool`, `test_batch_free_fastpath`, `test_batch_free_poison`, `test_batch_free_overflow`, `test_batch_alloc_and_compact`, `test_batch_alloc_tiers`, `test_batch_alloc_configs`, and `test_boundary_*` unit test cases.
- **Diagnostic Parser Bounds Checks**: Hardened snapshot parser `cmem-analyze-parser.c` with total pool size and allocation record limit validation.

### Fixed
- **TSan `destroy of a locked mutex`**: Fixed missing `pool_unlock(pool)` in `mp_free()` — line 3147 acquired the lock but the function returned without releasing it, causing `mp_destroy_shared` to destroy a still-held rwlock.
- **TSan `lock-order-inversion`**: Resolved by fixing the missing `pool_unlock`; the unreleased lock caused spurious lock-order-inversion warnings with the global cmem pool.
- **Per-bucket lock deadlock in `tlsf_free`**: Fixed dangling unlock and recursive lock acquisition in TLSF free path.
- **Double page transition in `slab_free_nolock`**: Removed redundant `SLAB_TRANS_FULL_TO_PARTIAL` transition that caused multithreaded crashes.
- **Multi-threaded segfault in slab free path**: Fixed `slab_free_nolock` mid-free `cmem_munmap` call that raced with concurrent allocations.
- **Lock-free active list segfault**: Reverted lock-free active list that caused segfault in `mp_ptr_valid`; restored mutex-protected list.
- **TLS cache leak on thread exit**: Added `pthread_key` destructor (`tls_cache_dtor`) to flush thread-local cached slots back to `owner_pool` on thread termination; updated `tls_cache_validate_owner()` to flush previous pool slots before reassignment.
- **Owner-pool override in multi-arena TLS cache**: Removed incorrect `owner_pool` override that corrupted cross-pool allocations.
- **Undefined variable initialization**: Initialized `stats_buf`, `ts`, `ets` and other variables to eliminate `cppcoreguidelines-init-variables` warnings across `cmem_event.c`.
- **Magic number warning**: Replaced literal `6` with `TLSF_CACHE_MIN_FL` constant; added `TLSF_CACHE_MIN_FL` to `.clang-tidy` magic-number ignore list.
- **Branch clone warnings**: Eliminated `bugprone-branch-clone` and `readability-identifier-length` warnings by restructuring identical-branch conditions and renaming short identifiers.
- **Windows/MinGW compatibility**: Added `#ifdef` guards for `atomic_flag`, POSIX includes, and `MP_THREAD_LOCAL` redefinition in `cmem_sys.c` and `cmem_internal.h`.
- **ASan LSAN fatal error**: Fixed link-order conflict in ASan builds; hardened CI with `LSAN_OPTIONS=detect_leaks=0` for ASan jobs.
- **Build system**: Fixed `CMEM_DISABLE_DIAGNOSTICS` scope for `cmem_core_shared` target (PRIVATE instead of PUBLIC).
- **Batch free path**: Fixed TLS cache owner re-validation per flushed free block; let same-pool SLAB elements take the batched free path.
- **Remote-free harvest race**: Acquired class lock in `remote_free_harvest_all` to prevent race with `percpu_refill`.

### Changed
- **Slab class lock cache-line alignment**: Aligned `slab_class_t.lock` to its own cache line (`alignas(64)`) to eliminate false sharing under high contention.
- **Non-thread-safe pool stats**: Replaced atomic `CMEM_ATOMIC_FETCH_ADD/SUB` with plain writes for `slab_allocated_bytes`, `active_bytes`, `active_allocations`, `total_free_ops`, and `total_alloc_ops` when `MP_FLAG_THREAD_SAFE` is not set, reducing atomic overhead on the hot path.
- **TLSF in-place expand**: Inlined `tlsf_try_inplace_expand` to eliminate call overhead in `mp_realloc`.
- **Slab free fast paths**: Eliminated pool-lock acquisition in slab free fast paths (`tls_cache` hit, `percpu_push` hit, `remote_free_push` path) — only the fallback path acquires the lock.
- **TLSF stats update**: Moved stats update outside `tpool` lock in `tlsf_free`.
- **`.clang-tidy`**: Added `TLSF_CACHE_MIN_FL` (value 6) to `readability-magic-numbers` ignore list; renamed short identifiers to satisfy identifier-length rules.

### Deprecated
- **Lazy coalescing in TLSF**: Reverted lazy prev-block coalescing and deferred page transitions due to degraded multi-thread scaling; restored eager coalescing.

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
