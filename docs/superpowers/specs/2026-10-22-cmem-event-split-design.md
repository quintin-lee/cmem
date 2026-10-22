# Design: Split `cmem_event.c` by Responsibility — 6-File Decomposition

- **Date:** 2026-10-22
- **Status:** Draft for review
- **Type:** Pure refactoring — zero behavior change, no `.c`-level API change, no test change
- **Round:** 2 of the "优化完善" engineering-structure thread (round 1: header decomposition, commits `3f9767f` + `eaa8587`)

## 1. Context & Problem

`src/cmem_event.c` is **4152 lines** — the largest source file in the project (~2.4× the next, `cmem_slab.c` at 1975). It mixes 7 distinct responsibility clusters in one translation unit:

| Concern | Examples |
|---------|----------|
| Error recovery + thread quota + circuit breaker + ABI + cgroup + ASan | `mp_mark_pool_dirty`, `mp_set_thread_quota`, `mp_abi_version`, `mp_set_cgroup_aware`, `mp_asan_is_enabled` |
| Ring + event log + pprof + typed-pool + shared-memory destroy | `mp_ring_free`, `mp_event_log_record`, `mp_export_pprof`, `mp_typed_free`, `mp_destroy_shared` |
| Env-flag parsing + introspection + hot-reload + auto-compact + watermark + latency + slab-table config | `mp_parse_env_flags`, `mp_pressure`, `mp_reparse_env_flags`, `mp_record_latency`, `mp_set_slab_classes` |
| Lifecycle (destroy/reset) + idle-page reclaim + callbacks + NUMA + emergency reserve | `mp_destroy`, `mp_reset`, `mp_reclaim_idle_pages`, `mp_set_numa_node` |
| Alloc/free hot paths + batch + introspection + frame tail (THE MEAT) | `mp_alloc_batch`, `mp_free_batch`, `mp_free`, `mp_usable_size`, `mp_ptr_valid`, `mp_frame_end` |
| Per-thread multi-arena lifecycle | `mp_enable_multi_arena`, `mp_bind_thread_to_arena`, `mp_get_arena_count` |

A single 4152-line TU is hard to review, hard to bisect, and forces every build of the library to recompile the whole file when any one concern changes. Splitting by responsibility keeps the public API identical (all `mp_*` symbols still export from `libcmem.so`) while making each new file a focused unit.

## 2. Goal

Decompose `src/cmem_event.c` into **six** responsibility-scoped `.c` files and **delete** `cmem_event.c`, such that:

- Every public symbol previously exported still exports (verified by `nm -D` symbol-delta = empty).
- The three CTest suites (unit / advanced / cpp) pass under ASan/UBSan, unchanged.
- The build system (CMake `CMEM_CORE_SRCS` + format-check list, Makefile.make `CORE_SRCS` filter) is updated so no target loses the moved code.
- No `.h` public API change (the round-1 header layout is untouched).

## 3. Non-Goals (YAGNI)

