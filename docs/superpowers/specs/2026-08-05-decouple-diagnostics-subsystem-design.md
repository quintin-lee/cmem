# Spec: Decouple Diagnostics Subsystem into Separate Header and Library Targets

## Overview

This specification defines the architectural decoupling of the diagnostics and observability subsystem (`cmem_diag`) from the core `cmem` allocator engine. By physically separating public headers into `include/cmem.h` (core allocator) and `include/cmem_diag.h` (diagnostics & telemetry), creating distinct build targets (`libcmem_core.a` / `libcmem_diag.a`), and introducing the `CMEM_ENABLE_DIAGNOSTICS` build option, `cmem` can be compiled into a lightweight core memory manager for embedded systems with zero diagnostic overhead.

---

## Detailed Requirements

### 1. Header File Split (`include/cmem.h` & `include/cmem_diag.h`)

- **`include/cmem.h`**:
  - Retain all core pool creation, allocation, freeing, batching, arena management, alignment, static buffer, and compression APIs.
  - Forward-declare `memory_pool_t` and basic stats (`mp_stats_t`, `mp_allocation_info_t`).
  - Move all diagnostic-specific declarations (`mp_audit_heap`, `mp_check_leaks`, `mp_analyze_leaks`, `mp_export_leak_report`, `mp_export_html_report`, `mp_export_binary_snapshot`, `mp_parse_binary_snapshot`, `mp_diff_snapshots`, `mp_dump_info`, `mp_dump_histogram`, `mp_dump_tree_info`, `mp_dump_json_stats`, `mp_export_prometheus_metrics`, `mp_export_pprof`, `mp_leak_severity_t`, `mp_leak_pattern_t`) to `include/cmem_diag.h`.
  - Include `#include "cmem_diag.h"` in `include/cmem.h` only when `CMEM_ENABLE_DIAGNOSTICS` is enabled or in full-compatibility mode.

- **`include/cmem_diag.h`**:
  - Dedicated public header for introspection, heap auditing, leak analysis, snapshot diffing, and telemetry exporters (Prometheus, pprof, HTML, JSON, binary crash dumps).

### 2. Core Allocator Decoupling (`src/cmem_event.c` & `src/cmem.c`)

- Remove implicit calls to `mp_check_leaks()` from `mp_destroy()` in `src/cmem_event.c`. Leak checks will only be triggered when explicitly called or when `MP_FLAG_REPORT_LEAKS_ON_DESTROY` is explicitly set under `CMEM_ENABLE_DIAGNOSTICS`.
- Wrap diagnostic-only fields in `memory_pool_t` (`alloc_latency_histogram`, `alloc_latency_count`, `alloc_latency_sum_ns`) with `#ifdef CMEM_ENABLE_DIAGNOSTICS` guards to shrink pool header memory footprint in embedded core builds.

### 3. Build System Targets (`CMakeLists.txt` & `Makefile`)

- **Libraries**:
  - `cmem_core` / `libcmem_core.a` / `libcmem_core.so`: Contains only `cmem.c`, `cmem_slab.c`, `cmem_tlsf.c`, `cmem_sys.c`, `cmem_compress.c`, `cmem_event.c`. Zero dependency on `cmem_diag.c`.
  - `cmem_diag` / `libcmem_diag.a` / `libcmem_diag.so`: Contains `cmem_diag.c`, linking against `cmem_core`.
  - `cmem` / `libcmem.a` / `libcmem.so`: Full combined library (containing both `cmem_core` and `cmem_diag`) for backward compatibility.
- **Build Options**:
  - CMake option: `-DCMEM_ENABLE_DIAGNOSTICS=ON/OFF` (default `ON`). When `OFF`, builds `cmem_core` only without `cmem_diag.c`.
  - Makefile target: `make core` / `make NO_DIAG=1`.

---

## File Changes Summary

- **Create `include/cmem_diag.h`**:
  - Move diagnostic API prototypes and diagnostic data types from `include/cmem.h`.
- **Modify `include/cmem.h`**:
  - Keep core allocator APIs and conditionally include `cmem_diag.h`.
- **Modify `src/cmem_event.c` & `src/cmem_internal.h`**:
  - Remove implicit calls to `mp_check_leaks()` from core destructor.
  - Guard latency histogram fields with `#ifdef CMEM_ENABLE_DIAGNOSTICS`.
- **Modify `CMakeLists.txt` & `Makefile`**:
  - Add `cmem_core` static and shared library targets.
  - Add `cmem_diag` library targets.
  - Add `CMEM_ENABLE_DIAGNOSTICS` option.
  - Update `format-check` and `add_library` file lists per `AGENTS.md` rules.

---

## Verification Plan

1. **Compilation & Formatting**:
   - `make format-check` and `make check-mermaid` pass cleanly.
   - `cmake -B build_cmake -G Ninja -DCMEM_ENABLE_DIAGNOSTICS=OFF && cmake --build build_cmake` builds lightweight `cmem_core` successfully.
   - `cmake -B build_cmake -G Ninja && cmake --build build_cmake` builds both `cmem_core` and `cmem_diag` successfully.
2. **Unit Tests & Integration**:
   - `ctest --test-dir build_cmake` passes 100%.
3. **Symbol Verification**:
   - Verify `nm -D build_cmake/libcmem_core.so` contains no `mp_analyze_leaks` or `mp_export_prometheus_metrics` symbols.
