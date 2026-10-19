# cmem Compressed Storage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add explicit compressed storage to cmem: an in-pool compression area backed by a self-contained LZ4 block codec, with ownership-transferring `mp_compress_block` / `mp_decompress_block` / `mp_free_compressed` APIs, a configurable budget with oldest-first eviction, and stats reporting.

**Architecture:** A new `src/cmem_compress.c` module provides (a) a pure LZ4 block codec (literals+matches, 16-bit LE offsets, 4KiB window), (b) a 4KiB-slot allocator over a pool-owned compression area grown via `mp_expand_pool`, and (c) a 4096-entry handle table with generation counters. `mp_compress_block` takes ownership of caller data (frees it after storing the compressed copy), returns an opaque `compressed_handle_t`; `mp_decompress_block` returns a fresh pool allocation; eviction removes the oldest block when the budget is exceeded. All of it is pure user-space C11, no external deps.

**Tech Stack:** C11, existing cmem pool machinery (`mp_expand_pool`, coarse pool lock, `mp_alloc`/`mp_free`), pthread (pool lock), CMake + Makefile builds (Makefile uses `wildcard src/*.c` so new file is auto-included).

---

## Phase 1: LZ4 Block Codec

### Task 1.1: Create src/cmem_compress.c with the LZ4 codec

**Files:**
- Create: `src/cmem_compress.c`

- [ ] **Step 1: Create the file skeleton + LZ4 codec**

