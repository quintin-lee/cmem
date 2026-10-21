# Compile-Time Memory Safety Configuration Design

**Date**: 2026-10-20
**Status**: Draft (revised after review)
**Owner**: quintin
**Supersedes**: `MEMORY_SAFETY_CONFIG.md`, `MEMORY_SAFETY_CONFIG_ANALYSIS.md` (repository root; both to be deleted)

## 1. Context & Goal

Commit `bcde285` added two root-level analysis documents describing a "flexible
memory safety configuration" system. Neither document describes shipped code, and
neither is implementable as written:

- They propose `-DMP_FLAG_DEBUG_CANARY` / `-DMP_FLAG_POISON_ON_FREE`. Those names are
  members of the runtime `mp_flags_t` enum (`include/cmem.h:74-107`), not
  preprocessor macros. Redefining them on the command line rewrites the enum
  declaration and breaks compilation.
- They propose `static inline` helpers that `strcmp()` a runtime build-type string
  and a duplicate `mp_create_safe()` API. Both add runtime work and a second
  creation path for a decision that is fixed at build time.
- They duplicate summary sections and contradict themselves on which flags are
  active in which build.

This design replaces both documents with one compile-time knob that the build
systems own, so the default policy cannot drift between translation units and
costs nothing at run time.

**Goal**

- One build-time control: `CMEM_SAFETY_LEVEL` (0-3), exposed as a friendly
  `auto | none | canary | maximal` option in CMake and Make.
- Apply the selected policy only to calls that pass `MP_FLAG_DEFAULT` (0). Any
  non-zero explicit flag set is used verbatim.
- Keep `CMEM_CONF` as a runtime, additive override for pools created after the
  environment is set.
- Make the effective policy observable from tests and diagnostics without adding
  a general flag getter.

**Non-goals**

- No runtime `mp_set_safety_level()` and no per-pool policy object. All pools in a
  binary share the compile-time policy unless the caller passes explicit flags or
  `CMEM_CONF`.
- No change to the canary or poison implementations themselves; this design only
  selects when they are enabled by default.
- No repair of the broken `Makefile.make` compatibility path (see §2.3.3).
- No change to `MP_FLAG_*` enum values or to the public struct layout.

## 2. Design

### 2.1 The knob

`CMEM_SAFETY_LEVEL` is a plain decimal preprocessor value in `0..3`:

| Value | Friendly name | `CMEM_DEFAULT_SAFETY_FLAGS` | Intended profile |
|-------|---------------|-----------------------------|------------------|
| 0 | `auto` | `MP_FLAG_DEBUG_CANARY` when `NDEBUG` is defined, otherwise `MP_FLAG_DEBUG_CANARY \| MP_FLAG_POISON_ON_FREE` | Header default; infer from the profile |
| 1 | `none` | `MP_FLAG_DEFAULT` (0) | Opt out of all default safety flags |
| 2 | `canary` | `MP_FLAG_DEBUG_CANARY` | Release |
| 3 | `maximal` | `MP_FLAG_DEBUG_CANARY \| MP_FLAG_POISON_ON_FREE` | Debug and sanitizer builds |

Add this to `include/cmem.h`, immediately after the `mp_flags_t` enum and before
any function that uses `mp_flags_t`:

```c
/*
 * Compile-time default safety policy for pools created with MP_FLAG_DEFAULT.
 * The value MUST be a plain decimal literal in 0..3; build systems translate
 * the friendly names none/canary/maximal/auto to these numbers.
 *
 *   0 = infer from NDEBUG (canary in NDEBUG builds, canary+poison otherwise)
 *   1 = none
 *   2 = canary
 *   3 = canary + poison-on-free
 */
#ifndef CMEM_SAFETY_LEVEL
#define CMEM_SAFETY_LEVEL 0
#endif

#define CMEM_SAFETY_LEVEL_CAT_(a, b) a##b
#define CMEM_SAFETY_LEVEL_CAT(a, b) CMEM_SAFETY_LEVEL_CAT_(a, b)

#define CMEM_SAFETY_LEVEL_VALID_0 1
#define CMEM_SAFETY_LEVEL_VALID_1 1
#define CMEM_SAFETY_LEVEL_VALID_2 1
#define CMEM_SAFETY_LEVEL_VALID_3 1

/* Rejects 4, -1, empty, "none", "bogus", "2U", "(2)"; accepts 0/1/2/3. */
#if !CMEM_SAFETY_LEVEL_CAT(CMEM_SAFETY_LEVEL_VALID_, CMEM_SAFETY_LEVEL)
#error "CMEM_SAFETY_LEVEL must be a plain decimal integer in 0..3"
#endif

#if CMEM_SAFETY_LEVEL == 1
#define CMEM_DEFAULT_SAFETY_FLAGS ((mp_flags_t)MP_FLAG_DEFAULT)
#elif CMEM_SAFETY_LEVEL == 2
#define CMEM_DEFAULT_SAFETY_FLAGS ((mp_flags_t)MP_FLAG_DEBUG_CANARY)
#elif CMEM_SAFETY_LEVEL == 3
#define CMEM_DEFAULT_SAFETY_FLAGS \
    ((mp_flags_t)(MP_FLAG_DEBUG_CANARY | MP_FLAG_POISON_ON_FREE))
#else /* 0: infer from NDEBUG */
#ifdef NDEBUG
#define CMEM_DEFAULT_SAFETY_FLAGS ((mp_flags_t)MP_FLAG_DEBUG_CANARY)
#else
#define CMEM_DEFAULT_SAFETY_FLAGS \
    ((mp_flags_t)(MP_FLAG_DEBUG_CANARY | MP_FLAG_POISON_ON_FREE))
#endif
#endif

/**
 * @brief Return the compile-time default safety flags for this build.
 *
 * This is the value a pool receives when its creator passes MP_FLAG_DEFAULT
 * (0). Non-zero explicit flags are never modified. The value is a compile-time
 * constant; no environment variable or global state is read.
 */
mp_flags_t mp_default_pool_flags(void);
```

The token-paste check is deliberate. `#if CMEM_SAFETY_LEVEL >= 1 && ...` cannot
reject non-numeric input: an undefined identifier evaluates to 0, so
`-DCMEM_SAFETY_LEVEL=bogus` would silently select `auto`. The paste produces
`CMEM_SAFETY_LEVEL_VALID_bogus`, which is undefined and therefore 0, so the
`#error` fires. This was verified with `gcc -E` for `0,1,2,3` (accepted) and
`4, none, bogus, empty` (rejected).

