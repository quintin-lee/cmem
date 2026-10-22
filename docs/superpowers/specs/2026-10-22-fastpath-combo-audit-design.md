# FAST_PATH Boundary-Combination Audit — Design

Date: 2026-10-22
Status: Approved by user (approach A: audit + probe verification)

## Context

Since commit `19b31ae perf(core): ✨ add fastpath default safety for release builds`,
Release default pools (level 2, `NDEBUG`) carry `MP_FLAG_FAST_PATH` implicitly via
`CMEM_DEFAULT_SAFETY_FLAGS`. FAST_PATH changes per-path behavior: bookkeeping
(magic pre-19b31ae, track fields, active-list maintenance, stats locking) is
conditionally skipped across ~6 alloc sites, ~8 free branches, realloc, batch,
and audit APIs. The FAST_PATH default-interaction semantics with other pool
types and API mixtures have not been exhaustively audited.

User goal: 代码质量完善 — a targeted audit of semantic boundary combinations,
not a refactor or warning sweep.

## Goal

Every audit cell below ends with a written verdict backed by one of:
code-walk evidence, a probe run, or a fix diff. Confirmed defects get minimal
fixes plus permanent regression tests.

## Audit Matrix

| # | Combination | Risk focus |
|---|-------------|-----------|
| 1 | FAST_PATH flag × plain `mp_alloc`/`mp_free`/`mp_realloc` | stats, magic lifecycle, active-list skip semantics |
| 2 | `mp_alloc_fast` mixed with `mp_free` (and reverse) | known stats drift; document pairing rule or guard |
| 3 | FAST_PATH × `MP_FLAG_SHARED_MEMORY` | percpu/TLS caches over process-shared memory |
| 4 | FAST_PATH × `MP_FLAG_STATIC_BUFFER` | from_buffer path under audit APIs |
| 5 | FAST_PATH × multi-arena children (verbatim inheritance) | child flag masking residue |
| 6 | FAST_PATH × diag (`mp_check_leaks`, `mp_enumerate_regions`) | counters skipped vs. expected |
| 7 | `CMEM_CONF` runtime reparse × FAST_PATH pool | dynamic flag rewrite vs. structural assumptions |
| 8 | structural audit walk × concurrent free | lock order, free_list walk width/ABA |
| 9 | OS/emergency tier under FAST_PATH | `raw_base == header` layout assumption |

## Method (per cell)

1. **Code-walk**: read the concrete branches this combination traverses today
   (grep/read evidence required; no memory-based claims). Record what FAST_PATH
   changes on each path.
2. **Probe**: for cells where the walk leaves doubt, write a minimal C probe
   (never committed) under `/tmp/opencode/`, compile against the repo sources,
   run in both `build_safety_release` (flag present) and
   `build_safety_debug`/`none` (flag absent) to isolate flag-dependent behavior.
3. **Verdict**:
   - Defect → minimal fix + permanent regression test in `tests/test_advanced.c`
     (one `fix(core): 🐛 …` commit per defect; no drive-by refactors).
   - Documented trade-off → clarify in docs (`docs(core): 📝 …`).
   - Unresolvable by inspection/probe → surface to the user; do not guess.

## Known Legacy Debt (in scope, no prejudged outcome)

- Cell 2: `mp_alloc_fast` + `mp_free` (and mirror) are already known to be
  stats-inconsistent. Outcome must be either a documented pairing rule or a
  guard — decide on audit evidence.
- Cells 8/9: the structural audit shipped in 19b31ae is re-verified here for
  concurrency and OS-layout assumptions.

## Test and Acceptance

- Each fix: full CTest matrix 3/3 in `build_safety_release`,
  `build_safety_debug`, `build_safety_asan`, `build_safety_none`.
- Gates before delivery: `make format-check CONFIG=cmake` exit 0,
  `git diff --check` clean, clang-format on every touched file.
- Deliverable: audit report with per-cell verdicts; probes deleted; working
  tree contains only untracked plan files afterward.

## Commit Plan

- `fix(core): 🐛 …` per confirmed defect (minimal; with regression test).
- `docs(core): 📝 …` for documented trade-offs / audit summary notes.
- No commits if no defect: report evidence-only.

## Out of Scope

- Header compression (P0), stats batching (P2), adaptive refills (P4),
  clang-tidy warning cleanup, perf tuning.
