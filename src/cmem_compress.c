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