```c
/*
 * cmem_compress.c — in-pool compressed storage.
 *
 * Self-contained LZ4 block format codec (literals + matches, 16-bit LE
 * offsets, 4KiB window) plus the compression-area slot allocator and
 * handle table.  Pure user-space; no external dependencies.
 */
#include "cmem.h"
#include "cmem_internal.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

/*
 * LZ4 block codec.  Follows the classic LZ4 block format:
 *   token = (literal_len<<4) | (match_len-4) ; lengths extended with
 *   255-byte runs when the nibble is 15
 *   literals ... ; 2-byte little-endian offset ; match copy
 * Each sequence is: token, literal bytes, offset (unless it is the final
 * literal-only sequence), then match_len bytes copied from (dst - offset).
 * The encoder is greedy: it accumulates literals between matches and
 * flushes them inside the match token.
 */

/* Token nibble 15 means "length continues in 255-byte chunks". */
#define CMEM_LZ4_MAX_EXT 255
/* Minimum match length encoded in the token (matches shorter are skipped). */
#define CMEM_LZ4_MIN_MATCH 4
/* Maximum offset representable in the 16-bit LE offset field. */
#define CMEM_LZ4_MAX_DIST 0xFFFFu

static int cmem_lz4_write_len(uint8_t *op, const uint8_t *oend, int len)
{
    int written = 0;
    while (len >= CMEM_LZ4_MAX_EXT) {
        if (op >= oend) {
            return -1;
        }
        *op++ = (uint8_t)CMEM_LZ4_MAX_EXT;
        len -= CMEM_LZ4_MAX_EXT;
        written++;
    }
    if (op >= oend) {
        return -1;
    }
    *op++ = (uint8_t)len;
    return written + 1;
}

int cmem_lz4_compress(const uint8_t *src, uint8_t *dst, int src_size, int dst_cap)
{
    const uint8_t *ip = src;
    const uint8_t *const iend = src + src_size;
    uint8_t *op = dst;
    const uint8_t *const oend = dst + dst_cap;
    const uint8_t *anchor = src; /* start of pending literals */

    if (src_size <= 0 || dst_cap <= 0) {
        return -1;
    }

    while (ip < iend) {
        /* Find the longest match within the 64KiB window (bounded probe). */
        int best_len = 0;
        int best_off = 0;
        int probe_len = (int)(iend - ip);
        if (probe_len > 4096) {
            probe_len = 4096;
        }
        if (ip > src) {
            int max_off = (int)(ip - src);
            if (max_off > CMEM_LZ4_MAX_DIST) {
                max_off = CMEM_LZ4_MAX_DIST;
            }
            for (int off = 1; off <= max_off; off++) {
                const uint8_t *cand = ip - off;
                int len = 0;
                while (len < probe_len && cand[len] == ip[len]) {
                    len++;
                }
                if (len > best_len) {
                    best_len = len;
                    best_off = off;
                    if (len == probe_len) {
                        break;
                    }
                }
            }
        }
        if (best_len < CMEM_LZ4_MIN_MATCH) {
            ip++; /* gather one more literal */
            continue;
        }

        /* Flush pending literals plus this match in one sequence.
         * Bounds are checked per step (extension bytes make a single
         * upfront check insufficient); every failure returns -1 cleanly. */
        int lit_len = (int)(ip - anchor);
        int mlen = best_len;
        if (op + 1 > oend) {
            return -1;
        }
        *op++ = (uint8_t)((lit_len < 15 ? lit_len : 15) << 4) |
                (uint8_t)(mlen - CMEM_LZ4_MIN_MATCH < 15 ? mlen - CMEM_LZ4_MIN_MATCH : 15);
        if (lit_len >= 15) {
            int ext = cmem_lz4_write_len(op, oend, lit_len - 15);
            if (ext < 0) {
                return -1;
            }
            op += ext;
        }
        if (lit_len > 0) {
            if (op + lit_len > oend) {
                return -1;
            }
            memcpy(op, anchor, (size_t)lit_len);
            op += lit_len;
        }
        if (op + 2 > oend) {
            return -1;
        }
        uint16_t off = (uint16_t)best_off;
        op[0] = (uint8_t)(off & 0xFFu);
        op[1] = (uint8_t)(off >> 8);
        op += 2;
        if (mlen - CMEM_LZ4_MIN_MATCH >= 15) {
            int ext = cmem_lz4_write_len(op, oend, mlen - CMEM_LZ4_MIN_MATCH - 15);
            if (ext < 0) {
                return -1;
            }
            op += ext;
        }
        ip += best_len;
        anchor = ip;
    }

    /* Trailing literals-only sequence (valid as the final sequence).
     * Per-step bounds checks; see the match-sequence comment above. */
    if (anchor < iend) {
        int lit_len = (int)(iend - anchor);
        if (op + 1 > oend) {
            return -1;
        }
        *op++ = (uint8_t)((lit_len < 15 ? lit_len : 15) << 4);
        if (lit_len >= 15) {
            int ext = cmem_lz4_write_len(op, oend, lit_len - 15);
            if (ext < 0) {
                return -1;
            }
            op += ext;
        }
        if (op + lit_len > oend) {
            return -1;
        }
        memcpy(op, anchor, (size_t)lit_len);
        op += lit_len;
    }
    return (int)(op - dst);
}

int cmem_lz4_decompress(const uint8_t *src, uint8_t *dst, int src_size, int dst_size)
{
    const uint8_t *ip = src;
    const uint8_t *const iend = src + src_size;
    uint8_t *op = dst;
    const uint8_t *const oend = dst + dst_size;

    while (ip < iend) {
        uint8_t token = *ip++;
        int lit_len = (token >> 4) & 0x0F;
        if (lit_len == 15) {
            for (;;) {
                if (ip >= iend) {
                    return -1;
                }
                uint8_t ext = *ip++;
                lit_len += ext;
                if (ext != (uint8_t)CMEM_LZ4_MAX_EXT) {
                    break;
                }
            }
        }
        if (op + lit_len > oend || ip + lit_len > iend) {
            return -1;
        }
        memcpy(op, ip, (size_t)lit_len);
        op += lit_len;
        ip += lit_len;
        if (ip >= iend) {
            break;
        }
        if (ip + 2 > iend) {
            return -1;
        }
        uint16_t off = (uint16_t)(ip[0] | ((uint16_t)ip[1] << 8));
        ip += 2;
        if (off == 0 || off > (uint32_t)(op - dst)) {
            return -1;
        }
        int match_len = (token & 0x0F);
        if (match_len == 15) {
            for (;;) {
                if (ip >= iend) {
                    return -1;
                }
                uint8_t ext = *ip++;
                match_len += ext;
                if (ext != (uint8_t)CMEM_LZ4_MAX_EXT) {
                    break;
                }
            }
        }
        match_len += CMEM_LZ4_MIN_MATCH;
        if (op + match_len > oend) {
            return -1;
        }
        for (int i = 0; i < match_len; i++) {
            op[i] = op[i - (int)off];
        }
        op += match_len;
    }
    return (int)(op - dst);
}
```

- [ ] **Step 2: Verify the codec with a smoke driver**

