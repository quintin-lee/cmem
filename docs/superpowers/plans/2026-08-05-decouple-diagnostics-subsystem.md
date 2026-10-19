# Decouple Diagnostics Subsystem Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decouple the `cmem_diag` diagnostics subsystem from the core `cmem` allocator engine by creating `include/cmem_diag.h`, creating `libcmem_core.a`/`libcmem_diag.a` build targets, adding the `CMEM_ENABLE_DIAGNOSTICS` build option, and removing hardcoded diagnostic dependencies from the core allocator.

**Architecture:** Create `include/cmem_diag.h`, update `include/cmem.h` to conditionally include `cmem_diag.h`, decouple `src/cmem_event.c` from `cmem_diag.c`, update `CMakeLists.txt` (editing all THREE required places per `AGENTS.md`), and update `Makefile`.

**Tech Stack:** C11, C++17 PMR, GCC/Clang, CMake, Ninja, Make

---

### Task 1: Create `include/cmem_diag.h` and Move Diagnostic Declarations from `include/cmem.h`

**Files:**
- Create: `include/cmem_diag.h`
- Modify: `include/cmem.h:1200-1400`
- Modify: `include/cmem.hpp` (if needed)

- [ ] **Step 1: Create `include/cmem_diag.h` with diagnostic types and function prototypes**

Extract diagnostic structures and prototypes from `include/cmem.h` into `include/cmem_diag.h`:

```c
/**
 * @file cmem_diag.h
 * @brief Diagnostics, Inspection, Leak Analysis, and Telemetry Exporters for cmem.
 */

#ifndef CMEM_DIAG_H
#define CMEM_DIAG_H

#include "cmem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MP_LEAK_SEVERITY_CRITICAL,
    MP_LEAK_SEVERITY_WARNING,
    MP_LEAK_SEVERITY_INFO
} mp_leak_severity_t;

typedef struct {
    const char *pattern_name;
    int confidence;
    const char *suggestion;
} mp_leak_pattern_t;

bool mp_audit_heap(memory_pool_t *pool);
bool mp_check_leaks(memory_pool_t *pool);
size_t mp_analyze_leaks(memory_pool_t *pool, char *report_buf, size_t max_len);
bool mp_export_leak_report(memory_pool_t *pool, const char *filepath);
bool mp_export_html_report(memory_pool_t *pool, const char *filepath);
bool mp_export_binary_snapshot(memory_pool_t *pool, const char *filepath);
bool mp_parse_binary_snapshot(const char *filepath, char *out_report, size_t max_len);
bool mp_diff_snapshots(const char *snapshot_a_path,
                               const char *snapshot_b_path,
                               char *out_report,
                               size_t max_len);
void mp_dump_info(memory_pool_t *pool);
void mp_dump_histogram(memory_pool_t *pool);
void mp_dump_tree_info(memory_pool_t *pool);
size_t mp_dump_json_stats(memory_pool_t *pool, char *buf, size_t max_len);
size_t mp_export_prometheus_metrics(memory_pool_t *pool, char *out_buf, size_t max_len);
size_t mp_export_pprof(memory_pool_t *pool, char *out_buf, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif // CMEM_DIAG_H
```

- [ ] **Step 2: Update `include/cmem.h` to conditionally include `cmem_diag.h`**

In `include/cmem.h`, include `cmem_diag.h` by default (unless `CMEM_DISABLE_DIAGNOSTICS` is defined or `CMEM_ENABLE_DIAGNOSTICS=0`):

```c
#if !defined(CMEM_DISABLE_DIAGNOSTICS) && (!defined(CMEM_ENABLE_DIAGNOSTICS) || CMEM_ENABLE_DIAGNOSTICS == 1)
#include "cmem_diag.h"
#endif
```

- [ ] **Step 3: Run formatting check**

Run: `clang-format -i include/cmem.h include/cmem_diag.h && make format-check`
Expected: Passes formatting check cleanly.

