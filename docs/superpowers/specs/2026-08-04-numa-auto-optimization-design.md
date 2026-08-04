# NUMA Auto-Optimization Design

- **Date**: 2026-08-04
- **Status**: Draft (pending review)
- **Scope**: cmem library — `MP_FLAG_AUTO_NUMA` automatic NUMA-aware allocation

## 1. Motivation

cmem currently offers manual NUMA binding via `mp_set_numa_node(pool, node)`:
every system allocation is pinned to the configured node with `mbind(MPOL_BIND)`.
This requires the user to detect topology themselves, create per-node pools, and
assign threads manually — error-prone and not adaptive to thread migration.

The architecture document (§12, "NUMA Auto-Optimization") lists automatic NUMA
node detection and binding as a future direction. This design implements it as a
per-pool flag that tracks the allocating thread's current node and binds each
backing allocation to it.

## 2. Goals

- Automatically bind backing memory to the NUMA node of the thread performing
  the allocation (local-first locality), with no manual node configuration.
- Expose minimal, safe query APIs for topology introspection.
- Zero behavioral change when the flag is not used; graceful degradation on
  non-Linux platforms and non-NUMA systems.
- Keep the existing `mp_set_numa_node` API and semantics intact; explicit manual
  binding takes precedence over automatic selection.

## 3. Non-Goals

- No per-node arenas or per-node freelists (deferred to a future, larger effort).
- No RDMA / distributed memory (separate roadmap item).
- No automatic thread migration or scheduler changes.
- No Windows/macOS NUMA implementation (flag accepted but no-op there).

## 4. Public API

### 4.1 New flag

```c
#define MP_FLAG_AUTO_NUMA (1u << 16)   /* auto-bind allocations to current node */
```

- Added to the existing `mp_flags_t` bitmask in `include/cmem.h`.
- Accepted on every platform; only takes effect on Linux when a NUMA topology
  is detected.

### 4.2 New query functions

```c
/* Number of NUMA nodes detected (>= 1; 1 on non-NUMA or unsupported platforms). */
int mp_numa_node_count(void);

/* NUMA node id owning the given CPU (0-based CPU index); 0 on failure/unknown. */
int mp_cpu_to_node(int cpu);
```

- Declared in `include/cmem.h`, implemented in `src/cmem_sys.c`.
- Thread-safe; results cached after first call.

### 4.3 Interaction with existing API

- `mp_set_numa_node(pool, node)` remains the explicit override. When a pool has
  `MP_FLAG_AUTO_NUMA` AND a manually-set node, the manual node wins.

## 5. Internal Design

### 5.1 Topology cache (`cmem_internal.h` + `cmem_sys.c`)

New internal structure:

```c
typedef struct cmem_numa_topology {
    int  node_count;   /* number of NUMA nodes (>= 1)              */
    int *cpu_to_node;  /* array indexed by CPU id -> node id       */
    int  cpu_count;    /* number of valid entries in cpu_to_node   */
} cmem_numa_topology_t;
```

Initialization (lazy, once, thread-safe via atomic guard):

1. Read `/sys/devices/system/node/online` → node count. On any failure or a
   single-node system, `node_count = 1` and no mbind is ever issued.
2. Read `/sys/devices/system/cpu/possible` for CPU count.
3. For each node, read `/sys/devices/system/node/node<N>/cpulist` and populate
   `cpu_to_node`. Any read failure → node_count 1 fallback.

This keeps the implementation dependency-free (no libnuma), consistent with the
project's syscall-based `SYS_mbind` approach already in `src/cmem_sys.c`.

### 5.2 Allocation path change (`sys_mem_alloc`, Linux branch)

```c
#if defined(__linux__) && defined(SYS_mbind)
    int numa_node = (pool != NULL) ? pool->numa_node : -1;
    if (numa_node < 0 && pool != NULL && (pool->flags & MP_FLAG_AUTO_NUMA)) {
        numa_node = cmem_numa_current_node();   /* sched_getcpu -> lookup cache */
    }
    if (numa_node >= 0 && ptr != NULL) {
        unsigned long nodemask = (1UL << numa_node);
        syscall(SYS_mbind, ptr, size, CMEM_MPOL_BIND, &nodemask, sizeof(nodemask) * 8, 0);
    }
#endif
```

`cmem_numa_current_node()` uses the existing `cmem_sched_getcpu()` and looks up
the cached topology, falling back to `-1` (no binding) when topology is unknown.

### 5.3 Diagnostics

- The first time an AUTO_NUMA pool performs an allocation, log a one-time
  topology summary:
  `[CMEM NUMA] Auto-NUMA enabled: N nodes detected (CPU x -> node y, ...)`
- Uses the existing diagnostic logging style (see `mp_set_numa_node`).

## 6. Error Handling / Edge Cases

| Case | Behavior |
| :--- | :--- |
| Non-Linux platform | Flag accepted; no mbind; query APIs return node 0 / count 1 |
| Single-node system | Topology count 1; auto-selection yields node 0; binding harmless |
| mbind syscall fails | Silently ignored (same as today's manual path) |
| sched_getcpu fails/unsupported | Falls back to no binding for that allocation |
| CPU id outside cached range | `mp_cpu_to_node` returns 0; auto path skips binding |
| Manual node set + AUTO flag | Manual node wins (explicit override) |

## 7. Testing

### 7.1 Unit tests (`tests/test_main.c`)

- `mp_numa_node_count() >= 1` always holds.
- `mp_cpu_to_node(0)` returns a valid node in `[0, count)` (Linux); on
  non-NUMA returns 0.
- `mp_cpu_to_node(-1)` and `mp_cpu_to_node(huge)` return 0 (bounds).
- Pool with `MP_FLAG_AUTO_NUMA` creates and allocates successfully (behavioral
  no-op on CI's single-node machine).
- Pool with AUTO + explicit `mp_set_numa_node(5)` still binds to 5 (manual
  precedence — verified via diagnostics/log or code path, not hardware).

### 7.2 Manual hardware validation (documented, not automated)

- `numactl --hardware` on a 2-node machine; create an AUTO_NUMA pool; run
  `numastat` / `/proc/<pid>/numa_maps` to confirm pages land on the allocating
  thread's node.

### 7.3 Regression

- Existing suite (ctest, unit_tests, advanced_tests, stress_test) must remain
  green; zero clang-tidy warnings on all touched files.

## 8. Files Touched

| File | Change |
| :--- | :--- |
| `include/cmem.h` | New `MP_FLAG_AUTO_NUMA`; declare `mp_numa_node_count`, `mp_cpu_to_node` |
| `src/cmem_internal.h` | `cmem_numa_topology_t` struct; extern declarations |
| `src/cmem_sys.c` | Topology probe + cache; `cmem_numa_current_node()`; auto path in `sys_mem_alloc`; two new public functions |
| `tests/test_main.c` | NUMA query/bounds/flag tests |
| `docs/en/performance.md`, `docs/zh/performance.md` | Document `MP_FLAG_AUTO_NUMA` + new APIs |
| `CHANGELOG.md` | Unreleased entry |

## 9. Resolved Decisions

- Flag bit `(1u << 16)` — confirmed free. `MP_FLAG_REPORT_LEAKS_ON_DESTROY`
  already occupies bit 15 (verified against `include/cmem.h`); bit 16 is the
  next unused bit in `mp_flags_t`.
- No `mp_numa_bind_pool()` convenience API — flag-only activation (YAGNI).