Definition in `src/cmem_event.c` (near `mp_parse_env_flags`):

```c
mp_flags_t mp_default_pool_flags(void)
{
    return CMEM_DEFAULT_SAFETY_FLAGS;
}
```

The header only accepts numbers. Friendly names are a build-layer vocabulary;
the CMake and Make layers translate them to the numeric define (§2.3).

### 2.2 Resolution semantics

The rule is intentionally narrow and total:

```c
effective = (flags == MP_FLAG_DEFAULT) ? CMEM_DEFAULT_SAFETY_FLAGS : flags;
effective = mp_parse_env_flags(effective);   /* additive only */
pool->flags = effective;                     /* creation-specific flags OR'd after */
```

Only an all-zero request is replaced. A caller that passes `MP_FLAG_FAST_PATH`,
`MP_FLAG_THREAD_LOCAL_CACHE`, `MP_FLAG_THREAD_SAFE`, or any combination gets
exactly those flags, plus anything `CMEM_CONF` adds. This keeps explicit control
for benchmarks, the global pool, and code that deliberately opts into a
non-default layout.

The resolver is a `static inline` in `src/cmem_internal.h` (which already
includes `cmem.h` at line 13), so both `cmem.c` and `cmem_event.c` use one
definition:

```c
static inline mp_flags_t cmem_resolve_default_flags(mp_flags_t flags)
{
    return (flags == MP_FLAG_DEFAULT) ? CMEM_DEFAULT_SAFETY_FLAGS : flags;
}
```

#### 2.2.1 Application points

| Function | Where the policy is applied | Order |
|----------|-----------------------------|-------|
| `mp_create_custom()` (`src/cmem.c:269`) | Wrapper: `flags = mp_parse_env_flags(cmem_resolve_default_flags(flags))` before calling the new pure constructor | defaults, then env |
| `mp_create_child()` (`src/cmem.c:230`) | Public wrapper: same resolution as `mp_create_custom`, then the existing child wiring | defaults, then env |
| `mp_create_from_buffer()` (`src/cmem_event.c:872`) | At entry, before the `MP_FLAG_PERCPU_FREELIST` check and before `pool->flags = flags \| MP_FLAG_STATIC_BUFFER` | defaults, then env, then `STATIC_BUFFER` |
| `mp_create_shared()` (`src/cmem_event.c:731`) | Resolve defaults before `flags \| MP_FLAG_SHARED_MEMORY`; the inner `mp_create_from_buffer` call then sees non-zero flags (its own resolve is a no-op) and applies `CMEM_CONF` | defaults, then `SHARED_MEMORY`, then env, then `STATIC_BUFFER` |
| `mp_enable_multi_arena()` children (`src/cmem_event.c:3937`) | **No policy.** Children are created through an internal verbatim path | inherited flags only |

`mp_create_from_buffer()` currently does **not** call `mp_parse_env_flags()`.
After this change it does. That is a deliberate consistency fix: a pool created
from a static buffer is still a pool, and `CMEM_CONF` is documented as a global
additive override. It is listed in the behavior inventory (§2.4) and the
CHANGELOG.

`CMEM_CONF` keys that only affect OS mapping (`guard`, `hugepages`) are inert
for `STATIC_BUFFER` because `cmem_sys.c` bypasses those paths;
`CACHE_ALIGNED` and `TRACK_LOCATIONS` still apply. Document which keys are
meaningful for static/shared pools rather than claiming every key is active.

`mp_create_shared()` currently tests `if (flags & MP_FLAG_THREAD_SAFE)` after the
`mp_create_from_buffer()` call. The resolved value must be assigned back to
`flags` before that test, otherwise a future default that includes
`THREAD_SAFE` would not configure process-shared locks.

#### 2.2.2 Constructor refactor for multi-arena

The naive rule breaks one existing path. `mp_create_custom()` auto-enables
multi-arena when the flag is present (`src/cmem.c:306-307`), and
`mp_enable_multi_arena()` creates each child with:

```c
mp_create_child(pool, 0, pool->flags & ~MP_FLAG_MULTI_ARENA, name);
```

If the parent was created with only `MP_FLAG_MULTI_ARENA`, the masked child flags
are 0. Passing 0 through the public `mp_create_child()` would resolve to the
safety defaults, so the parent would have no safety flags while every child had
them. The spec's earlier claim that this mask is always non-zero was wrong.

Refactor `src/cmem.c` as follows; public signatures do not change:

```c
/* Core constructor: no default resolution, no environment parsing. */
static memory_pool_t *mp_create_custom_impl(size_t initial_capacity,
                                            mp_flags_t flags,
                                            const mp_sys_allocator_t *sys_allocator);

memory_pool_t *mp_create_custom(size_t initial_capacity,
                                mp_flags_t flags,
                                const mp_sys_allocator_t *sys_allocator)
{
    flags = mp_parse_env_flags(cmem_resolve_default_flags(flags));
    return mp_create_custom_impl(initial_capacity, flags, sys_allocator);
}

/* Shared child wiring; apply_policy selects whether defaults/env are applied. */
static memory_pool_t *mp_create_child_impl(memory_pool_t *parent,
                                           size_t initial_capacity,
                                           mp_flags_t flags,
                                           const char *arena_name,
                                           bool apply_policy);

memory_pool_t *mp_create_child(memory_pool_t *parent,
                               size_t initial_capacity,
                               mp_flags_t flags,
                               const char *arena_name)
{
    return mp_create_child_impl(parent, initial_capacity, flags, arena_name, true);
}

/* Internal only; declared in src/cmem_internal.h, not in include/cmem.h. */
memory_pool_t *mp_create_child_verbatim(memory_pool_t *parent,
                                        size_t initial_capacity,
                                        mp_flags_t flags,
                                        const char *arena_name)
{
    return mp_create_child_impl(parent, initial_capacity, flags, arena_name, false);
}
```

`mp_create_custom_impl()` keeps the existing
`if (flags & MP_FLAG_MULTI_ARENA) mp_enable_multi_arena(pool, 0);` block, so the
public wrapper and the verbatim child path share one constructor. In
`mp_create_child_impl`, `apply_policy == true` calls the public
`mp_create_custom()` (defaults + environment); `apply_policy == false` calls
`mp_create_custom_impl()` directly, so the masked flags stay verbatim. The parent
sys-allocator copy and the `parent->first_child` link are unchanged.