Run:
```bash
cat > /tmp/lz4_smoke.c <<'EOF'
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* The codec is defined in src/cmem_compress.c; declare the API here.
 * (No cmem_compress.h header exists — externs live in cmem_internal.h.) */
int cmem_lz4_compress(const uint8_t *, uint8_t *, int, int);
int cmem_lz4_decompress(const uint8_t *, uint8_t *, int, int);
int main(void) {
    const char *text = "hello hello hello world world world world hello hello";
    size_t n = strlen(text);
    uint8_t comp[512], out[512];
    int clen = cmem_lz4_compress((const uint8_t *)text, comp, (int)n, 512);
    printf("compress: %d (src %zu)\n", clen, n);
    if (clen <= 0) return 1;
    int dlen = cmem_lz4_decompress(comp, out, clen, 512);
    printf("decompress: %d\n", dlen);
    if (dlen != (int)n || memcmp(out, text, n) != 0) return 2;
    /* Incompressible data must still round-trip. */
    uint8_t rnd[64];
    for (int i = 0; i < 64; i++) rnd[i] = (uint8_t)(i * 37 + 11);
    clen = cmem_lz4_compress(rnd, comp, 64, 512);
    if (clen <= 0) return 3;
    dlen = cmem_lz4_decompress(comp, out, clen, 512);
    if (dlen != 64 || memcmp(out, rnd, 64) != 0) return 4;
    printf("OK\n");
    return 0;
}
EOF
gcc -O2 -std=gnu11 -D_GNU_SOURCE -I./include -Isrc /tmp/lz4_smoke.c src/cmem_compress.c -o /tmp/lz4_smoke && /tmp/lz4_smoke
```
Expected: `compress: N (src 57)`, `decompress: 57`, `OK`, exit 0.

- [ ] **Step 3: Commit**

```bash
git add src/cmem_compress.c
git commit -m "feat(compress): add self-contained LZ4 block codec"
```

## Phase 2: Public API Declarations

### Task 2.1: Declare compressed-storage API in include/cmem.h

**Files:**
- Modify: `include/cmem.h` (after `mp_get_compressed_stats`-adjacent diagnostics section, near `mp_set_auto_compact` ~line 1303)

- [ ] **Step 1: Add the handle typedef and 5 prototypes**

Add after the `mp_enable_emergency_reserve` / `mp_set_numa_node` block (keep grouping with pool-configuration APIs):

```c
/**
 * @brief Opaque handle to a compressed block stored inside a pool.
 *
 * Zero is always an invalid handle.  Handles remain valid until the
 * block is explicitly freed (mp_free_compressed) or evicted by budget
 * pressure; stale handles are rejected safely by every API.
 */
typedef uint64_t compressed_handle_t;

/**
 * @brief Compresses data and transfers its ownership to the pool.
 *
 * The buffer at @p data is compressed into the pool's compression area
 * and then released via mp_free().  On success the caller must no longer
 * use @p data.  Returns 0 (and leaves @p data untouched) when the data
 * does not compress, the pool has no compression facility, or no budget
 * slot can be made available.
 *
 * @param pool Pool to store the compressed block in
 * @param data Buffer to compress (ownership transferred on success)
 * @param size Size of @p data in bytes
 * @return Handle, or 0 on failure (original data retained)
 */
compressed_handle_t mp_compress_block(memory_pool_t *pool, void *data, size_t size);

/**
 * @brief Decompresses a block back into a fresh pool allocation.
 *
 * Safe to call multiple times; each call returns an independent copy.
 * @return Pointer to the decompressed data, or NULL on invalid/stale
 *         handles or corrupted data.
 */
void *mp_decompress_block(memory_pool_t *pool, compressed_handle_t handle);

/**
 * @brief Frees a compressed block and invalidates its handle.
 * @return true on success, false if the handle was invalid or stale.
 */
bool mp_free_compressed(memory_pool_t *pool, compressed_handle_t handle);

/**
 * @brief Sets the maximum bytes the compression area may occupy.
 *
 * A budget of 0 disables further compression (existing handles remain
 * valid and decompressible).  When the budget is exceeded, the oldest
 * block is evicted to make room.
 *
 * @note The compression area is grown once, on the first use of
 *       compression (either this call or mp_compress_block) and sized to
 *       the budget in effect at that moment.  Raising the budget after
 *       the area exists raises the eviction threshold only; it does not
 *       enlarge the area.
 * @return true on success.
 */
bool mp_set_compressed_budget(memory_pool_t *pool, size_t max_bytes);

/**
 * @brief Reports compression-area usage.  Any out parameter may be NULL.
 * @return true on success.
 */
bool mp_get_compressed_stats(memory_pool_t *pool, size_t *used, size_t *budget,
                             size_t *block_count);
```

- [ ] **Step 2: Verify header compiles standalone**

Run: `gcc -std=gnu11 -D_GNU_SOURCE -fsyntax-only -I./include -xc - <<< '#include "cmem.h"'`
Expected: no output, exit 0.

- [ ] **Step 3: Commit**

```bash
git add include/cmem.h
git commit -m "feat(compress): declare compressed-storage public API"
```

## Phase 3: Internal Structures

### Task 3.1: Add entry struct, pool fields, and externs to cmem_internal.h