- Do **not** change any public `mp_*` signature, constant value, or error-code.
- Do **not** touch `include/cmem.h`, `include/cmem_internal.h`, or any subsystem header (round-1's territory) **except** the one new `#define` hoist in `cmem_internal.h` (§6).
- Do **not** change test files, benchmarks, or examples.
- Do **not** rename public functions.
- Do **not** reorder or rewrite function bodies — this is a *move*, not a rewrite. Each function's source text is copied verbatim into its new home.

## 4. Six-File Layout

`cmem_event.c` cut points (verified by `awk`/`grep` against the current file):

| New file | Cluster | `cmem_event.c` lines | Public fns moved |
|----------|---------|----------------------|------------------|
| `src/cmem_guard.c` | A | L33–409 | `mp_mark_pool_dirty`, `mp_clear_pool_dirty`, `mp_is_pool_dirty`, `mp_isolate_bad_block`, `mp_set_thread_quota`, `mp_set_circuit_breaker`, `mp_get_thread_allocated_bytes`, `mp_reset_thread_quota`, `mp_is_circuit_breaker_tripped`, `mp_abi_version`, `mp_set_cgroup_aware`, `mp_get_cgroup_mem_limit`, `mp_asan_is_enabled`, `mp_asan_report_error`, `mp_asan_check_memory`, `mp_set_asan_integration` |
| `src/cmem_ring_event.c` | B+C | L410–817 | `mp_ring_free`, `mp_ring_destroy`, `mp_event_log_destroy`, `mp_event_log_record`, `mp_event_log_consume`, `mp_event_log_pending`, `mp_event_log_clear`, `mp_export_pprof`, `mp_typed_free`, `mp_typed_pool_destroy`, `mp_destroy_shared` |
| `src/cmem_config.c` | D | L818–1452 | `mp_default_pool_flags`, `mp_parse_env_flags`, `mp_set_name`, `mp_get_child_count`, `mp_pressure`, `mp_freeable`, `mp_resident`, `mp_reparse_env_flags`, `mp_get_env_generation`, `mp_auto_compact_check`, `mp_check_arena_quota`, `mp_set_fallback_on_oom`, `mp_set_gc_callback`, `mp_set_eviction_callback`, `mp_record_latency`, `mp_get_latency_p99`, `mp_get_latency_avg`, `mp_reset_latency_stats`, `mp_reset_stats`, `mp_preferred_size`, `mp_preferred_size_for_pool`, `mp_set_slab_classes`, `mp_get_slab_class_count`, `mp_get_slab_classes` |
| `src/cmem_lifecycle.c` | E | L1453–1958 | `mp_destroy`, `mp_reset`, `mp_set_idle_page_reclaim`, `mp_reclaim_idle_pages`, `mp_get_idle_page_count`, `mp_set_event_callback`, `mp_set_numa_node`, `mp_enable_emergency_reserve` |
| `src/cmem_alloc.c` | F | L1959–3999 | `mp_alloc_batch`, `mp_free_batch`, `mp_free`, `mp_usable_size`, `mp_alloc_size`, `mp_enumerate_regions`, `mp_ptr_valid`, `mp_get_allocation_info`, `mp_frame_end`, `mp_frame_arena_destroy` |
| `src/cmem_multi_arena.c` | G | L4000–4152 | `mp_purge_thread_arena_bindings`, `mp_enable_multi_arena`, `mp_bind_thread_to_arena`, `mp_get_arena_count` |

**Prologue (L1–32):** each new file gets its own prologue: `#include "cmem.h"` + `#include "cmem_internal.h"` + the standard-library includes it needs (`<fcntl.h>` for the shared-mem fn, `<stdarg.h>` for `mp_export_pprof`, `<stdlib.h>`). The 7 `#define`s at L20–27 are distributed per §6.

**Static functions:** move *with* their cluster (each static is used only by same-cluster fns, verified):

| Static | Home | Used by |
|--------|------|---------|
| `active_list_add` (L1920) / `active_list_remove` (L1935) | `cmem_lifecycle.c` | `mp_destroy` / `mp_reset` |
| `mp_free_stats_update` (L1959) | `cmem_alloc.c` | `mp_free_batch` + `mp_free` |
| `mp_free_stats_no_lock` (L1987) | `cmem_alloc.c` | `mp_free` |
| `batch_format_emergency` (L2513) / `batch_format_os` (L2548) / `batch_breaker_accrue` (L2592) | `cmem_alloc.c` | `mp_alloc_batch` |
| `flush_slab_block` (L2973) | `cmem_alloc.c` | `mp_free_batch` |
| `mp_verify_free_canary` (L3123) | `cmem_alloc.c` | `mp_free` |
| `mp_fastpath_slot_live` (L3680) | `cmem_alloc.c` | `mp_ptr_valid` + `mp_get_allocation_info` |
| `tls_arena_map` (pthread TLS, L3989) | `cmem_multi_arena.c` | cluster G fns |

> Cross-cluster static usage was verified to be **zero** — no static is called from a different cluster. If a future grep contradicts this during implementation, stop and re-scope.

## 5. Build-System Edits (3 files)

AGENTS.md's "3 CMakeLists edits" note is **stale** for the current layout; the actual edit surface is:

1. **`CMakeLists.txt` — `CMEM_CORE_SRCS` (≈L178–185).** This is the single source of truth; all six lib targets (`cmem_core`, `cmem_core_shared`, `cmem`, `cmem_diag`, `cmem_shared`, `cmem_diag_shared`) reference `${CMEM_CORE_SRCS}`. Replace `src/cmem_event.c` with the six new files.
2. **`CMakeLists.txt` — `format-check` file list (≈L59–66).** Add the six new `.c`, remove `src/cmem_event.c` from the clang-format `--Werror` set.
3. **`Makefile.make` — `CORE_SRCS` filter (L38).** `CORE_SRCS = $(filter src/cmem.c src/cmem_slab.c src/cmem_tlsf.c src/cmem_sys.c src/cmem_event.c src/cmem_compress.c, $(ALL_SRC))`. Replace `src/cmem_event.c` in the filter with the six new names. `ALL_SRC=$(wildcard src/*.c)` (L30) auto-picks the new files for test/example/bench executables, so only the `CORE_SRCS`-filtered lib targets need the filter fix.

Verify after each build: `nm -D build_cmake/libcmem.so` — the exported-symbol set must be identical to the pre-split baseline (see §8).

## 6. `#define` Ownership (one hoist, six in-file)

| `#define` | Value | Used at (`cmem_event.c`) | New home |
|-----------|-------|--------------------------|----------|
| `CMEM_ALIGN_MASK` | `7u` | L619, L889, L1343, L1368, L1613 — spans **3** files | **HOIST to `cmem_internal.h`** (precedent: `CMEM_FREED_MAGIC` @L161, `MP_PERCPU_MAX_BATCH` @L113 already live there). Remove from `cmem_event.c`. |
| `CMEM_DECIMAL_BASE` | `10` | L214 | `cmem_config.c` (in-file) |
| `CMEM_SHM_OPEN_MODE` | `0666` | L745 | `cmem_ring_event.c` (in-file) |
| `CMEM_NSEC_PER_SEC_U64` | `1000000000ULL` | L502 | `cmem_ring_event.c` (in-file) |
| `CMEM_LATENCY_HISTOGRAM_MAX_INDEX` | `31u` | L1213 | `cmem_config.c` (in-file) |
| `CMEM_LATENCY_P99_NUMERATOR` | `99u` | L1239 | `cmem_config.c` (in-file) |
| `CMEM_LATENCY_P99_DENOMINATOR` | `100u` | L1239 | `cmem_config.c` (in-file) |
| `CMEM_FREE_BATCH_CHUNK` | `256` | L2960, L2963, L3060, L3088 | `cmem_alloc.c` (in-file) |
| `MAX_TLS_BOUND_POOLS` | `8` | L3976, L3985, L4042, L4123 | `cmem_multi_arena.c` (in-file) |

Rule: a `#define` moves *with* the cluster that uses it; the single exception is `CMEM_ALIGN_MASK`, which is used by three different clusters and therefore becomes a shared const in `cmem_internal.h`.

## 7. `CMEM_DISABLE_DIAGNOSTICS` Guard

`cmem_core_shared` compiles with `-DCMEM_DISABLE_DIAGNOSTICS`, which compiles out `mp_check_leaks()`. The **only** reference to a diagnostics symbol in `cmem_event.c` is `mp_check_leaks(pool)` at L1465, inside `mp_destroy` (cluster E → `cmem_lifecycle.c`). That call is **already** wrapped in:

```c
#if !defined(CMEM_DISABLE_DIAGNOSTICS)
    mp_check_leaks(pool);
#endif
```

Move `mp_destroy` into `cmem_lifecycle.c` **with the `#if` block verbatim** — no new guard is needed. The other five split files have zero references to diagnostics symbols (grep-verified). Do not touch this guard's semantics.

## 8. Acceptance Matrix

| Check | Pass condition |
|-------|----------------|
| `make cmake-clean && make cmake-build` | 0 compile errors; `format-check` (`clang-format --Werror`) clean; `check-mermaid` clean; all 66+ objects build |
| `ctest --test-dir build_cmake --output-on-failure` | 3/3 suites pass (CUnitTests, CAdvancedUnitTests, CppUnitTests) under ASan/UBSan |
| **Symbol delta** | `nm -D build_cmake/libcmem.so \| sort > /tmp/sym_before` **before** the split, `> /tmp/sym_after` **after**; `diff /tmp/sym_before /tmp/sym_after` is **empty** |
| `cmem_event.c` gone | `test ! -f src/cmem_event.c` |
| Six new files exist | `ls src/cmem_guard.c src/cmem_ring_event.c src/cmem_config.c src/cmem_lifecycle.c src/cmem_alloc.c src/cmem_multi_arena.c` |
| `CMEM_ALIGN_MASK` hoisted | `grep -c 'define CMEM_ALIGN_MASK' include/cmem_internal.h` = 1; `grep -c 'define CMEM_ALIGN_MASK' src/*.c` = 0 |
| Diagnostics guard intact | `grep -n 'mp_check_leaks' src/cmem_lifecycle.c` → present, still inside `#if !defined(CMEM_DISABLE_DIAGNOSTICS)` |
| Behavior | 3 CTest suites green ⇒ semantically inert (pure function move, no body rewrite) |

## 9. Risks

- **Missed static call across clusters:** mitigated by the zero-cross-cluster-statics invariant (§4 table). Re-verify with `grep` during implementation; stop if violated.
- **`CMEM_DISABLE_DIAGNOSTICS` link failure in `cmem_core_shared`:** mitigated — the single `mp_check_leaks` call already has a correct guard that travels with `mp_destroy` (§7).
- **Build-system list drift (a new file missing from one of the 3 edit points):** mitigated by the `nm -D` symbol-delta check (§8) — a missing `.c` would drop its public symbols and fail the empty-diff test.
- **clang-tidy `readability-identifier-length` on moved code:** the moved bodies are unchanged; any pre-existing warnings in the original file carry over but are non-fatal (the build's clang-tidy runs as a warning pass, not a hard gate, in the current config).
- **Pre-existing clang-tidy warnings** at `benchmarks/bench_main.c:1208/1210` (short loop vars `s`/`t`) are unrelated to this split and non-fatal.

## 10. Testing Strategy

- **No new tests** (no behavior change; the three existing suites are the regression gate).
- **Symbol-delta check** (`nm -D` before/after, empty diff) is the primary evidence of "pure move" — stronger than test-only verification, because it proves no exported symbol was added, dropped, or renamed.
- Re-run the ASan/UBSan suite to prove the move is memory-safe and semantically inert.

## 11. Decision Log

- **D1:** Six-file split (approach A) over the conservative 3-file (B) and in-file-banner (C) alternatives — chosen because the 7 clusters have clean static-function boundaries (zero cross-cluster static calls), making a full split safe.
- **D2:** `CMEM_ALIGN_MASK` hoisted to `cmem_internal.h` (the only cross-file shared const); all other `#define`s stay in-file with their using cluster.
- **D3:** The `CMEM_DISABLE_DIAGNOSTICS` guard travels verbatim with `mp_destroy` into `cmem_lifecycle.c` — no new guard, no semantic change.
- **D4:** Build-system edits concentrate on `CMEM_CORE_SRCS` (single source of truth for all six lib targets) + the format-check list + the Makefile.make `CORE_SRCS` filter. The stale "3 CMakeLists edits" note in AGENTS.md is superseded by this.
