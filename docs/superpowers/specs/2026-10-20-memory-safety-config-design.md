# Compile-Time Memory Safety Configuration Design

**Date**: 2026-10-20
**Status**: Draft (pending review)
**Owner**: quintin
**Supersedes**: `MEMORY_SAFETY_CONFIG.md`, `MEMORY_SAFETY_CONFIG_ANALYSIS.md` (repository root; both to be deleted)

## 1. Context & Goal

Commit `bcde285` added two root-level analysis documents describing a "flexible
memory safety configuration" system. Neither document describes shipped code, and
neither is implementable as written:

- They propose `-DMP_FLAG_DEBUG_CANARY` / `-DMP_FLAG_POISON_ON_FREE`. Those
  identifiers are **runtime enumerators** of `mp_flags_t`
  (`include/cmem.h:74-107`), not preprocessor macros. Defining them on the command
  line rewrites the enum declaration into `1 = (1 << 1)` and breaks compilation.
- They propose `static inline` helpers that `strcmp()` a runtime "build type"
  string, and a duplicate `mp_create_safe()` entry point. Both add runtime cost
  and API surface for a decision that is fixed at build time.
- They duplicate summary sections and contradict each other on defaults.

What actually exists:

| Piece | Location | Behavior |
|-------|----------|----------|
| `mp_flags_t` (runtime bitmask) | `include/cmem.h:74-107` | `MP_FLAG_DEFAULT == 0`; canary = `1 << 1`, poison-on-free = `1 << 6` |
| `mp_parse_env_flags()` | `src/cmem_event.c:825` | Parses `CMEM_CONF` and **ORs** directives into the flags; returns input unchanged when unset |
| `mp_create_custom()` | `src/cmem.c:270` | Calls `mp_parse_env_flags()` first, then stores `pool->flags` |
| `mp_create_from_buffer()` | `src/cmem_event.c:872` | Stores `flags \| MP_FLAG_STATIC_BUFFER`; does **not** consult `CMEM_CONF` |
| `mp_create_shared()` | `src/cmem_event.c:731` | Calls `mp_create_from_buffer(..., flags \| MP_FLAG_SHARED_MEMORY)` |
| `mp_reparse_env_flags()` | `src/cmem_event.c:1055` | Re-applies `CMEM_CONF` to a live pool; returns the pool's effective flags |

**Goal**: one compile-time knob that chooses safe defaults for any pool created
with `MP_FLAG_DEFAULT`, wired through both build systems, verified by tests, and
documented in place of the two incorrect root documents.

**Non-goals**:

- No runtime safety-level API, no `mp_create_safe()`, no string comparison.
- No OR-ing of safety flags into pools that pass explicit non-zero flags.
- No change to `CMEM_CONF` semantics (still additive, still `mp_create_custom` only).
- No new safety mechanisms: canary and poison-on-free already exist and are
  accounted for by the slab and TLSF sizers.
- No changes to multi-arena arena inheritance, which passes the parent's
  explicit flags verbatim.

## 2. Design

### 2.1 The knob

`include/cmem.h`, immediately after the `mp_flags_t` enum:

```c
#ifndef CMEM_SAFETY_LEVEL
#define CMEM_SAFETY_LEVEL 0
#endif

#if CMEM_SAFETY_LEVEL == 0        /* auto: infer from NDEBUG */
#  ifdef NDEBUG
#    define CMEM_DEFAULT_SAFETY_FLAGS ((mp_flags_t)(MP_FLAG_DEBUG_CANARY))
#  else
#    define CMEM_DEFAULT_SAFETY_FLAGS \
        ((mp_flags_t)(MP_FLAG_DEBUG_CANARY | MP_FLAG_POISON_ON_FREE))
#  endif
#elif CMEM_SAFETY_LEVEL == 1      /* none */
#  define CMEM_DEFAULT_SAFETY_FLAGS ((mp_flags_t)MP_FLAG_DEFAULT)
#elif CMEM_SAFETY_LEVEL == 2      /* canary */
#  define CMEM_DEFAULT_SAFETY_FLAGS ((mp_flags_t)(MP_FLAG_DEBUG_CANARY))
#elif CMEM_SAFETY_LEVEL == 3      /* maximal */
#  define CMEM_DEFAULT_SAFETY_FLAGS \
      ((mp_flags_t)(MP_FLAG_DEBUG_CANARY | MP_FLAG_POISON_ON_FREE))
#else
#  error "CMEM_SAFETY_LEVEL must be 0 (auto), 1 (none), 2 (canary) or 3 (maximal)"
#endif

/** Effective default safety flags for this translation unit. */
mp_flags_t mp_default_pool_flags(void);
```

Levels:

| Value | Name | Flags applied to `MP_FLAG_DEFAULT` pools |
|-------|------|------------------------------------------|
| `0` | `auto` | canary when `NDEBUG` is defined, canary + poison otherwise |
| `1` | `none` | nothing (legacy behavior) |
| `2` | `canary` | `MP_FLAG_DEBUG_CANARY` |
| `3` | `maximal` | `MP_FLAG_DEBUG_CANARY \| MP_FLAG_POISON_ON_FREE` |

Rationale for a numeric level rather than a string: it is directly comparable in
the preprocessor (`#if`), needs no `strcmp`, cannot drift between translation
units, and produces a compile-time error on typos instead of silently falling
back to `auto`.

`mp_default_pool_flags()` (new public function, defined in `src/cmem_event.c`)
returns `CMEM_DEFAULT_SAFETY_FLAGS`. It exists so applications and tests can
assert which policy a binary was built with without reaching into internals. The
value is a compile-time constant folded to a single `mov`.

### 2.2 Flag resolution semantics

A single rule, applied exactly once per pool:

```c
static mp_flags_t resolve_pool_flags(mp_flags_t flags)
{
    if (flags == MP_FLAG_DEFAULT) {
        return CMEM_DEFAULT_SAFETY_FLAGS;
    }
    return flags;
}
```

- Only a literal all-zero request (`MP_FLAG_DEFAULT`) is replaced. Any non-zero
  flag set is used verbatim — no OR-ing, no partial merge.
- `CMEM_CONF` is applied **after** resolution, so it can only strengthen a pool,
  never weaken the compiled-in default.
- Creation-specific internal flags (`MP_FLAG_STATIC_BUFFER`,
  `MP_FLAG_SHARED_MEMORY`) are applied **after** resolution.

Application points:

| Site | Change | Why |
|------|--------|-----|
| `mp_create_custom()` (`src/cmem.c:274`) | `flags = resolve_pool_flags(flags);` before the existing `mp_parse_env_flags()` call | Single funnel for `mp_create()` and `mp_create_child()` |
| `mp_create_from_buffer()` (`src/cmem_event.c:872`) | resolve before `flags \| MP_FLAG_STATIC_BUFFER` | Covers direct callers (embedded/static buffers) |
| `mp_create_shared()` (`src/cmem_event.c:761`) | resolve before `flags \| MP_FLAG_SHARED_MEMORY`, then pass the resolved value | The OR makes flags non-zero, so resolving inside `mp_create_from_buffer()` alone would never fire for shared pools |

`mp_create_from_buffer()` sees non-zero flags from `mp_create_shared()` and
therefore does not resolve twice; the two sites are idempotent by construction.

Unaffected paths (verified):

- `mp_get_global_pool()` passes `MP_FLAG_THREAD_SAFE` — explicit.
- `mp_enable_multi_arena()` creates children with `pool->flags & ~MP_FLAG_MULTI_ARENA`
  (`src/cmem_event.c:3937`) — non-zero, inherited verbatim.
- Every caller passing `MP_FLAG_FAST_PATH`, `MP_FLAG_THREAD_LOCAL_CACHE`, etc.

### 2.3 Build system mapping

Both systems translate their native option into a numeric
`-DCMEM_SAFETY_LEVEL=<n>` and apply it to **every** target (static library,
shared library, tests, examples, benchmarks, tools), so header and library agree.

**CMake** (`CMakeLists.txt`):

```cmake
set(CMEM_SAFETY_LEVEL "auto" CACHE STRING
    "Compile-time memory safety default: auto|none|canary|maximal")
```

| Build type | Default level |
|------------|---------------|
| `Release`, `RelWithDebInfo`, `MinSizeRel` | `canary` (2) |
| `Debug` | `maximal` (3) |
| `ASAN`, `TSAN`, `UBSAN`, `Coverage` | `maximal` (3) |
| anything else | `auto` (0) |

Mapping happens before `include_directories()` and the library targets, with
`add_compile_definitions(CMEM_SAFETY_LEVEL=${CMEM_SAFETY_LEVEL_NUM})` so all
targets inherit it. An explicit `-DCMEM_SAFETY_LEVEL=none|canary|maximal` wins;
an unrecognized value is a fatal configuration error (`message(FATAL_ERROR ...)`).

**Makefile** (top-level `Makefile` → `Makefile.cmake` / `Makefile.make`):

```make
SAFETY_LEVEL ?= auto
```

| `CONFIG` | Default level |
|----------|---------------|
| `cmake` | forwarded as `-DCMEM_SAFETY_LEVEL=$(SAFETY_LEVEL)` |
| `make` (`-O3`) | `canary` (2) |
| `debug` | `maximal` (3) |
| `asan`, `tsan`, `ubsan` | `maximal` (3) |

`Makefile.make` appends `-DCMEM_SAFETY_LEVEL=<n>` to both `CFLAGS` and
`CXXFLAGS` after the per-`CONFIG` blocks, so objects, tests and tools all match.

