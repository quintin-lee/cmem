/**
 * @file cmem_tlsf.c
 * @brief Two-Level Segregated Fit (TLSF) allocator implementation.
 *
 * TLSF is an O(1) dynamic allocation scheme used here to serve the
 * mid-size tier (a few hundred bytes up to 4 MB). Free blocks are kept
 * in a two-level bitmap index:
 *
 *   - First level  (fl): the floor(log2(size)) of the block, giving the
 *     size class within a power-of-two range.
 *   - Second level (sl): a sub-division of each first-level class into
 *     TLSF_SL_COUNT equal segments, so a request lands on a small,
 *     bounded free list.
 *
 * `fl_bitmap` has a bit per first-level bin; each `sl_bitmap[fl]` holds the
 * per-second-level occupancy for that bin. `blocks[fl][sl]` heads a doubly
 * linked free list.  The scheme keeps allocation and free in worst-case O(1)
 * while keeping fragmentation bounded like a good-fit strategy.
 *
 * Each mid-size allocation is preceded by a `tlsf_block_t` header (managed
 * here) followed by the cmem `mp_block_header_t` (managed in cmem.c); the
 * returned payload points after both. Blocks are split on allocation and
 * coalesced with their physical neighbours on free.
 */

#include "cmem.h"
#include "cmem_internal.h"
#include <string.h>

/* TLSF block sizes are kept at 8-byte granularity (low 3 bits of a size are
 * always zero), so a size can be rounded up by masking with this value. */
#define TLSF_ALIGN_MASK 7u
/* Index of the highest bit of the 32-bit first-level (fl) bitmap. */
#define TLSF_HIGH_BIT_INDEX 31

/**
 * @brief Find the index of the highest set bit (floor of log2).
 *
 * Built on __builtin_clz so it compiles to a single amd64 `lzcnt`/`bsr`
 * instruction. This maps a size to its TLSF first-level bucket.
 *
 * @param val Value to analyse (nonzero).
 * @return Position of the highest set bit (0-based), or -1 if val == 0.
 */
static inline int tlsf_fls(size_t val)
{
    if (val == 0) {
        return -1;
    }
    return TLSF_HIGH_BIT_INDEX - __builtin_clz((uint32_t)val);
}

/**
 * @brief Find the index of the lowest set bit (trailing zeros).
 *
 * Built on __builtin_ctz; used to locate the first occupied second-level
 * bucket in a bitmap.
 *
 * @param val Bitmap to scan (nonzero).
 * @return Position of the lowest set bit (0-based), or -1 if val == 0.
 */
static inline int tlsf_ffs(uint32_t val)
{
    if (val == 0) {
        return -1;
    }
    return __builtin_ctz(val);
}

/**
 * @brief Compute the first/second-level buckets a request maps onto.
 *
 * For small sizes (< 2^TLSF_SL_SHIFT) the whole range falls into fl=0 with
 * sl equal to the size.  For larger sizes, fl = floor(log2(size)) and
 * sl selects which sub-segment inside that power-of-two class the size lies.
 *
 * @param size Requested block size.
 * @param fl   [out] First-level bucket index.
 * @param sl   [out] Second-level bucket index.
 */
void tlsf_mapping_insert(size_t size, int *fl, int *sl)
{
    if (size < (1 << TLSF_SL_SHIFT)) {
        *fl = 0;
        *sl = (int)size;
    } else {
        *fl = tlsf_fls(size);
        *sl = (int)((size >> (*fl - TLSF_SL_SHIFT)) ^ (1 << TLSF_SL_SHIFT));
    }
}

/**
 * @brief Round a requested size UP to its search bucket (good-fit).
 *
 * Search treats the requested size as the bucket it would land in and
 * rounds a mid-class request up to the top of its second-level segment so
 * tlsf_mapping_insert() pins it to the correct (larger) free list, avoiding
 * a wasted O(1) probe every allocation.
 *
 * @param size Requested block size.
 * @param fl   [out] First-level bucket index.
 * @param sl   [out] Second-level bucket index.
 */
void tlsf_mapping_search(size_t size, int *fl, int *sl)
{
    if (size >= (1 << TLSF_SL_SHIFT)) {
        size_t round = (1 << (*fl = tlsf_fls(size) - TLSF_SL_SHIFT)) - 1;
        size += round;
    }
    tlsf_mapping_insert(size, fl, sl);
}