`mp_enable_multi_arena()` changes its one call to
`mp_create_child_verbatim(...)`. The parent's flags already include any
`CMEM_CONF` additions from its own creation, so skipping the second parse is
both correct and closer to "verbatim". The child's `sys_allocator` copy, parent
link, and arena name handling stay in `mp_create_child_impl`, unchanged.

`mp_create()` (`src/cmem_event.c:716`) calls `mp_create_custom()` and therefore
inherits the policy with no extra code. The multi-arena child flag value `0`
remains `0`.

### 2.3 Build system mapping

Both build systems pass an **explicit numeric** level for known profiles instead
of relying on `auto`. Reason: `tests/test_advanced.c:6-8` does `#undef NDEBUG`
before including `cmem.h`. If a Release build used level 0, the library
translation units would see `NDEBUG` and select canary, while the test
translation unit would not and select canary+poison. An explicit `2` keeps them
identical.

#### 2.3.1 CMake

Add a cache string before the targets (after the existing options at
`CMakeLists.txt:109-110`):

```cmake
set(CMEM_SAFETY_LEVEL "" CACHE STRING
    "Default safety policy for MP_FLAG_DEFAULT pools: auto|none|canary|maximal|0|1|2|3 (empty = map by CMAKE_BUILD_TYPE)")
```

Resolution order:

1. Empty (the default): map `CMAKE_BUILD_TYPE`.
2. `auto` or `0`: set the numeric define to 0 (header infers from `NDEBUG`).
3. `none`/`1`, `canary`/`2`, `maximal`/`3`: use that number.
4. Anything else: `message(FATAL_ERROR ...)`.

Build-type mapping (case-sensitive; these are the names CI and
`CMakeLists.txt:365` already use):

| `CMAKE_BUILD_TYPE` | Numeric level | Why |
|--------------------|---------------|-----|
| `Debug` | 3 | Full checking |
| `Release`, `RelWithDebInfo`, `MinSizeRel` | 2 | Canary is cheap enough for production |
| `ASan`, `TSan`, `UBSan`, `Coverage` | 3 | Sanitizer/coverage runs should be maximal |
| other (unknown) | 0 | Infer from `NDEBUG` |

The mapping is inserted after `CMakeLists.txt:105-107`, which assigns
`CMAKE_BUILD_TYPE=Release` when it is empty. An empty value therefore never
reaches the `other` row. This design assumes a single-config generator; a
multi-config generator sees the forced `Release` value and gets level 2 for
every configuration. Per-configuration levels would need generator expressions
and are out of scope.

Translate the cache value once, before any target:

```cmake
if(CMEM_SAFETY_LEVEL STREQUAL "")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(CMEM_SAFETY_LEVEL_NUM 3)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Release" OR
           CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR
           CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
        set(CMEM_SAFETY_LEVEL_NUM 2)
    elseif(CMAKE_BUILD_TYPE STREQUAL "ASan" OR
           CMAKE_BUILD_TYPE STREQUAL "TSan" OR
           CMAKE_BUILD_TYPE STREQUAL "UBSan" OR
           CMAKE_BUILD_TYPE STREQUAL "Coverage")
        set(CMEM_SAFETY_LEVEL_NUM 3)
    else()
        set(CMEM_SAFETY_LEVEL_NUM 0)
    endif()
elseif(CMEM_SAFETY_LEVEL STREQUAL "auto" OR CMEM_SAFETY_LEVEL STREQUAL "0")
    set(CMEM_SAFETY_LEVEL_NUM 0)
elseif(CMEM_SAFETY_LEVEL STREQUAL "none" OR CMEM_SAFETY_LEVEL STREQUAL "1")
    set(CMEM_SAFETY_LEVEL_NUM 1)
elseif(CMEM_SAFETY_LEVEL STREQUAL "canary" OR CMEM_SAFETY_LEVEL STREQUAL "2")
    set(CMEM_SAFETY_LEVEL_NUM 2)
elseif(CMEM_SAFETY_LEVEL STREQUAL "maximal" OR CMEM_SAFETY_LEVEL STREQUAL "3")
    set(CMEM_SAFETY_LEVEL_NUM 3)
else()
    message(FATAL_ERROR "CMEM_SAFETY_LEVEL must be auto|none|canary|maximal|0|1|2|3")
endif()
```

`cmake_minimum_required(VERSION 3.10)` means `add_compile_definitions()` (3.12+)
is unavailable. Apply the in-tree define with the directory-scoped form before
the first target:

```cmake
add_definitions(-DCMEM_SAFETY_LEVEL=${CMEM_SAFETY_LEVEL_NUM})
```

This covers the static/shared libraries, tests, examples, benchmarks, and tools
in this tree. For installed consumers, the definition must travel with the
targets, otherwise a consumer translation unit could evaluate
`CMEM_DEFAULT_SAFETY_FLAGS` at level 0 while linking a library built at level 2:

```cmake
# cmem_core always exists; the others are conditional (CMEM_BUILD_SHARED,
# CMEM_ENABLE_DIAGNOSTICS). Guard each call or configure fails when a target
# is not built.
foreach(_cmem_target cmem_core cmem_core_shared cmem cmem_shared cmem_diag cmem_diag_shared)
    if(TARGET ${_cmem_target})
        target_compile_definitions(${_cmem_target} PUBLIC
            CMEM_SAFETY_LEVEL=${CMEM_SAFETY_LEVEL_NUM})
    endif()
endforeach()
```

Place this block **after** the last `add_library`/`add_executable` target
definition (after `CMakeLists.txt:282`, the closing `endif()` of the diagnostics
conditional, and before the `install(FILES ...)` block at `:284`;
`install(EXPORT)` is at `:330-334` and `configure_file` at `:337-341`).
`if(TARGET ...)` is false for a target that has not been declared yet, so the
block cannot sit next to the cache option, and it must be outside the
`CMEM_BUILD_SHARED`/`CMEM_ENABLE_DIAGNOSTICS` branches so it runs for every
configuration. Add this guard immediately before the `foreach` above (shown
after it only for readability) so a misplaced block fails loudly:

```cmake
if(NOT TARGET cmem_core)
    message(FATAL_ERROR
        "cmem_core is missing; place the safety-definition block after the target definitions")
endif()
```