**Files:**
- Modify: `src/cmem_internal.h` (entry struct near other per-pool structs ~line 250; pool fields in `struct memory_pool`; externs near line 583)

- [ ] **Step 1: Add the entry struct and externs**

```c
/* Compressed-storage entry: one slot in the pool's handle table. */
typedef struct cmem_compressed_entry {
    size_t   original_size; /* uncompressed payload size */
    size_t   comp_offset;   /* byte offset into compression_area */
    size_t   comp_size;     /* compressed payload size */
    uint32_t generation;    /* bumped on every reuse (stale-handle guard) */
    uint32_t alloc_seq;     /* monotonic order for oldest-first eviction */
    bool     used;
} cmem_compressed_entry_t;
```

And in the `System memory & platform (cmem_sys.c)` extern section (after `extern int cmem_cpu_to_node(int cpu);`):

```c
extern int cmem_lz4_compress(const uint8_t *src, uint8_t *dst, int src_size, int dst_cap);
extern int cmem_lz4_decompress(const uint8_t *src, uint8_t *dst, int src_size, int dst_size);
```

- [ ] **Step 2: Add compression fields to `struct memory_pool`**

Add after the existing pool fields (near `cmem_numa`/flags region):

```c
    /* --- Compressed storage --- */
    cmem_compressed_entry_t *compressed_entries; /* handle table, NULL = disabled */
    uint32_t                 compressed_capacity;/* table capacity (0 = disabled) */
    uint32_t                 compressed_seq;     /* next allocation sequence */
    size_t                   compressed_budget;  /* 0 = disabled */
    size_t                   compressed_used;    /* bytes currently stored */
    void                    *compressed_area;    /* compression area base */
    size_t                   compressed_area_size; /* usable bytes in area */
```

- [ ] **Step 3: Commit**

```bash
git add src/cmem_internal.h
git commit -m "chore(compress): add internal compressed-storage structures"
```

## Phase 4: Core Implementation

### Task 4.1: Slot allocator + handle table + public API impl in cmem_compress.c

**Files:**
- Modify: `src/cmem_compress.c` (append after codec)

- [ ] **Step 1: Add constants, slot allocator, and lazy init**

```c
/* Compression-area management.  Slots are 4KiB-aligned blocks carved
 * from the pool-owned area; a free list chains reusable offsets. */
#define CMEM_COMPRESS_SLOT_SIZE 4096u
#define CMEM_COMPRESS_DEFAULT_BUDGET (16ul * 1024ul * 1024ul) /* 16 MiB */
#define CMEM_COMPRESS_MAX_ENTRIES 4096u
#define CMEM_COMPRESS_SLOT_MASK (CMEM_COMPRESS_SLOT_SIZE - 1u)

static bool cmem_compress_ensure_area(memory_pool_t *pool)
{
    if (pool->compressed_area != NULL) {
        return true;
    }
    size_t want = pool->compressed_budget;
    if (want == 0) {
        want = CMEM_COMPRESS_DEFAULT_BUDGET;
        pool->compressed_budget = want; /* apply default so compression proceeds */
    }
    /* Grow the pool (subject to its memory limit) to back the area. */
    if (!mp_expand_pool(pool, want)) {
        return false;
    }
    void *area = mp_alloc(pool, want);
    if (area == NULL) {
        return false;
    }
    /* Reserve the table. */
    pool->compressed_entries =
        (cmem_compressed_entry_t *)calloc(CMEM_COMPRESS_MAX_ENTRIES,
                                          sizeof(cmem_compressed_entry_t));
    if (pool->compressed_entries == NULL) {
        mp_free(pool, area);
        return false;
    }
    pool->compressed_capacity = CMEM_COMPRESS_MAX_ENTRIES;
    pool->compressed_area = area;
    pool->compressed_area_size = want;
    return true;
}

static void cmem_compress_evict_oldest(memory_pool_t *pool)
{
    uint32_t oldest = UINT32_MAX;
    int32_t  oldest_idx = -1;
    for (uint32_t i = 0; i < pool->compressed_capacity; i++) {
        cmem_compressed_entry_t *e = &pool->compressed_entries[i];
        if (e->used && e->alloc_seq < oldest) {
            oldest = e->alloc_seq;
            oldest_idx = (int32_t)i;
        }
    }
    if (oldest_idx < 0) {
        return;
    }
    cmem_compressed_entry_t *e = &pool->compressed_entries[oldest_idx];
    e->used = false;
    e->generation++; /* invalidate stale handles */
    pool->compressed_used -= e->comp_size;
}
```

- [ ] **Step 2: Implement the five public functions**

