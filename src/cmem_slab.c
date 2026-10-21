/**
 * @file cmem_slab.c
 * @brief Slab (small-object) allocator implementation.
 *
 * The slab tier serves allocations up to SLAB_MAX_SIZE (512 bytes) through
 * fixed size classes.  Memory is carved into SLAB_PAGE_SIZE (64 KB) pages,
 * each dedicated to a single size class and split into equal-size slots
 * threaded onto an intrusive free list.  Per class, pages are organised into
 * partial (refill source) and full lists, optionally hot/cold tagged for
 * cache-friendly separation.  Two accelerators sit on top of the per-class
 * locks: a thread-local cache and a per-CPU lock-free freelist.
 */

#include "cmem.h"
#include "cmem_internal.h"

#include <assert.h>

/* Slab page slot layout keeps 8-byte alignment; sizes are rounded up by
 * masking with this value. */
#define CMEM_SLAB_ALIGN_MASK 7u

/** Fixed slot sizes served by the slab tier, one class per entry. */
const size_t kSlabSizes[SLAB_CLASS_COUNT] = {8, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512};

const uint8_t cmem_size_to_class[513] = {
    0,                                                             /* 0 bytes */
    0,  0,  0,  0,  0,  0,  0,  0,                                 /* 1..8 -> class 0 */
    1,  1,  1,  1,  1,  1,  1,  1,                                 /* 9..16 -> class 1 */
    2,  2,  2,  2,  2,  2,  2,  2,                                 /* 17..24 -> class 2 */
    3,  3,  3,  3,  3,  3,  3,  3,                                 /* 25..32 -> class 3 */
    4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4, /* 33..48 -> class 4 */
    5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, /* 49..64 -> class 5 */
    6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
    6,  6,  6,  6,  6,  6,  6,  6,  6,  6, /* 65..96 -> class 6 */
    7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,
    7,  7,  7,  7,  7,  7,  7,  7,  7,  7, /* 97..128 -> class 7 */
    8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
    8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
    8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8, /* 129..192 ->
                                                                                      class 8 */
    9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,
    9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,
    9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9, /* 193..256 ->
                                                                                      class 9 */
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, /* 257..384 -> class 10
                                                                             */
    11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, /* 385..512 -> class 11
                                                                             */
};

MP_THREAD_LOCAL thread_cache_t tls_cache = {0};
MP_THREAD_LOCAL mp_thread_quota_t thread_quota = {0, 0};

/* Forward declaration: slab_free_nolock is defined after remote_free_harvest
 * but must be visible to it.
 *
 * Returns bit flags: 1=full→partial transition, 2=partial→empty transition.
 * The caller must apply the returned transitions while holding sc->lock.
 * Stats (slab_allocated_bytes) are updated by the caller, not here.
 */
#define SLAB_TRANS_FULL_TO_PARTIAL 1
#define SLAB_TRANS_PARTIAL_TO_EMPTY 2
int slab_free_nolock(memory_pool_t *pool, mp_block_header_t *header);

/* --------------------------------------------------------------------------
 * Per-thread TLSF arena (multi-arena child pools, owner-thread path).
 *
 * Invariants (shared-TLSF compatible):
 *  - size_and_flags & BLOCK_SIZE_MASK = distance from this block's start to
 *    the next physical block's start (own header included).
 *  - The final sizeof(tlsf_block_t) bytes hold a sentinel with
 *    size_and_flags == 0 (never FREE) and prev_physical maintained.
 *  - FREE <=> block is on arena->free_list (mailboxed blocks keep FREE clear
 *    so they can neither be scanned nor coalesced).
 *  - The free list is doubly-linked (next_free/prev_free) and address-sorted.
 *  - live_bytes counts the spans of live blocks only.
 * -------------------------------------------------------------------------- */

size_t tlsf_arena_span(const tlsf_block_t *block)
{
    return block->size_and_flags & BLOCK_SIZE_MASK;
}

void tlsf_arena_unlink_free(tlsf_arena_t *arena, tlsf_block_t *block)
{
    if (block->prev_free != NULL) {
        block->prev_free->next_free = block->next_free;
    } else {
        arena->free_list = block->next_free;
    }
    if (block->next_free != NULL) {
        block->next_free->prev_free = block->prev_free;
    }
    block->next_free = NULL;
    block->prev_free = NULL;
}

/** Insert a FREE-stamped block (address order) and coalesce both directions. */
void tlsf_arena_insert_free(tlsf_arena_t *arena, tlsf_block_t *block)
{
    tlsf_block_t *prev = NULL;
    tlsf_block_t *curr = arena->free_list;

    while (curr != NULL && (uintptr_t)curr < (uintptr_t)block) {
        prev = curr;
        curr = curr->next_free;
    }

    /* Forward merge: curr starts exactly where block ends. */
    if (curr != NULL && (uint8_t *)block + tlsf_arena_span(block) == (uint8_t *)curr) {
        tlsf_block_t *after = curr->next_free;
        size_t merged = tlsf_arena_span(block) + tlsf_arena_span(curr);
        tlsf_arena_unlink_free(arena, curr);
        block->size_and_flags = merged | BLOCK_STATE_FREE;
        curr = after;
    }

    /* Backward merge: prev ends exactly where block begins. */
    if (prev != NULL && (uint8_t *)prev + tlsf_arena_span(prev) == (uint8_t *)block) {
        tlsf_block_t *before = prev->prev_free;
        size_t merged = tlsf_arena_span(prev) + tlsf_arena_span(block);
        tlsf_arena_unlink_free(arena, prev);
        prev->size_and_flags = merged | BLOCK_STATE_FREE;
        block = prev;
        prev = before;
    }

    block->next_free = curr;
    block->prev_free = prev;
    if (prev != NULL) {
        prev->next_free = block;
    } else {
        arena->free_list = block;
    }
    if (curr != NULL) {
        curr->prev_free = block;
    }

    /* Physical metadata for the successor of the merged block. */
    tlsf_block_t *after = (tlsf_block_t *)((uint8_t *)block + tlsf_arena_span(block));
    if (after != arena->sentinel) {
        after->size_and_flags |= BLOCK_STATE_PREV_FREE;
    }
    after->prev_physical = block;
}

bool tlsf_arena_init(memory_pool_t *pool)
{
    void *raw = sys_mem_alloc(pool, TLSF_ARENA_DEFAULT_SIZE, 8);
    if (raw == NULL) {
        return false;
    }
    tlsf_arena_t *arena = (tlsf_arena_t *)calloc(1, sizeof(tlsf_arena_t));
    if (arena == NULL) {
        sys_mem_free(pool, raw, TLSF_ARENA_DEFAULT_SIZE);
        return false;
    }

    arena->raw = (uint8_t *)raw;
    arena->size = TLSF_ARENA_DEFAULT_SIZE;
    arena->sentinel = (tlsf_block_t *)(arena->raw + arena->size - sizeof(tlsf_block_t));
    arena->owner_pool = pool;
    arena->owner_thread = (unsigned long)pthread_self();

    tlsf_block_t *first = (tlsf_block_t *)arena->raw;
    first->size_and_flags = (arena->size - sizeof(tlsf_block_t)) | BLOCK_STATE_FREE;
    first->prev_physical = NULL;
    first->next_free = NULL;
    first->prev_free = NULL;
    arena->free_list = first;

    arena->sentinel->size_and_flags = 0;
    arena->sentinel->prev_physical = first;
    arena->sentinel->next_free = NULL;
    arena->sentinel->prev_free = NULL;

    pthread_mutex_lock(&pool->arena_registry_lock);
    arena->registry_next = pool->arena_registry;
    pool->arena_registry = arena;
    pthread_mutex_unlock(&pool->arena_registry_lock);

    CMEM_ATOMIC_FETCH_ADD(&pool->total_pool_size, arena->size, CMEM_ORDER_RELAXED);
    tls_cache.tlsf_arena = arena;
    return true;
}

void tlsf_arena_reap(tlsf_arena_t *arena)
{
    tlsf_block_t *block = CMEM_ATOMIC_EXCHANGE(&arena->remote_free, NULL, CMEM_ORDER_ACQUIRE);
    while (block != NULL) {
        tlsf_block_t *next = block->next_free; /* mailbox link */
        block->size_and_flags = tlsf_arena_span(block) | BLOCK_STATE_FREE;
        /* prev_physical is intentionally preserved: it still points at the
         * block's true physical predecessor, which the validator walks. */
        tlsf_arena_insert_free(arena, block);
        block = next;
    }
}

/**
 * Sole-mutator cleanup: every block is free by now (live_bytes reached zero,
 * so the owner is detached or retirement observed zero). Reaps leftovers,
 * then releases the region to the OS.
 *
 * NOTE (deliberate): the region is NOT inserted into the shared TLSF buckets.
 * Memory cannot both join the shared free list and return to the OS, and the
 * pool does not track foreign regions. The spec text is corrected in Chunk 5.
 */
