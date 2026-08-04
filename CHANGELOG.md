# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.2] - 2026-08-02

### Documentation
- Updated all documentation to reflect current API surface
- Added missing API sections for `mp_get_allocation_info` and `mp_enumerate_regions`
- Fixed Event Types enumeration values to match header (`MP_EVENT_ALLOC = 1`, removed `MP_EVENT_WATERMARK_HIGH/LOW/CUSTOM`)
- Fixed test count from 35+ to 44+ across all docs
- Fixed GitHub links from `your-repo` to `quintin-lee/cmem`
- Updated MSVC compiler version from 2022+ to 2019+
- Added architecture sections for Memory Error Recovery, Thread Quota & Circuit Breaker

## [1.0.1] - 2026-08-02

## [Unreleased]

### Added
- `VERSION` file as single source of truth for version number
- `scripts/tag.sh` for automated version bumping and git tagging
- `compile_commands.json` auto-generation via `set(CMAKE_EXPORT_COMPILE_COMMANDS ON)` for clangd/clang-tidy/ccls integration
- Standalone `main()` for `fuzz_alloc` via `STANDALONE_FUZZ` compile definition
- GitHub Release workflow with auto-generated release notes and multi-platform artifacts
- `CONTRIBUTING.md` with development workflow and gitmoji commit conventions
- Codecov coverage threshold enforcement (`80%` project, `80%` patch)
- Docker reproducible build support (`Dockerfile`, `.dockerignore`, `make docker-build`)
- `CODE_OF_CONDUCT.md` (Contributor Covenant 2.1)
- Targeted coverage tests for OS fallback allocation, debug canary/zero flags, slab full-page transitions, and lazy RSS purge paths
- `mp_get_allocation_info()` for per-allocation metadata inspection (type, size, source location, backtrace)
- `mp_enumerate_regions()` for enumerating all backing memory regions (Slab pages, TLSF pools, OS mappings)
- `MP_FLAG_REPORT_LEAKS_ON_DESTROY` for automatic leak reporting on `mp_destroy()`
- C++ `MemoryPool` wrappers: `get_allocation_info()` and `enumerate_regions()`
- Windows/MSVC support: `mmap`/`madvise` backed by `VirtualAlloc`, corrected `Interlocked` atomic fallback semantics, and a dedicated MSVC CMake branch (`/W3 /std:c11` with `_STDC_LIMIT_MACROS` / `_STDC_FORMAT_MACROS`)

### Changed
- Updated benchmark results in README with real measured values
- Strengthened API/ABI stability promise in README
- Documented GitHub Private Vulnerability Reporting setup in `SECURITY.md`
- Added ThreadSanitizer badge, Docker instructions, and runtime analysis section to README
- Added test step to release workflow for pre-release validation
- Build now defines `-D_GNU_SOURCE` (replaces per-file `_POSIX_C_SOURCE` / `_GNU_SOURCE` feature-test macros) in both CMake and Makefile
- Fixed `make lib` to compile each source into a per-file object before archiving (multi-source `-c` with a single `-o` was rejected by GCC)
- Eliminated all clang-tidy warnings (368) across library sources, tests, benchmarks, and examples; tuned `.clang-tidy` (magic-number ignore list, powers-of-two exemption, constant-int-expression exemption)

### Fixed
- Removed redundant `pool_lock`/`pool_unlock` in `tlsf_alloc` to prevent rwlock recursion deadlock
- Fixed ASan integration test assertion for `mp_asan_check_memory(pool, NULL, 0)`
- Enforced memory limit before huge allocation test for cross-platform portability
- Guarded `__has_extension` usage in `cmem_override.h` for GCC compatibility
- Fixed clang-format violations in `tests/fuzz_alloc.c`
- Linked `fuzz_alloc` with `STANDALONE_FUZZ` definition in CMake
- Fixed lcov `exclude` pattern error in `.github/workflows/coverage.yml` causing CI exit code 25
- Resolved duplicate `error_recovery_cb` definition and clang-format violations in `tests/test_advanced.c`

### Documentation
- Updated LICENSE copyright year to `2024-2026`
- Added OS-specific ignore patterns to `.gitignore`
- Updated platform support matrices (Windows/MSVC) and build/toolchain notes across README and `docs/`

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
