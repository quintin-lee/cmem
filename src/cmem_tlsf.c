/**
 * @file cmem_tlsf.c
 * @brief Extracted module implementation.
 */

#include "cmem.h"
#include "cmem_internal.h"
#include <string.h>
static inline int tlsf_fls(size_t val)
{
    if (val == 0) {
        return -1;
    }
    return 31 - __builtin_clz((uint32_t)val);
}

static inline int tlsf_ffs(uint32_t val)
{
    if (val == 0) {
        return -1;
    }
    return __builtin_ctz(val);
}

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

void tlsf_mapping_search(size_t size, int *fl, int *sl)
{
    if (size >= (1 << TLSF_SL_SHIFT)) {
        size_t round = (1 << (*fl = tlsf_fls(size) - TLSF_SL_SHIFT)) - 1;
        size += round;
    }
    tlsf_mapping_insert(size, fl, sl);
}

tlsf_pool_t *tlsf_create_pool_custom(memory_pool_t *pool, size_t size, void *custom_mem)
{
    size          = (size + 7) & ~7;
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

    tlsf_block_t *block   = (tlsf_block_t *)tpool->raw_area;
    block->size_and_flags = (size - sizeof(tlsf_block_t)) | BLOCK_STATE_FREE;
    block->prev_physical  = NULL;
    block->next_free      = NULL;
    block->prev_free      = NULL;

    tlsf_block_t *sentinel =
        (tlsf_block_t *)((uint8_t *)block + (block->size_and_flags & BLOCK_SIZE_MASK));
    sentinel->size_and_flags = 0;
    sentinel->prev_physical  = block;

    int fl, sl;
    tlsf_mapping_insert(block->size_and_flags & BLOCK_SIZE_MASK, &fl, &sl);
    tpool->fl_bitmap |= (1U << fl);
    tpool->sl_bitmap[fl] |= (1U << sl);
    tpool->blocks[fl][sl] = block;

    return tpool;
}

void tlsf_insert_free_block(tlsf_pool_t *tpool, tlsf_block_t *block)
{
    int    fl, sl;
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

void tlsf_remove_free_block(tlsf_pool_t *tpool, tlsf_block_t *block)
{
    int    fl, sl;
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

tlsf_block_t *tlsf_find_suitable_block(tlsf_pool_t *tpool, size_t total_needed)
{
    int fl = 0, sl = 0;
    tlsf_mapping_search(total_needed, &fl, &sl);

    uint32_t sl_map = tpool->sl_bitmap[fl] & (~0U << sl);
    if (sl_map) {
        sl = tlsf_ffs(sl_map);
        return tpool->blocks[fl][sl];
    }

    uint32_t fl_map = tpool->fl_bitmap & (~0U << (fl + 1));
    if (fl_map) {
        fl     = tlsf_ffs(fl_map);
        sl_map = tpool->sl_bitmap[fl];
        sl     = tlsf_ffs(sl_map);
        return tpool->blocks[fl][sl];
    }

    return NULL;
}

void *tlsf_alloc(memory_pool_t *pool, size_t req_size)
{
    size_t total_needed = sizeof(tlsf_block_t) + sizeof(mp_block_header_t) + req_size +
                          ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0);
    total_needed        = (total_needed + 7) & ~7;
    if (total_needed < TLSF_MIN_BLOCK_SIZE) {
        total_needed = TLSF_MIN_BLOCK_SIZE;
    }

    tlsf_pool_t *tpool = pool->tlsf_root;
    if (!tpool) {
        size_t init_sz  = 4 * 1024 * 1024;
        pool->tlsf_root = tlsf_create_pool_custom(pool, init_sz, NULL);
        if (!pool->tlsf_root) {
            return NULL;
        }
        tpool = pool->tlsf_root;
        pool->stats.total_pool_size += init_sz + sizeof(tlsf_pool_t);
    }

    tlsf_block_t *block       = NULL;
    tlsf_pool_t  *target_pool = tpool;

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
        new_p->next     = pool->tlsf_root;
        pool->tlsf_root = new_p;
        target_pool     = new_p;
        pool->stats.total_pool_size += expand_sz + sizeof(tlsf_pool_t);

        block = tlsf_find_suitable_block(target_pool, total_needed);
        if (!block) {
            return NULL;
        }
    }

    tpool = target_pool;
    tlsf_remove_free_block(tpool, block);

    size_t current_size = block->size_and_flags & BLOCK_SIZE_MASK;
    size_t remaining    = current_size - total_needed;

    if (remaining >= TLSF_MIN_BLOCK_SIZE + sizeof(tlsf_block_t)) {
        block->size_and_flags = total_needed | (block->size_and_flags & BLOCK_STATE_PREV_FREE);

        tlsf_block_t *split_block   = (tlsf_block_t *)((uint8_t *)block + total_needed);
        split_block->size_and_flags = remaining | BLOCK_STATE_FREE;
        split_block->prev_physical  = block;

        tlsf_block_t *next_phys  = (tlsf_block_t *)((uint8_t *)split_block + remaining);
        next_phys->prev_physical = split_block;
        next_phys->size_and_flags |= BLOCK_STATE_PREV_FREE;

        tlsf_insert_free_block(tpool, split_block);
    } else {
        block->size_and_flags &= ~BLOCK_STATE_FREE;
        tlsf_block_t *next_phys = (tlsf_block_t *)((uint8_t *)block + current_size);
        next_phys->size_and_flags &= ~BLOCK_STATE_PREV_FREE;
    }

    mp_block_header_t *header = (mp_block_header_t *)((uint8_t *)block + sizeof(tlsf_block_t));
    header->magic             = MP_MAGIC_HEAD;
    header->alloc_type        = ALLOC_TYPE_TLSF;
    header->slab_class        = 0;
    header->flags             = 0;
    header->requested_size    = req_size;
    header->usable_size       = total_needed - sizeof(tlsf_block_t) - sizeof(mp_block_header_t);
    header->raw_base          = block;
    header->subpool           = tpool;
    header->alloc_file        = NULL;
    header->alloc_line        = 0;
    header->alloc_func        = NULL;
    header->backtrace_depth   = 0;

    void *payload = (void *)((uint8_t *)header + sizeof(mp_block_header_t));

    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
        uint8_t *canary = (uint8_t *)payload + req_size;
        *canary         = MP_CANARY_BYTE;
    }

    if (pool->flags & MP_FLAG_ZERO_ON_ALLOC) {
        memset(payload, 0, req_size);
    }

    pool->stats.tlsf_allocated_bytes += header->usable_size;
    return payload;
}