static void tlsf_arena_cleanup(tlsf_arena_t *arena)
{
    tlsf_arena_reap(arena);
#ifndef NDEBUG
    assert(arena->free_list != NULL && arena->free_list->next_free == NULL);
    assert(tlsf_arena_validate(arena));
#endif
    memory_pool_t *pool = arena->owner_pool;
    if (arena->raw != NULL) {
        sys_mem_free(pool, arena->raw, arena->size);
        CMEM_ATOMIC_FETCH_SUB(&pool->total_pool_size, arena->size, CMEM_ORDER_RELAXED);
        arena->raw = NULL;
    }
    CMEM_ATOMIC_STORE(&arena->finished, true, CMEM_ORDER_RELEASE);
}

void tlsf_arena_retire(memory_pool_t *pool)
{
    tlsf_arena_t *arena = tls_cache.tlsf_arena;
    if (arena == NULL || arena->owner_pool != pool) {
        return;
    }
    /* Detach first: this thread can no longer take the direct path. */
    tls_cache.tlsf_arena = NULL;
    CMEM_ATOMIC_STORE(&arena->retired, true, CMEM_ORDER_RELEASE);

    if (CMEM_ATOMIC_LOAD(&arena->live_bytes, CMEM_ORDER_ACQUIRE) == 0) {
        bool expected = false;
        if (CMEM_ATOMIC_COMPARE_EXCHANGE(
                &arena->cleanup_claimed, &expected, true, CMEM_ORDER_ACQ_REL, CMEM_ORDER_RELAXED)) {
            tlsf_arena_cleanup(arena);
        }
    }
}

void tlsf_arena_registry_release(memory_pool_t *pool)
{
    /* Non-thread-safe shared pools never initialize arena_registry_lock and
     * can never own arenas; reading the registry is safe at destroy time. */
    if (pool->arena_registry == NULL) {
        return;
    }
    pthread_mutex_lock(&pool->arena_registry_lock);
    tlsf_arena_t *arena = pool->arena_registry;
    pool->arena_registry = NULL;
    pthread_mutex_unlock(&pool->arena_registry_lock);

    while (arena != NULL) {
        tlsf_arena_t *next = arena->registry_next;
        if (!CMEM_ATOMIC_LOAD(&arena->finished, CMEM_ORDER_ACQUIRE)) {
            /* Pool destroyed with blocks still outstanding: force-release. */
            if (arena->raw != NULL) {
                memory_pool_t *owner = arena->owner_pool;
                sys_mem_free(owner, arena->raw, arena->size);
                CMEM_ATOMIC_FETCH_SUB(&owner->total_pool_size, arena->size, CMEM_ORDER_RELAXED);
                arena->raw = NULL;
            }
        }
        free(arena);
        arena = next;
    }
}

tlsf_block_t *tlsf_arena_alloc(tlsf_arena_t *arena, size_t total_needed)
{
    if (CMEM_ATOMIC_LOAD(&arena->remote_free, CMEM_ORDER_ACQUIRE) != NULL) {
        tlsf_arena_reap(arena);
    }

    tlsf_block_t *block = arena->free_list;
    while (block != NULL && tlsf_arena_span(block) < total_needed) {
        block = block->next_free;
    }
    if (block == NULL) {
        return NULL;
    }

    size_t block_size = tlsf_arena_span(block);
    size_t remaining = block_size - total_needed;
    tlsf_block_t *after = (tlsf_block_t *)((uint8_t *)block + block_size);

    tlsf_arena_unlink_free(arena, block);

    if (remaining >= TLSF_MIN_BLOCK_SIZE + sizeof(tlsf_block_t)) {
        tlsf_block_t *remainder = (tlsf_block_t *)((uint8_t *)block + total_needed);
        block->size_and_flags = total_needed; /* live: FREE cleared */
        remainder->size_and_flags = remaining | BLOCK_STATE_FREE;
        remainder->prev_physical = block;
        tlsf_arena_insert_free(arena, remainder);
    } else {
        block->size_and_flags = block_size; /* live: FREE cleared */
        after->prev_physical = block;
        if (after != arena->sentinel) {
            after->size_and_flags &= ~BLOCK_STATE_PREV_FREE;
        }
    }

    CMEM_ATOMIC_FETCH_ADD(&arena->live_bytes, tlsf_arena_span(block), CMEM_ORDER_RELAXED);
#ifndef NDEBUG
    assert(tlsf_arena_validate(arena));
#endif
    return block;
}

void tlsf_arena_free_block(tlsf_arena_t *arena, tlsf_block_t *block)
{
    size_t span = tlsf_arena_span(block);
    bool owner =
        (tls_cache.tlsf_arena == arena) && !CMEM_ATOMIC_LOAD(&arena->retired, CMEM_ORDER_ACQUIRE);

    if (owner) {
        block->size_and_flags = span | BLOCK_STATE_FREE;
        /* prev_physical is intentionally preserved (not NULLed): the block's
         * true physical predecessor is needed by the validator's chain walk. */
        tlsf_arena_insert_free(arena, block);
    } else {
        /* Deferred free: link BEFORE publishing (CAS retry loop), so a
         * concurrent consumer that exchanges the mailbox can never observe
         * `block` with a stale `next_free`. Still done BEFORE the counter
         * decrement so a decrement-to-zero implies the push is visible. */
        tlsf_block_t *expected = CMEM_ATOMIC_LOAD(&arena->remote_free, CMEM_ORDER_ACQUIRE);
        for (;;) {
            block->next_free = expected;
            if (CMEM_ATOMIC_COMPARE_EXCHANGE(&arena->remote_free,
                                             &expected,
                                             block,
                                             CMEM_ORDER_ACQ_REL,
                                             CMEM_ORDER_ACQUIRE)) {
                break;
            }
        }
    }

    size_t prev_live = CMEM_ATOMIC_FETCH_SUB(&arena->live_bytes, span, CMEM_ORDER_ACQ_REL);
    if (prev_live == span && CMEM_ATOMIC_LOAD(&arena->retired, CMEM_ORDER_ACQUIRE)) {
        bool claim_expected = false;
        if (CMEM_ATOMIC_COMPARE_EXCHANGE(&arena->cleanup_claimed,
                                         &claim_expected,
                                         true,
                                         CMEM_ORDER_ACQ_REL,
                                         CMEM_ORDER_RELAXED)) {
            tlsf_arena_cleanup(arena); /* defined in Task 2.1 */
        }
    }
#ifndef NDEBUG
    /* Cleanup nulls arena->raw and marks `finished`; only validate while the
     * region is still live, otherwise the post-cleanup walk dereferences NULL. */
    if (!CMEM_ATOMIC_LOAD(&arena->finished, CMEM_ORDER_ACQUIRE)) {
        assert(tlsf_arena_validate(arena));
    }
#endif
}

bool tlsf_arena_validate(tlsf_arena_t *arena)
{
    if (arena == NULL || arena->raw == NULL) {
        return false;
    }
    /* An empty free list is legal: a final alloc may absorb a sub-floor
     * remainder whole, leaving the arena fully allocated. */

    /* Intentionally NOT checked here:
     *  - live (allocated) blocks: by design they are not required to be on any
     *    free list, so there is no structural list to cross-check them against;
     *  - live_bytes: it is maintained purely by atomic counters, is not
     *    structurally derivable from the block layout without a full sweep, and
     *    races with concurrent mailbox pushes — so it is verified by the
     *    lifecycle/randomized tests, not by this structural validator. */
    /* Physical chain: every block advances inside the region; sentinel last. */
    tlsf_block_t *prev = NULL;
    tlsf_block_t *block = (tlsf_block_t *)arena->raw;
    uint8_t *end = arena->raw + arena->size - sizeof(tlsf_block_t);
    while ((uint8_t *)block < end) {
        size_t span = tlsf_arena_span(block);
        if (span < TLSF_MIN_BLOCK_SIZE || (span & TLSF_ALIGN_MASK) != 0) {
            return false;
        }
        if ((uint8_t *)block + span > end) {
            return false;
        }
        if (block->prev_physical != prev) {
            return false;
        }
        /* PREV_FREE consistency: a block whose physical predecessor is FREE
         * must carry BLOCK_STATE_PREV_FREE; a block whose predecessor is not
         * free (allocated, or mailboxed/live-looking) must NOT. */
        bool prev_free = (prev != NULL) && ((prev->size_and_flags & BLOCK_STATE_FREE) != 0);
        bool prev_free_flag = (block->size_and_flags & BLOCK_STATE_PREV_FREE) != 0;
        if (prev_free != prev_free_flag) {
            return false;
        }
        prev = block;
        block = (tlsf_block_t *)((uint8_t *)block + span);
    }
    if ((uint8_t *)block != end || block->size_and_flags != 0) {
        return false;
    }
    if (block->prev_physical != prev) {
        return false;
    }

    /* Free list: sorted, doubly-linked, in-region, no adjacent pair. */
    tlsf_block_t *prev_free = NULL;
    for (block = arena->free_list; block != NULL; block = block->next_free) {
        if ((block->size_and_flags & BLOCK_STATE_FREE) == 0) {
            return false;
        }
        if (block->prev_free != prev_free) {
            return false;
        }
        if (prev_free != NULL) {
            if ((uintptr_t)prev_free >= (uintptr_t)block) {
                return false;
            }
            if ((uint8_t *)prev_free + tlsf_arena_span(prev_free) == (uint8_t *)block) {
                return false; /* adjacent free blocks must have coalesced */
            }
        }
        if ((uint8_t *)block < arena->raw || (uint8_t *)block + tlsf_arena_span(block) > end) {
            return false;
        }
        prev_free = block;
    }
    return true;
}

