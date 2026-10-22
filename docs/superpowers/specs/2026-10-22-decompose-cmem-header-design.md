# Design: Decompose `cmem.h` — Invert Subsystem-Header Include Direction

- **Date:** 2026-10-22
- **Status:** Draft for review
- **Supersedes (in part):** `2026-08-05-decompose-public-headers-design.md`
- **Type:** Pure refactoring — zero behavior change, no `.c` edits

## 1. Context & Problem

`include/cmem.h` is 1827 lines. Eight subsystem headers already exist on disk
(`cmem_arena.h`, `cmem_frame.h`, `cmem_ring.h`, `cmem_snapshot.h`,
`cmem_tlsf.h`, `cmem_typed_pool.h`, `cmem_diag.h`, `cmem_metrics.h`), but the
original 2026-08-05 decomposition spec was only *half* landed:

- Each subsystem header opens with `#include "cmem.h"` (its line 9 or 13).
- `cmem.h` ends by `#include`-ing all eight subsystem headers (L1804–1815),
  with `cmem_diag.h` / `cmem_metrics.h` guarded by `#if !CMEM_DISABLE_DIAGNOSTICS`.

This is a **circular-include pattern**: every subsystem header transitively
pulls in the entire 1827-line `cmem.h`, and `cmem.h` itself still owns the
opaque typedefs it should have given up. The result:

1. `cmem.h` cannot shrink — the subsystem headers depend on it, so it must
   remain a superset.
2. Every consumer that includes *any* single subsystem header gets the whole
   library, defeating the purpose of the split (e.g. embedded `libcmem_core`
   without diagnostics).
3. Three opaque typedefs (`cmem_ring_buffer_t` L190, `mp_typed_pool_t` L195,
   `cmem_frame_arena_t` L200) are **duplicated** — defined in both `cmem.h`
   and their subsystem header.

## 2. Goal

Break the cycle so that:

- Subsystem headers **never** `#include "cmem.h"`. They use minimal forward
  declarations instead.
- `cmem.h` keeps all **core** types and definitions, then `#include`s the
  eight subsystem headers **one-way** at the tail.
- 100% source compatibility: any existing translation unit that includes only
  `cmem.h` sees the exact same API surface as today.

## 3. Non-Goals (YAGNI)

- Do **not** create a new `cmem_slab.h` or `cmem_stats.h`.
- Do **not** move the core-stats structs (`mp_stats_t`, `mp_pool_stats_t`,
  `mp_allocation_info_t`, `mp_block_stats_t`, `mp_latency_stats_t`,
  `mp_histogram_t`) out of `cmem.h` — they are the pool's public stats
  contract and are needed by core signatures.
- Do **not** touch `.c` files, `cmem_internal.h`, or the build-system source
  lists (CMakeLists already registers all eight headers in `format-check`
  and `install(FILES)`).

## 4. Responsibility Boundaries

### 4.1 `cmem.h` RETAINS

- Include guard, stdlib includes, `extern "C"` open/close.
- `CMEM_HISTOGRAM_BUCKETS`, `mp_flags_t`, the `CMEM_SAFETY_LEVEL` block,
  `mp_event_type_t`, `mp_alloc_type_t`.
- Opaque `memory_pool_t` (L185) and `compressed_handle_t` (L345) — used by
  core signatures.
- All core-stats structs (L211–298).
- Core pool API: `mp_create` / `mp_destroy` / `mp_reset` / `mp_expand`,
  `mp_alloc` / `mp_free` / `mp_calloc` / `mp_realloc` / fast / batch /
  aligned / introspection (`mp_usable_size` / `mp_alloc_size` /
  `mp_ptr_valid` / `mp_preferred_size`), helpers (`mp_strdup` / `mp_memdup`
  / `mp_asprintf` + `_loc` variants).
- **The TLSF/slab internal block (L1068–1144):** `mp_slab_slot_t`,
  `CMEM_SLAB_CLASS_COUNT`, `TLSF_CACHE_*` defines, `tlsf_cache_entry_t`,
  `tlsf_arena` forward decl, `thread_cache_t`, `MP_BLOCK_FLAG_TLSF_ARENA`,
  `MP_BLOCK_FLAG_COUNTED`, `MP_MAGIC_HEAD`, `MP_CANARY_BYTE`,
  `mp_block_header_t`, and the two TLS externs.
  - **Rationale (corrects an earlier draft):** this block is *not* TLSF
    subsystem-only. It is directly referenced by the **inline**
    `mp_alloc_fast` / `mp_free_fast` (L1148–1215), which are part of the
    core fast-path API that lives in `cmem.h`. Moving it out would force
    `cmem.h`'s inline functions to depend on a separately-included header.
    Keep it.
