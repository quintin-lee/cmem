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
static inline void tlsf_mapping_insert(size_t size, int *fl, int *sl)
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
static inline void tlsf_mapping_search(size_t size, int *fl, int *sl)
{
    if (size >= (1 << TLSF_SL_SHIFT)) {
        int fl_idx = tlsf_fls(size);
        size_t round = (1u << (fl_idx - TLSF_SL_SHIFT)) - 1;
        size += round;
        *fl = fl_idx;
    } else {
        *fl = 0;
    }
    /* Compute sl from the (possibly rounded) size without re-computing fl. */
    if (*fl == 0) {
        *sl = (int)size;
    } else {
        *sl = (int)((size >> (*fl - TLSF_SL_SHIFT)) ^ (1 << TLSF_SL_SHIFT));
    }
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
/**
 * @brief Acquire locks for two buckets in sorted order to prevent deadlocks.
 *
 * @param tpool  TLSF arena.
 * @param fl1    First first-level bucket index.
 * @param sl1    First second-level bucket index.
 * @param fl2    Second first-level bucket index.
 * @param sl2    Second second-level bucket index.
 */
static inline void tlsf_lock_buckets(tlsf_pool_t *tpool, int fl1, int sl1, int fl2, int sl2)
{
    if (fl1 < fl2 || (fl1 == fl2 && sl1 < sl2)) {
        pthread_mutex_lock(&tpool->bucket_locks[fl1][sl1]);
        pthread_mutex_lock(&tpool->bucket_locks[fl2][sl2]);
    } else {
        pthread_mutex_lock(&tpool->bucket_locks[fl2][sl2]);
        pthread_mutex_lock(&tpool->bucket_locks[fl1][sl1]);
    }
}

static inline void tlsf_unlock_buckets(tlsf_pool_t *tpool, int fl1, int sl1, int fl2, int sl2)
{
    if (fl1 < fl2 || (fl1 == fl2 && sl1 < sl2)) {
        pthread_mutex_unlock(&tpool->bucket_locks[fl2][sl2]);
        pthread_mutex_unlock(&tpool->bucket_locks[fl1][sl1]);
    } else {
        pthread_mutex_unlock(&tpool->bucket_locks[fl1][sl1]);
        pthread_mutex_unlock(&tpool->bucket_locks[fl2][sl2]);
    }
}

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
    pthread_mutex_init(&tpool->lock, NULL);
    for (int fl = 0; fl < TLSF_FL_MAX; fl++) {
        for (int sl = 0; sl < TLSF_SL_COUNT; sl++) {
            pthread_mutex_init(&tpool->bucket_locks[fl][sl], NULL);
        }
    }
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

    int fl = 0, sl = 0;
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
    int fl = 0, sl = 0;
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
    int fl = 0, sl = 0;
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

    /* Create the initial pool if needed (under pool_lock). */
    if (!tpool) {
        size_t init_sz = 4 * 1024 * 1024;
        pool_lock(pool);
        pool->tlsf_root = tlsf_create_pool_custom(pool, init_sz, NULL);
        if (!pool->tlsf_root) {
            pool_unlock(pool);
            return NULL;
        }
        tpool = pool->tlsf_root;
        CMEM_ATOMIC_FETCH_ADD(
            &pool->total_pool_size, init_sz + sizeof(tlsf_pool_t), CMEM_ORDER_RELAXED);
        pool_unlock(pool);
    }

    tlsf_block_t *block = NULL;
    tlsf_pool_t *target_pool = tpool;

    /* Walk the pool chain, trying per-bucket locks for fine-grained concurrency.
     * tlsf_find_suitable_block is lock-free (reads only bitmaps).
     * We acquire bucket locks only when modifying free lists. */
    while (target_pool) {
        block = tlsf_find_suitable_block(target_pool, total_needed);
        if (block) {
            /* Inline tlsf_remove_free_block using already-known bucket from search. */
            size_t block_size = block->size_and_flags & BLOCK_SIZE_MASK;
            int r_fl = 0, r_sl = 0;
            tlsf_mapping_insert(block_size, &r_fl, &r_sl);

            pthread_mutex_lock(&target_pool->bucket_locks[r_fl][r_sl]);
            if (block->prev_free) {
                block->prev_free->next_free = block->next_free;
            } else {
                target_pool->blocks[r_fl][r_sl] = block->next_free;
            }
            if (block->next_free) {
                block->next_free->prev_free = block->prev_free;
            }
            if (!target_pool->blocks[r_fl][r_sl]) {
                target_pool->sl_bitmap[r_fl] &= ~(1U << r_sl);
                if (!target_pool->sl_bitmap[r_fl]) {
                    target_pool->fl_bitmap &= ~(1U << r_fl);
                }
            }
            pthread_mutex_unlock(&target_pool->bucket_locks[r_fl][r_sl]);

            size_t current_size = block->size_and_flags & BLOCK_SIZE_MASK;
            size_t remaining = current_size - total_needed;
            if (remaining >= TLSF_MIN_BLOCK_SIZE + sizeof(tlsf_block_t)) {
                block->size_and_flags =
                    total_needed | (block->size_and_flags & BLOCK_STATE_PREV_FREE);
                tlsf_block_t *split_block = (tlsf_block_t *)((uint8_t *)block + total_needed);
                split_block->size_and_flags = remaining | BLOCK_STATE_FREE;
                split_block->prev_physical = block;
                tlsf_block_t *next_phys = (tlsf_block_t *)((uint8_t *)split_block + remaining);
                next_phys->prev_physical = split_block;
                next_phys->size_and_flags |= BLOCK_STATE_PREV_FREE;
                /* Inline tlsf_insert_free_block for the split remainder. */
                int s_fl = 0, s_sl = 0;
                tlsf_mapping_insert(remaining, &s_fl, &s_sl);
                /* Lock both buckets in order if they differ. */
                if (s_fl == r_fl && s_sl == r_sl) {
                    pthread_mutex_lock(&target_pool->bucket_locks[s_fl][s_sl]);
                } else {
                    tlsf_lock_buckets(target_pool, r_fl, r_sl, s_fl, s_sl);
                }
                split_block->next_free = target_pool->blocks[s_fl][s_sl];
                split_block->prev_free = NULL;
                if (target_pool->blocks[s_fl][s_sl]) {
                    target_pool->blocks[s_fl][s_sl]->prev_free = split_block;
                }
                target_pool->blocks[s_fl][s_sl] = split_block;
                target_pool->fl_bitmap |= (1U << s_fl);
                target_pool->sl_bitmap[s_fl] |= (1U << s_sl);
                if (s_fl == r_fl && s_sl == r_sl) {
                    pthread_mutex_unlock(&target_pool->bucket_locks[s_fl][s_sl]);
                } else {
                    tlsf_unlock_buckets(target_pool, r_fl, r_sl, s_fl, s_sl);
                }
            } else {
                block->size_and_flags &= ~BLOCK_STATE_FREE;
                tlsf_block_t *next_phys = (tlsf_block_t *)((uint8_t *)block + current_size);
                next_phys->size_and_flags &= ~BLOCK_STATE_PREV_FREE;
            }
            tpool = target_pool;
            break;
        }
        target_pool = target_pool->next;
    }

    /* Expand if no suitable block was found. */
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
        pool_lock(pool);
        new_p->next = pool->tlsf_root;
        pool->tlsf_root = new_p;
        CMEM_ATOMIC_FETCH_ADD(
            &pool->total_pool_size, expand_sz + sizeof(tlsf_pool_t), CMEM_ORDER_RELAXED);
        pool_unlock(pool);

        block = tlsf_find_suitable_block(new_p, total_needed);
        if (!block) {
            return NULL;
        }
        /* Inline removal using computed bucket. */
        size_t block_size = block->size_and_flags & BLOCK_SIZE_MASK;
        int r_fl = 0, r_sl = 0;
        tlsf_mapping_insert(block_size, &r_fl, &r_sl);
        pthread_mutex_lock(&new_p->bucket_locks[r_fl][r_sl]);
        if (block->prev_free) {
            block->prev_free->next_free = block->next_free;
        } else {
            new_p->blocks[r_fl][r_sl] = block->next_free;
        }
        if (block->next_free) {
            block->next_free->prev_free = block->prev_free;
        }
        if (!new_p->blocks[r_fl][r_sl]) {
            new_p->sl_bitmap[r_fl] &= ~(1U << r_sl);
            if (!new_p->sl_bitmap[r_fl]) {
                new_p->fl_bitmap &= ~(1U << r_fl);
            }
        }
        pthread_mutex_unlock(&new_p->bucket_locks[r_fl][r_sl]);

        size_t current_size = block->size_and_flags & BLOCK_SIZE_MASK;
        size_t remaining = current_size - total_needed;
        if (remaining >= TLSF_MIN_BLOCK_SIZE + sizeof(tlsf_block_t)) {
            block->size_and_flags = total_needed | (block->size_and_flags & BLOCK_STATE_PREV_FREE);
            tlsf_block_t *split_block = (tlsf_block_t *)((uint8_t *)block + total_needed);
            split_block->size_and_flags = remaining | BLOCK_STATE_FREE;
            split_block->prev_physical = block;
            tlsf_block_t *next_phys = (tlsf_block_t *)((uint8_t *)split_block + remaining);
            next_phys->prev_physical = split_block;
            next_phys->size_and_flags |= BLOCK_STATE_PREV_FREE;
            /* Inline insertion for split remainder. */
            int s_fl = 0, s_sl = 0;
            tlsf_mapping_insert(remaining, &s_fl, &s_sl);
            if (s_fl == r_fl && s_sl == r_sl) {
                pthread_mutex_lock(&new_p->bucket_locks[s_fl][s_sl]);
            } else {
                tlsf_lock_buckets(new_p, r_fl, r_sl, s_fl, s_sl);
            }
            split_block->next_free = new_p->blocks[s_fl][s_sl];
            split_block->prev_free = NULL;
            if (new_p->blocks[s_fl][s_sl]) {
                new_p->blocks[s_fl][s_sl]->prev_free = split_block;
            }
            new_p->blocks[s_fl][s_sl] = split_block;
            new_p->fl_bitmap |= (1U << s_fl);
            new_p->sl_bitmap[s_fl] |= (1U << s_sl);
            if (s_fl == r_fl && s_sl == r_sl) {
                pthread_mutex_unlock(&new_p->bucket_locks[s_fl][s_sl]);
            } else {
                tlsf_unlock_buckets(new_p, r_fl, r_sl, s_fl, s_sl);
            }
        } else {
            block->size_and_flags &= ~BLOCK_STATE_FREE;
            tlsf_block_t *next_phys = (tlsf_block_t *)((uint8_t *)block + current_size);
            next_phys->size_and_flags &= ~BLOCK_STATE_PREV_FREE;
        }
        tpool = new_p;
    }

    /* Stamp the cmem metadata header. */
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

    CMEM_ATOMIC_FETCH_ADD(&pool->tlsf_allocated_bytes, header->usable_size, CMEM_ORDER_RELAXED);
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

    /* Update stats atomically outside the lock to reduce hold time. */
    CMEM_ATOMIC_FETCH_SUB(&pool->tlsf_allocated_bytes, header->usable_size, CMEM_ORDER_RELAXED);

    size_t size = block->size_and_flags & BLOCK_SIZE_MASK;
    block->size_and_flags |= BLOCK_STATE_FREE;

    /* Collect buckets that need locking, then acquire in sorted order. */
    int lock_fl[4], lock_sl[4];
    int lock_count = 0;

