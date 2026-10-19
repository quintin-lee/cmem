# Decompose Public Headers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decompose `include/cmem.h` into modular subsystem headers (`cmem_ring.h`, `cmem_tlsf.h`, `cmem_diag.h`, `cmem_snapshot.h`, `cmem_metrics.h`, `cmem_arena.h`, `cmem_frame.h`, `cmem_typed_pool.h`), and update build targets in `CMakeLists.txt` and `Makefile`.

**Architecture:** Create 8 specialized subsystem headers under `include/`, update `include/cmem.h` to include all sub-headers by default for 100% backward compatibility, and update `CMakeLists.txt` and `Makefile` installation rules.

**Tech Stack:** C11, C++17 PMR, CMake, Ninja, Make

---

### Task 1: Create Subsystem Headers in `include/`

**Files:**
- Create: `include/cmem_ring.h`
- Create: `include/cmem_tlsf.h`
- Create: `include/cmem_snapshot.h`
- Create: `include/cmem_metrics.h`
- Create: `include/cmem_arena.h`
- Create: `include/cmem_frame.h`
- Create: `include/cmem_typed_pool.h`
- Modify: `include/cmem_diag.h`

- [ ] **Step 1: Create `include/cmem_ring.h`**

Define ring buffer allocator and event log ring buffer APIs in `include/cmem_ring.h`.

- [ ] **Step 2: Create `include/cmem_tlsf.h`**

Define TLSF specific constants and structures in `include/cmem_tlsf.h`.

- [ ] **Step 3: Create `include/cmem_snapshot.h`**

Define post-mortem binary crash dump snapshot and diff APIs in `include/cmem_snapshot.h`.

- [ ] **Step 4: Create `include/cmem_metrics.h`**

Define Prometheus metrics, JSON stats dump, histogram, P99/avg latency, and pprof APIs in `include/cmem_metrics.h`.

- [ ] **Step 5: Create `include/cmem_arena.h`**

Define hierarchical child pool, multi-arena partitioning, and arena quota APIs in `include/cmem_arena.h`.

- [ ] **Step 6: Create `include/cmem_frame.h`**

Define dual ping-pong frame arena APIs in `include/cmem_frame.h`.

- [ ] **Step 7: Create `include/cmem_typed_pool.h`**

Define typed object pool allocator APIs and macros in `include/cmem_typed_pool.h`.

- [ ] **Step 8: Update `include/cmem_diag.h`**

Ensure `include/cmem_diag.h` focuses on heap auditing and leak analysis APIs (snapshot/metrics functions delegated to `cmem_snapshot.h` and `cmem_metrics.h`).

- [ ] **Step 9: Commit**

```bash
git add include/
git commit -m "feat(headers): create modular subsystem headers under include/"
```

---

### Task 2: Refactor `include/cmem.h` to Include All Sub-Headers

**Files:**
- Modify: `include/cmem.h`

- [ ] **Step 1: Trim extracted declarations from `include/cmem.h`**

Remove declarations moved into subsystem headers from `include/cmem.h`.

- [ ] **Step 2: Add includes for all sub-headers in `include/cmem.h`**

At the bottom of `include/cmem.h`:
```c
#include "cmem_ring.h"
#include "cmem_tlsf.h"
#include "cmem_snapshot.h"
#include "cmem_arena.h"
#include "cmem_frame.h"
#include "cmem_typed_pool.h"

#if !defined(CMEM_DISABLE_DIAGNOSTICS) && (!defined(CMEM_ENABLE_DIAGNOSTICS) || CMEM_ENABLE_DIAGNOSTICS == 1)
#include "cmem_diag.h"
#include "cmem_metrics.h"
#endif
```

- [ ] **Step 3: Run formatting check**

Run: `clang-format -i include/*.h && make format-check`
Expected: Passes cleanly.

- [ ] **Step 4: Commit**

```bash
git add include/cmem.h
git commit -m "refactor(headers): include subsystem headers in cmem.h for backward compatibility"
```

---

### Task 3: Update `CMakeLists.txt` and `Makefile` for New Subsystem Headers

**Files:**
- Modify: `CMakeLists.txt` (edit format-check list and install FILES list)
- Modify: `Makefile` (edit install and uninstall targets)

- [ ] **Step 1: Update `CMakeLists.txt`**

Add all 7 new `.h` files to `format-check` list and `install(FILES ... DESTINATION include)`.

- [ ] **Step 2: Update `Makefile`**

Add all 7 new `.h` files to `install` and `uninstall` rules.

- [ ] **Step 3: Run full verification suite across build systems**

Run:
```bash
make format-check && make check-mermaid && make lib
cmake -B build_cmake -G Ninja -DCMEM_ENABLE_DIAGNOSTICS=OFF && cmake --build build_cmake
cmake -B build_cmake -G Ninja -DCMEM_ENABLE_DIAGNOSTICS=ON && cmake --build build_cmake && ctest --test-dir build_cmake
```
Expected: 100% tests pass, format check passes, all targets build cleanly.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt Makefile
git commit -m "build: update CMakeLists.txt and Makefile for modular subsystem headers"
```
