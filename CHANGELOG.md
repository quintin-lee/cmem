# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Standalone `main()` for `fuzz_alloc` via `STANDALONE_FUZZ` compile definition
- GitHub Release workflow with auto-generated release notes and multi-platform artifacts
- `CONTRIBUTING.md` with development workflow and gitmoji commit conventions
- Codecov coverage threshold enforcement (`85%` project, `80%` patch)

### Changed
- Updated benchmark results in README with real measured values
- Strengthened API/ABI stability promise in README
- Documented GitHub Private Vulnerability Reporting setup in `SECURITY.md`

### Fixed
- Removed redundant `pool_lock`/`pool_unlock` in `tlsf_alloc` to prevent rwlock recursion deadlock
- Fixed ASan integration test assertion for `mp_asan_check_memory(pool, NULL, 0)`
- Enforced memory limit before huge allocation test for cross-platform portability
- Guarded `__has_extension` usage in `cmem_override.h` for GCC compatibility
- Fixed clang-format violations in `tests/fuzz_alloc.c`
- Linked `fuzz_alloc` with `STANDALONE_FUZZ` definition in CMake

### Documentation
- Updated LICENSE copyright year to `2024-2026`

## [1.0.0] - 2026-07-27

### Added
- Production readiness: implemented 5 missing public APIs (`mp_get_percpu_freelist`, `mp_get_percpu_cpu_count`, `mp_set_fallback_on_oom`, `mp_set_gc_callback`, `mp_set_eviction_callback`)
- Advanced C unit tests covering previously untested APIs
- Per-CPU lock-free freelist for low-contention fast path
- Thread-local cache optimization achieving 1.73x speedup over system malloc
- Configurable Slab Class table API
- Runtime config hot-reload via `CMEM_CONF` environment variable
- Auto-compaction trigger and arena quota enforcement
- Latency P99 statistics recording
- Structured event log ring buffer and pprof export
- Memory error recovery with dirty pool marking and bad-block isolation
- Graceful degradation with fallback malloc, GC and eviction callbacks
- Thread-level quota and circuit breaker
- ABI versioning and cgroup awareness
- Online pool expansion without service interruption
- AddressSanitizer integration layer
- Encrypted memory support with `mlock`, `MADV_DONTDUMP` and secure zero
- Hot/Cold page separation for TLB optimization
- C++17 PMR adapter and STL allocator
- Memory introspection APIs (`mp_usable_size`, `mp_alloc_size`, `mp_ptr_valid`, `mp_preferred_size`)
- Memory compaction and OS page reclamation (`mp_trim`, `mp_madvise`, `mp_compact`, `mp_purge_lazy`, `mp_resident`, `mp_freeable`)
- High/low watermark threshold alert callbacks
- Batch allocations and memory compaction
- Leak analysis report and heap audit
- Binary crash memory snapshot dump and parser with incremental diff leak detection
- Interactive HTML profiler report export
- Prometheus / OpenTelemetry metrics exporter
- POSIX shared memory pool and zero-copy IPC
- Global `malloc`/`free` symbol interception (`cmem_override.h`)
- Real-time allocation QPS and bandwidth throughput meter
- 0-overhead typed object pool allocator
- Cache line 64B alignment and false sharing elimination
- Allocation size histogram diagnostics
- Child arenas and visual HTML report export
- Fast arena reset and JSON exporter
- Static buffer arena and event callbacks
- Guard pages protection via `PROT_NONE`
- Linux HugePages (`MAP_HUGETLB`) acceleration
- DPDK-style ultra-fast lock-free ring buffer allocator
- `mp_reallocarray` overflow-safe reallocation
- Convenience helpers (`mp_strdup`, `mp_memdup`, `mp_asprintf`)
- Tree-shaped memory arena navigation with recursive destroy/reset
- Arena metadata APIs (`mp_set_name`, `mp_get_name`, `mp_get_parent`, `mp_get_child_count`)
- Bilingual documentation (English and Chinese)
- ABI stability policy and platform support matrix
- Shields.io badges in README

### Fixed
- Suppress unused variable warnings in example programs
- Use `CMAKE_CURRENT_SOURCE_DIR` for clang-format paths in CMake
- Replace `fscanf` with `fgets+strtoull` for robust cgroup parsing
- Check `fread` return value in `mp_diff_snapshots`
- Fix `mp_reparse_env_flags` return type for C++ compilation
- Position ASan flags before compiler inputs in Makefile to fix link order error
- Validate pointer via `mp_ptr_valid` before dereferencing header in `mp_usable_size` and `mp_alloc_size`
- Zero-initialize test buffer to eliminate uninitialized stack memory warning in introspection tests

### Documentation
- Comprehensive Doxygen documentation for all public APIs
- Architecture, API reference, development, testing, performance and security guides
- Enhanced README with architecture overview, C++17 PMR usage, concurrency model and complete API reference