#define TLSF_ADD_LOCK(fl, sl)                                                                      \
    do {                                                                                           \
        int _fl = (fl), _sl = (sl);                                                                \
        int _dup = 0;                                                                              \
        for (int _i = 0; _i < lock_count; _i++) {                                                  \
            if (lock_fl[_i] == _fl && lock_sl[_i] == _sl) {                                        \
                _dup = 1;                                                                          \
                break;                                                                             \
            }                                                                                      \
        }                                                                                          \
        if (!_dup && lock_count < 4) {                                                             \
            lock_fl[lock_count] = _fl;                                                             \
            lock_sl[lock_count] = _sl;                                                             \
            lock_count++;                                                                          \
        }                                                                                          \
    } while (0)

#define TLSF_LOCK_BUCKET()                                                                         \
    do {                                                                                           \
        for (int _i = 0; _i < lock_count; _i++) {                                                  \
            for (int _j = _i + 1; _j < lock_count; _j++) {                                         \
                if (lock_fl[_j] < lock_fl[_i] ||                                                   \
                    (lock_fl[_j] == lock_fl[_i] && lock_sl[_j] < lock_sl[_i])) {                   \
                    int _tf = lock_fl[_i];                                                         \
                    int _ts = lock_sl[_i];                                                         \
                    lock_fl[_i] = lock_fl[_j];                                                     \
                    lock_sl[_i] = lock_sl[_j];                                                     \
                    lock_fl[_j] = _tf;                                                             \
                    lock_sl[_j] = _ts;                                                             \
                }                                                                                  \
            }                                                                                      \
        }                                                                                          \
        for (int _i = 0; _i < lock_count; _i++) {                                                  \
            pthread_mutex_lock(&tpool->bucket_locks[lock_fl[_i]][lock_sl[_i]]);                    \
        }                                                                                          \
    } while (0)

