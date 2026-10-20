# Per-Thread TLSF Arena Hardening Design

**Date**: 2026-10-19
**Status**: Approved
**Owner**: quintin
**Supersedes**: [2026-08-08-per-thread-tlsf-arena-design.md](2026-08-08-per-thread-tlsf-arena-design.md)

## 1. Context & Goal

Commit `ee8957f` added a per-thread TLSF arena for the lock-free hot path. The
arena is active only for multi-arena child pools (`pool->is_multi_arena_child`);
this scope is deliberately kept — the arena will **not** be generalized to
ordinary pools.

The original plan (`docs/superpowers/plans/2026-08-08-per-thread-tlsf-arena.md`)
still shows 37 unchecked boxes and does not reflect what actually shipped. A
source audit (2026-10-19) confirmed the shipped implementation deviates from the
approved spec in several load-bearing ways, including two critical defects
(memory corruption via `mp_realloc`, NULL-`tpool` crash on master-handle frees).

**Goal**: harden the arena to the shared-TLSF invariants, prove correctness with
targeted + randomized + multithreaded tests, add the missing lifecycle
(retire/release), benchmark, and only then tune performance. Close out the stale
plan and docs.

**Non-goals**: generalizing the arena to non-multi-arena pools; dynamic arena
sizing; compaction; cross-thread free-list stealing; new public API.

## 2. Defect Inventory

Audit evidence for each item: source lines at commit `0aa8692`.