- Emergency-reserve / NUMA / compaction / trim / madvise / purge_lazy,
  ABI & cgroup, encrypted-memory, ASan, online-expansion declarations.
- Tail one-way `#include` of the eight subsystem headers.

### 4.2 `cmem.h` REMOVES

**Verified against the actual file (L180–1803 body, doc-comment L1–179 excluded):** the set
of *duplicated* subsystem-fn declarations that exist in BOTH `cmem.h` body and a
subsystem header is **exactly four** — the latency fns. All other subsystem fns
(ring, typed-pool, frame, arena, snapshot, diag, pprof, dump, `mp_get_stats`)
are declared *only* in their subsystem headers and have **no** fn declaration in
`cmem.h` body:

| Duplicated declaration | `cmem.h` body line | Sole definition in |
|------------------------|--------------------|--------------------|
| `mp_record_latency` | L1449 | `cmem_metrics.h` |
| `mp_get_latency_p99` | L1457 | `cmem_metrics.h` |
| `mp_get_latency_avg` | L1465 | `cmem_metrics.h` |
| `mp_reset_latency_stats` | L1472 | `cmem_metrics.h` |

Plus the three duplicated opaque typedefs `cmem_ring_buffer_t` (L190),
`mp_typed_pool_t` (L195), `cmem_frame_arena_t` (L200).

> **Non-duplicates confirmed by grep (do NOT touch):**
> - `mp_get_stats` — has **no** fn declaration in `cmem.h` body (the only text
>   match at L246 is a doc-comment *mention* inside `mp_stats_t`). Declared
>   solely in `cmem_metrics.h`.
> - `mp_get_allocation_info` — declared in `cmem.h` at **L1362** (not L266;
>   L266 is the `mp_allocation_info_t` struct doc comment). It is core
>   introspection and is *also* declared in `cmem_diag.h`. **Decision:** keep
>   the L1362 declaration in place (core fn); it is *not* a removable
>   duplicate — its duplicate home is `cmem_diag.h`, and leaving both is
>   harmless (identical signature) and preserves core introspection in `cmem.h`.
> - `mp_enumerate_regions` (L1375), per-CPU freelist fns (L1487–1503), graceful
>   degradation / error-recovery / thread-quota / encrypted-memory / ASan /
>   online-expansion fns — all core (not in any subsystem header). Keep.

> **Concretely, remove ONLY the four latency fns (L1449–1472) and the three
> opaque typedefs (L190/195/200) from `cmem.h`.** Nothing else.

### 4.3 Subsystem Headers GAIN

Each subsystem header (a) drops `#include "cmem.h"`, (b) adds the minimal
forward declarations it needs, (c) keeps its opaque typedef and its own fn
declarations. The core types they reference (`memory_pool_t`,
`mp_flags_t`, `mp_stats_t`, `mp_allocation_info_t`, etc.) are visible at
tail-include time because `cmem.h` defines them *before* the `#include`
tail.

| Header | Forward declarations to add |
|--------|------------------------------|
| `cmem_ring.h` | `typedef struct cmem_ring_buffer cmem_ring_buffer_t;` `typedef struct mp_event_log mp_event_log_t;` `typedef struct memory_pool memory_pool_t;` |
| `cmem_diag.h` | `typedef struct memory_pool memory_pool_t;` |
| `cmem_metrics.h` | `typedef struct memory_pool memory_pool_t;` |
| `cmem_arena.h` | `typedef struct memory_pool memory_pool_t;` **only.** Do **NOT** forward-decl `mp_flags_t` or `mp_watermark_callback_t` — `mp_flags_t` is an **enum** (`typedef enum { … } mp_flags_t;`, cmem.h L74–107) and `mp_watermark_callback_t` is a function-pointer typedef; C has no valid standalone forward-decl for either. These are visible only when tail-included by `cmem.h` (the documented entry point). |
| `cmem_frame.h` | `typedef struct cmem_frame_arena cmem_frame_arena_t;` |
| `cmem_typed_pool.h` | `typedef struct mp_typed_pool mp_typed_pool_t;` |
| `cmem_snapshot.h` | `typedef struct memory_pool memory_pool_t;` |
| `cmem_tlsf.h` | `typedef struct tlsf_pool tlsf_pool_t;` |

> **Consequence (critical):** `cmem_arena.h` and `cmem_metrics.h` are **not
> standalone-includable** after this refactor — their fn signatures use
> `mp_flags_t` (enum, by value) / `mp_stats_t` / `mp_allocation_info_t`, which
> have no safe standalone forward decl. This is acceptable and **documented**:
> the single public entry point is `cmem.h`; the eight subsystem headers are
> tail-included components. A TU that wants only e.g. `mp_ring_create` still
> includes `cmem.h` (unchanged from today), but it no longer transitively
> re-pulls `cmem.h` from `cmem_ring.h` — the cycle is broken. Standalone
> inclusion of `cmem_arena.h`/`cmem_metrics.h`/`cmem_diag.h` is **not
> supported** and must not be attempted.