```c
compressed_handle_t mp_compress_block(memory_pool_t *pool, void *data, size_t size)
{
    if (pool == NULL || data == NULL || size == 0) {
        return (compressed_handle_t)0;
    }
    /* Small payloads compress poorly; keep them as-is. */
    if (size < 64) {
        return (compressed_handle_t)0;
    }
    /* The codec works on int-sized buffers; reject oversized inputs. */
    if (size > (size_t)INT_MAX) {
        return (compressed_handle_t)0;
    }
    if (pool->compressed_area == NULL && !cmem_compress_ensure_area(pool)) {
        return (compressed_handle_t)0;
    }
    if (pool->compressed_budget == 0) {
        return (compressed_handle_t)0; /* budget disabled */
    }

    /* Compress into a scratch buffer sized like the source. */
    uint8_t *scratch = (uint8_t *)malloc(size);
    if (scratch == NULL) {
        return (compressed_handle_t)0;
    }
    int clen = cmem_lz4_compress((const uint8_t *)data, scratch, (int)size, (int)size);
    if (clen <= 0 || (size_t)clen >= size) {
        free(scratch);
        return (compressed_handle_t)0; /* no gain — caller keeps data */
    }

    /* Make room under the budget (evict oldest first).  Guard against an
     * unbounded loop when a single block cannot fit even after eviction. */
    while (pool->compressed_used + (size_t)clen > pool->compressed_budget) {
        size_t before = pool->compressed_used;
        cmem_compress_evict_oldest(pool);
        if (pool->compressed_used == before) {
            free(scratch);
            return (compressed_handle_t)0; /* nothing left to evict */
        }
    }

    /* Find a free entry in the handle table. */
    uint32_t slot = UINT32_MAX;
    for (uint32_t i = 0; i < pool->compressed_capacity; i++) {
        if (!pool->compressed_entries[i].used) {
            slot = i;
            break;
        }
    }
    if (slot == UINT32_MAX) {
        free(scratch);
        return (compressed_handle_t)0;
    }

    /* Carve a slot in the compression area. */
    size_t off = 0;
    uint8_t *area = (uint8_t *)pool->compressed_area;
    size_t aligned = ((size_t)clen + CMEM_COMPRESS_SLOT_MASK) & ~CMEM_COMPRESS_SLOT_MASK;
    if (aligned == 0) {
        aligned = CMEM_COMPRESS_SLOT_SIZE;
    }
    for (off = 0; off + aligned <= pool->compressed_area_size; off += CMEM_COMPRESS_SLOT_SIZE) {
        /* Simple first-fit; the area is private to this pool. */
        bool busy = false;
        for (uint32_t i = 0; i < pool->compressed_capacity; i++) {
            cmem_compressed_entry_t *e = &pool->compressed_entries[i];
            if (e->used && e->comp_offset < off + aligned && e->comp_offset + e->comp_size > off) {
                busy = true;
                break;
            }
        }
        if (!busy) {
            break;
        }
    }
    if (off + aligned > pool->compressed_area_size) {
        free(scratch);
        return (compressed_handle_t)0;
    }

    memcpy(area + off, scratch, (size_t)clen);
    free(scratch);

    cmem_compressed_entry_t *e = &pool->compressed_entries[slot];
    e->original_size = size;
    e->comp_offset = off;
    e->comp_size = (size_t)clen;
    e->generation++;
    e->alloc_seq = pool->compressed_seq++;
    e->used = true;
    pool->compressed_used += (size_t)clen;

    /* Ownership transfer: release the caller's buffer. */
    mp_free(pool, data);

    return (compressed_handle_t)((((uint64_t)slot) << 32) | e->generation);
}

void *mp_decompress_block(memory_pool_t *pool, compressed_handle_t handle)
{
    if (pool == NULL || handle == 0) {
        return NULL;
    }
    uint32_t slot = (uint32_t)(handle >> 32);
    uint32_t gen = (uint32_t)(handle & 0xFFFFFFFFu);
    if (slot >= pool->compressed_capacity) {
        return NULL;
    }
    cmem_compressed_entry_t *e = &pool->compressed_entries[slot];
    if (!e->used || e->generation != gen) {
        return NULL; /* stale or evicted */
    }
    void *out = mp_alloc(pool, e->original_size);
    if (out == NULL) {
        return NULL;
    }
    int dlen = cmem_lz4_decompress((const uint8_t *)pool->compressed_area + e->comp_offset,
                                   (uint8_t *)out, (int)e->comp_size, (int)e->original_size);
    if (dlen != (int)e->original_size) {
        mp_free(pool, out);
        return NULL; /* corrupted */
    }
    return out;
}

bool mp_free_compressed(memory_pool_t *pool, compressed_handle_t handle)
{
    if (pool == NULL || handle == 0) {
        return false;
    }
    uint32_t slot = (uint32_t)(handle >> 32);
    uint32_t gen = (uint32_t)(handle & 0xFFFFFFFFu);
    if (slot >= pool->compressed_capacity) {
        return false;
    }
    cmem_compressed_entry_t *e = &pool->compressed_entries[slot];
    if (!e->used || e->generation != gen) {
        return false;
    }
    e->used = false;
    e->generation++;
    pool->compressed_used -= e->comp_size;
    return true;
}

bool mp_set_compressed_budget(memory_pool_t *pool, size_t max_bytes)
{
    if (pool == NULL) {
        return false;
    }
    pool->compressed_budget = max_bytes;
    if (max_bytes > 0 && pool->compressed_area == NULL) {
        return cmem_compress_ensure_area(pool);
    }
    return true;
}

bool mp_get_compressed_stats(memory_pool_t *pool, size_t *used, size_t *budget,
                             size_t *block_count)
{
    if (pool == NULL) {
        return false;
    }
    if (used != NULL) {
        *used = pool->compressed_used;
    }
    if (budget != NULL) {
        *budget = pool->compressed_budget;
    }
    if (block_count != NULL) {
        size_t count = 0;
        for (uint32_t i = 0; i < pool->compressed_capacity; i++) {
            if (pool->compressed_entries[i].used) {
                count++;
            }
        }
        *block_count = count;
    }
    return true;
}
```