The targets are exported through `cmemTargets` (`CMakeLists.txt:330-334`), so
`PUBLIC` definitions reach consumers of the installed package. Also add the
define to `cmake/cmem.pc.in` line 10:

```
Cflags: -I${includedir} -DCMEM_SAFETY_LEVEL=@CMEM_SAFETY_LEVEL_NUM@
```

The variable must be set before `configure_file(... @ONLY)` at
`CMakeLists.txt:337-341`.

#### 2.3.2 Makefile

The top-level `Makefile` computes the number and exports it. Place this block
**before** the `ifeq ($(CONFIG),cmake)` include block and after
`CMAKE_BUILD_TYPE ?= Release` (line 26): `Makefile.make` evaluates
`SAFETY_LEVEL_NUM` at include time, so a block placed after the include is
ignored and the Makefile matrix silently falls back to the header default.
Empty means "map by `CONFIG`" (for `CONFIG=cmake` the CMake layer maps by
`CMAKE_BUILD_TYPE` instead); `auto` is an explicit request for 0:

```make
SAFETY_LEVEL ?=
ifeq ($(strip $(SAFETY_LEVEL)),)
  ifeq ($(CONFIG),make)
    SAFETY_LEVEL_NUM = 2
  else ifeq ($(CONFIG),release)
    SAFETY_LEVEL_NUM = 2
  else ifeq ($(CONFIG),debug)
    SAFETY_LEVEL_NUM = 3
  else ifeq ($(CONFIG),asan)
    SAFETY_LEVEL_NUM = 3
  else ifeq ($(CONFIG),tsan)
    SAFETY_LEVEL_NUM = 3
  else ifeq ($(CONFIG),ubsan)
    SAFETY_LEVEL_NUM = 3
  else ifeq ($(CONFIG),cmake)
    SAFETY_LEVEL_NUM =
  else
    SAFETY_LEVEL_NUM = 0
  endif
else ifeq ($(SAFETY_LEVEL),auto)
  SAFETY_LEVEL_NUM = 0
else ifeq ($(SAFETY_LEVEL),none)
  SAFETY_LEVEL_NUM = 1
else ifeq ($(SAFETY_LEVEL),canary)
  SAFETY_LEVEL_NUM = 2
else ifeq ($(SAFETY_LEVEL),maximal)
  SAFETY_LEVEL_NUM = 3
else ifeq ($(SAFETY_LEVEL),0)
  SAFETY_LEVEL_NUM = 0
else ifeq ($(SAFETY_LEVEL),1)
  SAFETY_LEVEL_NUM = 1
else ifeq ($(SAFETY_LEVEL),2)
  SAFETY_LEVEL_NUM = 2
else ifeq ($(SAFETY_LEVEL),3)
  SAFETY_LEVEL_NUM = 3
else
  $(error SAFETY_LEVEL must be auto|none|canary|maximal|0|1|2|3)
endif

export SAFETY_LEVEL
```

In `Makefile.make`, after the per-CONFIG blocks (lines 9-21), append:

```make
ifneq ($(strip $(SAFETY_LEVEL_NUM)),)
CFLAGS   += -DCMEM_SAFETY_LEVEL=$(SAFETY_LEVEL_NUM)
CXXFLAGS += -DCMEM_SAFETY_LEVEL=$(SAFETY_LEVEL_NUM)
endif
```

`CONFIG=release` is in the table because CI uses it. `Makefile.make` has no
`release` branch (it falls back to `-O3` without `NDEBUG`), but the safety level
is still explicit, so the missing branch cannot flip the policy.

`Makefile.cmake` currently only runs `cmake --build` and never configures, so a
forwarded `-D` cannot take effect. Add the configure step:

```make
cmake-build: format-check check-mermaid
	cmake -B $(CMAKE_BUILD_DIR) -G $(CMAKE_GENERATOR) \
	      -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) \
	      -DCMEM_SAFETY_LEVEL=$(SAFETY_LEVEL)
	cmake --build $(CMAKE_BUILD_DIR)
```

#### 2.3.3 Known pre-existing Makefile breakage

`Makefile.make` depends on `format-check` in `lib`, `test`, `test_advanced`,
`test_cpp`, `bench`, `examples`, and `stress_test`; `all` additionally depends on
`check-mermaid`. It defines neither target (they exist only in `Makefile.cmake`). Its `build_program`
macro also calls `$(shell basename $(1) .c)` with a source list, which errors. At
HEAD, `make test` (default `CONFIG=cmake`) has no `test` target, and
`make CONFIG=release test` stops at "No rule to make target 'format-check'".
CI drives this path (`.github/workflows/ci.yml:36-43`).

This design does **not** repair that compatibility layer. It is a separate
pre-existing defect. Consequences for this spec:

- Runtime verification uses CMake, not `make test`.
- Flag plumbing in the Makefile is verified with the object-file target that
  does not depend on `format-check`:
  `make -n CONFIG=release build-release/cmem.o | grep -- -DCMEM_SAFETY_LEVEL=2`.
- CI's Makefile jobs cannot validate this feature until the compatibility path
  is repaired; add/keep a CMake job for the safety-level matrix and file a
  follow-up for `Makefile.make`.

### 2.4 Behavior change inventory

`MP_FLAG_DEFAULT` is 0, so every call that passes it gets
`CMEM_DEFAULT_SAFETY_FLAGS` instead. Explicit non-zero flags are never replaced
by the default policy; they can still receive additive `CMEM_CONF` flags.