/**
 * @brief Create a new TLSF arena of `size` bytes.
 *
 * The arena's first 8-aligned `size` bytes come from `custom_mem` if given
 * (used for static-buffer pools), otherwise from sys_mem_alloc(). The arena
 * is seeded with one giant free block spanning all `size` bytes, registered
 * in the bitmap index so future allocations can split it.
 *
 * @param pool       Owning pool (may be NULL).
 * @param size       Arena size in bytes (rounded up to a multiple of 8).
 * @param custom_mem Optional pre-allocated backing buffer (NULL to alloc).
 * @return New tlsf_pool_t, or NULL on allocation failure.
 */
tlsf_pool_t *tlsf_create_pool_custom(memory_pool_t *pool, size_t size, void *custom_mem)
{

    size = (size + TLSF_ALIGN_MASK) & ~(size_t)TLSF_ALIGN_MASK;

    void *raw_mem = custom_mem;
    if (!raw_mem) {
        raw_mem = sys_mem_alloc(pool, sizeof(tlsf_pool_t) + size, 8);
        if (!raw_mem) {
            return NULL;
        }
    }

    tlsf_pool_t *tpool = (tlsf_pool_t *)raw_mem;
    memset(tpool, 0, sizeof(tlsf_pool_t));
    tpool->raw_area = (void *)((uint8_t *)raw_mem + sizeof(tlsf_pool_t));
    tpool->raw_size = size;
    tpool->owner_pool = pool;

    /* Seed the arena with one covering free block. */
    tlsf_block_t *block = (tlsf_block_t *)tpool->raw_area;
    block->size_and_flags = (size - sizeof(tlsf_block_t)) | BLOCK_STATE_FREE;
    block->prev_physical = NULL;
    block->next_free = NULL;
    block->prev_free = NULL;

    /* Sentinel block after the arena acts as the terminator for coalescing. */
    tlsf_block_t *sentinel =
        (tlsf_block_t *)((uint8_t *)block + (block->size_and_flags & BLOCK_SIZE_MASK));
    sentinel->size_and_flags = 0;
    sentinel->prev_physical = block;

    int fl, sl;
    tlsf_mapping_insert(block->size_and_flags & BLOCK_SIZE_MASK, &fl, &sl);
    tpool->fl_bitmap |= (1U << fl);
    tpool->sl_bitmap[fl] |= (1U << sl);
    tpool->blocks[fl][sl] = block;

    return tpool;
}

/**
 * @brief Insert a free block into the correct TLSF free list.
 *
 * Computes the block's (fl, sl) buckets and pushes it onto the head of that
 * list, updating the corresponding bitmap bits so later searches find it.
 *
 * @param tpool TLSF arena owning the list structure.
 * @param block Block to mark free and link.
 */
void tlsf_insert_free_block(tlsf_pool_t *tpool, tlsf_block_t *block)
{
    int fl, sl;
    size_t size = block->size_and_flags & BLOCK_SIZE_MASK;
    tlsf_mapping_insert(size, &fl, &sl);

    block->next_free = tpool->blocks[fl][sl];
    block->prev_free = NULL;
    if (tpool->blocks[fl][sl]) {
        tpool->blocks[fl][sl]->prev_free = block;
    }
    tpool->blocks[fl][sl] = block;

    tpool->fl_bitmap |= (1U << fl);
    tpool->sl_bitmap[fl] |= (1U << sl);
}

/**
 * @brief Remove a block from its TLSF free list.
 *
 * Unlinks the block from the doubly linked free list and clears the bitmap
 * bits when the (fl, sl) list empties.
 *
 * @param tpool TLSF arena owning the list.
 * @param block Free block to unlink.
 */
void tlsf_remove_free_block(tlsf_pool_t *tpool, tlsf_block_t *block)
{
    int fl, sl;
    size_t size = block->size_and_flags & BLOCK_SIZE_MASK;
    tlsf_mapping_insert(size, &fl, &sl);

    if (block->prev_free) {
        block->prev_free->next_free = block->next_free;
    } else {
        tpool->blocks[fl][sl] = block->next_free;
    }

    if (block->next_free) {
        block->next_free->prev_free = block->prev_free;
    }

    if (!tpool->blocks[fl][sl]) {
        tpool->sl_bitmap[fl] &= ~(1U << sl);
        if (!tpool->sl_bitmap[fl]) {
            tpool->fl_bitmap &= ~(1U << fl);
        }
    }
}