bool tlsf_arena_validate_current(void)
{
    if (tls_cache.tlsf_arena == NULL) {
        return true;
    }
    return tlsf_arena_validate(tls_cache.tlsf_arena);
}

void tls_cache_flush_pool(memory_pool_t *pool)
{
    if (tls_cache.owner_pool == pool) {
        tls_cache.owner_pool = NULL;
        for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
            while (tls_cache.slots[i]) {
                mp_slab_slot_t *slot = tls_cache.slots[i];
                if (((uintptr_t)slot & CMEM_SLAB_ALIGN_MASK) != 0 || (uintptr_t)slot < 0x10000) {
                    tls_cache.slots[i] = NULL;
                    break;
                }
                tls_cache.slots[i] = slot->next;
                mp_block_header_t *header = (mp_block_header_t *)slot;
                header->magic = MP_MAGIC_HEAD;
                header->alloc_type = ALLOC_TYPE_SLAB;
                header->slab_class = (uint8_t)i;
                header->raw_base = (void *)slot;
                slab_free(pool, header);
            }
            tls_cache.counts[i] = 0;
        }
        /* Flush TLSF free-block cache: re-insert cached blocks into their
         * original tlsf_pool free lists. */
        for (int i = 0; i < TLSF_CACHE_SIZES; i++) {
            while (tls_cache.tlsf_slots[i]) {
                tlsf_cache_entry_t *entry = tls_cache.tlsf_slots[i];
                tls_cache.tlsf_slots[i] = entry->next;
                tlsf_block_t *block = (tlsf_block_t *)entry->block;
                tlsf_pool_t *tpool = (tlsf_pool_t *)entry->tpool;
                block->size_and_flags |= BLOCK_STATE_FREE;
                /* Arena blocks: return to the per-thread arena instead of the
                 * shared pool, to keep the arena's free list in sync. */
                if (tls_cache.tlsf_arena != NULL && (uint8_t *)block >= tls_cache.tlsf_arena->raw &&
                    (uint8_t *)block < tls_cache.tlsf_arena->raw + tls_cache.tlsf_arena->size) {
                    tlsf_arena_free_block(tls_cache.tlsf_arena, block);
                } else {
                    tlsf_insert_free_block(tpool, block);
                }
            }
            tls_cache.tlsf_counts[i] = 0;
        }
        /* All cached arena blocks are back in the arena; if it has no live
         * blocks it can now be released and unregistered from the pool. */
        tlsf_arena_retire(pool);
    }
}

#ifndef _WIN32
static pthread_key_t g_tls_cache_key;
static pthread_once_t g_tls_cache_key_once = PTHREAD_ONCE_INIT;

static void tls_cache_dtor(void *arg)
{
    (void)arg;
    if (tls_cache.owner_pool) {
        tls_cache_flush_pool(tls_cache.owner_pool);
    }
}

static void make_tls_cache_key(void)
{
    pthread_key_create(&g_tls_cache_key, tls_cache_dtor);
}
#endif

void tls_cache_validate_owner(memory_pool_t *pool)
{
    /* Fast-path: same pool as last time — skip the pthread_setspecific
     * syscall entirely on the hot allocation path. */
    if (tls_cache.owner_pool == pool) {
        return;
    }

#ifndef _WIN32
    pthread_once(&g_tls_cache_key_once, make_tls_cache_key);
    pthread_setspecific(g_tls_cache_key, (void *)1);
#endif

    if (tls_cache.owner_pool != pool) {
        if (tls_cache.owner_pool != NULL) {
            tls_cache_flush_pool(tls_cache.owner_pool);
        }
        tls_cache.owner_pool = pool;
        /* Auto-cache multi-arena binding: when entering a child arena for
         * the first time, record the master pool so tls_cache_get_bound_arena()
         * can bypass the master pool rwlock on subsequent calls. */
        if (pool->is_multi_arena_child && pool->master_pool) {
            tls_cache.bound_arena = pool;
        } else {
            tls_cache.bound_arena = NULL;
        }
        for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
            tls_cache.slots[i] = NULL;
            tls_cache.counts[i] = 0;
        }
    }
}

/**
 * @brief Return the multi-arena child pool bound to the current thread,
 * or NULL if the thread is not bound to any multi-arena.
 *
 * Checks the TLS-cached bound_arena first (fast path, no locks, no
 * atomic operations). Falls back to the tls_arena_map for safety on
 * the first call or when the cache is invalidated.
 */
memory_pool_t *tls_cache_get_bound_arena(void)
{
    if (tls_cache.bound_arena != NULL) {
        return tls_cache.bound_arena;
    }
    /* Fallback: use the existing binding table lookup. */
    return mp_get_thread_bound_arena(tls_cache.owner_pool);
}

/**
 * @brief Initialise every slab size class for a pool.
 *
 * Each class records its slot size (from kSlabSizes or a custom table) and
 * receives its own mutex together with empty partial/full page lists.
 *
 * @param pool Pool whose slab_classes[] will be initialised.
 * @return true on success (always).
 */
bool slab_init(memory_pool_t *pool)
{
    for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
        if (pool->use_custom_slab_sizes) {
            pool->slab_classes[i].slot_size = pool->custom_slab_sizes[i];
        } else {
            pool->slab_classes[i].slot_size = kSlabSizes[i];
        }
        pthread_mutex_init(&pool->slab_classes[i].lock, NULL);
        pool->slab_classes[i].partial_pages = NULL;
        pool->slab_classes[i].full_pages = NULL;
        pool->slab_classes[i].empty_pages = NULL;
        pool->slab_classes[i].empty_page_count = 0;
        pool->slab_classes[i].max_empty_pages = 2;
        CMEM_ATOMIC_INIT(&pool->remote_free_queue[i], 0);
        CMEM_ATOMIC_INIT(&pool->remote_free_pending, 0);
        CMEM_ATOMIC_INIT(&pool->remote_free_pending_class[i], 0);
    }
    return true;
}

void remote_free_push(memory_pool_t *pool, uint8_t class_idx, mp_slab_slot_t *slot)
{
    if (!pool || class_idx >= SLAB_CLASS_COUNT || !slot) {
        return;
    }
    cmem_atomic_size_t *headp = &pool->remote_free_queue[class_idx];
    size_t old_head = CMEM_ATOMIC_LOAD(headp, CMEM_ORDER_RELAXED);
    do {
        slot->next = (mp_slab_slot_t *)(uintptr_t)old_head;
    } while (!CMEM_ATOMIC_COMPARE_EXCHANGE(
        headp, &old_head, (size_t)(uintptr_t)slot, CMEM_ORDER_RELEASE, CMEM_ORDER_RELAXED));
    /* Signal that there is pending remote free work. */
    CMEM_ATOMIC_STORE(&pool->remote_free_pending, 1, CMEM_ORDER_RELEASE);
    CMEM_ATOMIC_STORE(&pool->remote_free_pending_class[class_idx], 1, CMEM_ORDER_RELEASE);
}