| Site | Count / location | Before | After |
|------|------------------|--------|-------|
| `mp_create(0, MP_FLAG_DEFAULT)` in `tests/test_advanced.c` | 27 calls | 0 | `CMEM_DEFAULT_SAFETY_FLAGS` |
| `tests/test_main.c` | 39 `MP_FLAG_DEFAULT` arguments (31 of them zero-capacity `mp_create(0, ...)` calls) | 0 | `CMEM_DEFAULT_SAFETY_FLAGS` |
| C++ `cmem::MemoryPool` default ctor (`include/cmem.hpp:50`) | default argument `MP_FLAG_DEFAULT` | 0 | `CMEM_DEFAULT_SAFETY_FLAGS` |
| C++ child ctor (`include/cmem.hpp:90`) when caller passes `MP_FLAG_DEFAULT` | public API | 0 | `CMEM_DEFAULT_SAFETY_FLAGS` |
| `benchmarks/bench_main.c` | lines 173, 225, 404, 1270 | 0 | `CMEM_DEFAULT_SAFETY_FLAGS` |
| Fixed-arena pools in `src/cmem_event.c` | lines 3778-3779 | 0 | `CMEM_DEFAULT_SAFETY_FLAGS` |
| `mp_create_from_buffer(..., MP_FLAG_DEFAULT)` | public API | `STATIC_BUFFER` | `CMEM_DEFAULT_SAFETY_FLAGS \| STATIC_BUFFER`, plus `CMEM_CONF` |
| `mp_create_shared(..., MP_FLAG_DEFAULT)` | public API | `STATIC_BUFFER \| SHARED_MEMORY` | `CMEM_DEFAULT_SAFETY_FLAGS \| STATIC_BUFFER \| SHARED_MEMORY`, plus `CMEM_CONF` |
| `mp_create_from_buffer(..., explicit non-zero flags)` | public API | explicit \| `STATIC_BUFFER` | same, plus `CMEM_CONF` (new) |
| `mp_create_shared(..., explicit non-zero flags)` | public API | explicit \| `STATIC_BUFFER` \| `SHARED_MEMORY` | same, plus `CMEM_CONF` (new) |
| Public `mp_create_child(..., MP_FLAG_DEFAULT, ...)` | e.g. `tests/test_main.c:306-307`, `:1820` | 0 | `CMEM_DEFAULT_SAFETY_FLAGS` |
| Multi-arena children (`mp_enable_multi_arena`) | `src/cmem_event.c:3937` | parent flags minus `MULTI_ARENA` | unchanged by the default policy, including the masked-to-zero case; a later call no longer re-parses `CMEM_CONF`, so children inherit the parent's flags as of parent creation |
| Global pool (`src/cmem.c:738`) | `MP_FLAG_THREAD_SAFE` | `THREAD_SAFE` | unchanged |
| Pools with explicit flags via `mp_create_custom` (most benchmarks) | many | explicit + `CMEM_CONF` | unchanged |

Costs, per `docs/en/performance.md:116-118`: canary adds one byte per block plus
a one-byte comparison on free, documented at roughly 3% overhead; poison-on-free
is below 1%. Release now pays the canary cost by default. The implementation must
measure Release before/after with `benchmarks/bench_main.c`; if the delta exceeds
3%, record the number in `docs/en/performance.md` (and the zh mirror).

Existing tests assert only lower bounds on `mp_usable_size()` (e.g.
`tests/test_main.c` expects `>= 120`), so the one-byte canary header should not
break them. The full suite still has to run.

### 2.5 Edge cases

| Case | Behavior | Rationale |
|------|----------|-----------|
| Build system defines `CMEM_SAFETY_LEVEL`, user adds another `-D` | The compiler sees two definitions; identical values are silent, different values warn ("redefined"). Under GCC/Clang the last `-D` wins, so the build-system value is not guaranteed to win. | `#ifndef` only supplies a default when nothing is defined; it cannot suppress a second command-line definition. Users who need a different level must use the build option, not a private `-D`. |
| Non-numeric or out-of-range value passed directly to the compiler | `#error` at include time via the token-paste check | Typos must fail loudly. Verified for `4`, `none`, `bogus`, empty. The build layers translate friendly names before emitting the numeric define. |
| `-DCMEM_SAFETY_LEVEL=0` in CMake | Means `auto` (infer from `NDEBUG`), not "map by build type". To get the build-type table, leave the cache variable empty. | Keeps one meaning for 0 across the header and both build layers. |
| `CMEM_DEFAULT_SAFETY_FLAGS` in an installed consumer | Must match the library. Propagated with `PUBLIC` target definitions and `cmem.pc` `Cflags`. | Otherwise a consumer TU can select a different default than the library it links. |
| `CMEM_CONF` set to a weaker value | Impossible; the parser only ORs flags | The environment can strengthen a pool, never weaken the compile-time default. |
| `CMEM_CONF=poison=1` in a Release build | Adds `MP_FLAG_POISON_ON_FREE` | Contradicts the old claim that poison is "maximal builds only"; document it accurately. |
| Windows | Shared-memory test is `#ifndef _WIN32`; `unsetenv` gets a `_putenv_s(name, "")` shim | Windows CI runs `advanced_tests`; the existing file only shims `setenv`. |
| `CMEM_CONF` left set by an earlier test | New tests must unset it before pool creation; `test_reparse_env_flags` must unset it at the end | The existing test sets `CMEM_CONF=DEBUG_CANARY=1` and never clears it. That key is a no-op for the parser (it matches lowercase `canary=1`), but leaving the environment set is fragile and confusing; clear it. |
| Multi-arena parent created with only `MP_FLAG_MULTI_ARENA` | Children are created through `mp_create_child_verbatim`; their flags stay 0 | Avoids a parent/child policy split caused by the `~MULTI_ARENA` mask. |
| Shared pool | Defaults are resolved before `SHARED_MEMORY` is OR'd; the inner `from_buffer` resolve is a no-op | Otherwise the non-zero `SHARED_MEMORY` bit would suppress the defaults. |
| Explicit `MP_FLAG_DEFAULT` from a caller that wants no safety flags | Use `CMEM_SAFETY_LEVEL=none` at build time, or pass a harmless explicit flag (e.g. `MP_FLAG_FAST_PATH` only if its semantics fit) | There is no runtime per-pool opt-out; the build owns the default. |

### 2.6 API and ABI

- New exported function `mp_flags_t mp_default_pool_flags(void)`. It is compiled
  into `cmem_core`/`cmem` and both shared variants. Verify with
  `nm -D build_cmake/libcmem.so | grep mp_default_pool_flags` (or the build-tree
  equivalent).
- New public macro `CMEM_DEFAULT_SAFETY_FLAGS` and public knob
  `CMEM_SAFETY_LEVEL`.
- `mp_create_child_verbatim()` is internal by contract: declared in
  `src/cmem_internal.h`, never in `include/cmem.h`. It cannot be `static`
  because it is called across translation units, so it will appear in the
  shared library's dynamic symbol table unless the project applies hidden
  visibility. Accept and document that one extra internal symbol, or add a
  hidden-visibility attribute.
