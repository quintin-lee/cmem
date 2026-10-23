# Unified Thread-Local Child-Pool Binding — Design

- **Date**: 2026-10-23
- **Status**: Approved (user sign-off on A + full design)
- **Scope**: `include/cmem.h`, `src/cmem_multi_arena.c`, `src/cmem_slab.c`, `src/cmem_internal.h`, `tests/test_advanced.c`
- **ABI**: Append-only public struct change; stays inside ABI v1.

## 1. Background

cmem currently keeps **two parallel thread-local states** that both encode
"which child pool does this thread use":

### System A — `tls_cache` (public `thread_cache_t`, include/cmem.h)

- `owner_pool` — the pool the thread is currently allocating from (single
  slot; switched in `tls_cache_validate_owner()`, which flushes the previous
  owner's caches).
- `bound_arena` — set **only** when `owner_pool` is a multi-arena child; the
  value is always `owner_pool` itself. 100% derivable, pure redundancy.
- `tlsf_arena` — this thread's per-thread TLSF arena. Lazily created on first
  child-pool allocation (`cmem_alloc.c:328`); retired and NULLed when the
  thread switches `owner_pool` away (`tls_cache_flush_pool()` →
  `tlsf_arena_retire()`).

### System B — `tls_arena_map` (file-static, src/cmem_multi_arena.c:24)

- `bindings[8]` of `{master_pool, bound_arena, arena_index}` — long-term
  memory of "for master M, this thread uses child C", surviving
  `owner_pool` switches (System A cannot remember this: single slot).
- Writers: `mp_get_thread_bound_arena()` (auto-bind via CPU affinity, then
  round-robin fallback) and `mp_bind_thread_to_arena()` (explicit API).
- Readers: `mp_alloc()`/`mp_alloc_batch()` master→child routing
  (`cmem_alloc.c:499`, `:668`) and the `mp_get_thread_arena()` API.
- Purge: `mp_purge_thread_arena_bindings()` (called from `mp_destroy`),
  which must ALSO clear System A's `bound_arena` — the two systems are kept
  coherent by hand across three code paths.

### Dead code found during audit

`tls_cache_get_bound_arena()` (src/cmem_slab.c:561, declared in
`src/cmem_internal.h:623`) has **zero callers** anywhere (src, tests,
include). It exists only as a leftover seam of the dual-state design.

## 2. Goals

1. **One TLS object.** All thread↔child-pool binding state lives in
   `tls_cache`; the static `tls_arena_map` disappears.
2. **Single purge path.** `mp_purge_thread_arena_bindings()` operates on one
   structure; no cross-variable coherence duty.
3. **Zero behavior change.** Public API semantics, routing, arena lifecycle,
   and lock behavior are byte-for-byte equivalent.
4. **Remove the dead seam** `tls_cache_get_bound_arena()`.

## 3. Non-Goals

- No file merge (`cmem_multi_arena.c` stays, ~170 lines after).
- No change to `tlsf_arena` lifecycle (init/retire/cleanup/registry release).
- No public API signature changes; no MP flag changes.
- No per-binding arenas (would multiply memory 8×256 KB/thread and break
  retire-on-switch semantics).

## 4. Design

### 4.1 Public struct change (include/cmem.h) — append-only

```c
/** Unified per-thread child-pool binding (master → arena child). */
#define CMEM_MAX_TLS_BOUND_POOLS 8
typedef struct {
    memory_pool_t *master_pool;  /**< Key: the multi-arena master pool */
    memory_pool_t *bound_arena;  /**< The child pool this thread uses  */
    int arena_index;             /**< Child index in master->arenas[]  */
} cmem_arena_binding_t;
```

`thread_cache_t` gains, appended at the end (README ABI policy: "New fields
are appended only at the end of public structs"):

```c
    /* Unified thread-local child-pool bindings (absorbs tls_arena_map). */
    cmem_arena_binding_t arena_bindings[CMEM_MAX_TLS_BOUND_POOLS];
    int arena_binding_count;
```

TLS footprint grows ~200 B/thread (8 × 24 B + 4). The existing `bound_arena`
field is KEPT (chosen over deletion to avoid a struct-layout ABI break); its
doc-comment is updated to mark it a derived cache
(`== owner_pool` when owner is a multi-arena child), not authoritative.

### 4.2 src/cmem_multi_arena.c — map absorbed

Delete `tls_arena_binding_t` / `tls_arena_map_t` / static `tls_arena_map`
(L11–24) and `MAX_TLS_BOUND_POOLS` (replaced by the public
`CMEM_MAX_TLS_BOUND_POOLS`). Rewrite the four accessors against
`tls_cache.arena_bindings`:

- `mp_get_thread_bound_arena(pool)` — linear scan by `master_pool`; on miss
  auto-bind exactly as today: CPU affinity first
  (`cmem_current_cpu() % num_arenas`), else relaxed-atomic round-robin
  (`arena_rr_index`); cache the binding only when `count < MAX`. **When the
  table is full, return the arena without caching** (matches current
  behavior).
- `mp_bind_thread_to_arena(pool, idx)` — update-or-insert, line-for-line
  equivalent logic.
- `mp_purge_thread_arena_bindings(pool)` — swap-remove entries matching
  `master_pool == pool || bound_arena == pool`; also clear
  `tls_cache.bound_arena` when it equals `pool`.
- `mp_get_thread_arena` / `mp_get_arena_count` / `mp_get_arena` unchanged.

### 4.3 src/cmem_slab.c — dead seam removed

- Delete `tls_cache_get_bound_arena()` (L536–568) and its declaration
  (`cmem_internal.h:623`) plus the stale comment referencing it (L539).
- `tls_cache_validate_owner()` unchanged (still writes the derived
  `bound_arena` for public observers).
- `tlsf_arena` single-slot + flush→retire semantics untouched.

### 4.4 Invariants preserved (acceptance checklist)

| Invariant | How preserved |
|---|---|
| Explicit bind sticks for subsequent gets | line-equivalent update-or-insert |
| CPU-affinity first, RR fallback, relaxed atomics | transplanted logic |
| 9+ masters: served but not cached | `count < MAX` guard kept |
| `mp_destroy` purges bindings matching master OR child | same match condition |
| Owner switch flushes + retires arena | not touched |
| `mp_alloc`/`mp_alloc_batch` master→child recursion | not touched |
| Zero-init semantics | `tls_cache = {0}` zero-fills bindings (same as the old static map) |
| Windows: plain `thread_local`, no pthread_key for bindings | bindings ride inside `tls_cache`, which already has identical TLS linkage on both platforms |

## 5. Test & Verification Plan

1. `make cmake-clean && make cmake-build && ctest --test-dir build_cmake` —
   baseline must stay green (3/3).
2. `nm -D` symbol delta **must be empty** (the removed function was
   internal; `tls_arena_map` was static).
3. New tests in `tests/test_advanced.c` (next sequential number, registered
   in `main()`):
   - **Two-master cross binding**: create two multi-arena masters; bind and
     allocate alternately on one thread; assert each master resolves to its
     own bound child and bindings survive owner switches.
   - **Purge on destroy**: destroy one master, re-create a pool that may
     reuse the same address; assert stale bindings are gone
     (regression guard for the stale-TLS hazard the purge comment describes).
4. TSan build (`-DCMAKE_BUILD_TYPE=TSan`) + full ctest — TLS state changed,
   race check mandatory.
5. 15 s stress run (`-Iinclude`, 4 threads, multi-arena) — rc 0, no leaks.
6. Format check runs inside `cmake-build` automatically.

## 6. Risks

| Risk | Mitigation |
|---|---|
| Public struct growth surprises external `sizeof(thread_cache_t)` users | Append-only; no existing field moved; within README ABI policy |
| Subtle behavior drift in auto-bind order | Tests 1–2 + line-equivalent transplant; TSan + stress |
| Hidden dependency on `tls_cache_get_bound_arena` | Grep-verified zero callers (src/tests/include) |

## 7. References

- Audit (2026-10-23): two-TLS-state redundancy, dead-code finding.
- README "ABI Stability & Versioning" — append-only struct policy.
- AGENTS.md per-thread arena contract (unchanged by this design).
