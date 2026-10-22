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

- The three duplicated opaque typedefs `cmem_ring_buffer_t` (L190),
  `mp_typed_pool_t` (L195), `cmem_frame_arena_t` (L200) — owned by their
  subsystem headers.
- Subsystem-exclusive function declarations that are *only* meaningful to
  one subsystem and already fully declared there:
  ring/event-log fns, snapshot fns, diag fns, metrics fns, arena fns,
  frame fns, typed-pool fns.
  - **Only** remove a declaration if its *sole* definition is in a subsystem
    header. Core introspection / alloc / free / batch / aligned signatures
    stay even when a subsystem fn also takes a `memory_pool_t*`.

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
| `cmem_arena.h` | `typedef struct memory_pool memory_pool_t;` `typedef unsigned int mp_flags_t;` `typedef void (*mp_watermark_callback_t)(...);` |
| `cmem_frame.h` | `typedef struct cmem_frame_arena cmem_frame_arena_t;` |
| `cmem_typed_pool.h` | `typedef struct mp_typed_pool mp_typed_pool_t;` |
| `cmem_snapshot.h` | `typedef struct memory_pool memory_pool_t;` |
| `cmem_tlsf.h` | `typedef struct tlsf_pool tlsf_pool_t;` |

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
   - Remove subsystem-exclusive fn declarations that have their sole
     definition in a subsystem header (keep core alloc/free/introspection).
   - Verify the tail `#include` block is ordered after all core type
     definitions.
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
| `cmem.h` line count | reduced (3 typedefs + subsystem-exclusive fns removed), core fn set unchanged |
| Behavior | identical (header reordering only; no `.c` change) |

## 7. Risks

- **Forward-decl arity:** `mp_flags_t` is an `unsigned int` typedef; if any
  subsystem fn signature embeds `mp_flags_t` by value (§4.3 arena row), the
  forward decl must be `typedef unsigned int mp_flags_t;` — verify the real
  underlying type at migration time. If it is actually an `enum`, use the
  enum name instead.
- **Stats struct visibility:** `mp_metrics` / `mp_diag` fns take
  `mp_stats_t*` / `mp_allocation_info_t*` — these remain fully-defined in
  `cmem.h` before the tail-include, so no forward decl is needed for them in
  the standalone-subsystem case *only if* those headers are included via
  `cmem.h`. If a user includes `cmem_metrics.h` standalone, they must have
  already included `cmem.h`; document that the eight headers are
  tail-included components, not standalone entry points (entry point =
  `cmem.h`).

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