/**
 * @brief Find a free block large enough for `total_needed`.
 *
 * Two-level search: first scan the second-level bits of the exact
 * first-level bucket that already covers the rounded request; if empty,
 * scan upward through higher first-level buckets. Returns the first
 * (smallest index) suitable block found - a "good-fit".
 *
 * @param tpool        Arena to search.
 * @param total_needed Required block size (incl. headers, rounded up).
 * @return A free tlsf_block_t capable of serving the request, or NULL.
 */
tlsf_block_t *tlsf_find_suitable_block(tlsf_pool_t *tpool, size_t total_needed)
{
    int fl = 0, sl = 0;
    tlsf_mapping_search(total_needed, &fl, &sl);

    /* 1) within the same first-level class, at or above the requested sl. */
    uint32_t sl_map = tpool->sl_bitmap[fl] & (~0U << sl);
    if (sl_map) {
        sl = tlsf_ffs(sl_map);
        return tpool->blocks[fl][sl];
    }

    /* 2) move up to the next occupied first-level class. */
    uint32_t fl_map = tpool->fl_bitmap & (~0U << (fl + 1));
    if (fl_map) {
        fl = tlsf_ffs(fl_map);
        sl_map = tpool->sl_bitmap[fl];
        sl = tlsf_ffs(sl_map);
        return tpool->blocks[fl][sl];
    }

    return NULL;
}

/**
 * @brief Allocate `req_size` bytes from the pool's TLSF tier.
 *
 * Walks the arena chain for a suitable free block, expanding the pool with
 * a fresh arena (doubling the request up to a 4 MB minimum) when none is
 * available. The winning block is split so the remainder returns to the
 * free list, then an `mp_block_header_t` is stamped right after the TLSF
 * header so cmem.c can track the allocation.  Optional canary byte and
 * zero-on-alloc behaviour are applied here.
 *
 * @param pool     Owning memory pool.
 * @param req_size User-requested payload size.
 * @return Pointer to user payload, or NULL on failure.
 */