void remote_free_harvest_all(memory_pool_t *pool)
{
    if (!pool) {
        return;
    }
    /* Fast path: skip harvest when no remote frees are pending. */
    if (CMEM_ATOMIC_LOAD(&pool->remote_free_pending, CMEM_ORDER_RELAXED) == 0) {
        return;
    }
    /* Atomically clear the pending flag before harvesting.
     * If new remote frees arrive during harvest, they will set it again. */
    CMEM_ATOMIC_STORE(&pool->remote_free_pending, 0, CMEM_ORDER_RELEASE);
    for (uint8_t i = 0; i < SLAB_CLASS_COUNT; i++) {
        /* Skip classes with no pending remote frees. */
        if (CMEM_ATOMIC_LOAD(&pool->remote_free_pending_class[i], CMEM_ORDER_RELAXED) == 0) {
            continue;
        }
        /* Atomically clear the per-class pending flag. */
        CMEM_ATOMIC_STORE(&pool->remote_free_pending_class[i], 0, CMEM_ORDER_RELEASE);
        /* Must hold the class lock: slab_free_nolock mutates page state
         * (free_list, free_count, page lists) which percpu_refill and other
         * path walkers read under this same lock. */
        mp_slab_class_t *sc = &pool->slab_classes[i];
        if (pool->flags & MP_FLAG_THREAD_SAFE) {
            pthread_mutex_lock(&sc->lock);
        }
        cmem_atomic_size_t *headp = &pool->remote_free_queue[i];
        size_t head = (size_t)CMEM_ATOMIC_EXCHANGE(headp, 0, CMEM_ORDER_RELAXED);
        mp_slab_slot_t *slot = (mp_slab_slot_t *)(uintptr_t)head;
        while (slot) {
            if (((uintptr_t)slot & CMEM_SLAB_ALIGN_MASK) != 0 || (uintptr_t)slot < 0x10000) {
                break;
            }
            mp_slab_slot_t *next = slot->next;
            mp_block_header_t *header = (mp_block_header_t *)slot;
            header->magic = MP_MAGIC_HEAD;
            header->alloc_type = ALLOC_TYPE_SLAB;
            header->slab_class = i;
            header->raw_base = (void *)slot;
            int trans = slab_free_nolock(pool, header);
            /* Apply transitions and update stats under the held lock. */
            if (trans & SLAB_TRANS_FULL_TO_PARTIAL) {
                uintptr_t ptr_val = (uintptr_t)header->raw_base;
                uintptr_t page_base = ptr_val & ~(SLAB_PAGE_SIZE - 1);
                mp_slab_page_t *pg = (mp_slab_page_t *)page_base;
                if (pg->prev) {
                    pg->prev->next = pg->next;
                } else {
                    sc->full_pages = pg->next;
                }
                if (pg->next) {
                    pg->next->prev = pg->prev;
                }
                pg->next = sc->partial_pages;
                pg->prev = NULL;
                if (sc->partial_pages) {
                    sc->partial_pages->prev = pg;
                }
                sc->partial_pages = pg;
            }
            if (trans & SLAB_TRANS_PARTIAL_TO_EMPTY) {
                uintptr_t ptr_val = (uintptr_t)header->raw_base;
                uintptr_t page_base = ptr_val & ~(SLAB_PAGE_SIZE - 1);
                mp_slab_page_t *pg = (mp_slab_page_t *)page_base;
                pg->idle_since_ts = cmem_now_ms();
                if (pg->prev) {
                    pg->prev->next = pg->next;
                } else {
                    sc->partial_pages = pg->next;
                }
                if (pg->next) {
                    pg->next->prev = pg->prev;
                }
                pg->next = sc->empty_pages;
                pg->prev = NULL;
                if (sc->empty_pages) {
                    sc->empty_pages->prev = pg;
                }
                sc->empty_pages = pg;
                sc->empty_page_count++;
            }
            /* Update stats under lock. */
            if (CMEM_ATOMIC_LOAD(&pool->slab_allocated_bytes, CMEM_ORDER_RELAXED) >=
                sc->slot_size) {
                CMEM_ATOMIC_FETCH_SUB(
                    &pool->slab_allocated_bytes, sc->slot_size, CMEM_ORDER_RELAXED);
            } else {
                CMEM_ATOMIC_STORE(&pool->slab_allocated_bytes, 0, CMEM_ORDER_RELAXED);
            }
            slot = next;
        }
        if (pool->flags & MP_FLAG_THREAD_SAFE) {
            pthread_mutex_unlock(&sc->lock);
        }
    }
}

void remote_free_harvest(memory_pool_t *pool, uint8_t class_idx)
{
    if (!pool || class_idx >= SLAB_CLASS_COUNT) {
        return;
    }
    /* Fast path: skip when no remote frees are pending for this class. */
    if (CMEM_ATOMIC_LOAD(&pool->remote_free_pending_class[class_idx], CMEM_ORDER_RELAXED) == 0) {
        return;
    }
    /* Clear the per-class pending flag; caller holds sc->lock. */
    mp_slab_class_t *sc = &pool->slab_classes[class_idx];
    CMEM_ATOMIC_STORE(&pool->remote_free_pending_class[class_idx], 0, CMEM_ORDER_RELEASE);
    cmem_atomic_size_t *headp = &pool->remote_free_queue[class_idx];
    size_t head = (size_t)CMEM_ATOMIC_EXCHANGE(headp, 0, CMEM_ORDER_RELAXED);
    mp_slab_slot_t *slot = (mp_slab_slot_t *)(uintptr_t)head;
    while (slot) {
        if (((uintptr_t)slot & CMEM_SLAB_ALIGN_MASK) != 0 || (uintptr_t)slot < 0x10000) {
            break;
        }
        mp_slab_slot_t *next = slot->next;
        mp_block_header_t *header = (mp_block_header_t *)slot;
        header->magic = MP_MAGIC_HEAD;
        header->alloc_type = ALLOC_TYPE_SLAB;
        header->slab_class = class_idx;
        header->raw_base = (void *)slot;
        /* Callers of remote_free_harvest already hold sc->lock; use the
         * no-lock variant to avoid recursive re-locking (self-deadlock). */
        int trans = slab_free_nolock(pool, header);
        /* Apply transitions and update stats under the already-held lock. */
        if (trans & SLAB_TRANS_FULL_TO_PARTIAL) {
            uintptr_t ptr_val = (uintptr_t)header->raw_base;
            uintptr_t page_base = ptr_val & ~(SLAB_PAGE_SIZE - 1);
            mp_slab_page_t *pg = (mp_slab_page_t *)page_base;
            if (pg->prev) {
                pg->prev->next = pg->next;
            } else {
                sc->full_pages = pg->next;
            }
            if (pg->next) {
                pg->next->prev = pg->prev;
            }
            pg->next = sc->partial_pages;
            pg->prev = NULL;
            if (sc->partial_pages) {
                sc->partial_pages->prev = pg;
            }
            sc->partial_pages = pg;
        }
        if (trans & SLAB_TRANS_PARTIAL_TO_EMPTY) {
            uintptr_t ptr_val = (uintptr_t)header->raw_base;
            uintptr_t page_base = ptr_val & ~(SLAB_PAGE_SIZE - 1);
            mp_slab_page_t *pg = (mp_slab_page_t *)page_base;
            pg->idle_since_ts = cmem_now_ms();
            if (pg->prev) {
                pg->prev->next = pg->next;
            } else {
                sc->partial_pages = pg->next;
            }
            if (pg->next) {
                pg->next->prev = pg->prev;
            }
            pg->next = sc->empty_pages;
            pg->prev = NULL;
            if (sc->empty_pages) {
                sc->empty_pages->prev = pg;
            }
            sc->empty_pages = pg;
            sc->empty_page_count++;
        }
        if (CMEM_ATOMIC_LOAD(&pool->slab_allocated_bytes, CMEM_ORDER_RELAXED) >= sc->slot_size) {
            CMEM_ATOMIC_FETCH_SUB(&pool->slab_allocated_bytes, sc->slot_size, CMEM_ORDER_RELAXED);
        } else {
            CMEM_ATOMIC_STORE(&pool->slab_allocated_bytes, 0, CMEM_ORDER_RELAXED);
        }
        slot = next;
    }
}

/**
 * @brief Create and carve a new slab page for a size class.
 *
 * Maps SLAB_PAGE_SIZE (64 KB) of memory and aligns it to a page boundary,
 * releasing any margin that remains unused. The area past the embedded
 * mp_slab_page_t header is divided into equal-size slots, each of
 * total_slot_size (header + payload + optional canary, 8-byte aligned),
 * and every slot is threaded onto the page's intrusive free list.
 *
 * @param pool      Pool the page belongs to (for flags and accounting).
 * @param class_idx Index of the size class this page will serve.
 * @return The newly created page, or NULL on allocation failure.
 */
mp_slab_page_t *slab_create_page(memory_pool_t *pool, uint8_t class_idx)
{
    size_t slot_payload_size = pool->slab_classes[class_idx].slot_size;
    size_t header_overhead = sizeof(mp_block_header_t);
    size_t total_slot_size =
        header_overhead + slot_payload_size + ((pool->flags & MP_FLAG_DEBUG_CANARY) ? 1 : 0);
    total_slot_size = (total_slot_size + CMEM_SLAB_ALIGN_MASK) & ~(size_t)CMEM_SLAB_ALIGN_MASK;

#ifdef _WIN32
    void *raw_mem = cmem_aligned_malloc(SLAB_PAGE_SIZE, SLAB_PAGE_SIZE);
    if (raw_mem == NULL) {
        return NULL;
    }
#else
    /* Overallocate by one page so the returned region can be page-aligned. */
    size_t map_size = SLAB_PAGE_SIZE + SLAB_PAGE_SIZE - 1;
    void *raw_mem =
        mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw_mem == MAP_FAILED) {
        return NULL;
    }

    uintptr_t start = (uintptr_t)raw_mem;
    uintptr_t aligned = (start + SLAB_PAGE_SIZE - 1) & ~(SLAB_PAGE_SIZE - 1);
    if (aligned + SLAB_PAGE_SIZE > start + map_size) {
        munmap(raw_mem, map_size);
        return NULL;
    }

    /* Release the unneeded head/tail margins so only the exact page remains. */
    if (aligned > start) {
        munmap((void *)start, aligned - start);
    }
    if (aligned + SLAB_PAGE_SIZE < start + map_size) {
        munmap((void *)(aligned + SLAB_PAGE_SIZE), start + map_size - aligned - SLAB_PAGE_SIZE);
    }
#endif

#ifdef _WIN32
    void *page_mem = raw_mem;