#define TLSF_UNLOCK_BUCKET()                                                                       \
    do {                                                                                           \
        for (int _i = lock_count - 1; _i >= 0; _i--) {                                             \
            pthread_mutex_unlock(&tpool->bucket_locks[lock_fl[_i]][lock_sl[_i]]);                  \
        }                                                                                          \
    } while (0)

    /* Coalesce with the next physical block if it is free. */
    /* Collect buckets for coalescing (next + prev) before acquiring any lock. */
    tlsf_block_t *next_phys = (tlsf_block_t *)((uint8_t *)block + size);
    if (next_phys->size_and_flags & BLOCK_STATE_FREE) {
        size_t next_size = next_phys->size_and_flags & BLOCK_SIZE_MASK;
        int n_fl = 0, n_sl = 0;
        tlsf_mapping_insert(next_size, &n_fl, &n_sl);
        TLSF_ADD_LOCK(n_fl, n_sl);
    }

    /* Coalesce with the previous physical block if it is free. */
    if (block->size_and_flags & BLOCK_STATE_PREV_FREE) {
        tlsf_block_t *prev_phys = block->prev_physical;
        if (prev_phys && (prev_phys->size_and_flags & BLOCK_STATE_FREE)) {
            size_t prev_size = prev_phys->size_and_flags & BLOCK_SIZE_MASK;
            int p_fl = 0, p_sl = 0;
            tlsf_mapping_insert(prev_size, &p_fl, &p_sl);
            TLSF_ADD_LOCK(p_fl, p_sl);
        }
    }

    /* Acquire all collected locks in sorted order (prevents deadlocks). */
    TLSF_LOCK_BUCKET();

    /* Perform coalescing and list modifications under lock. */
    size = block->size_and_flags & BLOCK_SIZE_MASK;

    /* Coalesce with next. */
    next_phys = (tlsf_block_t *)((uint8_t *)block + size);
    if (next_phys->size_and_flags & BLOCK_STATE_FREE) {
        size_t next_size = next_phys->size_and_flags & BLOCK_SIZE_MASK;
        int n_fl = 0, n_sl = 0;
        tlsf_mapping_insert(next_size, &n_fl, &n_sl);
        if (next_phys->prev_free) {
            next_phys->prev_free->next_free = next_phys->next_free;
        } else {
            tpool->blocks[n_fl][n_sl] = next_phys->next_free;
        }
        if (next_phys->next_free) {
            next_phys->next_free->prev_free = next_phys->prev_free;
        }
        if (!tpool->blocks[n_fl][n_sl]) {
            tpool->sl_bitmap[n_fl] &= ~(1U << n_sl);
            if (!tpool->sl_bitmap[n_fl]) {
                tpool->fl_bitmap &= ~(1U << n_fl);
            }
        }
        size += next_size;
        block->size_and_flags =
            size | (block->size_and_flags & BLOCK_STATE_PREV_FREE) | BLOCK_STATE_FREE;
        tlsf_block_t *after_next = (tlsf_block_t *)((uint8_t *)block + size);
        after_next->prev_physical = block;
    }

    /* Coalesce with prev (if flagged). */
    if (block->size_and_flags & BLOCK_STATE_PREV_FREE) {
        tlsf_block_t *prev_phys = block->prev_physical;
        if (prev_phys && (prev_phys->size_and_flags & BLOCK_STATE_FREE)) {
            size_t prev_size = prev_phys->size_and_flags & BLOCK_SIZE_MASK;
            int p_fl = 0, p_sl = 0;
            tlsf_mapping_insert(prev_size, &p_fl, &p_sl);
            if (prev_phys->prev_free) {
                prev_phys->prev_free->next_free = prev_phys->next_free;
            } else {
                tpool->blocks[p_fl][p_sl] = prev_phys->next_free;
            }
            if (prev_phys->next_free) {
                prev_phys->next_free->prev_free = prev_phys->prev_free;
            }
            if (!tpool->blocks[p_fl][p_sl]) {
                tpool->sl_bitmap[p_fl] &= ~(1U << p_sl);
                if (!tpool->sl_bitmap[p_fl]) {
                    tpool->fl_bitmap &= ~(1U << p_fl);
                }
            }
            prev_phys->size_and_flags = (prev_size + size) |
                                        (prev_phys->size_and_flags & BLOCK_STATE_PREV_FREE) |
                                        BLOCK_STATE_FREE;
            tlsf_block_t *after_block = (tlsf_block_t *)((uint8_t *)prev_phys + prev_size + size);
            after_block->prev_physical = prev_phys;
            block = prev_phys;
            size = prev_size + size;
        }
    }

    /* Compute the insert bucket for the (possibly merged) block. */
    int fl = 0, sl = 0;
    tlsf_mapping_insert(size, &fl, &sl);
    /* Lock the insert bucket if it wasn't already collected. */
    {
        int _ins_dup = 0;
        for (int _i = 0; _i < lock_count; _i++) {
            if (lock_fl[_i] == fl && lock_sl[_i] == sl) {
                _ins_dup = 1;
                break;
            }
        }
        if (!_ins_dup && lock_count < 4) {
            lock_fl[lock_count] = fl;
            lock_sl[lock_count] = sl;
            lock_count++;
            /* Re-sort to maintain lock ordering. */
            for (int _i = 0; _i < lock_count; _i++) {
                for (int _j = _i + 1; _j < lock_count; _j++) {
                    if (lock_fl[_j] < lock_fl[_i] ||
                        (lock_fl[_j] == lock_fl[_i] && lock_sl[_j] < lock_sl[_i])) {
                        int _tf = lock_fl[_i];
                        int _ts = lock_sl[_i];
                        lock_fl[_i] = lock_fl[_j];
                        lock_sl[_i] = lock_sl[_j];
                        lock_fl[_j] = _tf;
                        lock_sl[_j] = _ts;
                    }
                }
            }
            pthread_mutex_lock(&tpool->bucket_locks[fl][sl]);
        }
    }

    next_phys = (tlsf_block_t *)((uint8_t *)block + size);
    next_phys->size_and_flags |= BLOCK_STATE_PREV_FREE;

    /* Insert merged block into its bucket. */
    block->next_free = tpool->blocks[fl][sl];
    block->prev_free = NULL;
    if (tpool->blocks[fl][sl]) {
        tpool->blocks[fl][sl]->prev_free = block;
    }
    tpool->blocks[fl][sl] = block;
    tpool->fl_bitmap |= (1U << fl);
    tpool->sl_bitmap[fl] |= (1U << sl);

    TLSF_UNLOCK_BUCKET();

#undef TLSF_ADD_LOCK
#undef TLSF_LOCK_BUCKET
#undef TLSF_UNLOCK_BUCKET
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
    CMEM_ATOMIC_FETCH_ADD(
        &pool->tlsf_allocated_bytes, (new_usable - header->usable_size), CMEM_ORDER_RELAXED);
    header->requested_size = new_size;
    header->usable_size = new_usable;

    void *payload = (void *)((uint8_t *)header + sizeof(mp_block_header_t));
    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
        uint8_t *canary = (uint8_t *)payload + new_size;
        *canary = MP_CANARY_BYTE;
    }

    return true;
}