**Why both systems pass a number instead of relying on `auto`**: the test suite
does `#undef NDEBUG` before including `cmem.h` (`tests/test_advanced.c:6-8`), so
an `NDEBUG`-based inference would flip a Release-built test binary to `maximal`
while the library stayed at `canary`. Passing the number explicitly removes that
mismatch. `auto` remains available for consumers building the sources directly
and for `-DCMEM_SAFETY_LEVEL=auto` when someone explicitly asks for inference.

### 2.4 Behavior change inventory

This is a deliberate default change and the only user-visible consequence of the
feature:

| Call site | Before | After |
|-----------|--------|-------|
| 30+ `mp_create(0, MP_FLAG_DEFAULT)` calls in `tests/test_advanced.c` | flags `0` | Release: canary; ASan/UBSan run: canary + poison |
| `tests/test_advanced.c:559`, `:645` | `0` | same as above (plus `STATIC_BUFFER` for the buffer variant) |
| `benchmarks/bench_main.c:173, 225, 404` | `0` | Release: canary — benchmarks now measure the shipped default |
| Fixed-arena pools (`src/cmem_event.c:3778`) | `0` | Release: canary |
| `cmem::MemoryPool` default ctor (`include/cmem.hpp:50`) | `0` | Release: canary; debug/sanitizer: canary + poison |
| `mp_create_shared(..., MP_FLAG_DEFAULT)` | `SHARED_MEMORY` only | defaults + `SHARED_MEMORY` |
| Explicit-flag pools (global pool, multi-arena children, most benchmarks) | unchanged | unchanged |

Cost: canary adds one byte per allocation (already included by the slab/TLSF
sizing arithmetic) and one byte comparison on free. Poison adds a `memset` over
the payload on free, only in `maximal` builds (`MP_FLAG_POISON_ON_FREE` sites in
`src/cmem_event.c:2966, 3145`).

Tests assert only lower bounds on `usable_size` (`tests/test_main.c:116`,
`tests/test_advanced.c:849`), so the extra canary byte does not invalidate them.

### 2.5 Error handling & edge cases

| Case | Behavior |
|------|----------|
| `CMEM_SAFETY_LEVEL` out of range / non-numeric | `#error` at include time with the accepted values |
| `CMEM_SAFETY_LEVEL=none` | `CMEM_DEFAULT_SAFETY_FLAGS == 0`; `MP_FLAG_DEFAULT` pools stay at 0; `CMEM_CONF` can still add flags |
| Both build system and user define the macro | `#ifndef` guard means the command-line definition is used; build systems own the value, so no redefinition warning |
| `CMEM_CONF` set to a weaker value | Impossible: the environment only ORs flags |
| Shared-memory pool spanning processes | The level is compile-time; all processes mapping the pool must be built with the same level. Documented; not detectable at runtime |
| `mp_create_from_buffer()` and `CMEM_CONF` | Unchanged: the environment is still not parsed for static buffers (pre-existing behavior, out of scope) |
| `mp_reparse_env_flags()` on a live pool | Can add flags later; cannot remove compile-time defaults (pre-existing semantics) |

### 2.6 API and ABI impact

- One new exported function, `mp_default_pool_flags()`, compiled into both
  `libcmem.a` and `libcmem.so` because `src/cmem_event.c` is in both source lists.
- No struct layout change; `memory_pool_t` stays opaque.
- Existing binaries keep working; only their default pool flags change when they
  passed `MP_FLAG_DEFAULT`.

## 3. Testing

All tests live in `tests/test_advanced.c` and are registered in `main()` with
unique numbers (Test 38 is taken).

| ID | Test | Assertion |
|----|------|-----------|
| T1 | `test_safety_level_default` | `mp_default_pool_flags()` equals the value implied by `CMEM_SAFETY_LEVEL` (compile-time `#if` in the test) |
| T2 | `test_safety_default_pool` | `mp_create(0, MP_FLAG_DEFAULT)`; `unsetenv("CMEM_CONF")`; `mp_reparse_env_flags(pool) == mp_default_pool_flags()` |
| T3 | `test_safety_explicit_flags` | `mp_create(0, MP_FLAG_FAST_PATH)`; effective flags are exactly `MP_FLAG_FAST_PATH` |
| T4 | `test_safety_from_buffer` | `mp_create_from_buffer(buf, sizeof buf, MP_FLAG_DEFAULT)`; effective flags are `mp_default_pool_flags() \| MP_FLAG_STATIC_BUFFER` |
| T5 | `test_safety_shared_pool` | POSIX only: `mp_create_shared(name, 65536, MP_FLAG_DEFAULT)`; effective flags are `mp_default_pool_flags() \| MP_FLAG_SHARED_MEMORY`; destroy + unlink |

