# FAST_PATH × Boundary-Flag Combo Audit Report

Spec: `docs/superpowers/specs/2026-10-22-fastpath-combo-audit-design.md` (commit `a151752`).
Fix commit: `c8f36a1` + `6233b43` (`fix(core): 🐛 balance stats and audit fields across fastpath/plain api pairing`).
Date: 2026-10-22.

## Method
Per-cell code walk with grep/read evidence; probes compiled against `src/*.c` in `/tmp/opencode/`
(never committed) run in the Release build (`CMEM_SAFETY_LEVEL=2`, default pool = canary +
`MP_FLAG_FAST_PATH`) and in the Debug build (`CMEM_SAFETY_LEVEL=3`, no FAST_PATH in the default
pool) to isolate flag-dependent behavior. Cell 8 used a standalone `-fsanitize=thread` build.

## Verdicts

| # | Cell | Verdict | Evidence |
|---|------|---------|----------|
| 1 | FAST_PATH × plain `mp_alloc`/`mp_free`/`mp_realloc` | **PASS** | realloc copy path: plain `mp_alloc` (COUNTED set) + `mp_free` (decrement gated on the old header's COUNTED bit) — symmetric net-zero; in-shrink and TLSF in-place paths touch only `requested_size`/canary/event fields. |
| 2 | `mp_alloc_fast` mixed with `mp_free` (and reverse) | **DEFECT → FIXED** | Stale header fields after a free-list link clobber (`mp_slab_slot_t` is the first 8 bytes of the header): `magic=0`, stale `requested_size`/`usable_size`, `mp_get_allocation_info` reported false; plain `mp_alloc` followed by `mp_free_fast` left stats counters drifted (+24 residual blocks). Fix: `MP_BLOCK_FLAG_COUNTED` header bit set by every counted allocation site; `mp_alloc_fast` re-stamps the full auditable header; stats-free cache push in `mp_free_fast` gated on the bit; all free-side decrements gated on it; `mp_free`/`mp_free_batch` magic checks made unconditional. Regression tests: `test_fastpath_mixed_api` + `test_fastpath_audit_fields`. Probes: counters exact (`active=0` after 400 allocs/400 frees), `mp_check_leaks` clean. |
| 3 | FAST_PATH × `MP_FLAG_SHARED_MEMORY` | **PASS** | Default level-2 shared pools never carry `MP_FLAG_PERCPU_FREELIST` (defaults are additive only). TLS caches are `thread_local` → process-local, correct for IPC; slab page free lists and the pool rwlock are shared via pshared attributes. Pre-existing latent hazard documented: explicitly enabling `MP_FLAG_PERCPU_FREELIST` on a shared pool stores a process-private `calloc`ed table pointer in the shared struct (other processes see a dangling pointer). Flag docs and README now warn against the combination. |
| 4 | FAST_PATH × `MP_FLAG_STATIC_BUFFER` | **PASS** | `mp_create_from_buffer` resolves the compile-time default and env parsing BEFORE OR-ing the static bit, so defaults apply as intended; slab pages are created outside the buffer via `mmap(MAP_PRIVATE|MAP_ANONYMOUS)`, so the structural audit helper reads real mappings in every config. |
| 5 | FAST_PATH × multi-arena children | **PASS** | Child pools inherit parent flags verbatim (`mp_create_child_verbatim` masks `MP_FLAG_MULTI_ARENA` but keeps canary + FAST_PATH); `mp_alloc` on the master routes through `mp_get_thread_bound_arena` → `mp_alloc(arena, ...)` so arena allocations count on the child. Probe: master counters exact, `mp_check_leaks`/`mp_purge_lazy` clean, per-arena `mp_get_stats` consistent. |
| 6 | FAST_PATH × diagnostics | **PASS** | `mp_check_leaks` detects pending slab and TLSF allocations on default pools (leak list renders `Tier: SLAB`/`TLSF` with sizes); `mp_enumerate_regions` returns slab + TLSF regions; `mp_get_stats` totals balanced. Probe: 201 outstanding allocations reported, counters exact after frees. |
| 7 | `CMEM_CONF` runtime reparse × FAST_PATH | **PASS (invariant)** | `mp_reparse_env_flags` only ORs recognized `name=1|on` entries onto the current flags; it cannot strip `MP_FLAG_FAST_PATH` (or any construction-time flag). FAST_PATH membership is immutable by design. Documented in the api-reference §3.4 note. |
| 8 | Structural audit walk × concurrent free | **PASS** | `mp_fastpath_slot_live` walks `page->free_list` under `sc->lock`; free-side mutations of `page->free_list` hold the same lock. 8-thread TSan probe (mixed `mp_alloc`/`mp_alloc_fast`/`mp_free`/`mp_free_fast` with concurrent `mp_ptr_valid` calls) ran clean, counters exact. |
| 9 | OS/emergency tier `raw_base` invariants | **PASS** | Every allocation tier stamps `magic = MP_MAGIC_HEAD` (unconditional since the fix), sets `raw_base` (self for slab slots, block base for OS blocks), and `subpool`. `mp_fastpath_slot_live` for non-slab tiers requires `raw_base != NULL`; freed OS/TLSF blocks may still report live until reused — documented limitation (structural liveness is exact only for the slab tier). |

## Remaining trade-offs (documented, not defects)
- Freed OS/TLSF blocks on FAST_PATH pools may report as live in audit APIs until reused
  (no active list to walk); slab-tier liveness is exact.
- `MP_FLAG_PERCPU_FREELIST` + `MP_FLAG_SHARED_MEMORY` is unsafe (process-private table inside a
  shared struct); now warned in `docs/en|zh/api-reference.md` and `docs/en|zh/development.md`.
- `MP_BLOCK_FLAG_COUNTED` adds one branch to every counted free; benchmark shows no measurable
  throughput regression (plain `mp_alloc` ≈ 28 Mops/s, `mp_alloc_fast` lane ≈ 141 Mops/s,
  matching pre-fix baseline within noise).

## Probe cleanup
All probes (`probe_mixdiag`, `probe_purge`, `probe_multiarena`, `probe_diag`, `probe_tsan` +
binaries) were in `/tmp/opencode/` only and have been deleted.

## Acceptance
- CTest: 3/3 in `build_safety_release`, `build_safety_debug`, `build_safety_none`,
  `build_safety_asan`.
- `make format-check CONFIG=cmake`: exit 0. `git diff --check`: clean.
- `clang-format` applied to every touched source file.