- No struct layout changes; ABI-compatible. The behavior change is limited to
  pools created with `MP_FLAG_DEFAULT` and to `mp_create_from_buffer()` honoring
  `CMEM_CONF`.

## 3. Testing

### 3.1 New and adjusted tests

Add these to `tests/test_advanced.c` and call them from `main()`
(`tests/test_advanced.c:1519-1582`). This file uses unnumbered test functions;
do **not** add "Test N" numbering (the numbered convention is
`tests/test_main.c`, where Test 38 is `test_compressed_storage` at line 541).
Append the calls after `test_static_buffer_pool()`.

`unsetenv("CMEM_CONF")` must run **before** pool creation in T1-T5. T7 and T8 set
the variable deliberately and clear it afterwards. Add a Windows shim next to the
existing `setenv` shim (`tests/test_advanced.c:17-19`):

```c
#ifdef _WIN32
static int cmem_unsetenv(const char *name) { return _putenv_s(name, ""); }
#define unsetenv cmem_unsetenv
#endif
```

| ID | Test | Steps | Expected |
|----|------|-------|----------|
| T1 | `test_safety_defaults` | `unsetenv("CMEM_CONF")`; `mp_create(0, MP_FLAG_DEFAULT)`; call `mp_reparse_env_flags(pool)`; destroy | `mp_reparse_env_flags(pool) == CMEM_DEFAULT_SAFETY_FLAGS` and `mp_default_pool_flags() == CMEM_DEFAULT_SAFETY_FLAGS` |
| T2 | `test_safety_explicit_flags` | `unsetenv("CMEM_CONF")`; `mp_create(0, MP_FLAG_FAST_PATH)`; `mp_reparse_env_flags(pool)`; destroy | exactly `MP_FLAG_FAST_PATH`; no canary/poison bits |
| T3 | `test_safety_static_buffer` | `unsetenv("CMEM_CONF")`; aligned 64 KiB buffer; `mp_create_from_buffer(buffer, sizeof buffer, MP_FLAG_DEFAULT)`; `mp_reparse_env_flags(pool)`; `mp_destroy(pool)` | `CMEM_DEFAULT_SAFETY_FLAGS \| MP_FLAG_STATIC_BUFFER` |
| T4 | `test_safety_shared_pool` | POSIX only; unique name (pid); `unsetenv("CMEM_CONF")`; `mp_create_shared(name, 65536, MP_FLAG_DEFAULT)`; `mp_reparse_env_flags(pool)`; `mp_destroy_shared(pool, name)` | `CMEM_DEFAULT_SAFETY_FLAGS \| MP_FLAG_STATIC_BUFFER \| MP_FLAG_SHARED_MEMORY` |
| T5 | `test_safety_multi_arena_children` | `unsetenv("CMEM_CONF")`; parent `mp_create(0, MP_FLAG_MULTI_ARENA)`; `mp_get_arena_count(parent) > 0`; child `mp_get_arena(parent, 0)`; `mp_reparse_env_flags(child)`; also a parent with `MP_FLAG_MULTI_ARENA \| MP_FLAG_THREAD_SAFE`; destroy | first child flags == 0; second child flags == `MP_FLAG_THREAD_SAFE`; parent flags include `MULTI_ARENA` |
| T6 | fix `test_reparse_env_flags` | after `mp_get_env_generation()`, call `unsetenv("CMEM_CONF")` before `mp_destroy(pool)` | later tests see a clean environment |
| T7 | `test_safety_env_override` | save and clear `CMEM_CONF`; set `CMEM_CONF=poison=1`; create a DEFAULT pool and a `mp_create_from_buffer(..., MP_FLAG_DEFAULT)` pool; read flags; restore/unset | heap pool flags == `CMEM_DEFAULT_SAFETY_FLAGS \| MP_FLAG_POISON_ON_FREE`; static-buffer pool flags == `CMEM_DEFAULT_SAFETY_FLAGS \| MP_FLAG_POISON_ON_FREE \| MP_FLAG_STATIC_BUFFER`; environment restored. The Release (level 2) row is the meaningful case; in level 3 poison is already in the default. |
| T8 | `test_safety_explicit_env` | save and clear `CMEM_CONF`; set `CMEM_CONF=canary=1`; aligned 64 KiB buffer; `mp_create_from_buffer(buffer, sizeof buffer, MP_FLAG_FAST_PATH)`; read flags; restore/unset | `MP_FLAG_FAST_PATH \| MP_FLAG_DEBUG_CANARY \| MP_FLAG_STATIC_BUFFER`; the explicit flag is preserved and the env flag is OR'd, not replaced |

`mp_reparse_env_flags()` calls `mp_parse_env_flags(pool->flags)`. With
`CMEM_CONF` unset it returns `pool->flags` unchanged, so it is a safe test-only
flag reader and no new getter is needed. It is public (`include/cmem.h:1306`),
not behind the diagnostics option.

T4 notes: include `<unistd.h>` for `getpid()` and build a per-process unique
name so parallel tests do not collide. Assert `pool != NULL` before
`mp_reparse_env_flags()`; `mp_create_shared()` can fail on hosts without
`/dev/shm`. Assert `getenv("CMEM_CONF") == NULL` before the read, so an ambient
environment cannot turn the read into a mutation. `mp_create_shared()` maps `capacity < 64 KiB` to 1 MiB; 65536 stays
64 KiB. `mp_destroy_shared()` uses `pool->total_pool_size` for `munmap` and
unlinks the name.

T5 notes: `mp_create_custom()` auto-enables multi-arena for the flag
(`src/cmem.c:306-307`). `mp_get_arena()` returns `memory_pool_t *`, so
`mp_reparse_env_flags()` can read a child's flags. The zero-flag child is the
regression that the verbatim path fixes.

### 3.2 Build and configuration matrix

Run from a clean build directory. At HEAD the Makefile uses `build-<CONFIG>/`
(or `build_cmake/` for `CONFIG=cmake`), while a default `cmake ..` uses `build/`;
do not mix build systems in one directory (see `AGENTS.md`).