void *tlsf_alloc(memory_pool_t *pool, size_t req_size)
{
    size_t total_needed = sizeof(tlsf_block_t) + sizeof(mp_block_header_t) + req_size +
                          ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0);

    total_needed = (total_needed + TLSF_ALIGN_MASK) & ~(size_t)TLSF_ALIGN_MASK;

    if (total_needed < TLSF_MIN_BLOCK_SIZE) {
        total_needed = TLSF_MIN_BLOCK_SIZE;
    }

    tlsf_pool_t *tpool = pool->tlsf_root;
    if (!tpool) {
        size_t init_sz = 4 * 1024 * 1024;
        pool->tlsf_root = tlsf_create_pool_custom(pool, init_sz, NULL);
        if (!pool->tlsf_root) {
            return NULL;
        }
        tpool = pool->tlsf_root;
        pool->stats.total_pool_size += init_sz + sizeof(tlsf_pool_t);
    }

    tlsf_block_t *block = NULL;
    tlsf_pool_t *target_pool = tpool;

    while (target_pool) {
        block = tlsf_find_suitable_block(target_pool, total_needed);
        if (block) {
            break;
        }
        target_pool = target_pool->next;
    }

    if (!block) {
        if (pool->flags & MP_FLAG_STATIC_BUFFER) {
            return NULL;
        }
        size_t expand_sz =
            (total_needed * 2 > 4 * 1024 * 1024) ? total_needed * 2 : 4 * 1024 * 1024;
        tlsf_pool_t *new_p = tlsf_create_pool_custom(pool, expand_sz, NULL);
        if (!new_p) {
            return NULL;
        }
        new_p->next = pool->tlsf_root;
        pool->tlsf_root = new_p;
        target_pool = new_p;
        pool->stats.total_pool_size += expand_sz + sizeof(tlsf_pool_t);

        block = tlsf_find_suitable_block(target_pool, total_needed);
        if (!block) {
            return NULL;
        }
    }

    tpool = target_pool;
    tlsf_remove_free_block(tpool, block);

    size_t current_size = block->size_and_flags & BLOCK_SIZE_MASK;
    size_t remaining = current_size - total_needed;

    /* Split off a fresh free block when the remainder is worth keeping. */
    if (remaining >= TLSF_MIN_BLOCK_SIZE + sizeof(tlsf_block_t)) {
        block->size_and_flags = total_needed | (block->size_and_flags & BLOCK_STATE_PREV_FREE);

        tlsf_block_t *split_block = (tlsf_block_t *)((uint8_t *)block + total_needed);
        split_block->size_and_flags = remaining | BLOCK_STATE_FREE;
        split_block->prev_physical = block;

        tlsf_block_t *next_phys = (tlsf_block_t *)((uint8_t *)split_block + remaining);
        next_phys->prev_physical = split_block;
        next_phys->size_and_flags |= BLOCK_STATE_PREV_FREE;

        tlsf_insert_free_block(tpool, split_block);
    } else {
        block->size_and_flags &= ~BLOCK_STATE_FREE;
        tlsf_block_t *next_phys = (tlsf_block_t *)((uint8_t *)block + current_size);
        next_phys->size_and_flags &= ~BLOCK_STATE_PREV_FREE;
    }

    /* Stamp the cmem metadata header that cmem.c relies on. */
    mp_block_header_t *header = (mp_block_header_t *)((uint8_t *)block + sizeof(tlsf_block_t));
    header->magic = MP_MAGIC_HEAD;
    header->alloc_type = ALLOC_TYPE_TLSF;
    header->slab_class = 0;
    header->flags = 0;
    header->requested_size = req_size;
    header->usable_size = total_needed - sizeof(tlsf_block_t) - sizeof(mp_block_header_t);
    header->raw_base = block;
    header->subpool = tpool;
    header->alloc_file = NULL;
    header->alloc_line = 0;
    header->alloc_func = NULL;
    header->backtrace_depth = 0;

    void *payload = (void *)((uint8_t *)header + sizeof(mp_block_header_t));

    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
        uint8_t *canary = (uint8_t *)payload + req_size;
        *canary = MP_CANARY_BYTE;
    }

    if (pool->flags & MP_FLAG_ZERO_ON_ALLOC) {
        memset(payload, 0, req_size);
    }

    pool->stats.tlsf_allocated_bytes += header->usable_size;
    return payload;
}

/**
 * @brief Free a TLSF allocation, coalescing neighbours.
 *
 * Clears the block's FREE flag, then merges the block with its physically
 * following block and (if flagged) its physically preceding neighbour so
 * adjacent free memory forms larger reusable blocks.  The merged block is
 * re-inserted into the free lists.
 *
 * @param pool   Owning memory pool.
 * @param header The mp_block_header_t that tlsf_alloc stamped.
 */
void tlsf_free(memory_pool_t *pool, mp_block_header_t *header)
{
    tlsf_block_t *block = (tlsf_block_t *)header->raw_base;
    tlsf_pool_t *tpool = (tlsf_pool_t *)header->subpool;

    pool->stats.tlsf_allocated_bytes -= header->usable_size;

    size_t size = block->size_and_flags & BLOCK_SIZE_MASK;
    block->size_and_flags |= BLOCK_STATE_FREE;

    /* Coalesce with the next physical block if it is free. */
    tlsf_block_t *next_phys = (tlsf_block_t *)((uint8_t *)block + size);
    if (next_phys->size_and_flags & BLOCK_STATE_FREE) {
        tlsf_remove_free_block(tpool, next_phys);
        size += (next_phys->size_and_flags & BLOCK_SIZE_MASK);
        block->size_and_flags =
            size | (block->size_and_flags & BLOCK_STATE_PREV_FREE) | BLOCK_STATE_FREE;

        tlsf_block_t *after_next = (tlsf_block_t *)((uint8_t *)block + size);
        after_next->prev_physical = block;
    }

    /* Coalesce with the previous physical block if it is free. */
    if (block->size_and_flags & BLOCK_STATE_PREV_FREE) {
        tlsf_block_t *prev_phys = block->prev_physical;
        if (prev_phys && (prev_phys->size_and_flags & BLOCK_STATE_FREE)) {
            tlsf_remove_free_block(tpool, prev_phys);
            size_t prev_size = prev_phys->size_and_flags & BLOCK_SIZE_MASK;
            prev_phys->size_and_flags = (prev_size + size) |
                                        (prev_phys->size_and_flags & BLOCK_STATE_PREV_FREE) |
                                        BLOCK_STATE_FREE;

            tlsf_block_t *after_block = (tlsf_block_t *)((uint8_t *)prev_phys + prev_size + size);
            after_block->prev_physical = prev_phys;
            block = prev_phys;
        }
    }

    next_phys = (tlsf_block_t *)((uint8_t *)block + (block->size_and_flags & BLOCK_SIZE_MASK));
    next_phys->size_and_flags |= BLOCK_STATE_PREV_FREE;

    tlsf_insert_free_block(tpool, block);
}

