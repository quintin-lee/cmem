# Enhanced Benchmark Scenario Suite Design

## Goal

Extend `benchmarks/bench_main.c` (currently 5 scenarios) with 6 new scenario
groups that exercise cmem's distinctive features — FAST_PATH, MULTI_ARENA,
batch APIs, compressed storage, allocation-size distributions, and alloc/free
patterns — and report honest throughput numbers comparable against system
malloc where meaningful.

## Motivation

The existing benchmark only covers small/medium allocs, arena reset, a single
4-thread run, and a game-style workload. It does not measure:

- The `MP_FLAG_FAST_PATH` speedup (temporarily measured in /tmp scripts: ~1.1-1.5x)
- MULTI_ARENA / thread-count scaling (measured once ad hoc: x4.3 at 8 threads)
- Batch allocation APIs vs single-call loops
- Compressed-storage throughput (added in the compressed-storage sub-project)
- Sensitivity of throughput to allocation-size distribution and alloc/free pattern

## Scope

Modify only `benchmarks/bench_main.c`. No library source changes. The benchmark
already links against full `src/*.c` via the Makefile (`make bench`).

All scenarios keep the established conventions:
- `static volatile void *bench_escape_sink` (already present) prevents gcc -O2
  from DCE-ing malloc loops.
- `get_time_sec()` monotonic timer.
- Throughput reported in Mops/sec (via `MILLION_OPS`).
- Every scenario prints `--- Benchmark N: <title> ---` before results.

## New Scenario Groups (Benchmarks 6-11)

### Benchmark 6: FAST_PATH vs Full Audit Mode

Compare `MP_FLAG_THREAD_LOCAL_CACHE` vs `MP_FLAG_THREAD_LOCAL_CACHE | MP_FLAG_FAST_PATH`
on the same interleaved small-alloc workload as Benchmark 1 (1M allocs,
32-256B, alloc immediately followed by free).

Expected: FAST_PATH 1.05-1.5x faster (matches /tmp/fastpath_bench measurements).
Report both Mops/s and the speedup ratio.

### Benchmark 7: Allocation Size Distribution

Three sub-cases, each 1M interleaved alloc+free ops, cmem (THREAD_LOCAL_CACHE)
vs system malloc side by side:

- **Fixed small**: constant 32B — exercises a single slab class hot slot.
- **Mixed small**: `16 + (i * 7) % 224` — irregular traversal across all slab
  classes, avoiding the clean `i % 224` stride.
- **Large objects**: `4096 + (i * 13) % 16384` — TLSF territory (above
  SLAB_MAX_SIZE=512), exercising the TLSF allocator path.

### Benchmark 8: Batch APIs

Compare `mp_alloc_batch` / `mp_free_batch` against single `mp_alloc`/`mp_free`
loops. Workload: 32B fixed size, 10,000 batches × 64 elements = 640k allocs.
Pool: `MP_FLAG_THREAD_LOCAL_CACHE`.

Report per-call overhead: batch mode cost per element vs loop mode cost per
element, plus overall Mops/sec for both.

### Benchmark 9: Thread Count Scaling

Matrix of thread counts {1, 2, 4, 8} × pool modes:
- `MP_FLAG_THREAD_SAFE | MP_FLAG_THREAD_LOCAL_CACHE` (single pool, baseline)
- `+ MP_FLAG_MULTI_ARENA` (auto CPU-bind child arenas)

Each thread does 100k interleaved alloc+free of 32-256B (`32 + (i % 256)`).
Report Mops/sec per config and a scaling-efficiency column (x-thread throughput
normalized against the 1-thread number of the same mode).

This replaces/supersedes nothing — Benchmark 4 stays as-is (batch alloc-then-
free per thread, 4 threads); Benchmark 9 adds the interleaved + scaling matrix.

### Benchmark 10: Compressed Storage

Pool: `mp_create(64MB, MP_FLAG_DEFAULT)`. Data: 4KB block of compressible
pattern (`char[i] = 'a' + i % 4`), repeated 10,000 times:

- **Compress path**: `mp_compress_block(pool, data, 4096)` loop, 10k iterations.
- **Decompress path**: compress one block first, then `mp_decompress_block`
  in a 10k loop (handle stays valid; generation unchanged).
- Report MB/s for each path and the observed compression ratio.

### Benchmark 11: Allocation Patterns

Four patterns on the same 200k ops, cmem `MP_FLAG_THREAD_LOCAL_CACHE` vs system
malloc, 32-256B:

- **Interleaved**: alloc+free immediately (TLS-cache friendly).
- **Batch**: alloc 100k, then free 100k (TLS-cache hostile — the pattern that
  previously showed 9.8x slowdown).
- **Random size**: size from an xorshift PRNG in 32-256, interleaved — measures
  cache-indexing robustness to arbitrary sizes.
- **Live set**: alloc all, keep 25% live, free 75%, re-alloc — simulates steady
  churn with a persistent live set.

## Reporting Format

Each new benchmark prints one `--- Benchmark N: <title> ---` header plus the
per-case lines. Benchmark 9 prints a small aligned table. Units: Mops/sec or
MB/sec as appropriate. No changes to Benchmark 1-5 output format (regression
baseline compatibility).

## Testing

- `make bench` must complete all 11 benchmarks without error, exit 0.
- `cd build_cmake && ninja -t clean && ninja` → 0 warnings/errors
  (clang-tidy runs inline; identifier length ≥2, no magic numbers beyond the
  allowlist — new constants must be named).
- `ctest` 3/3 unaffected (no test files touched).
- `make format-check` exit 0.
- All new loop counters and sizes defined as named constants.

## Deliverables

- `benchmarks/bench_main.c` extended with Benchmarks 6-11.
- One conventional commit: `perf(bench): add size/pattern/batch/thread/compression benchmark scenarios`.
