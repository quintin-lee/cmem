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
#define CMEM_LZ4_MAX_DIST 0xFFFF
/* Max value storable in a 4-bit token nibble (extension sentinel). */
#define CMEM_LZ4_TOKEN_NIBBLE 15
/* Mask isolating the match-length nibble of a token. */
#define CMEM_LZ4_NIBBLE_MASK 0x0F
/* Mask isolating one byte of the 16-bit LE offset field. */
#define CMEM_LZ4_OFF_BYTE_MASK 0xFFu
/* Mask isolating the generation half of a compressed handle. */
#define CMEM_LZ4_HANDLE_GEN_MASK 0xFFFFFFFFu

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
        *op++ =
            (uint8_t)((lit_len < CMEM_LZ4_TOKEN_NIBBLE ? lit_len : CMEM_LZ4_TOKEN_NIBBLE) << 4) |
            (uint8_t)(mlen - CMEM_LZ4_MIN_MATCH < CMEM_LZ4_TOKEN_NIBBLE ? mlen - CMEM_LZ4_MIN_MATCH
                                                                        : CMEM_LZ4_TOKEN_NIBBLE);
        if (lit_len >= CMEM_LZ4_TOKEN_NIBBLE) {
            int ext = cmem_lz4_write_len(op, oend, lit_len - CMEM_LZ4_TOKEN_NIBBLE);
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
        op[0] = (uint8_t)(off & CMEM_LZ4_OFF_BYTE_MASK);
        op[1] = (uint8_t)(off >> 8);
        op += 2;
        if (mlen - CMEM_LZ4_MIN_MATCH >= CMEM_LZ4_TOKEN_NIBBLE) {
            int ext =
                cmem_lz4_write_len(op, oend, mlen - CMEM_LZ4_MIN_MATCH - CMEM_LZ4_TOKEN_NIBBLE);
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
        *op++ = (uint8_t)((lit_len < CMEM_LZ4_TOKEN_NIBBLE ? lit_len : CMEM_LZ4_TOKEN_NIBBLE) << 4);
        if (lit_len >= CMEM_LZ4_TOKEN_NIBBLE) {
            int ext = cmem_lz4_write_len(op, oend, lit_len - CMEM_LZ4_TOKEN_NIBBLE);
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
        int lit_len = (token >> 4) & CMEM_LZ4_NIBBLE_MASK;
        if (lit_len == CMEM_LZ4_TOKEN_NIBBLE) {
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
        int match_len = (token & CMEM_LZ4_NIBBLE_MASK);
        if (match_len == CMEM_LZ4_TOKEN_NIBBLE) {
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
    pool->compressed_entries = (cmem_compressed_entry_t *)calloc(CMEM_COMPRESS_MAX_ENTRIES,
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
    int32_t oldest_idx = -1;
    for (uint32_t i = 0; i < pool->compressed_capacity; i++) {
        cmem_compressed_entry_t *entry = &pool->compressed_entries[i];
        if (entry->used && entry->alloc_seq < oldest) {
            oldest = entry->alloc_seq;
            oldest_idx = (int32_t)i;
        }
    }
    if (oldest_idx < 0) {
        return;
    }
    cmem_compressed_entry_t *entry = &pool->compressed_entries[oldest_idx];
    entry->used = false;
    entry->generation++; /* invalidate stale handles */
    pool->compressed_used -= entry->comp_size;
}

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
            cmem_compressed_entry_t *entry = &pool->compressed_entries[i];
            if (entry->used && entry->comp_offset < off + aligned &&
                entry->comp_offset + entry->comp_size > off) {
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

    cmem_compressed_entry_t *entry = &pool->compressed_entries[slot];
    entry->original_size = size;
    entry->comp_offset = off;
    entry->comp_size = (size_t)clen;
    entry->generation++;
    entry->alloc_seq = pool->compressed_seq++;
    entry->used = true;
    pool->compressed_used += (size_t)clen;

    /* Ownership transfer: release the caller's buffer. */
    mp_free(pool, data);

    return (compressed_handle_t)((((uint64_t)slot) << 32) | entry->generation);
}

void *mp_decompress_block(memory_pool_t *pool, compressed_handle_t handle)
{
    if (pool == NULL || handle == 0) {
        return NULL;
    }
    uint32_t slot = (uint32_t)(handle >> 32);
    uint32_t gen = (uint32_t)(handle & CMEM_LZ4_HANDLE_GEN_MASK);
    if (slot >= pool->compressed_capacity) {
        return NULL;
    }
    cmem_compressed_entry_t *entry = &pool->compressed_entries[slot];
    if (!entry->used || entry->generation != gen) {
        return NULL; /* stale or evicted */
    }
    void *out = mp_alloc(pool, entry->original_size);
    if (out == NULL) {
        return NULL;
    }
    int dlen = cmem_lz4_decompress((const uint8_t *)pool->compressed_area + entry->comp_offset,
                                   (uint8_t *)out,
                                   (int)entry->comp_size,
                                   (int)entry->original_size);
    if (dlen != (int)entry->original_size) {
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
    uint32_t gen = (uint32_t)(handle & CMEM_LZ4_HANDLE_GEN_MASK);
    if (slot >= pool->compressed_capacity) {
        return false;
    }
    cmem_compressed_entry_t *entry = &pool->compressed_entries[slot];
    if (!entry->used || entry->generation != gen) {
        return false;
    }
    entry->used = false;
    entry->generation++;
    pool->compressed_used -= entry->comp_size;
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

bool mp_get_compressed_stats(memory_pool_t *pool, size_t *used, size_t *budget, size_t *block_count)
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