#else
    void *page_mem = (void *)aligned;
#endif
    mp_slab_page_t *page = (mp_slab_page_t *)page_mem;
    page->class_index = class_idx;
    page->page_raw_mem = page_mem;
    page->next = NULL;
    page->prev = NULL;
    page->is_hot = false;
    page->idle_since_ts = 0;

    size_t usable_bytes = SLAB_PAGE_SIZE - sizeof(mp_slab_page_t);
    page->total_slots = (uint16_t)(usable_bytes / total_slot_size);
    page->free_count = page->total_slots;

    uint8_t *ptr = (uint8_t *)page_mem + sizeof(mp_slab_page_t);
    page->free_list = (mp_slab_slot_t *)ptr;
    CMEM_ATOMIC_STORE(&page->page_free_count, page->total_slots, CMEM_ORDER_RELAXED);

    /* Link every slot, point each one at the next, the last at NULL. */
    for (uint16_t i = 0; i < page->total_slots; i++) {
        mp_slab_slot_t *slot = (mp_slab_slot_t *)(ptr + i * total_slot_size);
        if (i < page->total_slots - 1) {
            slot->next = (mp_slab_slot_t *)(ptr + (i + 1) * total_slot_size);
        } else {
            slot->next = NULL;
        }
    }

    CMEM_ATOMIC_FETCH_ADD(&pool->total_pool_size, SLAB_PAGE_SIZE, CMEM_ORDER_RELAXED);
    return page;
}

/**
 * @brief Low-level replenishment function to allocate a single raw slot from slab pages.
 *
 * Serves as the bottom-tier supplier for Thread Cache and Per-CPU Cache refills.
 * Pops a slot from the partial page list under the class mutex, creating a new page
 * if partial pages are exhausted.
 *
 * @param pool      Pool serving the request.
 * @param class_idx Size class index for the request.
 * @return Pointer to raw slot, or NULL on allocation failure.
 */
mp_slab_slot_t *slab_alloc_slot(memory_pool_t *pool, uint8_t class_idx)
{
    mp_slab_class_t *sc = &pool->slab_classes[class_idx];

    /* Fast-path: check remote-free flag before the lock to skip the
     * remote_free_harvest call when there is nothing to reclaim.
     * The lock is still always acquired below for the allocation itself. */
    bool need_harvest =
        (CMEM_ATOMIC_LOAD(&pool->remote_free_pending_class[class_idx], CMEM_ORDER_RELAXED) != 0);
    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_lock(&sc->lock);
    }

    if (need_harvest) {
        remote_free_harvest(pool, class_idx);
    }

    mp_slab_page_t *page = sc->partial_pages;

    if (!page) {
        if (sc->empty_pages) {
            page = sc->empty_pages;
            sc->empty_pages = page->next;
            if (sc->empty_pages) {
                sc->empty_pages->prev = NULL;
            }
            sc->empty_page_count--;

            page->next = sc->partial_pages;
            page->prev = NULL;
            if (sc->partial_pages) {
                sc->partial_pages->prev = page;
            }
            sc->partial_pages = page;
        } else {
            page = slab_create_page(pool, class_idx);
            if (!page) {
                if (pool->flags & MP_FLAG_THREAD_SAFE) {
                    pthread_mutex_unlock(&sc->lock);
                }
                return NULL;
            }

            page->next = sc->partial_pages;
            page->prev = NULL;
            if (sc->partial_pages) {
                sc->partial_pages->prev = page;
            }
            sc->partial_pages = page;
        }
    }

    mp_slab_slot_t *slot = page->free_list;
    page->free_list = slot->next;
    page->free_count--;

    if (page->free_count == 0) {
        sc->partial_pages = page->next;
        if (page->next) {
            page->next->prev = NULL;
        }

        page->next = sc->full_pages;
        page->prev = NULL;
        if (sc->full_pages) {
            sc->full_pages->prev = page;
        }
        sc->full_pages = page;
    }

    /* Update slab allocated bytes — plain write for non-thread-safe pools. */
    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        CMEM_ATOMIC_FETCH_ADD(&pool->slab_allocated_bytes, sc->slot_size, CMEM_ORDER_RELAXED);
    } else {
        pool->slab_allocated_bytes += sc->slot_size;
    }

    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_unlock(&sc->lock);
    }

    return slot;
}

/**
 * @brief Allocate `req_size` bytes from the slab tier.
 *
 * Pops a slot off the class' partial page via slab_alloc_slot() and formats
 * it as an mp_block_header_t for direct/fallback allocations.
 *
 * @param pool      Pool serving the allocation.
 * @param class_idx Size class index for the request.
 * @param req_size  User-requested size (must fit the class slot size).
 * @return Pointer to the allocated payload, or NULL if no page can be made.
 */
void *slab_alloc(memory_pool_t *pool,
                 uint8_t class_idx, // NOLINT(bugprone-easily-swappable-parameters)
                 size_t req_size)
{
    mp_slab_slot_t *slot = slab_alloc_slot(pool, class_idx);
    if (!slot) {
        return NULL;
    }

    mp_slab_class_t *sc = &pool->slab_classes[class_idx];
    mp_block_header_t *header = (mp_block_header_t *)slot;
    header->magic = MP_MAGIC_HEAD;
    header->alloc_type = ALLOC_TYPE_SLAB;
    header->slab_class = class_idx;
    header->flags = 0;
    header->requested_size = req_size;
    header->usable_size = sc->slot_size;
    header->raw_base = slot;
    header->subpool = NULL;
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

    return payload;
}

/**
 * @brief Return an allocation to its owning slab page's free list.
 *
 * Recomputes the page base address by masking the slot address down to a
 * 64 KB boundary, pushes the slot back onto the page free list, and moves
 * the page from `full` back to `partial` when it gains its first free slot,
 * so it becomes a refill candidate once more.
 *
 * Callers MUST already hold the owning slab class's lock (`sc->lock`).
 *
 * @param pool   Pool owning the allocation.
 * @param header The mp_block_header_t to free (alloc_type == SLAB).
 */
int slab_free_nolock(memory_pool_t *pool, mp_block_header_t *header)
{
    uintptr_t ptr_val = (uintptr_t)header->raw_base;
    uintptr_t page_base = ptr_val & ~(SLAB_PAGE_SIZE - 1);
    mp_slab_page_t *page = (mp_slab_page_t *)page_base;

    bool was_full = (page->free_count == 0);
    bool will_be_empty = false;

    mp_slab_slot_t *slot = (mp_slab_slot_t *)header->raw_base;
    slot->next = page->free_list;
    page->free_list = slot;
    page->free_count++;
    /* Publish the updated count atomically so lock-free readers see the
     * correct value without acquiring the class lock. Skip for non-thread-safe
     * pools to avoid atomic store overhead in the fast path. */
    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        CMEM_ATOMIC_STORE(&page->page_free_count, page->free_count, CMEM_ORDER_RELEASE);
    }

    will_be_empty = (page->free_count == page->total_slots);

    return (was_full ? SLAB_TRANS_FULL_TO_PARTIAL : 0) |
           (will_be_empty ? SLAB_TRANS_PARTIAL_TO_EMPTY : 0);
}

/**
 * @brief Return an allocation to its owning slab page's free list.
 *
 * Recomputes the page base address by masking the slot address down to a
 * 64 KB boundary, pushes the slot back onto the page free list, and moves
 * the page from `full` back to `partial` when it gains its first free slot,
 * so it becomes a refill candidate once more.
 *
 * Takes the owning slab class's lock (`sc->lock`) when the pool is
 * thread-safe; see `slab_free_nolock` for the lock-free variant used by
 * callers that already hold the lock.
 *
 * @param pool   Pool owning the allocation.
 * @param header The mp_block_header_t to free (alloc_type == SLAB).
 */
