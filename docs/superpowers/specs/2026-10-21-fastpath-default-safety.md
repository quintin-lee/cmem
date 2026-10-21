# Spec: FAST_PATH as the Release default profile, with audit APIs that stay truthful

Date: 2026-10-21. Author: Sisyphus. Status: approved direction, implementation pending.

## Problem

Benchmarks measured the plain thread-safe small-object path at ~30 Mops/s vs 159
Mops/s for `MP_FLAG_FAST_PATH` pools (4.92x, malloc baseline 280 Mops/s). The cost
under FAST_PATH is three store bursts per allocation (track fields) plus a
`pool_lock/unlock` pair around `active_list_add` / `active_list_remove` on every
single alloc/free of a `MP_FLAG_THREAD_SAFE` pool (src/cmem_event.c:2363-2374,
3173-3177).

Making FAST_PATH the Release default (`CMEM_SAFETY_LEVEL=2`, auto-NDEBUG) was
attempted and reverted: `test_calloc_loc` failed because `mp_ptr_valid()` and
`mp_get_allocation_info()` depend on `MP_MAGIC_HEAD` and the active list, both of
which FAST_PATH pools intentionally skip. A default configuration that makes
public audit APIs return `false` for valid allocations is not shippable.

## Design decisions

D1 — FAST_PATH keeps skipping track fields and the active list. That is where
the speed comes from; this spec does not dilute it.

D2 — Magic is always maintained. `MP_MAGIC_HEAD` is a single store per
allocation. All alloc paths (slab, TLSF, OS, emergency) write it regardless of
FAST_PATH. Free-time magic validation stays optional (the canary check restored
in c5113a7 covers overflow detection; free-time magic checks remain a
non-fast-path behavior).

D3 — `mp_ptr_valid()` becomes FAST_PATH-aware: for `ALLOC_TYPE_SLAB`, validate
structurally: align the payload down to its 64 KB slab page, sanity-check the
page header (class_index < SLAB_CLASS_COUNT, pointer inside page), then walk that
page's `free_list` — a slot absent from the page free list is live. Exact
liveness for slab allocations without any global lock. For TLSF/OS types, check
magic plus alloc-type consistency (`raw_base` non-NULL). Non-fast-path pools keep
the existing magic + active-list walk unchanged.

D4 — `mp_get_allocation_info()` follows D3 for validity; fills `requested_size`
/ `usable_size` from the header; source fields (file/line/func) are reported NULL
on FAST_PATH pools — they are not maintained there.

D5 — Active-list-based diagnostics (snapshot/leak reports that iterate
`active_head`) remain non-FAST_PATH features; leak checking itself is
counter-based (`stats.active_allocations`) and works on FAST_PATH pools with the
plain API. The inline `mp_alloc_fast/mp_free_fast` pair remains allocate/free
consistent (stats skipped on both sides) and still requires consistent pairing —
pre-existing documented caveat.

D6 — Release profile: `CMEM_SAFETY_LEVEL` 2 and auto/NDEBUG become
`MP_FLAG_DEBUG_CANARY | MP_FLAG_FAST_PATH`. Debug/auto-non-NDEBUG stays
canary + poison (full audit). Explicit flags always win over defaults.

## Rollout

1. cmem.h: re-apply flag mapping (revert of the revert), with updated comments.
2. cmem_event.c: magic writes unconditional on all alloc paths; new structural
   slab validation helper; wire into `mp_ptr_valid` / `mp_get_allocation_info`.
3. Tests: Release ctest must pass 3/3 (test_calloc_loc becomes the gate); add
   FAST_PATH-specific ptr_valid semantics test (live=true, freed=false after
   mp_free, foreign ptr=false) to test_advanced.c.
4. Full matrix Release/Debug/ASan/none; benchmark before/after; docs (EN/ZH
   §3.4, api-reference for the two audit functions, CHANGELOG).

## Risks

- Structural check must never dereference a page header without the 64 KB
  alignment sanity check (foreign pointers of arbitrary provenance reach this
  API).
- Freed→reused race in the free_list walk is acceptable for a diagnostic API
  (documented, single-threaded-by-contract like ptr_valid today).