- [ ] **Step 3: clang-format + object verify**

Run:
```bash
clang-format -i src/cmem_compress.c
cd build_cmake && ninja CMakeFiles/cmem.dir/src/cmem_compress.c.o 2>&1 | grep -E 'warning:|error:' | grep -v note:
```
Expected: empty output (no warnings/errors).

- [ ] **Step 4: Commit**

```bash
git add src/cmem_compress.c
git commit -m "feat(compress): implement compression area, handle table, and public APIs"
```

## Phase 5: Pool Lifecycle Integration

### Task 5.1: Free compression state in mp_destroy and exclude from leak checks

**Files:**
- Modify: `src/cmem_event.c` (mp_destroy ~line 1412)
- Modify: `src/cmem_diag.c` (mp_check_leaks ~line 841)

**Why both files:** the compression area is allocated with `mp_alloc` (so it is
tracked in `pool->stats.active_allocations` and the active list, unlike the
emergency reserve which uses plain `malloc`). `mp_check_leaks` decides purely
on `active_allocations == 0`, and it runs (a) in `mp_destroy` at
`src/cmem_event.c:1422` while the area is still alive, and (b) in the unit
test mid-life while the area is alive.  Without an exclusion, both report a
false leak.  The area therefore needs (1) an explicit free in `mp_destroy`
before arena teardown, and (2) a counter adjustment in `mp_check_leaks`.

- [ ] **Step 1: Free compression area + table in mp_destroy**

Insert after the child-arena destroy loop (after line ~1430), before the
`if (!(pool->flags & MP_FLAG_STATIC_BUFFER))` arena-teardown block at
line ~1432 — the area is a normal tracked allocation and must be released
while the TLSF/slab arenas backing it are still alive:

```c
    /* Release compressed-storage state (blocks are owned by the pool). */
    if (pool->compressed_area != NULL) {
        mp_free(pool, pool->compressed_area);
        pool->compressed_area = NULL;
        pool->compressed_area_size = 0;
    }
    free(pool->compressed_entries);
    pool->compressed_entries = NULL;
    pool->compressed_capacity = 0;
    pool->compressed_used = 0;
```

- [ ] **Step 2: Exclude compression area from the leak verdict**

In `mp_check_leaks` (`src/cmem_diag.c:841`), the leak verdict is
`bool clean = (pool->stats.active_allocations == 0);`.  The compression area
is pool-owned infrastructure (one tracked `mp_alloc`), so it must not count.
Change that line to exclude it:

```c
    /* The compression area is pool-owned infrastructure (like the emergency
     * reserve); exclude it from the leak verdict. */
    size_t expected_internal = (pool->compressed_area != NULL) ? 1u : 0u;
    bool clean = (pool->stats.active_allocations == expected_internal);
```

(When `compressed_area == NULL` this reduces to the original check, so pools
that never compress are unaffected.  The area is freed in `mp_destroy` Step 1
before teardown, so no other counter path changes.)

- [ ] **Step 3: Verify + commit**