void slab_free(memory_pool_t *pool, mp_block_header_t *header)
{
    uint8_t class_idx = header->slab_class;
    mp_slab_class_t *sc = &pool->slab_classes[class_idx];

    /* Update slab allocated bytes — plain write for non-thread-safe pools. */
    const bool ts_slab = (pool->flags & MP_FLAG_THREAD_SAFE) != 0;
    if (ts_slab) {
        if (CMEM_ATOMIC_LOAD(&pool->slab_allocated_bytes, CMEM_ORDER_RELAXED) >= sc->slot_size) {
            CMEM_ATOMIC_FETCH_SUB(&pool->slab_allocated_bytes, sc->slot_size, CMEM_ORDER_RELAXED);
        } else {
            CMEM_ATOMIC_STORE(&pool->slab_allocated_bytes, 0, CMEM_ORDER_RELAXED);
        }
    } else {
        if (pool->slab_allocated_bytes >= sc->slot_size) {
            pool->slab_allocated_bytes -= sc->slot_size;
        } else {
            pool->slab_allocated_bytes = 0;
        }
    }

    if (ts_slab) {
        pthread_mutex_lock(&sc->lock);
    }

    int trans_flags = slab_free_nolock(pool, header);
    /* Apply page transitions under the held lock. */
    if (trans_flags & SLAB_TRANS_FULL_TO_PARTIAL) {
        uintptr_t ptr_val = (uintptr_t)header->raw_base;
        uintptr_t page_base = ptr_val & ~(SLAB_PAGE_SIZE - 1);
        mp_slab_page_t *pg = (mp_slab_page_t *)page_base;
        if (pg->prev) {
            pg->prev->next = pg->next;
        } else {
            sc->full_pages = pg->next;
        }
        if (pg->next) {
            pg->next->prev = pg->prev;
        }
        pg->next = sc->partial_pages;
        pg->prev = NULL;
        if (sc->partial_pages) {
            sc->partial_pages->prev = pg;
        }
        sc->partial_pages = pg;
    }
    if (trans_flags & SLAB_TRANS_PARTIAL_TO_EMPTY) {
        uintptr_t ptr_val = (uintptr_t)header->raw_base;
        uintptr_t page_base = ptr_val & ~(SLAB_PAGE_SIZE - 1);
        mp_slab_page_t *pg = (mp_slab_page_t *)page_base;
        pg->idle_since_ts = cmem_now_ms();
        if (pg->prev) {
            pg->prev->next = pg->next;
        } else {
            sc->partial_pages = pg->next;
        }
        if (pg->next) {
            pg->next->prev = pg->prev;
        }
        pg->next = sc->empty_pages;
        pg->prev = NULL;
        if (sc->empty_pages) {
            sc->empty_pages->prev = pg;
        }
        sc->empty_pages = pg;
        sc->empty_page_count++;
    }

    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_unlock(&sc->lock);
    }
}

/**
 * @brief Refill the calling thread's cache for a size class.
 *
 * Bulk-allocates up to 32 slots for the class directly from the slab tier
 * under a single class mutex lock acquisition and threads them onto the
 * thread-local cache's free list.
 *
 * @param pool      Pool to draw slots from.
 * @param class_idx Size class whose thread cache needs topping up.
 */
/**
 * @brief Internal helper: drain up to n slots from slab pages into the TLS cache.
 *
 * Called under sc->lock. Returns the number of slots actually added.
 */
static int tls_cache_drain_slots(memory_pool_t *pool, uint8_t class_idx, int n)
{
    mp_slab_class_t *sc = &pool->slab_classes[class_idx];
    int got = 0;

    while (got < n) {
        mp_slab_page_t *page = sc->partial_pages;
        if (!page) {
            if (sc->empty_pages) {
                page = sc->empty_pages;
                sc->empty_pages = page->next;
                if (sc->empty_pages) {
                    sc->empty_pages->prev = NULL;
                }
                sc->empty_page_count--;
                page->next = sc->partial_pages;
                page->prev = NULL;
                if (sc->partial_pages) {
                    sc->partial_pages->prev = page;
                }
                sc->partial_pages = page;
            } else {
                page = slab_create_page(pool, class_idx);
                if (!page) {
                    break;
                }
                page->next = sc->partial_pages;
                page->prev = NULL;
                if (sc->partial_pages) {
                    sc->partial_pages->prev = page;
                }
                sc->partial_pages = page;
            }
        }
        if (!page) {
            break;
        }

        mp_slab_slot_t *slot = page->free_list;
        page->free_list = slot->next;
        page->free_count--;
        if (page->free_count == 0) {
            sc->partial_pages = page->next;
            if (page->next) {
                page->next->prev = NULL;
            }
            page->next = sc->full_pages;
            page->prev = NULL;
            if (sc->full_pages) {
                sc->full_pages->prev = page;
            }
            sc->full_pages = page;
        }
        slot->next = tls_cache.slots[class_idx];
        tls_cache.slots[class_idx] = slot;
        tls_cache.counts[class_idx]++;
        got++;
    }

    CMEM_ATOMIC_FETCH_ADD(
        &pool->slab_allocated_bytes, (size_t)got * sc->slot_size, CMEM_ORDER_RELAXED);
    return got;
}

void tls_cache_refill(memory_pool_t *pool, uint8_t class_idx)
{
    mp_slab_class_t *sc = &pool->slab_classes[class_idx];
    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_lock(&sc->lock);
    }
    tls_cache_drain_slots(pool, class_idx, 64);
    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_unlock(&sc->lock);
    }
}

/**
 * @brief Bulk-allocate up to max_count slots for a size class.
 *
 * Pops slots directly from the slab tier under a single class mutex lock
 * acquisition and writes them into out_slots[0..n-1] in allocation order.
 * Page transitions (empty -> partial, partial -> full) are identical to
 * tls_cache_refill. On return the caller owns all produced slots and is
 * responsible for stamping each slot's block header.
 *
 * @param pool      Pool to draw slots from.
 * @param class_idx Size class to allocate from.
 * @param out_slots Receives up to max_count slot pointers (prefix).
 * @param max_count Maximum number of slots requested.
 * @return Number of slots actually produced.
 */
/**
 * @brief Refill TLS cache and allocate one slot under a single lock acquisition.
 *
 * Combines tls_cache_refill and slab_alloc_slot into one critical section,
 * eliminating the double-lock contention that occurs when the TLS cache is
 * empty and a direct slab allocation is needed afterward.
 *
 * @param pool      Pool to allocate from.
 * @param class_idx Size class index.
 * @return Slot pointer, or NULL on failure.
 */
mp_slab_slot_t *slab_alloc_with_tls_refill(memory_pool_t *pool, uint8_t class_idx)
{
    mp_slab_class_t *sc = &pool->slab_classes[class_idx];
    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_lock(&sc->lock);
    }

    /* Phase 1: Refill TLS cache */
    tls_cache_drain_slots(pool, class_idx, 64);

    /* Phase 2: Pop from TLS cache */
    mp_slab_slot_t *slot = NULL;
    if (tls_cache.counts[class_idx] > 0) {
        slot = tls_cache.slots[class_idx];
        tls_cache.slots[class_idx] = slot->next;
        tls_cache.counts[class_idx]--;
    }

    /* Phase 3: If TLS cache still empty, allocate directly from slab */
    if (!slot) {
        mp_slab_page_t *page = sc->partial_pages;
        if (!page) {
            if (sc->empty_pages) {
                page = sc->empty_pages;
                sc->empty_pages = page->next;
                if (sc->empty_pages) {
                    sc->empty_pages->prev = NULL;
                }
                sc->empty_page_count--;
                page->next = sc->partial_pages;
                page->prev = NULL;
                if (sc->partial_pages) {
                    sc->partial_pages->prev = page;
                }
                sc->partial_pages = page;
            } else {
                page = slab_create_page(pool, class_idx);
            }
        }
        if (page) {
            slot = page->free_list;
            page->free_list = slot->next;
            page->free_count--;
            if (page->free_count == 0) {
                sc->partial_pages = page->next;
                if (page->next) {
                    page->next->prev = NULL;
                }
                page->next = sc->full_pages;
                page->prev = NULL;
                if (sc->full_pages) {
                    sc->full_pages->prev = page;
                }
                sc->full_pages = page;
            }
            CMEM_ATOMIC_FETCH_ADD(&pool->slab_allocated_bytes, sc->slot_size, CMEM_ORDER_RELAXED);
        }
    }

    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_unlock(&sc->lock);
    }

    return slot;
}

size_t slab_alloc_batch(memory_pool_t *pool,
                        uint8_t class_idx,
                        mp_slab_slot_t **out_slots,
                        size_t max_count)
{
    mp_slab_class_t *sc = &pool->slab_classes[class_idx];
    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_lock(&sc->lock);
    }

    size_t produced = 0;
    while (produced < max_count) {
        mp_slab_page_t *page = sc->partial_pages;
        if (!page) {
            if (sc->empty_pages) {
                page = sc->empty_pages;
                sc->empty_pages = page->next;
                if (sc->empty_pages) {
                    sc->empty_pages->prev = NULL;
                }
                sc->empty_page_count--;

                page->next = sc->partial_pages;
                page->prev = NULL;
                if (sc->partial_pages) {
                    sc->partial_pages->prev = page;
                }
                sc->partial_pages = page;
            } else {
                page = slab_create_page(pool, class_idx);
                if (!page) {
                    break;
                }
                page->next = sc->partial_pages;
                page->prev = NULL;
                if (sc->partial_pages) {
                    sc->partial_pages->prev = page;
                }
                sc->partial_pages = page;
            }
        }

        mp_slab_slot_t *slot = page->free_list;
        page->free_list = slot->next;
        page->free_count--;

        if (page->free_count == 0) {
            sc->partial_pages = page->next;
            if (page->next) {
                page->next->prev = NULL;
            }

            page->next = sc->full_pages;
            page->prev = NULL;
            if (sc->full_pages) {
                sc->full_pages->prev = page;
            }
            sc->full_pages = page;
        }

        out_slots[produced] = slot;
        produced++;
    }

    CMEM_ATOMIC_FETCH_ADD(
        &pool->slab_allocated_bytes, produced * sc->slot_size, CMEM_ORDER_RELAXED);

    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_unlock(&sc->lock);
    }
    return produced;
}