| ID | Sev | Defect | Evidence | Effect | Fix |
|----|-----|--------|----------|--------|-----|
| D1 | **CRITICAL** | Allocated arena blocks keep `BLOCK_STATE_FREE` | `src/cmem_slab.c` `tlsf_arena_alloc` stamps `block->size_and_flags = total_needed \| BLOCK_STATE_FREE`; whole-block path leaves the flag set | `mp_realloc` in-place expand treats a **live** physical neighbor as free, absorbs its span and overwrites its `next_free` header → memory corruption | Clear FREE on alloc (preserve only PREV_FREE) |
| D2 | HIGH | Coalescing is dead | `tlsf_arena_free` adjacency test `curr == block + block_size + sizeof(tlsf_block_t)` (+H under the distance convention → never matches); merge arithmetic `+= curr_size + sizeof(tlsf_block_t)` (overlap if the test alone were fixed); no backward merge | Free blocks never recombine; arena fragments; spec step 4b unimplemented | Adjacency `block + size`; merge `+= curr_size`; bidirectional merge via sorted-scan predecessor |
| D3 | HIGH | No end sentinel | `tlsf_arena_init` writes one block for the whole region; nothing at the last `sizeof(tlsf_block_t)` bytes | Last 32 bytes unusable; drained blocks inserted into the shared TLSF can be coalesced past the region end (OOB) | Sentinel in the final header-sized bytes, matching `cmem_tlsf.c` |
| D4 | HIGH | `tlsf_arena_drain` / `tlsf_arena_destroy` have zero call sites | grep: only definitions | Thread exit leaks the 256 KiB `sys_mem_alloc` region (untracked by any pool); `total_pool_size` inflated; pool switch silently reuses one arena across pools | Wire drain/destroy into retire (pool switch, thread exit, pool destroy) |
| D5 | **CRITICAL** | Arena blocks have `subpool == NULL`; free routing depends on the passed pool | `src/cmem_event.c` free: `is_arena_block` requires `pool->is_multi_arena_child`; else cache push with `entry->tpool = header->subpool == NULL` | (i) `mp_alloc(master)`+`mp_free(master)` for an arena block → cache push with NULL tpool → NULL deref on flush/full-cache; (ii) cross-thread free → same crash | Self-describing blocks: `MP_BLOCK_FLAG_TLSF_ARENA` in `header->flags`, descriptor pointer in `header->subpool`; route by flag in `mp_free` and `mp_realloc` regardless of the passed pool |
| D6 | MED | Free-merge inflates `tlsf_arena_used` | `used += curr_size + sizeof(tlsf_block_t)` after `used -= block_size` | Live-byte accounting wrong (no readers today; needed for lifecycle) | Merging free blocks must not change live bytes; `live_bytes` is the renamed, atomic counter |
| D7 | MED | Realloc remainder insertion can violate address order | `asplit->next_free = anext->next_free; anext->next_free = asplit` — `asplit` is at a higher address than `anext` when growing | Unsorted free list → scan misses blocks; ad-hoc surgery duplicated in 4 places | Single `insert_free`/`unlink_free` helper used by free, realloc and flush |
| D8 | LOW | `TLSF_ARENA_MIN_SIZE` unused; no debug validator; no stats | — | Dead constant; no invariant checking | Keep constant reserved; add `tlsf_arena_validate` |
| D9 | — | Performance unverified | No benchmark exercises the arena | Spec claims (8 threads ~200→600+ Mops/s, p99 2 µs→0.5 µs) unmeasured | Add benchmark scenario, measure, tune only if warranted |
| D10 | — | Test coverage | `test_multi_arena` exists but allocates only 64 B → slab path; TLSF arena never exercised | Every defect above shipped undetected | Section 4 test plan |
| D11 | MED | Unpaired `pool_unlock` in `mp_realloc` arena in-place path | `src/cmem_event.c:3401` unlocks without a matching lock in that branch (the function's `pool_lock` is at ~3406) | Corrupts rwlock state whenever the arena in-place expand succeeds | Removed in the realloc rewrite (plan Task 1.6) |
| D12 | **CRITICAL** | Shared-TLSF (non-arena) data race | Reproducer: segfault/hang at ≥64 KiB with 2+ threads on `MP_FLAG_THREAD_SAFE` pools; TSan: 165 races (`tlsf_free` vs `tlsf_alloc`); crash `tlsf_free` ← `mp_free` (`src/cmem_event.c:3264`); all suites passed pre-fix (coverage gap) | Memory corruption / hangs for any threaded pool using the shared TLSF path | Coarse per-arena mutex (`tpool->lock`) serialization — Section 3.6 |
| D13 | **CRITICAL** | `mp_enable_multi_arena` recursively write-locks the parent rwlock | Holds `pool_lock(pool)` while `mp_create_child` internally takes the same write lock; glibc rwlock state degenerates (`__readers=0xFFFFFFF9` = -7) and any later wrlock (e.g. `mp_check_leaks`, `mp_destroy`) hangs forever | Every multi-arena pool built via `mp_enable_multi_arena` is corrupt; discovered by the Task 1.7 hang | Restructured: create children without the pool lock (children take it themselves), then publish `arenas`/`num_arenas`/flags under one write-lock (fixed in `2625b80`) |
| D14 | **CRITICAL** | TLSF per-thread cache hit ignores the block's true span | Cache buckets are `floor(log2)` classes (`tlsf_block_size_to_fl`); the hit path re-stamped `size_and_flags = total_needed`, so a cached 1024-byte block served for 1600 bytes extended into its live physical neighbor (observed fill-byte dereference `0x3131...` in `tlsf_remove_free_block_nolock`) | Cross-size reuse corrupts the physical chain: payload overlap (smaller span) or silently lost tail (larger span) | Serve only exact-span hits; otherwise mark the block FREE, insert it into the shared list, and fall through to arena/shared allocation, which splits correctly (fixed in `c945f47`) |

## 3. Design

### 3.1 Invariants & Data Structures

Arena descriptor (heap-allocated; lifetime independent of any thread):

```c
typedef struct tlsf_arena {
    uint8_t        *raw;            /* sys_mem_alloc'd region            */
    size_t          size;           /* TLSF_ARENA_DEFAULT_SIZE (256 KiB) */
    tlsf_block_t   *free_list;      /* doubly-linked, address-sorted     */
    tlsf_block_t   *sentinel;       /* raw + size - sizeof(tlsf_block_t) */
    _Atomic size_t  live_bytes;     /* Σ live block spans (H + payload)  */
    _Atomic(tlsf_block_t *) remote_free;  /* MPSC mailbox head          */
    memory_pool_t  *owner_pool;     /* pool this arena serves (registry owner) */
    unsigned long   owner_thread;   /* diagnostic only                   */
    _Atomic bool    retired;
    _Atomic bool    cleanup_claimed;
    _Atomic bool    finished;       /* cleanup done: raw released      */
} tlsf_arena_t;
```

`thread_cache_t` loses `tlsf_arena_raw_mem/_free/_used/_total` and holds a single
`tlsf_arena_t *tlsf_arena`.

Descriptor lifetime is **pool-owned**: each pool keeps a mutex-guarded registry
of its arenas. Creating an arena pushes its descriptor; descriptors are never
freed on the hot path. The cleanup winner (Section 3.3) marks `finished` and
releases the raw region; `mp_destroy` walks the registry, verifies every
descriptor is `finished` (all blocks returned), and frees the descriptor memory
itself. This removes any use-after-free on the descriptor: both potential
cleanup claimants always operate on memory that lives until pool destruction.

Block self-description:

- New public flag `MP_BLOCK_FLAG_TLSF_ARENA (0x0001)` in `header->flags` — a field
  only ever written as 0 today (verified across all stamp sites).
- Arena blocks store the descriptor pointer in `header->subpool`, disambiguated
  by the flag. `mp_free` / `mp_realloc` test the flag **before** the existing
  subpool-based routing, so behavior is independent of the pool handle passed by
  the caller and of the freeing thread.

Physical invariants (adopted verbatim from `cmem_tlsf.c`):

- `size_and_flags` size = distance from this block's start to the **next
  physical block's start** (own header included). No `+H` anywhere.
- End sentinel at `raw + size - sizeof(tlsf_block_t)`: `size_and_flags == 0`
  (never FREE → coalescing terminator), `prev_physical` maintained.
- FREE ⇔ the block is on the arena free list; PREV_FREE mirrors the physical
  predecessor. Both updated on every alloc/split/free/merge.
- Free list doubly-linked (using the currently-unused `prev_free`) and
  address-sorted. **All mutations go through one helper**:
  `tlsf_arena_insert_free(desc, block, size)` (sorted insert + bidirectional
  coalesce + neighbor `prev_physical`/PREV_FREE maintenance) and
  `tlsf_arena_unlink_free(desc, block)` (O(1) doubly-linked unlink).
- No two adjacent free blocks after any operation.
- `live_bytes` counts live spans only; merging free blocks never changes it.

### 3.2 Core Algorithms

**Alloc** (`tlsf_arena_alloc(desc, total_needed)`):
1. Reap the mailbox if non-NULL (owner-only consumer).
2. First-fit scan of the sorted free list.
3. Split only when `remaining >= TLSF_MIN_BLOCK_SIZE + H`: live block takes
   `size = total_needed`, FREE cleared, PREV_FREE preserved; remainder starts at
   `block + total_needed` with `FREE`, `prev_physical = block`; physical
   successor gets `prev_physical = remainder` and PREV_FREE set.
4. Whole-block case: FREE cleared, successor's PREV_FREE cleared.
5. `live_bytes += consumed span` (atomic RMW, relaxed).
6. Header: `flags |= MP_BLOCK_FLAG_TLSF_ARENA`, `subpool = desc`.

**Free**:
- Route by header flag → descriptor (ignore caller's pool).
- **Direct path** iff `thread_cache.tlsf_arena == desc && !retired`: insert via
  helper; `live_bytes -= size`.
- **Mailbox path** otherwise (other thread, or retired): push with
  `atomic_exchange(&desc->remote_free, block)`, leaving the FREE bit **clear** so
  a mailbox block looks live to the physical chain (cannot be coalesced or
  allocated until reaped); then `live_bytes -= size`. If the fetched old value
  equals this block's span, this freer observed the transition to zero → last
  freer (Section 3.3).

**Reap** (owner; cheap `atomic_load` gate at the start of alloc/free/realloc):
`exchange(mailbox, NULL)`, then set FREE and insert each block via the helper.
No locking: the list stays single-writer while not retired.

**Realloc**:
- Route by header flag (fixes the master-handle case for realloc, same as free).
- Reap first. Grow in place only if the next physical block is FREE and the
  merged span suffices: unlink next via helper, grow, split remainder via
  helper, `live_bytes += delta`.
- Else arena alloc → copy → free; if the arena is exhausted → shared-pool
  alloc → copy → free (existing path).
- Shrink keeps the existing behavior (size fields updated in place).

### 3.3 Lifecycle: Retire, Thread Exit, Pool Switch

Single-writer rule: only the thread whose `thread_cache.tlsf_arena == desc` and
which observes `retired == false` mutates the list. Everyone else — including
the former owner freeing a stale pointer — uses the mailbox. No mutex needed.

Retire triggers, all on the owning thread:

- `tls_cache_validate_owner` detects a pool switch (flush old pool first),
- `tls_cache_dtor` at thread exit,
- `tls_cache_flush_pool` called from `mp_destroy`.

Retire algorithm:

- `live_bytes == 0`: reap any mailbox leftovers, coalesce to a single free
  block covering `[raw, sentinel)` (validator-asserted), release the region
  with `sys_mem_free(raw)`, `total_pool_size -= size`, null `raw`, mark
  `finished` (release); the descriptor itself is released later by
  `mp_destroy`. Deliberate deviation: the region is **not** inserted into the
  shared TLSF's buckets — it cannot both join a shared free list and be
  returned to the OS, and the pool does not track foreign regions (§3.7).
- `live_bytes > 0`: `retired = true` (release), detach from the thread cache.
  The free list is frozen (not reusable). Later frees from any thread go to the
  mailbox. The freer that drives `live_bytes` to zero claims cleanup with a
  `cleanup_claimed` CAS, reaps the mailbox (acquire), and performs the same
  release sequence (raw freed, `finished` set); the descriptor memory stays
  alive until `mp_destroy`.

Correctness argument:

- Each freer pushes **before** decrementing `live_bytes`; push uses release,
  decrement uses acq_rel RMW. Observing zero therefore implies all pushes are
  complete and visible to the cleaner's acquire exchange.
- The zero-transition is unique: `fetch_sub` returns the previous value; exactly
  one freer sees `old == own span` (with `live_bytes` accounting correct).
- After zero, no thread can touch the descriptor for block processing: every
  freer dereferences `desc` only before its own decrement. Descriptor memory
  itself is never freed by this path — it belongs to the pool registry until
  `mp_destroy` — so a late CAS attempt by the losing claimant cannot be a
  use-after-free.
- Cleanup arbitration: the retiring thread claims via the same
  `cleanup_claimed` CAS; exactly one of {retiring thread, last freer} wins. The
  loser must not touch the descriptor after failing the CAS.
- Cleanup is safe because with `live_bytes == 0` and correct coalescing the
  arena is exactly one free block starting at `raw` with `prev_physical == NULL`
  (validator-asserted), ending at the sentinel. The whole `[raw, sentinel]`
  region is then unmapped in one `sys_mem_free`, so no allocator can ever walk
  out of it.
- `tlsf_arena_registry_release` force-releases any unfinished arena region at
  pool teardown; it is only reachable after the pool contract (all blocks
  returned) or a leaky shutdown, and never inserts memory into another pool.

Documented consequences:

- A thread exiting with outstanding arena blocks freezes up to 256 KiB until the
  last block is freed. Never-freed blocks leak, exactly like `malloc`.
- A thread alternating pools while holding outstanding blocks can accumulate one
  arena per pool; each is released at its own last free. Fixed size, no dynamic
  sizing.
- `mp_destroy` keeps the existing contract (all blocks returned) and additionally
  releases every descriptor in the pool's arena registry. Retired descriptors
  still reachable through live block headers remain valid until then.
- Slab path, `total_cache_bytes`, and `mp_check_leaks` semantics are untouched;
  arena blocks remain ordinary `active_allocations` until freed.

### 3.4 Integration Sites (every `header->subpool` reader)

`header->subpool` currently has one meaning (tlsf_pool_t* for TLSF blocks, pool
otherwise); arena blocks reuse it for the descriptor. Every reader must test
`MP_BLOCK_FLAG_TLSF_ARENA` **first**:

| Site | Today | Change |
|------|-------|--------|
| `mp_free` dispatch (cmem_event.c:3099) | casts `subpool` to `tlsf_pool_t*` | flag → arena free path; never cast the descriptor |
| TLSF cache push (cmem_event.c:3258) | `entry->tpool = header->subpool` | non-arena blocks only (arena blocks bypass the cache) |
| `mp_realloc` arena gate (cmem_event.c:3328) | `header->subpool == NULL && is_multi_arena_child` | flag-based routing regardless of the passed pool |
| `tlsf_free` (cmem_tlsf.c:567) | casts `subpool` unconditionally | arena blocks must never reach it (assert in debug) |
| `tlsf_try_inplace_expand` (cmem_tlsf.c:782) | `if (!tpool) return false;` | flag guard: return false for arena blocks |
| `mp_free_batch` (cmem_event.c:3028) | already falls back to `mp_free` for arena-shaped blocks | no change (verify) |

`mp_usable_size` and the diagnostics/compression subsystems never dereference
`subpool` as a pool — verified, no change.

### 3.5 Validator

`bool tlsf_arena_validate(const tlsf_arena_t *)` in `src/cmem_slab.c`, declared
in `src/cmem_internal.h`. Returns false instead of asserting so tests can use it
in any build:

- physical chain: `size >= TLSF_MIN_BLOCK_SIZE`, 8-aligned, `next = cur + size`,
  `next->prev_physical == cur`, sentinel exactly at `raw + size - H` with
  `size_and_flags == 0`, no block crossing the sentinel;
- free list: address-sorted, doubly-linked consistency, each FREE block exactly
  once, no adjacent free blocks;
- `live_bytes == Σ live spans`; PREV_FREE flags consistent with the physical
  predecessor.

Called under `#ifndef NDEBUG` at the end of alloc/free/realloc/reap/retire
(Makefile builds do not define NDEBUG; CMake Release does, so zero release
cost), and directly by tests.

### 3.6 Shared-TLSF Serialization (D12)

The hardening work surfaced a **pre-existing, unrelated** defect: the shared TLSF
allocator (any `MP_FLAG_THREAD_SAFE` pool — multi-arena not required) races under
concurrent alloc/free. The fine-grained per-bucket locks (`bucket_locks[30][16]`)
are unsound by construction:

- `tlsf_find_suitable_block` reads the FL/SL bitmaps without holding the matching
  bucket lock;
- split/coalesce paths mutate block headers (flags, physical links) partially
  outside the bucket locks;
- `tlsf_free` marks a block FREE *before* taking locks, and its multi-bucket
  coalescing collects/sorts across buckets while alloc walkers race it.

Evidence on `5831392`: a threaded reproducer segfaults or hangs at sizes ≥ 64 KiB
with 2+ threads; TSan reports 165 races (`tlsf_free:626/752` vs
`tlsf_alloc:401/424`); the crash backtrace is `tlsf_free` ← `mp_free`
(`src/cmem_event.c:3264`). All suites passed pre-fix — there was no threaded
shared-TLSF test (coverage gap; closed by `test_shared_tlsf_threaded_race` in
`tests/test_advanced.c`).

**Fix**: one coarse `pthread_mutex_t lock` per `tlsf_pool_t` (field existed,
previously unused) serializes every structural operation — find/remove/split/
insert on both alloc arms, the full free path (FREE marking, both coalescing
directions, merged insert), and in-place realloc expansion. Public
`tlsf_insert_free_block`/`tlsf_remove_free_block` keep their signatures and take
the lock; static `*_nolock` cores serve already-locked regions. The bucket-lock
helpers are deleted; the `bucket_locks` arrays remain initialized but unused
(cleanup deferred).

Lock order: pool rwlock (`pool_lock`, write, only for `MP_FLAG_THREAD_SAFE`) →
`tpool->lock`; nothing takes the pool rwlock while holding an arena lock, so the
order is acyclic. `tlsf_alloc`'s lazy-init path double-checks `pool->tlsf_root`
under `pool_lock`.

TSan surfaced two residual pre-existing races in `src/cmem_event.c`, also fixed:

- `mp_free` derived `block_size` from `block->size_and_flags`, which a concurrent
  `tlsf_alloc` may OR with `BLOCK_STATE_PREV_FREE`; it now derives the size from
  `header->usable_size + sizeof(tlsf_block_t) + sizeof(mp_block_header_t)`
  (invariant holds at every TLSF stamp site, including both in-place realloc
  paths);
- `mp_alloc_internal`'s `size_histogram[bucket]++` is now a ts-gated relaxed
  atomic add (`mp_alloc_batch`'s writer already runs under `pool_lock`).

Post-fix verification: reproducer matrix (3 sizes × 4 thread counts, 20 k
iterations) all exit 0; TSan reproducer and `make CONFIG=tsan test_all` report
**zero** warnings; `make test_all` and `make CONFIG=asan test_all` green.

### 3.7 Post-implementation findings & deviations

- **D13** (pre-existing, fixed in `2625b80`): `mp_enable_multi_arena` created
  children while holding the parent write lock; `mp_create_child` takes the same
  lock, so the rwlock was recursively write-locked and corrupted. Now children
  are created outside the lock and only publication takes it.
- **D14** (pre-existing, fixed in `c945f47`): the TLSF cache-hit path re-stamped
  the block size to the request size without checking the cached block's actual
  span. Cache buckets are `floor(log2)` classes, so an over-sized request could
  extend a block into a live neighbor. The hit is now exact-span only; a
  mismatched block returns to the shared free list and the normal path splits a
  correctly sized piece.
- **Cleanup releases instead of draining** (§3.3): the retired arena region is
  unmapped with `sys_mem_free` after the final free; it is not inserted into
  the shared TLSF. Joining a shared free list and returning memory to the OS are
  mutually exclusive, and the pool does not track foreign regions.
- **Fallback rate is not instrumented** (§5): the benchmark reports throughput
  and latency only; exhaustion/fallback is covered functionally by the
  exhaustion test and the sub-256 KiB stress sizes, not measured statistically.
- **Helper exposure** (Task 1.6): `tlsf_arena_span`, `tlsf_arena_unlink_free`
  and `tlsf_arena_insert_free` are cross-TU (declared in `src/cmem_internal.h`)
  so `mp_realloc` can perform arena in-place growth with the same list
  discipline; they were planned as static.
- **Registry mutex ownership** (Task 2.2): `tlsf_arena_registry_release`
  detaches and frees descriptors but does **not** destroy
  `arena_registry_lock`; `mp_destroy_shared` remains the single destroyer to
  avoid a double destroy on the shared path.

## 4. Testing & Verification

Added to `tests/test_advanced.c` and registered in its `main()` (no new binary,
no CMake changes). Today's `test_multi_arena` only allocates 64 B → slab path;
these tests target the TLSF path (> 8 KiB sizes to clear the TLSF cache range):

1. `test_tlsf_arena_basic` — alloc/free/validator/leak check.
2. `test_tlsf_arena_realloc` — in-place grow into a free successor, grow
   requiring a move, shrink; payload preserved; validator after each step.
3. `test_tlsf_arena_coalesce` — free orders exercising forward, backward and
   both-direction merges; re-allocate full capacity afterwards.
4. `test_tlsf_arena_master_free` — `mp_alloc(master)` + `mp_free(master)`
   (the D5 repro) and mixed child/master handles.
5. `test_tlsf_arena_cross_thread_free` — worker threads free the owner's blocks
   while the owner is alive (mailbox reap), and sizes above the TLSF cache.
6. `test_tlsf_arena_thread_exit` — thread exits with live blocks; remaining
   frees release the region; pool accounting returns to baseline.
7. `test_tlsf_arena_pool_switch` — thread switches pools with outstanding
   blocks; old arena retires; new pool gets a fresh arena.
8. `test_tlsf_arena_exhaustion` — allocate until fallback to the shared pool;
   mixed blocks free cleanly.

**Randomized differential test** — model = sorted interval array; 20 k
deterministic-seed ops (alloc/free/realloc, sizes 16–192 KiB), payload patterns
verified every op, validator + model agreement every 256 ops, final free-all
restores full arena capacity.

**Concurrency stress** — extend `tests/stress_test.c` with a multi-arena TLSF
scenario: exactly 4 bound threads, random alloc/free/realloc (512 B–256 KiB,
crossing the arena ceiling so the shared-pool fallback is covered), ~10 % of
frees delegated to another thread through per-thread mail slots, payload
verification on every free/realloc. Workers exit with live slots (exercising
retire-with-outstanding-blocks); the main thread frees leftovers and drains the
mail slots. Runs in CI length and within the existing long-run stress harness
(`-DSTRESS_DURATION_SEC=15` quick check).

**Gates** (exact Makefile targets; per-config `build-$(CONFIG)` dirs, no
collision; test targets run with `LSAN_OPTIONS=detect_leaks=0`):

- `make clean && make test_all` (unit + advanced, default release dir)
- `make CONFIG=asan test_all`, `make CONFIG=ubsan test_all`,
  `make CONFIG=tsan test_all` (TSan: zero warnings)
- `make cpp` / `make test_cpp` for the C++ PMR wrapper
- CMake: `make cmake-build && make cmake-test` (Ninja + ctest; Release adds
  `-DNDEBUG`)
- `make format-check`, plus clang-tidy inline in the CMake build (strict)

## 5. Benchmark & Tuning

Extend `benchmarks/bench_main.c` with a multi-arena TLSF scenario:

- sizes 1 KiB, 8 KiB, 64 KiB, 192 KiB (arena-eligible) plus 1 MiB (shared-pool
  reference, beyond arena capacity); thread counts 1/2/4/8; metrics = Mops/s
  (alloc+free pairs) and p50/p99 latency;
- A/B: multi-arena pool (arena on) vs regular pool (arena off), same sizes and
  thread counts;
- same-thread baseline of this scenario can run on current HEAD before the fix
  (broken paths need realloc or cross-thread frees), giving a true before/after
  on the same axis;
- the spec's 8-thread and p99 claims are treated as indicative targets, not
  contractual, since the feature is deliberately multi-arena-scoped; the bench
  does not instrument the shared-pool fallback rate.

Tuning knobs, only if measurements demand: best-fit vs first-fit, next-fit
cursor, arena size increase if fallback were measured high (not instrumented
today). No tuning without a measurement justifying it.

Implementation: a new `bench_tlsf_arena(...)` scenario in
`benchmarks/bench_main.c` reusing the `bench_scaling_run(pool, threads,
allocs_per_thread)` helper (definition ~line 1073), declared with the other
forward declarations (~lines 261-274) and invoked from `main()` (~line 280).

## 6. Definition of Done

1. D1–D8 fixed (plus D11–D14, Sections 3.6–3.7); validator clean in every test across
   all four sanitizer configurations.
2. Randomized differential + multithreaded stress green, including master-handle
   and cross-thread frees; thread-exit and pool-switch paths exercised; pool
   accounting returns to baseline after release.
3. All Section 4 gates pass, TSan with zero warnings.
4. Benchmark recorded (arena vs no-arena, 1–8 threads, Mops/s + p99) with a
   stated conclusion on tuning.
5. Docs close-out: old spec carries a "superseded by" banner; the 2026-08-08
   plan checkboxes are reconciled to reality; AGENTS.md gets a short
   arena-invariants note; bilingual docs updated only if multi-arena becomes
   user-documented (short prose, no diagrams).

## 7. Execution Order

1. Benchmark scenario + baseline capture (same-thread only on current HEAD).
2. Core invariants: sentinel, FREE/PREV_FREE discipline, distance convention,
   single `insert_free`/`unlink_free` helper, `live_bytes`.
3. Flag-based routing in free and realloc; master-handle fix.
4. Mailbox + retire lifecycle; registry release wiring.
5. Realloc via helper; remove ad-hoc list surgery.
6. Targeted tests + randomized differential + stress.
7. Validator wiring in debug builds.
8. Benchmark re-run; tuning only if warranted.
9. Docs close-out and commits.

Commits follow the repo's conventional-commit + emoji style, one logical unit
per commit, no push, no amend.

## 8. Risks & Open Questions

| Risk | Mitigation |
|------|------------|
| Mailbox growth if the owner thread is blocked while others free | Bounded by live blocks; reaped at every owner operation |
| `header->flags` assumed free | Verified: all stamp sites write 0; no reader exists |
| `thread_cache_t` layout change (public header) | Field is internal-use; no ABI guarantee in this codebase; document in commit |
| TSan false positives on header/descriptor patterns | All cross-thread accesses are C11 atomics; owner-only fields are unshared |
| Retire path if `live_bytes` accounting is wrong | Validator + randomized differential test; cleanup additionally arbitrated by the `cleanup_claimed` CAS |
| Two cleanup claimants (retiring thread vs last freer) | Descriptors are pool-owned (registry, released only by `mp_destroy`); the loser's CAS touches live memory; regression: thread-exit test with concurrent frees |
| CMake Release has `-DNDEBUG` → validator no-ops | Tests run via Makefile; CMake runs the same test binaries with explicit calls to the validator |

## 9. Out of Scope

- Generalizing the arena beyond multi-arena child pools.
- Dynamic arena sizing (`TLSF_ARENA_MIN_SIZE` stays reserved).
- Compaction, cross-thread stealing, best-fit rewrite.
- Changes to slab/OS tiers and public API surface.
