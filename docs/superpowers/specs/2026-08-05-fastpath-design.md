# MP_FLAG_FAST_PATH Design

- **Date**: 2026-08-05
- **Status**: Approved (user: "ok" on design summary)
- **Prior context**: single-thread small-alloc benchmark showed cmem 44-68 Mops/s vs system malloc ~140-160 Mops/s. Remaining gap is architectural: ~14-16 header field stores per alloc + defensive check chain per free. This spec adds an opt-in flag to trade auditing for raw speed.

## 1. Goal

Add `MP_FLAG_FAST_PATH` to `memory_pool_t`: a create-time flag that skips non-essential block-header auditing and the active-list bookkeeping, for users who want raw allocation speed and are willing to give up corruption/leak auditing on that pool.

Target: single-thread interleaved small allocs from ~44 Mops/s toward 60+ Mops/s (conservative estimate; ~60% of header stores eliminated).

## 2. Non-goals

- No change to default-pool behavior. Pools without the flag behave exactly as today.
- No removal of the header structure itself. `mp_block_header_t` layout is unchanged; FAST_PATH only skips *writing* certain fields.
- No headerless slab fast path (tcmalloc-style page-metadata size classes). That is a much larger redesign, deferred.
- No new locking strategy. Locking behavior is orthogonal and unchanged.

## 3. Flag definition

`include/cmem.h`:

```c
MP_FLAG_FAST_PATH = (1 << 18), /**< Skip block-header auditing & active-list bookkeeping for raw speed */
```

Bit 18 is free (bit 17 = `MP_FLAG_MULTI_ARENA`). Flag is a create-time-only setting on `mp_create` / `mp_create_child`.

## 4. Semantics

### 4.1 What alloc still writes (required for correct free/tier routing)

`alloc_type`, `slab_class`, `requested_size`, `usable_size`, `raw_base`, `subpool`, `flags` — all still stamped. `requested_size` is kept so `MP_FLAG_DEBUG_CANARY` and `MP_FLAG_POISON_ON_FREE` keep working unchanged.

### 4.2 What alloc skips (auditing)

- `magic` (so free cannot detect corruption/double-free on this pool)
- `prev` / `next` (no active-list linkage)
- `alloc_file` / `alloc_line` / `alloc_func` / `backtrace_addrs[]` / `backtrace_depth` (no location tracking)

Net effect: ~20 header stores reduced to ~8.

### 4.3 What free skips