| # | Command | Expected |
|---|---------|----------|
| 1 | `cmake -B build_cmake -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build_cmake && ctest --test-dir build_cmake --output-on-failure` | level 2; T1-T8 pass; `mp_default_pool_flags()` returns canary |
| 2 | `cmake -B build_cmake -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build_cmake && ctest --test-dir build_cmake --output-on-failure` | level 3; T1-T8 pass; canary+poison |
| 3 | `cmake -B build_cmake -G Ninja -DCMAKE_BUILD_TYPE=ASan && cmake --build build_cmake && ctest --test-dir build_cmake --output-on-failure` | level 3 (exact `ASan`, not `ASAN`) |
| 4 | `cmake -B build_cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMEM_SAFETY_LEVEL=none && cmake --build build_cmake && ctest --test-dir build_cmake --output-on-failure` | level 1; T2/T3/T4 see no safety bits |
| 5 | `cmake -B build_cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMEM_SAFETY_LEVEL=bogus` | configure fails with `FATAL_ERROR` |
| 6 | `make -n CONFIG=release build-release/cmem.o \| grep -- -DCMEM_SAFETY_LEVEL=2` | prints the compile line with the define (Makefile compatibility targets are broken; this object target works) |
| 7 | `make -n CONFIG=asan build-asan/cmem.o \| grep -- -DCMEM_SAFETY_LEVEL=3` | same for the sanitizer profile |
| 8 | `printf '#include "cmem.h"\n' \| gcc -E -Iinclude -DCMEM_SAFETY_LEVEL=4 -x c - >/dev/null` | fails with the `#error`; repeat with `=bogus` and `=` (empty) |
| 9 | `printf '#include "cmem.h"\n' \| gcc -E -Iinclude -DCMEM_SAFETY_LEVEL=2 -x c - >/dev/null` | succeeds |

For #1-#4, also inspect the generated compile command to confirm the numeric
define, for example `grep -R "CMEM_SAFETY_LEVEL" build_cmake/compile_commands.json`
(when `CMAKE_EXPORT_COMPILE_COMMANDS` is on) or `ninja -C build_cmake -t
commands | grep CMEM_SAFETY_LEVEL`.

The repo has no ASan+UBSan combined `CONFIG`; the matrix uses the separate
sanitizer profiles. Do not describe a `make test` run as "ASan + UBSan".

### 3.3 What the matrix does not cover

- `Makefile.make`'s `test`/`test_advanced`/`all` targets are broken at HEAD
  (`format-check` undefined; `build_program` basename bug). They are not part of
  acceptance. Runtime acceptance uses CMake. File a follow-up to repair them.
- Multi-process attach to a shared pool is not tested; the code has no attach
  path. The shared test only exercises creation, flag selection, and teardown.

## 4. Documentation and repository cleanup

### 4.1 Update

- `docs/en/development.md`: add §3.4 "Memory Safety Configuration" after §3.3
  Compiler Flags (line 149). Cover the level table, the `MP_FLAG_DEFAULT`-only
  rule, the CMake/Make options, the behavior change, and the opt-out. Mirror in
  `docs/zh/development.md`.
- `docs/en/api-reference.md`: add `mp_default_pool_flags()` near
  `mp_create_custom` (line 113) and document `CMEM_SAFETY_LEVEL` /
  `CMEM_DEFAULT_SAFETY_FLAGS`. Mirror in `docs/zh/api-reference.md`.
- `include/cmem.h:75` and `docs/en/api-reference.md:1142` /
  `docs/zh/api-reference.md:1142`: the `MP_FLAG_DEFAULT` comment currently says
  `// Default configuration` (en) / `// 默认配置` (zh). Change it to
  say that `MP_FLAG_DEFAULT` applies `CMEM_DEFAULT_SAFETY_FLAGS` (canary in
  Release, canary+poison in Debug) and point at `CMEM_SAFETY_LEVEL`.
- `docs/en/performance.md:116` and `docs/zh/performance.md:116`: change
  `MP_FLAG_DEBUG_CANARY` from "Development/debugging" to "Default in Release
  builds (`CMEM_SAFETY_LEVEL=canary`); opt out with `CMEM_SAFETY_LEVEL=none`".
  Keep the ~3% figure; update it if the benchmark says otherwise.
- `docs/en/security.md:366` and `docs/zh/security.md:366`: the checklist item
  "Enable `MP_FLAG_DEBUG_CANARY` in production environment" becomes "Release
  builds enable the canary by default; verify with
  `CMEM_SAFETY_LEVEL=canary` and opt out only with justification."
- `README.md:738`: the production tuning advice currently says to disable canary
  and poison in Release. Change it to: Release enables canary by default; poison
  stays off; opt out with `-DCMEM_SAFETY_LEVEL=none` (CMake) or
  `SAFETY_LEVEL=none` (Make). Add a matching bullet to the performance section
  of `README.zh.md`; that file has no production-tuning bullet today.
- README's `CMEM_CONF` example: pre-parsing the environment
  (`flags = mp_parse_env_flags(MP_FLAG_DEFAULT)`) bypasses the compile-time
  default whenever `CMEM_CONF` is non-empty, because the parsed value is
  non-zero and the constructor treats it as explicit. Call
  `mp_create(0, MP_FLAG_DEFAULT)` and let the constructor parse the environment.
- `README.md` build/test description (around line 642): it says `make test` runs
  the C unit tests with ASan/UBSan. At HEAD `make test` is unavailable under the
  default `CONFIG=cmake`, and `Makefile.make` is broken; replace that claim with
  the CMake `ctest` commands in §3.2. Apply the same correction to
  `README.zh.md`.
- `CHANGELOG.md` under `## [Unreleased]`: Added `CMEM_SAFETY_LEVEL`,
  `CMEM_DEFAULT_SAFETY_FLAGS`, and `mp_default_pool_flags()`; Changed the default
  flags for `MP_FLAG_DEFAULT` pools and made `mp_create_from_buffer()` honor
  `CMEM_CONF`; note the opt-out and the ~3% Release canary cost.

There is no `docs/en/benchmarks.md`; the benchmark/tuning document is
`docs/en/performance.md` (zh mirror `docs/zh/performance.md`). Record the
before/after numbers there if the delta exceeds 3%.

### 4.2 Delete

- `MEMORY_SAFETY_CONFIG.md` and `MEMORY_SAFETY_CONFIG_ANALYSIS.md`: superseded by
  this spec and the new development/API-reference sections. They describe an API
  that does not exist and cannot be implemented as written.

### 4.3 Separate hygiene commit