- [ ] **Step 4: Commit**

```bash
git add include/cmem.h include/cmem_diag.h
git commit -m "feat(diag): create include/cmem_diag.h and decouple diagnostic header declarations"
```

---

### Task 2: Decouple Core Allocator (`src/cmem_event.c` and `src/cmem_internal.h`) from Diagnostics

**Files:**
- Modify: `src/cmem_internal.h:350-450`
- Modify: `src/cmem_event.c:1410-1430`

- [ ] **Step 1: Guard latency histogram fields in `memory_pool_t` with `CMEM_ENABLE_DIAGNOSTICS`**

In `src/cmem_internal.h`, wrap latency histogram and event log ring buffer fields with `#if !defined(CMEM_DISABLE_DIAGNOSTICS)`:

```c
#if !defined(CMEM_DISABLE_DIAGNOSTICS)
    size_t alloc_latency_histogram[32]; /**< Buckets for allocation-latency samples */
    size_t alloc_latency_count;         /**< Total latency samples                 */
    uint64_t alloc_latency_sum_ns;      /**< Accumulated latency (ns) for avg       */
#endif
```

- [ ] **Step 2: Remove implicit `mp_check_leaks()` from `mp_destroy()` in `src/cmem_event.c`**

In `src/cmem_event.c`, update `mp_destroy()`:

```c
    if (pool->flags & MP_FLAG_REPORT_LEAKS_ON_DESTROY) {
#if !defined(CMEM_DISABLE_DIAGNOSTICS)
        mp_check_leaks(pool);
#endif
    }
```

- [ ] **Step 3: Run build and unit tests**

Run: `make format-check && make test && ctest --test-dir build_cmake`
Expected: All unit tests pass 100%.

- [ ] **Step 4: Commit**

```bash
git add src/cmem_internal.h src/cmem_event.c
git commit -m "refactor(core): decouple core destructor and pool header from diagnostics"
```

---

### Task 3: Update `CMakeLists.txt` and `Makefile` to Add Core and Diag Library Targets

**Files:**
- Modify: `CMakeLists.txt:50-180`
- Modify: `Makefile:10-80`

- [ ] **Step 1: Update `CMakeLists.txt` in all THREE required places per `AGENTS.md`**

In `CMakeLists.txt`:
1. Add `include/cmem_diag.h` to the `format-check` file list (~line 52).
2. Define `cmem_core` static and shared library targets containing only `src/cmem.c`, `src/cmem_compress.c`, `src/cmem_event.c`, `src/cmem_slab.c`, `src/cmem_sys.c`, `src/cmem_tlsf.c`.
3. Define `cmem_diag` static and shared library targets containing `src/cmem_diag.c`.
4. Keep `cmem` (full library containing all sources) for backward compatibility.
5. Add `option(CMEM_ENABLE_DIAGNOSTICS "Enable diagnostics subsystem" ON)`.

- [ ] **Step 2: Update `Makefile` with `core` target and `NO_DIAG` switch**

In `Makefile`:
Add `NO_DIAG` variable and `core` build target producing `build/libcmem_core.a`.

- [ ] **Step 3: Run format check, Mermaid check, and full test suite across build systems**

Run:
```bash
clang-format -i include/cmem.h include/cmem_diag.h src/*.c
make format-check && make check-mermaid && make bench
cmake -B build_cmake -G Ninja -DCMEM_ENABLE_DIAGNOSTICS=OFF && cmake --build build_cmake
cmake -B build_cmake -G Ninja -DCMEM_ENABLE_DIAGNOSTICS=ON && cmake --build build_cmake && ctest --test-dir build_cmake
```
Expected: All build targets compile cleanly, tests pass 100%, `nm -D build_cmake/libcmem_core.so` contains no `mp_analyze_leaks` symbols.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt Makefile
git commit -m "build: add cmem_core and cmem_diag build targets with CMEM_ENABLE_DIAGNOSTICS option"
```
