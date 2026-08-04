# Compressed Storage Design

- **Date**: 2026-08-04
- **Status**: Draft (pending review)
- **Scope**: cmem library — in-pool transparent LZ4 compression for infrequently
  accessed blocks

## 1. Motivation

cmem's tiered allocator (slab → TLSF → OS fallback) optimizes allocation speed
and fragmentation, but all live data occupies uncompressed memory. Workloads
with large, infrequently-accessed regions (caches, serialized state, cold
records) waste physical memory that could be reclaimed via compression.

The architecture document (§12, "Compressed Storage: Transparent memory
compression") lists this as a future direction. Transparent (pointer-transparent)
compression is infeasible in user space because no mechanism can intercept
arbitrary reads/writes through a plain pointer without page-fault hooks. This
design therefore provides an **explicit API model**: the user compresses a block,
gets back an opaque handle, and later decompresses it — with clear ownership
transfer semantics.

## 2. Goals

- Provide an in-pool compression facility with zero external dependencies
  (self-contained LZ4 block codec).
- Ownership-transfer semantics: `mp_compress_block` compresses AND frees the
  original memory; the returned handle owns the compressed copy.
- Dedicated compression area inside the pool, subject to the pool's memory
  limit and statistics.
- Budget cap with automatic eviction of the oldest block when exceeded.
- Full availability on all platforms (pure user-space codec, no syscalls).

## 3. Non-Goals

- No transparent (pointer-transparent) compression — infeasible in user space.
- No zstd/LZMA — LZ4 only (small, fast, self-contained).
- No write-time or background compression — compression is always explicit.
- No separate compressed arena object — the compression area lives inside the
  pool and is destroyed with it.

## 4. Public API

All functions declared in `include/cmem.h`, implemented in new `src/cmem_compress.c`.

### 4.1 Opaque handle

```c
/* Opaque compressed-block handle; 0 is always invalid. */
typedef uint64_t compressed_handle_t;
```

- Encodes an index into the pool's handle table plus a generation counter to
  detect stale handles (use-after-free).

### 4.2 Compression (ownership transfer)

```c
compressed_handle_t mp_compress_block(memory_pool_t *pool,
                                      void *data, size_t size);
```

- Compresses `data` into the pool's compression area, then frees `data`
  (ownership transfer).
- Returns `0` when: compression fails, the compressed size is not smaller than
  the original (no gain — original data is NOT freed), the pool has no
  compression facility, or budget eviction finds nothing to evict.
- When the compression area is full and a new block exceeds the budget, the
  oldest block is evicted (its handle is invalidated); if eviction is impossible
  (area still too small), returns `0`.

### 4.3 Decompression (repeatable)

```c
void *mp_decompress_block(memory_pool_t *pool, compressed_handle_t handle);
```

- Allocates a fresh ordinary block and decompresses into it; returns the
  pointer. The handle remains valid and may be called repeatedly.
- Returns `NULL` on invalid/stale/evicted handle or decompression failure.

### 4.4 Freeing the compressed copy

```c
bool mp_free_compressed(memory_pool_t *pool, compressed_handle_t handle);
```

- Releases the compressed copy and invalidates the handle.
- Returns `false` for an invalid/stale handle; the memory is still reclaimed
  on pool destruction regardless.

### 4.5 Budget and statistics

```c
bool mp_set_compressed_budget(memory_pool_t *pool, size_t max_bytes);
bool mp_get_compressed_stats(memory_pool_t *pool, size_t *used,
                             size_t *budget, size_t *block_count);
```

- `mp_set_compressed_budget` sets the upper bound for the compression area.
  `0` disables compression for the pool (existing handles remain valid).
- `mp_get_compressed_stats` reports current compressed usage, budget, and live
  block count. Accepts `NULL` for any output parameter.

## 5. Internal Design

### 5.1 New file: `src/cmem_compress.c`

Self-contained module with three internal components:

1. **LZ4 block codec** — ~300 lines, LZ4 block-format compatible: sequences of
   literals + matches, 16-bit little-endian offsets, 4KB match window. API:

   ```c
   int cmem_lz4_compress(const uint8_t *src, uint8_t *dst,
                         int src_size, int dst_cap);   /* >=0 compressed, -1 overflow */
   int cmem_lz4_decompress(const uint8_t *src, uint8_t *dst,
                           int src_size, int dst_size); /* dst_size on success, <0 error */
   ```

2. **Compression-area allocator** — 4 KiB slot allocator (free-list) managing
   the in-pool compression area. The area is expanded via the existing
   `mp_expand_pool` mechanism so it is subject to the pool memory limit and
   reflected in pool statistics.

3. **Handle table** — fixed-capacity array (e.g. 4096 entries) attached to the
   pool. Each entry:

   ```c
   typedef struct cmem_compressed_entry {
       size_t             original_size; /* bytes before compression       */
       size_t             comp_offset;   /* offset of data in compression area */
       size_t             comp_size;     /* bytes after compression        */
       uint32_t           generation;    /* bumped on free/evict           */
       uint32_t           alloc_seq;     /* insertion order for eviction   */
       bool               used;
   } cmem_compressed_entry_t;
   ```

### 5.2 Pool integration (`cmem_internal.h`)

`memory_pool_t` gains:

```c
cmem_compressed_entry_t *compressed_entries;   /* NULL = compression disabled */
uint32_t  compressed_capacity;                 /* handle-table slots          */
uint32_t  compressed_seq;                      /* alloc order counter         */
size_t    compressed_budget;                   /* 0 = disabled                */
size_t    compressed_used;                     /* bytes currently in use      */
void     *compressed_area;                     /* slot-allocated region       */
size_t    compressed_area_size;
```

- Lazy: `compressed_entries` and the compression area are created on first
  `mp_compress_block` call (or on `mp_set_compressed_budget` with non-zero
  budget), via `mp_expand_pool`.
- `mp_destroy` frees the area and entries table; `mp_check_leaks` must not
  report compression-area memory as leaks (owned by the pool, not handed out).
- Thread-safety: handle-table and area access guarded by the pool's existing
  coarse lock (same one used by `mp_expand_pool`).

### 5.3 Data flow

- **Compress**: `mp_compress_block` → LZ4 encode into a scratch buffer sized
  `size` → if result >= `size`, return 0 without freeing `data` → allocate a
  slot in the compression area (evicting oldest if over budget) → copy encoded
  bytes → register entry → `mp_free(pool, data)` → return handle.
- **Decompress**: lookup entry (generation check) → `mp_alloc(pool,
  original_size)` → LZ4 decode → return pointer.
- **Eviction**: on budget exceed, evict the entry with the lowest `alloc_seq`
  (oldest); free its slot, bump its generation, mark unused. Handles to
  evicted blocks then fail lookups.

## 6. Error Handling

| Scenario | Behavior |
| :--- | :--- |
| Compression fails / no gain (dst >= src) | Return `0`; original data NOT freed |
| Compression-area expansion fails (OOM) | Return `0`; original data NOT freed |
| Budget exceeded and nothing evictable | Return `0` |
| Invalid / stale / evicted handle | `mp_decompress_block` → `NULL`; `mp_free_compressed` → `false` |
| Decompression failure (corrupt data) | Return `NULL` (no crash) |
| Non-Linux platforms | Fully functional (pure user-space codec) |

## 7. Testing

Add `test_compressed_storage()` to `tests/test_main.c` (registered in `main()`):

- **Round-trip**: compress a block of repeated bytes → decompress → content
  identical to original.
- **No-gain**: compress incompressible random data → returns `0`, original
  pointer still valid and untouched (ownership NOT transferred).
- **Ownership transfer**: after successful compression, the original pointer is
  freed (verify via leak check not reporting it).
- **Repeatable decompress**: call `mp_decompress_block` twice → both pointers
  valid, contents equal.
- **Free handle**: `mp_free_compressed` → subsequent decompress returns `NULL`;
  second `mp_free_compressed` returns `false`.
- **Budget eviction**: set small budget, compress several blocks → oldest
  handle becomes invalid; stats reflect usage.
- **Stats**: `mp_get_compressed_stats` reports consistent used/budget/block_count
  after a known sequence.
- **Leak integrity**: destroy pool → `mp_check_leaks` clean, no compression-area
  false positives.
- **Budget disable**: `mp_set_compressed_budget(pool, 0)` → subsequent
  `mp_compress_block` returns `0`; existing handles still decompress.

## 8. Files Touched

- Create: `src/cmem_compress.c`
- Modify: `include/cmem.h` (handle typedef, 5 API prototypes),
  `src/cmem_internal.h` (entry struct, pool fields, externs),
  `src/cmem.c` or `src/cmem_event.c` (mp_destroy / mp_check_leaks integration —
  decided during planning),
  `tests/test_main.c` (new test),
  `docs/en/performance.md` + `docs/zh/performance.md` (§ compression),
  `CHANGELOG.md`
- `CMakeLists.txt` / `Makefile`: add `src/cmem_compress.c` to sources.

## 9. Resolved Decisions

- **Model**: explicit API with ownership transfer (not transparent, not
  copy-semantics, not write-time).
- **Algorithm**: built-in LZ4 block codec (self-contained; no external deps).
- **Storage**: dedicated in-pool compression area via `mp_expand_pool`.
- **Lifecycle**: `mp_compress_block` frees the original; handle repeatably
  decompressible until `mp_free_compressed`.
- **Capacity**: budget cap with oldest-first eviction; `0` disables.
- **No compression-ratio guarantee**: no-gain blocks return `0` and keep the
  original alive.