> **Compilation-order note:** forward declarations are only valid when the
> subsystem header is used standalone. Because `cmem.h` always defines the
> real structs/enums *before* its tail-`#include` block, the tail-included
> subsystem headers see complete definitions. The forward decls merely let
> each subsystem header compile *on its own* without pulling the whole
> world — the stated goal.

## 5. Migration Steps

1. **Subsystem headers (8):** delete the `#include "cmem.h"` line; insert the
   forward declarations from §4.3. No fn-declaration changes.
2. **`cmem.h`:**
    - Remove the three duplicated opaque typedefs (L190/195/200).
    - Remove the **four** duplicate latency-fn declarations identified in §4.2:
      `mp_record_latency` (L1449), `mp_get_latency_p99` (L1457),
      `mp_get_latency_avg` (L1465), `mp_reset_latency_stats` (L1472).
      **Do NOT remove** `mp_get_allocation_info` (L1362) — it is core.
      **Do NOT remove** `mp_get_stats` — it has no fn decl in `cmem.h` body.
      All other subsystem fns (ring/typed/frame/arena/snapshot/diag/pprof/
      dump) have **no** declaration in the `cmem.h` body and need no removal.
   - Verify the tail `#include` block (L1804–1815) is ordered after all core
     type definitions (it is, today: types are all defined by L~1144).
3. **Verify include direction:** `grep -l '#include "cmem.h"' include/cmem_*.h`
   must return *only* `cmem.h` itself (i.e. zero subsystem headers).
4. **No `.c` / `cmem_internal.h` / CMakeLists edits** required.

## 6. Acceptance Matrix

| Check | Pass condition |
|-------|----------------|
| `make format-check` | 0 warnings/errors (`--Werror`) |
| `make clean && make test` | unit + advanced + cpp suites green under ASan/UBSan |
| `cmake -B build_cmake -G Ninja && ctest --test-dir build_cmake` | green (run after `make clean` to avoid stale ASan binary) |
| `make lib && make lib_shared` | static + shared built; `nm -D libcmem.so \| grep -c mp_ring_create` ≥ 1 |
| Subsystem back-reference | `grep -l '#include "cmem.h"' include/cmem_{arena,frame,ring,snapshot,tlsf,typed_pool,diag,metrics}.h` → empty |
| `cmem.h` line count | reduced by removing 3 typedefs + 4 latency fn decls (L1449–1472 block); core fn set otherwise unchanged |
| Behavior | identical (header reordering only; no `.c` change) |

## 7. Risks

- **Non-standalone subsystem headers (accepted, not a bug):** `cmem_arena.h`,
  `cmem_metrics.h`, `cmem_diag.h` cannot be included standalone because they
  reference `mp_flags_t` (an enum), `mp_stats_t`, `mp_allocation_info_t`.
  These are fully defined in `cmem.h` before the tail-`#include` block, so
  the tail-included form always compiles. Document clearly: **`cmem.h` is the
  only supported entry point**; do not add these three to any downstream
  "include only the one subsystem you need" migration without first moving
  the referenced core types. This is a deliberate scope boundary, not an
  oversight.
- **`mp_get_allocation_info` is core, not diag-exclusive:** keep its
  declaration in `cmem.h` at L1362 (do not remove). Only the 4 latency fns
  (L1449–1472) are the removable duplicate set (§4.2 table). `mp_get_stats`
  has no fn decl in `cmem.h` body and is untouched.
- **Stats struct visibility:** `mp_metrics`/`mp_diag` fns take
  `mp_stats_t*`/`mp_allocation_info_t*` — these remain fully-defined in
  `cmem.h` before the tail-include, so no forward decl is needed in the
  tail-included case.

## 8. Testing Strategy

- No new tests required (no behavior change).
- Re-run the existing three CTest suites under ASan/UBSan to prove the
  include reordering is semantically inert.
- Run `nm -D` on `libcmem.so` to prove no exported symbol was dropped from
  the shared library.

## 9. Decision Log

- **D1:** TLSF/slab block stays in `cmem.h` (corrects §4 of the draft that
  proposed moving it to `cmem_tlsf.h`). It underpins inline fast-path.
- **D2:** Core pool ops remain in `cmem.h`; only subsystem-exclusive
  declarations move.
- **D3:** Include direction is inverted one-way; 100% backward compat via
  tail-include is preserved.