/**
 * @brief Allocate the per-CPU lock-free freelist table.
 *
 * Detects the number of online CPUs (capped at 256), reserves a
 * num_cpus x SLAB_CLASS_COUNT array of freelist entries, and stores it in
 * pool->percpu_freelists.  Each entry holds an atomic head pointer plus a
 * batch counter for lock-free push/pop.
 *
 * @param pool Pool receiving the table.
 */
void percpu_init(memory_pool_t *pool)
{
    if (!pool || pool->percpu_freelists) {
        return;
    }
#ifdef _WIN32
    pool->num_cpus = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
    pool->num_cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (pool->num_cpus <= 0) {
        pool->num_cpus = 1;
    }
    if (pool->num_cpus > 256) {
        pool->num_cpus = 256;
    }

    size_t sz = (size_t)pool->num_cpus * SLAB_CLASS_COUNT * sizeof(mp_percpu_freelist_entry_t);
    pool->percpu_freelists = (mp_percpu_freelist_entry_t *)calloc(1, sz);
    if (!pool->percpu_freelists) {
        pool->num_cpus = 0;
        return;
    }
}

/**
 * @brief Free and clear the per-CPU freelist table.
 *
 * @param pool Pool whose freelist table is released.
 */
void percpu_flush(memory_pool_t *pool)
{
    if (!pool || !pool->percpu_freelists) {
        return;
    }
    for (int cpu = 0; cpu < pool->num_cpus; cpu++) {
        for (int cls = 0; cls < SLAB_CLASS_COUNT; cls++) {
            size_t idx = (size_t)cpu * SLAB_CLASS_COUNT + cls;
            mp_percpu_freelist_entry_t *entry =
                &((mp_percpu_freelist_entry_t *)pool->percpu_freelists)[idx];
            cmem_atomic_size_t *headp = &entry->head;
            size_t head = (size_t)CMEM_ATOMIC_EXCHANGE(headp, 0, CMEM_ORDER_RELAXED);
            CMEM_ATOMIC_STORE(&entry->count, 0, CMEM_ORDER_RELAXED);

            mp_slab_slot_t *slot = (mp_slab_slot_t *)(uintptr_t)head;
            while (slot) {
                if (((uintptr_t)slot & CMEM_SLAB_ALIGN_MASK) != 0 || (uintptr_t)slot < 0x10000) {
                    break;
                }
                mp_slab_slot_t *next = slot->next;
                mp_block_header_t *header = (mp_block_header_t *)slot;
                header->magic = MP_MAGIC_HEAD;
                header->alloc_type = ALLOC_TYPE_SLAB;
                header->slab_class = (uint8_t)cls;
                header->raw_base = (void *)slot;
                slab_free(pool, header);
                slot = next;
            }
        }
    }
}

void percpu_destroy(memory_pool_t *pool)
{
    if (!pool || !pool->percpu_freelists) {
        return;
    }
    percpu_flush(pool);
    free(pool->percpu_freelists);
    pool->percpu_freelists = NULL;
    pool->num_cpus = 0;
}

/**
 * @brief Resolve the CPU index of the current thread.
 *
 * On Windows no sched_getcpu equivalent is available, so a constant 0 is
 * returned, collapsing all threads onto one logical CPU freelist.
 *
 * @return A non-negative CPU identifier for freelist indexing.
 */
int percpu_cpu_index(void)
{
#ifdef _WIN32
    return 0;
#else
    int cpu = cmem_current_cpu();
    if (cpu < 0) {
        cpu = 0;
    }
    return cpu;
#endif
}

/**
 * @brief Lock-free pop of a cached slot for (cpu, class) from the freelist.
 *
 * Loads the entry's atomic head, then tries a compare-and-swap to install
 * the next slot as the new head.  Returns NULL when the list is empty or the
 * CAS loses to a concurrent producer, letting the caller fall back to the
 * synchronous slab tier.
 *
 * @param pool      Pool holding the freelist table.
 * @param cpu       Owner CPU index into the table.
 * @param class_idx Size class to pop from.
 * @return A cached slot, or NULL if the list was empty or contended.
 */
mp_slab_slot_t *percpu_pop(memory_pool_t *pool, int cpu, uint8_t class_idx)
{
    if (!pool->percpu_freelists || cpu < 0 || cpu >= pool->num_cpus) {
        return NULL;
    }
    size_t idx = (size_t)cpu * SLAB_CLASS_COUNT + class_idx;
    mp_percpu_freelist_entry_t *entry =
        &((mp_percpu_freelist_entry_t *)pool->percpu_freelists)[idx];
    cmem_atomic_size_t *headp = &entry->head;
    size_t head = CMEM_ATOMIC_LOAD(headp, CMEM_ORDER_ACQUIRE);
    if (head == 0) {
        return NULL;
    }

    mp_slab_slot_t *slot = (mp_slab_slot_t *)head;
    mp_slab_slot_t *next = slot->next;
    if (!CMEM_ATOMIC_COMPARE_EXCHANGE(
            headp, &head, (size_t)next, CMEM_ORDER_ACQUIRE, CMEM_ORDER_RELAXED)) {
        return NULL;
    }
    CMEM_ATOMIC_FETCH_SUB(&entry->count, 1, CMEM_ORDER_RELAXED);
    return slot;
}

/**
 * @brief Top up a per-CPU freelist from the synchronous slab tier.
 *
 * When the entry's count has dropped below half the maximum batch, allocates
 * up to MP_PERCPU_MAX_BATCH slots from the slab, chains them together, then
 * atomically prepends the batch to the entry's head pointer.
 *
 * @param pool      Pool to draw slots from.
 * @param cpu       Owner CPU whose freelist to refill.
 * @param class_idx Size class to refill.
 */
void percpu_refill(memory_pool_t *pool, int cpu, uint8_t class_idx)
{
    if (!pool->percpu_freelists || cpu < 0 || cpu >= pool->num_cpus) {
        return;
    }
    size_t idx = (size_t)cpu * SLAB_CLASS_COUNT + class_idx;
    mp_percpu_freelist_entry_t *entry =
        &((mp_percpu_freelist_entry_t *)pool->percpu_freelists)[idx];
    if (entry->count > MP_PERCPU_MAX_BATCH / 2) {
        return;
    }

    mp_slab_class_t *sc = &pool->slab_classes[class_idx];
    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_lock(&sc->lock);
    }

    mp_slab_slot_t *slots[MP_PERCPU_MAX_BATCH];
    int got = 0;
    while (got < MP_PERCPU_MAX_BATCH) {
        mp_slab_page_t *page = sc->partial_pages;
        if (!page) {
            if (sc->empty_pages) {
                page = sc->empty_pages;
                sc->empty_pages = page->next;
                if (sc->empty_pages) {
                    sc->empty_pages->prev = NULL;
                }
                sc->empty_page_count--;

                page->next = sc->partial_pages;
                page->prev = NULL;
                if (sc->partial_pages) {
                    sc->partial_pages->prev = page;
                }
                sc->partial_pages = page;
            } else {
                page = slab_create_page(pool, class_idx);
                if (!page) {
                    break;
                }
                page->next = sc->partial_pages;
                page->prev = NULL;
                if (sc->partial_pages) {
                    sc->partial_pages->prev = page;
                }
                sc->partial_pages = page;
            }
        }

        mp_slab_slot_t *slot = page->free_list;
        page->free_list = slot->next;
        page->free_count--;

        if (page->free_count == 0) {
            sc->partial_pages = page->next;
            if (page->next) {
                page->next->prev = NULL;
            }

            page->next = sc->full_pages;
            page->prev = NULL;
            if (sc->full_pages) {
                sc->full_pages->prev = page;
            }
            sc->full_pages = page;
        }

        slots[got++] = slot;
    }

    CMEM_ATOMIC_FETCH_ADD(
        &pool->slab_allocated_bytes, (size_t)got * sc->slot_size, CMEM_ORDER_RELAXED);

    if (pool->flags & MP_FLAG_THREAD_SAFE) {
        pthread_mutex_unlock(&sc->lock);
    }

    if (got == 0) {
        return;
    }

    mp_slab_slot_t *head = slots[0];
    mp_slab_slot_t *tail = head;
    for (int i = 1; i < got; i++) {
        tail->next = slots[i];
        tail = slots[i];
    }
    size_t old_head = 0;
    do {
        old_head = CMEM_ATOMIC_LOAD(&entry->head, CMEM_ORDER_RELAXED);
        tail->next = (mp_slab_slot_t *)old_head;
    } while (!CMEM_ATOMIC_COMPARE_EXCHANGE(
        &entry->head, &old_head, (size_t)head, CMEM_ORDER_RELAXED, CMEM_ORDER_RELAXED));
    CMEM_ATOMIC_FETCH_ADD(&entry->count, (size_t)got, CMEM_ORDER_RELAXED);
}

/**
 * @brief Enable or disable the per-CPU freelist accelerator.
 *
 * Enabling sets MP_FLAG_PERCPU_FREELIST and lazily allocates the freelist
 * table; disabling clears the flag and frees the table.
 *
 * @param pool   Pool to configure.
 * @param enable true to turn on, false to turn off.
 */