Run: `clang-format -i src/cmem_event.c src/cmem_diag.c && cd build_cmake && ninja CMakeFiles/cmem.dir/src/cmem_event.c.o CMakeFiles/cmem.dir/src/cmem_diag.c.o 2>&1 | grep -E 'warning:|error:' | grep -v note:`
Expected: empty.
```bash
git add src/cmem_event.c src/cmem_diag.c
git commit -m "fix(compress): integrate compression state with pool teardown and leak checks"
```

## Phase 6: Build Integration

### Task 6.1: Add src/cmem_compress.c to CMake source lists

**Files:**
- Modify: `CMakeLists.txt` (line 51-56 source list; line 123 add_library)

- [ ] **Step 1: Append to both CMake source lists**

Line 51-56: append `src/cmem_compress.c` to the explicit source list.
Line 123: change `add_library(cmem STATIC src/cmem.c src/cmem_slab.c src/cmem_tlsf.c src/cmem_sys.c src/cmem_diag.c src/cmem_event.c)` to include `src/cmem_compress.c`.

Note: Makefile needs NO change (`SRC = $(wildcard src/*.c)` picks it up automatically).

- [ ] **Step 2: Reconfigure + verify build**

Run: `cd build_cmake && cmake .. >/dev/null && ninja 2>&1 | grep -E 'warning:|error:' | grep -v note:`
Expected: empty (or a fresh full build with zero warnings).

- [ ] **Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "build(cmake): add cmem_compress.c to library sources"
```

## Phase 7: Tests

### Task 7.1: Add test_compressed_storage to tests/test_main.c

**Files:**
- Modify: `tests/test_main.c` (after `test_numa_auto_optimization` ~line 497; register in main() after its call)

- [ ] **Step 1: Write the test function**

```c
/**
 * @brief Tests compressed storage: round-trip, ownership, eviction, stats.
 */
void test_compressed_storage()
{
    printf("\n--- Test 29: Compressed Storage ---\n");

    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);
    assert(mp_set_compressed_budget(pool, 256 * 1024) == true);

    /* Round-trip with repetitive data. */
    const size_t data_size = 4096;
    char *data = (char *)mp_alloc(pool, data_size);
    assert(data != NULL);
    for (size_t i = 0; i < data_size; i++) {
        data[i] = (char)('a' + (i % 3));
    }
    compressed_handle_t h = mp_compress_block(pool, data, data_size);
    assert(h != 0);

    char *out = (char *)mp_decompress_block(pool, h);
    assert(out != NULL);
    assert(memcmp(out, "aaa", 3) == 0);
    for (size_t i = 0; i < data_size; i++) {
        assert(out[i] == (char)('a' + (i % 3)));
    }
    mp_free(pool, out);

    /* Repeatable decompress. */
    out = (char *)mp_decompress_block(pool, h);
    assert(out != NULL);
    mp_free(pool, out);

    /* Stats consistent. */
    size_t used = 0, budget = 0, count = 0;
    assert(mp_get_compressed_stats(pool, &used, &budget, &count) == true);
    assert(used > 0 && used < data_size);
    assert(budget == 256 * 1024);
    assert(count == 1);

    /* Free handle: second free fails, decompress returns NULL. */
    assert(mp_free_compressed(pool, h) == true);
    assert(mp_free_compressed(pool, h) == false);
    assert(mp_decompress_block(pool, h) == NULL);

    /* No-gain: incompressible data keeps the original buffer. */
    char *rand_buf = (char *)mp_alloc(pool, 1024);
    assert(rand_buf != NULL);
    for (size_t i = 0; i < 1024; i++) {
        rand_buf[i] = (char)((i * 37 + 11) % 251);
    }
    char *rand_copy = (char *)malloc(1024);
    assert(rand_copy != NULL);
    memcpy(rand_copy, rand_buf, 1024);
    compressed_handle_t h2 = mp_compress_block(pool, rand_buf, 1024);
    assert(h2 == 0);            /* no gain — handle invalid */
    assert(memcmp(rand_buf, rand_copy, 1024) == 0); /* original untouched */
    free(rand_copy);
    mp_free(pool, rand_buf);

    /* Budget eviction: with budget 8KB all four ~19B blocks fit, then a
     * budget smaller than a single block evicts everything and rejects
     * the new block (deterministic; exercises the eviction loop guard). */
    assert(mp_set_compressed_budget(pool, 8 * 1024) == true);
    compressed_handle_t handles[4];
    for (int i = 0; i < 4; i++) {
        char *blk = (char *)mp_alloc(pool, 4096);
        assert(blk != NULL);
        for (size_t j = 0; j < 4096; j++) {
            blk[j] = (char)('x' + (i % 3));
        }
        handles[i] = mp_compress_block(pool, blk, 4096);
        assert(handles[i] != 0);
    }
    size_t c2 = 0;
    assert(mp_get_compressed_stats(pool, NULL, NULL, &c2) == true);
    assert(c2 == 4); /* all four fit under the 8KB budget */

    /* Shrink the budget below a single block's size: the next compress
     * evicts every stored block, then returns 0 (nothing left to evict)
     * and the caller's buffer is retained untouched. */
    assert(mp_set_compressed_budget(pool, 8) == true);
    char *blk5 = (char *)mp_alloc(pool, 4096);
    assert(blk5 != NULL);
    memset(blk5, 'q', 4096);
    compressed_handle_t h5 = mp_compress_block(pool, blk5, 4096);
    assert(h5 == 0); /* rejected — budget smaller than one block */
    for (size_t j = 0; j < 4096; j++) {
        assert(blk5[j] == 'q'); /* original buffer retained */
    }
    mp_free(pool, blk5);

    size_t c3 = 0;
    assert(mp_get_compressed_stats(pool, NULL, NULL, &c3) == true);
    assert(c3 == 0); /* all previous blocks were evicted */
    for (int i = 0; i < 4; i++) {
        assert(mp_decompress_block(pool, handles[i]) == NULL); /* stale */
    }

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_compressed_storage");
}
```

- [ ] **Step 2: Register the test in main()**

Add `test_compressed_storage();` after the `test_numa_auto_optimization();` call in `main()`.

- [ ] **Step 3: Verify**

Run:
```bash
clang-format -i tests/test_main.c
cd build_cmake && ninja unit_tests 2>&1 | grep -E 'warning:|error:' | grep -v note:
./unit_tests
```
Expected: no warnings; `./unit_tests` prints "ALL CMEM UNIT TESTS PASSED SUCCESSFULLY!" and Test 29 line passes.

- [ ] **Step 4: Commit**

```bash
git add tests/test_main.c
git commit -m "test(compress): add compressed-storage test suite"
```

## Phase 8: Documentation

### Task 8.1: Document compressed storage

**Files:**
- Modify: `docs/en/performance.md` (after §7.4 Auto-NUMA)
- Modify: `docs/zh/performance.md`
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Add §8.5 (or new §11) Compressed Storage to docs/en/performance.md + zh**

Add a section documenting: `mp_compress_block`/`mp_decompress_block`/`mp_free_compressed`, ownership transfer semantics, `mp_set_compressed_budget` + oldest-first eviction, `mp_get_compressed_stats`, the built-in LZ4 codec, and the "no-gain keeps original" rule. Provide a short usage example. Mirror in docs/zh/performance.md (Chinese).

- [ ] **Step 2: Update CHANGELOG.md**

In `## [Unreleased]` → `### Added`:
```markdown
- In-pool compressed storage: `mp_compress_block` / `mp_decompress_block` /
  `mp_free_compressed` with a built-in LZ4 codec, configurable budget with
  oldest-first eviction, and `mp_get_compressed_stats` reporting
```