void tlsf_free(memory_pool_t *pool, mp_block_header_t *header)
{
    tlsf_block_t *block = (tlsf_block_t *)header->raw_base;
    tlsf_pool_t  *tpool = (tlsf_pool_t *)header->subpool;

    pool->stats.tlsf_allocated_bytes -= header->usable_size;

    size_t size = block->size_and_flags & BLOCK_SIZE_MASK;
    block->size_and_flags |= BLOCK_STATE_FREE;

    tlsf_block_t *next_phys = (tlsf_block_t *)((uint8_t *)block + size);
    if (next_phys->size_and_flags & BLOCK_STATE_FREE) {
        tlsf_remove_free_block(tpool, next_phys);
        size += (next_phys->size_and_flags & BLOCK_SIZE_MASK);
        block->size_and_flags =
            size | (block->size_and_flags & BLOCK_STATE_PREV_FREE) | BLOCK_STATE_FREE;

        tlsf_block_t *after_next  = (tlsf_block_t *)((uint8_t *)block + size);
        after_next->prev_physical = block;
    }

    if (block->size_and_flags & BLOCK_STATE_PREV_FREE) {
        tlsf_block_t *prev_phys = block->prev_physical;
        if (prev_phys && (prev_phys->size_and_flags & BLOCK_STATE_FREE)) {
            tlsf_remove_free_block(tpool, prev_phys);
            size_t prev_size          = prev_phys->size_and_flags & BLOCK_SIZE_MASK;
            prev_phys->size_and_flags = (prev_size + size) |
                                        (prev_phys->size_and_flags & BLOCK_STATE_PREV_FREE) |
                                        BLOCK_STATE_FREE;

            tlsf_block_t *after_block  = (tlsf_block_t *)((uint8_t *)prev_phys + prev_size + size);
            after_block->prev_physical = prev_phys;
            block                      = prev_phys;
        }
    }

    next_phys = (tlsf_block_t *)((uint8_t *)block + (block->size_and_flags & BLOCK_SIZE_MASK));
    next_phys->size_and_flags |= BLOCK_STATE_PREV_FREE;

    tlsf_insert_free_block(tpool, block);
}

bool tlsf_try_inplace_expand(memory_pool_t *pool, mp_block_header_t *header, size_t new_size)
{
    if (header->alloc_type != ALLOC_TYPE_TLSF) {
        return false;
    }
    tlsf_block_t *block = (tlsf_block_t *)header->raw_base;
    tlsf_pool_t  *tpool = (tlsf_pool_t *)header->subpool;
    if (!block || !tpool) {
        return false;
    }

    size_t current_block_size = block->size_and_flags & BLOCK_SIZE_MASK;
    size_t total_needed       = sizeof(tlsf_block_t) + sizeof(mp_block_header_t) + new_size +
                                ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0);
    total_needed              = (total_needed + 7) & ~7;
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
    size_t remaining     = combined_size - total_needed;

    if (remaining >= TLSF_MIN_BLOCK_SIZE + sizeof(tlsf_block_t)) {
        block->size_and_flags = total_needed | (block->size_and_flags & BLOCK_STATE_PREV_FREE);

        tlsf_block_t *split_block   = (tlsf_block_t *)((uint8_t *)block + total_needed);
        split_block->size_and_flags = remaining | BLOCK_STATE_FREE;
        split_block->prev_physical  = block;

        tlsf_block_t *far_phys  = (tlsf_block_t *)((uint8_t *)split_block + remaining);
        far_phys->prev_physical = split_block;
        far_phys->size_and_flags |= BLOCK_STATE_PREV_FREE;

        tlsf_insert_free_block(tpool, split_block);
    } else {
        block->size_and_flags   = combined_size | (block->size_and_flags & BLOCK_STATE_PREV_FREE);
        tlsf_block_t *far_phys  = (tlsf_block_t *)((uint8_t *)block + combined_size);
        far_phys->prev_physical = block;
        far_phys->size_and_flags &= ~BLOCK_STATE_PREV_FREE;
    }

    size_t new_usable = (block->size_and_flags & BLOCK_SIZE_MASK) - sizeof(tlsf_block_t) -
                        sizeof(mp_block_header_t);
    pool->stats.tlsf_allocated_bytes += (new_usable - header->usable_size);
    header->requested_size = new_size;
    header->usable_size    = new_usable;

    void *payload = (void *)((uint8_t *)header + sizeof(mp_block_header_t));
    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
        uint8_t *canary = (uint8_t *)payload + new_size;
        *canary         = MP_CANARY_BYTE;
    }

    return true;
}