`mp_reparse_env_flags()` is used as the flag reader because it returns
`pool->flags` when `CMEM_CONF` contributes nothing; no new getter is required.

Build matrix:

1. `make test` — ASan + UBSan build (`maximal`): T1 expects canary + poison.
2. CMake `Release` + `ctest` — `canary`: T1 expects canary.
3. `-DCMEM_SAFETY_LEVEL=none` compile + run of the full suite: proves the opt-out
   path links and passes with legacy behavior.
4. `-DCMEM_SAFETY_LEVEL=bogus` must fail configuration (CMake) — manual check.

Performance: run `benchmarks/bench_main.c` before and after on the same host,
record ops/s for the `MP_FLAG_DEFAULT` scenarios (lines 173, 225, 404) in the
commit message and in `docs/en/benchmarks.md` if the delta exceeds 3%.

## 4. Documentation & repository cleanup

- Add "Memory Safety Configuration" to `docs/en/development.md` (new §3.4, after
  compiler flags) and the mirror section in `docs/zh/development.md`: level
  table, `CMEM_SAFETY_LEVEL` / `SAFETY_LEVEL` usage, default-flag-only semantics,
  cost, and the "all processes mapping a shared pool must agree" rule.
- README: short bullet under the build/usage section pointing at the new docs
  section.
- `CHANGELOG.md` under `[Unreleased]`: **Added** — compile-time safety level;
  **Changed** — pools created with `MP_FLAG_DEFAULT` now enable canary in Release
  and canary + poison in debug/sanitizer builds.
- Delete `MEMORY_SAFETY_CONFIG.md` and `MEMORY_SAFETY_CONFIG_ANALYSIS.md`; their
  only correct content (level table, cost notes) is folded into the docs above.
- Delete the stale unified-build artifacts, which no file references:
  `UNIFIED_BUILD_SYSTEM_SUMMARY.md`, `verify_build_system.sh`,
  `demo_unified_build.sh` (and `UNI_BUILD_SYSTEM_COMPLETE.md`,
  `test_report.html` if still present). Their accurate content already lives in
  `docs/en/development.md` §3 (Build System).

Commit style: `feat(core): ✨ ...` for the code change (subject may use `!` if
maintainers treat the default change as breaking), `docs(spec): 📝 ...` for this
document, `docs: 📝 ...` for the documentation/cleanup commit. No push.

## 5. Execution order

1. Header: `CMEM_SAFETY_LEVEL`, `CMEM_DEFAULT_SAFETY_FLAGS`,
   `mp_default_pool_flags()` declaration and doc comment.
2. `src/cmem_event.c`: define `mp_default_pool_flags()`; add resolution to
   `mp_create_from_buffer()` and `mp_create_shared()`.
3. `src/cmem.c`: add resolution to `mp_create_custom()`.
4. Build systems: CMake cache variable + mapping + `add_compile_definitions`;
   `Makefile` / `Makefile.cmake` / `Makefile.make` `SAFETY_LEVEL` plumbing.
5. Tests T1–T5.
6. Full verification runs (matrix above), benchmark before/after.
7. Docs, CHANGELOG, deletion of the stale root files.
8. Final `make format-check` + `clang-format -i` on touched sources; confirm
   `nm -D libcmem.so | grep mp_default_pool_flags`.

## 6. Risks & mitigations

| Risk | Mitigation |
|------|------------|
| Canary changes `usable_size` expectations in downstream code | Only +1 byte, already part of sizer arithmetic; `none` level restores exact legacy behavior |
| Release benchmark numbers regress and look like a performance bug | Measure before/after, publish the delta, note that it is the intended safe default |
| Sanitizer runs get slower from poison `memset` | Poison already existed behind `CMEM_CONF=poison=1`; runtime cost is a memset per free, acceptable in debug |
| Test binaries and library disagree on the level | Both build systems apply one numeric define to all targets; T1 asserts the compiled value |
| `mp_reparse_env_flags()` used as a getter in tests is surprising | `CMEM_CONF` is explicitly unset in those tests; the call is a no-op re-parse |
| Shared-memory peers built at different levels | Documented requirement; canary mismatch surfaces as corruption events, which is the intended loud failure |
| Users who want the old behavior cannot express it per pool | `CMEM_SAFETY_LEVEL=none` restores it globally; explicit flags still work per pool |

## 7. Out of scope

- Parsing `CMEM_CONF` in `mp_create_from_buffer()`.
- Any runtime getter API beyond `mp_default_pool_flags()`.
- Turning on `MP_FLAG_GUARD_PAGES`, `MP_FLAG_TRACK_LOCATIONS` or
  `MP_FLAG_ZERO_ON_ALLOC` by default (higher cost, different failure model).
- Multi-arena child flag inheritance changes.
- Rewriting the deleted root documents' performance claims; only the level table
  and cost notes are carried over.