- [ ] **Step 3: Commit**

```bash
git add docs/en/performance.md docs/zh/performance.md CHANGELOG.md
git commit -m "docs(compress): document compressed storage"
```

## Phase 9: Final Verification

### Task 9.1: Full clean rebuild + full test suite

- [ ] **Step 1: Full clean rebuild**

Run: `cd build_cmake && ninja -t clean && ninja 2>&1 | tee /tmp/final_build.log; grep -cE 'warning:|error:' /tmp/final_build.log`
Expected: `0`.

- [ ] **Step 2: Full test suite**

Run: `cd build_cmake && ctest` (3/3 passed), then `./unit_tests`, `./advanced_tests`, `./cpp_tests` (each exit 0), then stress short build:
```bash
gcc -Wall -Wextra -O2 -std=gnu11 -D_GNU_SOURCE -DSTRESS_DURATION_SEC=15 -I./include tests/stress_test.c src/*.c -pthread -o /tmp/stress_verify && /tmp/stress_verify
```
Expected: "Long-run stress test completed successfully." exit 0.

- [ ] **Step 3: Format check + clean tree**

Run: `make format-check` (exit 0), then `git status --porcelain` (only untracked plan/spec docs expected, if any).

- [ ] **Step 4: Final history check**

Run: `git log --oneline -12`
Expected: the 8 feature commits (`feat(compress): add self-contained LZ4 block codec`, `feat(compress): declare compressed-storage public API`, `chore(compress): add internal compressed-storage structures`, `feat(compress): implement compression area, handle table, and public APIs`, `fix(compress): integrate compression state with pool teardown and leak checks`, `build(cmake): add cmem_compress.c to library sources`, `test(compress): add compressed-storage test suite`, `docs(compress): document compressed storage`) on top of 9b6549e (spec).