- Magic-header validation (`header->magic != MP_MAGIC_HEAD` branch)
- `active_list_remove` (and alloc's `active_list_add`)

### 4.4 What free still does

- `active_allocations--` / `active_bytes -= requested_size` / `total_free_ops++` counter updates (cheap, keeps `mp_get_stats` counts accurate)
- Canary check (if `MP_FLAG_DEBUG_CANARY`)
- Poison fill (if `MP_FLAG_POISON_ON_FREE`)
- Tier dispatch and slab return (unchanged)

### 4.5 Interactions with existing flags

| Flag | Under FAST_PATH |
|---|---|
| `MP_FLAG_DEBUG_CANARY` | Works (needs `requested_size`, which is kept) |
| `MP_FLAG_POISON_ON_FREE` | Works |
| `MP_FLAG_ZERO_ON_ALLOC` | Works |
| `MP_FLAG_TRACK_LOCATIONS` | Silently degraded — location not recorded |
| `MP_FLAG_THREAD_SAFE` | Orthogonal, unchanged |
| `MP_FLAG_MULTI_ARENA` | Orthogonal, unchanged |
| `MP_FLAG_REPORT_LEAKS_ON_DESTROY` | Count-based check only; no address walk |

### 4.6 Diagnostics under FAST_PATH

- `mp_get_stats`: `active_allocations` / `active_bytes` remain accurate.
- `mp_check_leaks`: still compares `stats.active_allocations` against expected — verdict works, but the "N live allocations" address report is empty because there is no active list to walk. Documented.
- `mp_destroy`: teardown unchanged; leak report at destroy is count-only.
- `mp_audit_heap` (src/cmem_diag.c:43): walks `pool->active_head` checking magic + canary. Under FAST_PATH the list is empty and magic is never stamped → the walk finds nothing to check and every block would be reported "corrupted". **Guard: under FAST_PATH return `true` immediately** (audit is unavailable by design, not a failure). Documented.
- `mp_handle_double_free` (src/cmem_event.c:~L84): reads `header->magic` to validate before unlinking. Under FAST_PATH magic is not stamped → **guard: skip the magic validation when FAST_PATH** (double-free detection is unavailable by design).

## 5. Implementation

All in `src/cmem_event.c` (alloc/free/double-free), `src/cmem_diag.c` (audit guard), plus flag declaration + docs + tests.

### 5.1 alloc path (`mp_alloc_internal`)

Three places stamp headers and link the active list (verified against source):

1. **Slab branch** (~L2109): after `active_list_add(pool, header)` — guard the audit stores + `active_list_add` with `if (!(pool->flags & MP_FLAG_FAST_PATH))`.
2. **Emergency-buffer path** (~L1981): same guard around `active_list_add`.
3. **Direct-OS branch** (~L2073-2085, header init at 2073-2085): same guard.

Required stores kept in all three: `alloc_type`, `slab_class`, `requested_size`, `usable_size`, `raw_base`, `subpool`, `flags`.

Audit stores skipped: `magic`, `alloc_file`, `alloc_line`, `alloc_func`, `backtrace_depth` (and the `backtrace_addrs[16]` array is never touched when location tracking is off — it is only written under `MP_FLAG_TRACK_LOCATIONS`). Note: the OS branch does NOT write `prev`/`next` itself; `active_list_add` does.

TLSF branch (~L2060-2067): calls `tlsf_alloc` which manages its own header internally — no `mp_block_header_t` fields stamped here, nothing to skip.

### 5.2 free path (`mp_free`)

- Skip the `header->magic != MP_MAGIC_HEAD` validation branch (~L2243-2261) under FAST_PATH.
- `active_list_remove` + counter updates appear in SIX places in `mp_free` (verified: L2287/2293/2315/2321/2336/2373 — slab TLS-cache push ×2, percpu push ×2, slab-lock path, and the common tail). To avoid six duplicated branches, introduce a helper:

```c
static void mp_free_stats_update(memory_pool_t *pool, mp_block_header_t *header)
{
    if (!(pool->flags & MP_FLAG_FAST_PATH)) {
        active_list_remove(pool, header);
    }
    pool->stats.active_bytes -= header->requested_size;
    pool->stats.active_allocations--;
    pool->stats.total_free_ops++;
}
```

Replace all six `active_list_remove(...)` + counter triplets with `mp_free_stats_update(pool, header)`. This preserves existing lock ordering (callers still hold/acquire the pool lock around it as today).

- The `subpool` redirect branch (~L2264-2274) is required for correct routing — keep.
- Canary/poison branches sit behind their own flags — untouched.
- TLSF and OS free branches use the same helper — whole-pool uniform.

### 5.3 double-free handling (`mp_handle_double_free`, ~L84)

Guard: skip the `header->magic != MP_MAGIC_HEAD` validation under FAST_PATH (magic not stamped).

### 5.4 heap audit (`mp_audit_heap`, src/cmem_diag.c:43)

Guard at function top (after `pool` null-check): `if (pool->flags & MP_FLAG_FAST_PATH) { return true; }` — audit unavailable by design, not a failure.

### 5.5 Where the flag is validated

`mp_create` does not reject FAST_PATH combined with other flags (per 4.5, all combinations are safe). No new validation.

## 6. Testing

New test in `tests/test_main.c` (next free number after Test 38 = **Test 39**):

1. `mp_create(32MB, MP_FLAG_FAST_PATH)`
2. Interleaved alloc+free of 1M × 32-256B, all pointers non-NULL, sizes correct (verify usable via `mp_get_allocated_size` or write/read pattern)
3. `mp_get_stats`: `active_allocations == 0` after all frees
4. `mp_check_leaks(pool) == true` (count-based verdict)
5. Canary combination: pool with `FAST_PATH | DEBUG_CANARY`, one small alloc, write within bounds, free — no crash
6. Poison combination: pool with `FAST_PATH | POISON_ON_FREE`, alloc, fill, free, no crash
7. `mp_destroy` completes cleanly

Register call in `main()` after `test_compressed_storage();`.

## 7. Documentation

- `docs/en/performance.md` + `docs/zh/performance.md`: short subsection under the pool-configuration area describing FAST_PATH: what it skips, what it keeps, and the diagnostics caveat (no address-level leak report).
- `CHANGELOG.md` under [Unreleased] → Added.

## 8. Verification

- `clang-format -i` on touched files.
- `build_cmake`: `ninja -t clean && ninja` → 0 warnings; `ctest` 3/3.
- `./unit_tests ./advanced_tests ./cpp_tests` exit 0.
- Benchmark: `./build_cmake/benchmark` — Benchmark 1 (interleaved small) shows improvement vs current 44.11 Mops/s with the new flag. (Note: benchmark currently does NOT use FAST_PATH; add a FAST_PATH row to bench_small_allocs or run a scratch driver `/tmp/fastpath_bench.c` comparing `MP_FLAG_THREAD_LOCAL_CACHE` vs `MP_FLAG_THREAD_LOCAL_CACHE|MP_FLAG_FAST_PATH`.)
- Scratch driver `/tmp/fastpath_bench.c` (200k interleaved, like small_bench3): report both numbers.

## 9. Out of scope / future

- Headerless slab fast path (bigger win, larger redesign)
- Runtime flag toggle (FAST_PATH is create-time only; a mid-life toggle would leave half-tracked allocations)