The repository root also carries unrelated transient artifacts:
`UNIFIED_BUILD_SYSTEM_SUMMARY.md`, `verify_build_system.sh`,
`demo_unified_build.sh`, and `UNI_BUILD_SYSTEM_COMPLETE.md` are tracked (remove
them with `git rm`); `test_report.html` is generated and gitignored, so delete
it from the worktree without committing it. The user approved cleaning these up,
but they are not part of the safety feature. Delete the tracked files in a
separate `chore: 🧹 remove transient build-system artifacts` commit so the
safety change stays reviewable.

## 5. Execution order

1. `include/cmem.h`: add the knob, token validation, `CMEM_DEFAULT_SAFETY_FLAGS`,
   and the `mp_default_pool_flags()` declaration.
2. `src/cmem_event.c`: define `mp_default_pool_flags()`.
3. `src/cmem_internal.h`: add `cmem_resolve_default_flags()` and declare
   `mp_create_child_verbatim()`.
4. `src/cmem.c`: split `mp_create_custom_impl()` / `mp_create_child_impl()`;
   keep the public signatures; add the verbatim wrapper; apply the policy in the
   public wrappers.
5. `src/cmem_event.c`: apply the resolver in `mp_create_from_buffer()` and
   `mp_create_shared()`; switch `mp_enable_multi_arena()` to the verbatim child.
6. CMake: add the cache option, mapping/validation, `add_definitions`, `PUBLIC`
   target definitions, and the `cmem.pc.in` flag.
7. Makefile: add `SAFETY_LEVEL` mapping/export, append to `CFLAGS`/`CXXFLAGS`,
   and add the configure step to `Makefile.cmake`.
8. Tests: add T1-T5, T7, and T8, the `unsetenv` shim, and the T6 cleanup; append
   calls in `tests/test_advanced.c` `main()`.
9. Docs + CHANGELOG + delete the two root safety documents. Do the unrelated
   hygiene cleanup as a separate commit.
10. Run the matrix in §3.2, `make format-check`, and the shared-library symbol
    check. Add `src/cmem_internal.h` to both the CMake `format-check` file list
    and the Makefile's `format-check` inputs (or run
    `clang-format -i src/cmem_internal.h` explicitly); it is absent from both
    today. Record the Release benchmark delta.

Steps 1-5 must land together: after step 4, `mp_enable_multi_arena()` must
already use the verbatim path, or T5 will fail.

## 6. Risks and mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| `Makefile.make` compatibility path is broken, so `make test` cannot validate the feature | Certain at HEAD | High for CI confidence | Use CMake for runtime; verify Make flag plumbing with `make -n ... build-<cfg>/cmem.o`; file a follow-up |
| Installed consumers compile the header at a different level than the library | Medium | High (silent policy drift) | `PUBLIC` target definitions + `cmem.pc` `Cflags`; document that private `-D` overrides warn |
| CMake 3.10 lacks `add_compile_definitions` | Certain | Medium | Use `add_definitions` + `target_compile_definitions` |
| Multi-arena `~MULTI_ARENA` mask produces 0 | Certain for a MULTI_ARENA-only parent | Medium | Verbatim child path; T5 |
| Test `CMEM_CONF` pollution | Certain with the existing test | Medium (flaky assertions) | Unset before creation; T6 |
| Windows lacks `unsetenv` | Certain | Medium (advanced_tests fails) | `_putenv_s` shim; guard T4 |
| Release canary overhead is larger than documented | Low-medium | Medium (performance regression) | Benchmark before/after; record; `CMEM_SAFETY_LEVEL=none` opt-out |
| `mp_create_from_buffer` now honors `CMEM_CONF` | Certain (new behavior) | Low-medium | Document and CHANGELOG; tests unset the env explicitly |
| `test_advanced.c` undefines `NDEBUG` | Certain | High if level 0 is used | Build systems pass explicit 2/3 for known profiles |
| CMake build-type names are case-sensitive | Certain | Low-medium | Use exact `ASan`/`TSan`/`UBSan`/`Coverage`; matrix checks |

## 7. Out of scope

- Repairing `Makefile.make`'s missing `format-check`/`check-mermaid` targets and
  the `build_program` basename bug.
- A runtime per-pool safety setter or getter beyond `mp_default_pool_flags()`.
- Cross-process attach to a shared pool.
- Changing the canary/poison encoding or allocator fast paths.
- Updating unrelated documentation pages or the root unified-build artifacts
  (the latter is a separate hygiene commit).

## Appendix A: Verified source references

| Symbol | Location |
|--------|----------|
| `mp_flags_t` enum | `include/cmem.h:74-107` |
| `MP_FLAG_DEBUG_CANARY` / `MP_FLAG_POISON_ON_FREE` | `include/cmem.h:77`, `:83` |
| `mp_create_custom` declaration / definition | `include/cmem.h:693`, `src/cmem.c:269` |
| `mp_create_child` | `src/cmem.c:230` |
| `mp_parse_env_flags` call in `mp_create_custom` | `src/cmem.c:274` |
| Multi-arena auto-enable | `src/cmem.c:306-307` |
| `mp_create` / `mp_create_shared` | `src/cmem_event.c:716`, `:731` |
| Shared `SHARED_MEMORY` OR | `src/cmem_event.c:761` |
| `mp_parse_env_flags` definition | `src/cmem_event.c:824` |
| `mp_create_from_buffer` / `STATIC_BUFFER` OR | `src/cmem_event.c:872`, `:893` |
| `mp_reparse_env_flags` | `src/cmem_event.c:1055`, `include/cmem.h:1306` |
| Multi-arena child creation | `src/cmem_event.c:3937` |
| `mp_destroy_shared` | `src/cmem_event.c:792` |
| `mp_get_arena` | `include/cmem.h:535` |
| C++ default ctor / child ctor | `include/cmem.hpp:50`, `:90` |
| Test `#undef NDEBUG` / setenv shim | `tests/test_advanced.c:6-8`, `:17-19` |
| `test_reparse_env_flags` / `test_static_buffer_pool` | `tests/test_advanced.c:605-620`, `:641-654` |
| `test_advanced` `main()` | `tests/test_advanced.c:1519-1582` |
| Test 38 (`test_compressed_storage`) | `tests/test_main.c:541` (printf at `:543`) |
| CMake `Coverage` block / sanitizer flag vars / `ASan` test env | `CMakeLists.txt:118`, `:125-133`, `:365` |
| `cmem.pc.in` Cflags | `cmake/cmem.pc.in:10` |