void mp_set_percpu_freelist(memory_pool_t *pool, bool enable)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    if (enable) {
        pool->flags = (mp_flags_t)(pool->flags | MP_FLAG_PERCPU_FREELIST);
        if (!pool->percpu_freelists) {
            percpu_init(pool);
        }
    } else {
        pool->flags = (mp_flags_t)(pool->flags & ~MP_FLAG_PERCPU_FREELIST);
        percpu_destroy(pool);
    }
    pool_unlock(pool);
}

/**
 * @brief Query whether the per-CPU freelist feature is active.
 *
 * @param pool Pool to query.
 * @return true if the flag is set and the freelist table is allocated.
 */
bool mp_get_percpu_freelist(memory_pool_t *pool)
{
    if (!pool) {
        return false;
    }
    bool enabled = false;
    pool_rdlock(pool);
    if ((pool->flags & MP_FLAG_PERCPU_FREELIST) != 0 && pool->percpu_freelists != NULL) {
        enabled = true;
    }
    pool_rdunlock(pool);
    return enabled;
}

/**
 * @brief Report the number of online CPUs tracked for the freelist table.
 *
 * @param pool Pool to query.
 * @return The number of per-CPU freelist slots (0 if uninitialised).
 */
int mp_get_percpu_cpu_count(memory_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    pool_rdlock(pool);
    int count = pool->num_cpus;
    pool_rdunlock(pool);
    return count;
}

/**
 * @brief Mark the slab page backed by `page_raw_mem` as hot.
 *
 * Searches every size class' partial and full page lists for a page whose
 * backing address matches and sets its is_hot flag.  Used to hint the hot/cold
 * separation pass about frequently-touched pages.
 *
 * @param pool         Pool owning the slab pages.
 * @param page_raw_mem Raw base address of the page (page->page_raw_mem).
 * @return true if the page was found and marked, false otherwise.
 */
bool mp_mark_page_hot(memory_pool_t *pool, void *page_raw_mem)
{
    if (!pool || !page_raw_mem) {
        return false;
    }
    bool found = false;
    pool_lock(pool);

    for (int cls = 0; cls < SLAB_CLASS_COUNT; cls++) {
        mp_slab_class_t *sc = &pool->slab_classes[cls];
        mp_slab_page_t *curr = sc->partial_pages;

        while (curr) {
            if (curr->page_raw_mem == page_raw_mem) {
                curr->is_hot = true;
                found = true;
                break;
            }
            curr = curr->next;
        }
        if (found) {
            break;
        }
        curr = sc->full_pages;
        while (curr) {
            if (curr->page_raw_mem == page_raw_mem) {
                curr->is_hot = true;
                found = true;
                break;
            }
            curr = curr->next;
        }
        if (found) {
            break;
        }
    }
    pool_unlock(pool);
    return found;
}

/**
 * @brief Mark the slab page backed by `page_raw_mem` as cold.
 *
 * The inverse of mp_mark_page_hot: clears the is_hot flag on a matching page
 * so the hot/cold separation pass may consider releasing it.
 *
 * @param pool         Pool owning the slab pages.
 * @param page_raw_mem Raw base address of the page (page->page_raw_mem).
 * @return true if the page was found and marked, false otherwise.
 */
bool mp_mark_page_cold(memory_pool_t *pool, void *page_raw_mem)
{
    if (!pool || !page_raw_mem) {
        return false;
    }
    bool found = false;
    pool_lock(pool);

    for (int cls = 0; cls < SLAB_CLASS_COUNT; cls++) {
        mp_slab_class_t *sc = &pool->slab_classes[cls];
        mp_slab_page_t *curr = sc->partial_pages;

        while (curr) {
            if (curr->page_raw_mem == page_raw_mem) {
                curr->is_hot = false;
                found = true;
                break;
            }
            curr = curr->next;
        }
        if (found) {
            break;
        }
        curr = sc->full_pages;
        while (curr) {
            if (curr->page_raw_mem == page_raw_mem) {
                curr->is_hot = false;
                found = true;
                break;
            }
            curr = curr->next;
        }
        if (found) {
            break;
        }
    }
    pool_unlock(pool);
    return found;
}

/**
 * @brief Count slab pages currently marked hot across all size classes.
 *
 * @param pool Pool whose pages are scanned.
 * @return Total number of hot pages.
 */
size_t mp_get_hot_page_count(memory_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    size_t count = 0;
    pool_rdlock(pool);

    for (int cls = 0; cls < SLAB_CLASS_COUNT; cls++) {
        mp_slab_class_t *sc = &pool->slab_classes[cls];
        mp_slab_page_t *curr = sc->partial_pages;

        while (curr) {
            if (curr->is_hot) {
                count++;
            }
            curr = curr->next;
        }
        curr = sc->full_pages;
        while (curr) {
            if (curr->is_hot) {
                count++;
            }
            curr = curr->next;
        }
    }
    pool_rdunlock(pool);
    return count;
}

/**
 * @brief Count slab pages currently marked cold across all size classes.
 *
 * Cold pages are the hot-count subtracted from the total page count, and are
 * the candidates for the hot/cold separation pass.
 *
 * @param pool Pool whose pages are scanned.
 * @return Total number of cold (non-hot) pages.
 */
size_t mp_get_cold_page_count(memory_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    size_t total = 0;
    size_t hot = 0;
    pool_rdlock(pool);

    for (int cls = 0; cls < SLAB_CLASS_COUNT; cls++) {
        mp_slab_class_t *sc = &pool->slab_classes[cls];
        mp_slab_page_t *curr = sc->partial_pages;

        while (curr) {
            total++;
            if (curr->is_hot) {
                hot++;
            }
            curr = curr->next;
        }
        curr = sc->full_pages;
        while (curr) {
            total++;
            if (curr->is_hot) {
                hot++;
            }
            curr = curr->next;
        }
    }
    pool_rdunlock(pool);
    return total > hot ? total - hot : 0;
}

/**
 * @brief Count cold pages as a separation pass candidate.
 *
 * Only meaningful when MP_FLAG_HOT_COLD_SEPARATION is set: returns how many
 * pages are currently cold, giving callers a size estimate for a subsequent
 * hot/cold separation or compaction.
 *
 * @param pool Pool whose pages are scanned.
 * @return Number of cold pages, or 0 when the flag is not enabled.
 */
size_t mp_separate_hot_cold_pages(memory_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    if (!(pool->flags & MP_FLAG_HOT_COLD_SEPARATION)) {
        return 0;
    }
    size_t separated = 0;
    pool_lock(pool);

    for (int cls = 0; cls < SLAB_CLASS_COUNT; cls++) {
        mp_slab_class_t *sc = &pool->slab_classes[cls];
        mp_slab_page_t *curr = sc->partial_pages;

        while (curr) {
            if (!curr->is_hot) {
                separated++;
            }
            curr = curr->next;
        }
        curr = sc->full_pages;
        while (curr) {
            if (!curr->is_hot) {
                separated++;
            }
            curr = curr->next;
        }
    }
    pool_unlock(pool);
    return separated;
}

/**
 * @brief Lock-free push of a slot onto a per-CPU freelist.
 *
 * Returns false (leaving the slot unmanaged) when the entry's count is at
 * the maximum batch, so oversized returns fall back to the synchronous tier.
 * Otherwise prepends the slot to the atomic head via compare-and-swap.
 *
 * @param pool      Pool holding the freelist table.
 * @param cpu       Owner CPU index into the table.
 * @param class_idx Size class to push onto.
 * @param slot      Slot to return to the freelist.
 * @return true if the slot was accepted onto the freelist.
 */
bool percpu_push(memory_pool_t *pool, int cpu, uint8_t class_idx, mp_slab_slot_t *slot)
{
    if (!pool->percpu_freelists || cpu < 0 || cpu >= pool->num_cpus) {
        return false;
    }
    size_t idx = (size_t)cpu * SLAB_CLASS_COUNT + class_idx;
    mp_percpu_freelist_entry_t *entry =
        &((mp_percpu_freelist_entry_t *)pool->percpu_freelists)[idx];
    cmem_atomic_size_t *headp = &entry->head;
    size_t old_head = 0;
    size_t new_head = 0;
    do {
        old_head = CMEM_ATOMIC_LOAD(headp, CMEM_ORDER_RELAXED);
        if (CMEM_ATOMIC_LOAD(&entry->count, CMEM_ORDER_RELAXED) >= MP_PERCPU_MAX_BATCH) {
            return false;
        }
        slot->next = (mp_slab_slot_t *)(uintptr_t)old_head;
        new_head = (size_t)(uintptr_t)slot;
    } while (!CMEM_ATOMIC_COMPARE_EXCHANGE(
        headp, &old_head, new_head, CMEM_ORDER_RELAXED, CMEM_ORDER_RELAXED));

    CMEM_ATOMIC_FETCH_ADD(&entry->count, 1, CMEM_ORDER_RELAXED);
    return true;
}