/**
 * @brief Try to grow a TLSF allocation in place by absorbing the next free block.
 *
 * Used to serve realloc() without a copy when the physically-next block is
 * free and large enough. The neighbour is removed from the free list and
 * merged into this block; any surplus is split back off as a fresh free block.
 * Only valid for ALLOC_TYPE_TLSF headers.
 *
 * @param pool     Owning memory pool (for canary flag and stats).
 * @param header   The allocation's mp_block_header_t.
 * @param new_size New requested payload size (>= current requested size).
 * @return true if the block was expanded in place, false if it could not.
 */
bool tlsf_try_inplace_expand(memory_pool_t *pool, mp_block_header_t *header, size_t new_size)
{
    if (header->alloc_type != ALLOC_TYPE_TLSF) {
        return false;
    }
    tlsf_block_t *block = (tlsf_block_t *)header->raw_base;
    tlsf_pool_t *tpool = (tlsf_pool_t *)header->subpool;
    if (!block || !tpool) {
        return false;
    }

    size_t current_block_size = block->size_and_flags & BLOCK_SIZE_MASK;

    size_t total_needed = sizeof(tlsf_block_t) + sizeof(mp_block_header_t) + new_size +
                          ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0);
    total_needed = (total_needed + TLSF_ALIGN_MASK) & ~(size_t)TLSF_ALIGN_MASK;

    if (total_needed < TLSF_MIN_BLOCK_SIZE) {
        total_needed = TLSF_MIN_BLOCK_SIZE;
    }

    tlsf_block_t *next_phys = (tlsf_block_t *)((uint8_t *)block + current_block_size);
    if (!(next_phys->size_and_flags & BLOCK_STATE_FREE)) {
        return false;
    }

    size_t next_size = next_phys->size_and_flags & BLOCK_SIZE_MASK;
    if (current_block_size + next_size < total_needed) {
        return false;
    }

    tlsf_remove_free_block(tpool, next_phys);
    size_t combined_size = current_block_size + next_size;
    size_t remaining = combined_size - total_needed;

    /* Re-split the surplus tail into a fresh free block. */
    if (remaining >= TLSF_MIN_BLOCK_SIZE + sizeof(tlsf_block_t)) {
        block->size_and_flags = total_needed | (block->size_and_flags & BLOCK_STATE_PREV_FREE);

        tlsf_block_t *split_block = (tlsf_block_t *)((uint8_t *)block + total_needed);
        split_block->size_and_flags = remaining | BLOCK_STATE_FREE;
        split_block->prev_physical = block;

        tlsf_block_t *far_phys = (tlsf_block_t *)((uint8_t *)split_block + remaining);
        far_phys->prev_physical = split_block;
        far_phys->size_and_flags |= BLOCK_STATE_PREV_FREE;

        tlsf_insert_free_block(tpool, split_block);
    } else {
        block->size_and_flags = combined_size | (block->size_and_flags & BLOCK_STATE_PREV_FREE);
        tlsf_block_t *far_phys = (tlsf_block_t *)((uint8_t *)block + combined_size);
        far_phys->prev_physical = block;
        far_phys->size_and_flags &= ~BLOCK_STATE_PREV_FREE;
    }

    size_t new_usable = (block->size_and_flags & BLOCK_SIZE_MASK) - sizeof(tlsf_block_t) -
                        sizeof(mp_block_header_t);
    pool->stats.tlsf_allocated_bytes += (new_usable - header->usable_size);
    header->requested_size = new_size;
    header->usable_size = new_usable;

    void *payload = (void *)((uint8_t *)header + sizeof(mp_block_header_t));
    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
        uint8_t *canary = (uint8_t *)payload + new_size;
        *canary = MP_CANARY_BYTE;
    }

    return true;
}
