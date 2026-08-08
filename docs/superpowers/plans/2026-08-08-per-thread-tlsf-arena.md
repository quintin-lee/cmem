# Per-Thread TLSF Arena Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a per-thread TLSF arena to `thread_cache_t` that serves 80%+ of TLSF allocations without acquiring any lock, falling back to the shared pool only when the arena is exhausted.

**Architecture:** Each thread maintains a 256KB contiguous arena managed as a sorted singly-linked free list of `tlsf_block_t` nodes. Alloc scans the list for the first block that fits; free inserts in address order and coalesces with the next neighbor. On pool-switch or thread-exit, all remaining blocks are drained into the shared pool's TLSF via `tlsf_insert_free_block`.

**Tech Stack:** C11, pthread TLS, `sys_mem_alloc`/`sys_mem_free`, existing `tlsf_block_t` / `BLOCK_STATE_*` / `TLSF_*` constants.

---

## File Map

| File | Change | Role |
|------|--------|------|
| `include/cmem.h:1030-1045` | Add `TLSF_ARENA_*` constants + 4 fields to `thread_cache_t` | Arena metadata in thread-local cache |
| `src/cmem_slab.c:109-160` | Add `tlsf_arena_init`, `tlsf_arena_alloc`, `tlsf_arena_free`, `tlsf_arena_drain`, `tlsf_arena_destroy` | Core arena management (thread-local only, no locks) |
| `src/cmem_event.c:2196-2260` | Insert arena hit in `mp_alloc_internal` TLSF branch | Alloc fast path |
| `src/cmem_event.c:3192-3215` | Insert arena hit in `mp_free` TLSF branch | Free fast path |
| `src/cmem_slab.c:64-108` | Update `tls_cache_flush_pool` to drain arena | Pool-switch cleanup |
| `src/cmem_slab.c:105-115` | Update `tls_cache_dtor` to drain arena | Thread-exit cleanup |

---

### Task 1: Add arena constants and data fields

**Files:**
- Modify: `include/cmem.h:183-188` (add constants after existing TLSF defines)
- Modify: `include/cmem.h:1030-1045` (add fields to `thread_cache_t`)

- [ ] **Step 1: Add constants after the existing TLSF cache constants**

In `include/cmem.h`, after the existing block:
```c
/* Per-thread TLSF cache: covers fl indices 6-13 (64B-8KB blocks). */
#define TLSF_CACHE_MIN_FL 6 /* Minimum fl index covered by cache (64B)    */
#define TLSF_CACHE_SIZES 8
#define TLSF_CACHE_MAX_SLOTS 8
```

Add:
```c
/* Per-thread TLSF arena for lock-free hot-path allocations. */
#define TLSF_ARENA_DEFAULT_SIZE (256 * 1024)  /* 256 KB per thread              */
#define TLSF_ARENA_MIN_SIZE   (64 * 1024)     /* Minimum arena size in bytes    */
```

- [ ] **Step 2: Add 4 arena fields to `thread_cache_t`**

In `include/cmem.h`, in the `thread_cache_t` struct, after the existing `tlsf_entries` field:
```c
    /* Embedded entry storage: TLSF_CACHE_SIZES * TLSF_CACHE_MAX_SLOTS entries. */
    tlsf_cache_entry_t tlsf_entries[TLSF_CACHE_SIZES * TLSF_CACHE_MAX_SLOTS];

} thread_cache_t;
```

Replace with:
```c
    /* Embedded entry storage: TLSF_CACHE_SIZES * TLSF_CACHE_MAX_SLOTS entries. */
    tlsf_cache_entry_t tlsf_entries[TLSF_CACHE_SIZES * TLSF_CACHE_MAX_SLOTS];

    /* Per-thread TLSF arena (lock-free hot path for sizes 64B-4MB). */
    void       *tlsf_arena_raw_mem;   /**< Raw memory from sys_mem_alloc (for munmap) */
    tlsf_block_t *tlsf_arena_free;    /**< Head of address-ordered free list            */
    size_t      tlsf_arena_used;      /**< Bytes currently allocated from arena          */
    size_t      tlsf_arena_total;     /**< Total arena capacity (raw_size - block_hdr)   */
} thread_cache_t;
```

- [ ] **Step 3: Build to verify no compile errors**

Run: `make clean && make all 2>&1 | tail -5`
Expected: clean build, no errors.

- [ ] **Step 4: Commit**

```bash
git add include/cmem.h
git commit -m "feat(arena): add TLSF arena constants and thread_cache_t fields"
```

---

### Task 2: Implement arena helper functions

**Files:**
- Modify: `src/cmem_slab.c` (add new static helper functions before `tls_cache_flush_pool`)

- [ ] **Step 1: Add forward declarations and helper at the top of `src/cmem_slab.c`**

Add these forward declarations right before `void tls_cache_flush_pool`:
```c
/* Forward declarations for per-thread TLSF arena helpers. */
static bool tlsf_arena_init(memory_pool_t *pool);
static void *tlsf_arena_alloc(size_t total_needed);
static void tlsf_arena_free(tlsf_block_t *block, size_t block_size);
static void tlsf_arena_drain(memory_pool_t *pool);
static void tlsf_arena_destroy(void);
```

- [ ] **Step 2: Implement `tlsf_arena_init`**

Add this function before `tls_cache_flush_pool` in `src/cmem_slab.c`:
```c
/**
 * @brief Allocate and initialize the per-thread TLSF arena.
 *
 * Allocates TLSF_ARENA_DEFAULT_SIZE bytes via sys_mem_alloc. The arena
 * starts as a single free tlsf_block_t spanning the entire region.
 *
 * @param pool The memory pool to use for sys_mem_alloc (for NUMA/coherence).
 * @return true on success, false if allocation failed.
 */
static bool tlsf_arena_init(memory_pool_t *pool)
{
    size_t arena_size = TLSF_ARENA_DEFAULT_SIZE;
    void *raw = sys_mem_alloc(pool, arena_size, TLSF_ALIGN_MASK + 1);
    if (!raw) {
        return false;
    }
    /* First block in arena covers the full region minus the block header. */
    tlsf_block_t *first = (tlsf_block_t *)raw;
    size_t block_payload = arena_size - sizeof(tlsf_block_t);
    first->size_and_flags = block_payload | BLOCK_STATE_FREE;
    first->prev_physical = NULL;
    first->next_free = NULL;
    /* Sentinel: marks the end of the arena (size 0, not free). */
    tlsf_block_t *sentinel = (tlsf_block_t *)((uint8_t *)first + arena_size);
    sentinel->size_and_flags = 0;
    sentinel->prev_physical = first;
    sentinel->next_free = NULL;

    tls_cache.tlsf_arena_raw_mem = raw;
    tls_cache.tlsf_arena_free = first;
    tls_cache.tlsf_arena_used = 0;
    tls_cache.tlsf_arena_total = block_payload;
    return true;
}
```

- [ ] **Step 3: Implement `tlsf_arena_alloc`**

Add this function after `tlsf_arena_init`:
```c
/**
 * @brief Allocate a block from the per-thread TLSF arena (lock-free).
 *
 * Scans the address-ordered free list for the first block that fits
 * (first-fit strategy). Splits the block if the remainder is large enough.
 *
 * @param total_needed Total bytes needed (tlsf_block + mp_block_header + payload + canary).
 * @return Pointer to the tlsf_block_t on success, or NULL if no fit.
 */
static void *tlsf_arena_alloc(size_t total_needed)
{
    tlsf_block_t *prev = NULL;
    tlsf_block_t *cur = tls_cache.tlsf_arena_free;
    while (cur) {
        size_t cur_size = cur->size_and_flags & BLOCK_SIZE_MASK;
        if (cur_size >= total_needed) {
            size_t remaining = cur_size - total_needed;
            if (remaining >= TLSF_MIN_BLOCK_SIZE + sizeof(tlsf_block_t)) {
                /* Split: new block takes 'total_needed', remainder becomes free. */
                tlsf_block_t *new_block = (tlsf_block_t *)((uint8_t *)cur + total_needed);
                new_block->size_and_flags = remaining | BLOCK_STATE_FREE;
                new_block->prev_physical = cur;
                new_block->next_free = cur->next_free;
                if (cur->next_free) {
                    cur->next_free->prev_physical = new_block;
                }
                cur->size_and_flags = total_needed | (cur->size_and_flags & BLOCK_STATE_PREV_FREE);
                cur->next_free = NULL;
                /* Update sentinel if we split the last block. */
                tlsf_block_t *sentinel = (tlsf_block_t *)((uint8_t *)cur + cur_size);
                sentinel->prev_physical = new_block;
            } else {
                /* Too small to split — use the whole block as-is. */
                cur->size_and_flags &= ~BLOCK_STATE_FREE;
                /* Update sentinel's prev_physical if this was the last block. */
                tlsf_block_t *sentinel = (tlsf_block_t *)((uint8_t *)cur + cur_size);
                if (sentinel->size_and_flags == 0) {
                    sentinel->prev_physical = cur;
                }
            }
            tls_cache.tlsf_arena_used += total_needed;
            return (void *)cur;
        }
        prev = cur;
        cur = cur->next_free;
    }
    return NULL; /* No suitable block found. */
}
```

- [ ] **Step 4: Implement `tlsf_arena_free`**

Add this function after `tlsf_arena_alloc`:
```c
/**
 * @brief Return a block to the per-thread TLSF arena (lock-free).
 *
 * Inserts the block into the address-ordered free list and coalesces
 * with the next neighbor if physically adjacent and free.
 *
 * @param block     The tlsf_block_t to free.
 * @param block_size The original size of the block (from size_and_flags at free time).
 */
static void tlsf_arena_free(tlsf_block_t *block, size_t block_size)
{
    block->size_and_flags = block_size | BLOCK_STATE_FREE;
    block->prev_physical = NULL;
    block->next_free = NULL;
    tls_cache.tlsf_arena_used -= block_size;

    /* Insert in address order. */
    tlsf_block_t *prev = NULL;
    tlsf_block_t *cur = tls_cache.tlsf_arena_free;
    while (cur && cur < block) {
        prev = cur;
        cur = cur->next_free;
    }
    block->next_free = cur;
    if (cur) {
        cur->prev_physical = block;
    }
    if (prev) {
        prev->next_free = block;
    } else {
        tls_cache.tlsf_arena_free = block;
    }
    block->prev_physical = prev;

    /* Coalesce with next neighbor (address-ordered, so next is always > cur). */
    if (block->next_free) {
        tlsf_block_t *next = block->next_free;
        size_t block_end = ((size_t)block) + (block->size_and_flags & BLOCK_SIZE_MASK);
        if ((size_t)next == block_end && (next->size_and_flags & BLOCK_STATE_FREE)) {
            size_t next_size = next->size_and_flags & BLOCK_SIZE_MASK;
            block->size_and_flags = (block_size + sizeof(tlsf_block_t) + next_size) | BLOCK_STATE_FREE;
            block->next_free = next->next_free;
            if (next->next_free) {
                next->next_free->prev_physical = block;
            }
            /* Update sentinel if we merged the last block. */
            tlsf_block_t *sentinel = (tlsf_block_t *)((uint8_t *)block + (block->size_and_flags & BLOCK_SIZE_MASK));
            sentinel->prev_physical = block;
        }
    }
}
```

- [ ] **Step 5: Implement `tlsf_arena_drain` and `tlsf_arena_destroy`**

Add these functions after `tlsf_arena_free`:
```c
/**
 * @brief Drain all remaining free blocks from the per-thread arena back
 *        to the shared pool's TLSF free lists.
 *
 * Must be called under the pool lock (or when the pool is single-threaded).
 *
 * @param pool The owning memory pool.
 */
static void tlsf_arena_drain(memory_pool_t *pool)
{
    if (!tls_cache.tlsf_arena_free) {
        return;
    }
    tlsf_pool_t *tpool = pool->tlsf_root;
    if (!tpool) {
        /* Pool has no TLSF region yet; nothing to drain to.
         * The arena memory will be freed in tlsf_arena_destroy. */
        tls_cache.tlsf_arena_free = NULL;
        tls_cache.tlsf_arena_used = 0;
        return;
    }
    /* Walk the arena free list and insert each block into the shared pool. */
    tlsf_block_t *cur = tls_cache.tlsf_arena_free;
    while (cur) {
        tlsf_block_t *next = cur->next_free;
        size_t cur_size = cur->size_and_flags & BLOCK_SIZE_MASK;
        if (cur_size >= TLSF_MIN_BLOCK_SIZE) {
            /* Lock the appropriate bucket(s) in the shared pool. */
            int fl = 0, sl = 0;
            tlsf_mapping_insert(cur_size, &fl, &sl);
            pthread_mutex_lock(&tpool->bucket_locks[fl][sl]);
            cur->next_free = tpool->blocks[fl][sl];
            cur->prev_free = NULL;
            if (tpool->blocks[fl][sl]) {
                tpool->blocks[fl][sl]->prev_free = cur;
            }
            tpool->blocks[fl][sl] = cur;
            tpool->fl_bitmap |= (1U << fl);
            tpool->sl_bitmap[fl] |= (1U << sl);
            pthread_mutex_unlock(&tpool->bucket_locks[fl][sl]);
        }
        cur = next;
    }
    tls_cache.tlsf_arena_free = NULL;
    tls_cache.tlsf_arena_used = 0;
}

/**
 * @brief Release the per-thread arena memory back to the OS.
 */
static void tlsf_arena_destroy(void)
{
    if (tls_cache.tlsf_arena_raw_mem) {
        /* We need the pool to know how to free (custom allocator, huge pages, etc.).
         * The drain function already ran, so we pass NULL for pool to use default free. */
        sys_mem_free(NULL, tls_cache.tlsf_arena_raw_mem, TLSF_ARENA_DEFAULT_SIZE);
        tls_cache.tlsf_arena_raw_mem = NULL;
        tls_cache.tlsf_arena_free = NULL;
        tls_cache.tlsf_arena_used = 0;
        tls_cache.tlsf_arena_total = 0;
    }
}
```

- [ ] **Step 6: Build to verify no compile errors**

Run: `make clean && make all 2>&1 | tail -5`
Expected: clean build.

- [ ] **Step 7: Run existing tests to verify no regression**

Run: `make test 2>&1 | tail -3`
Expected: `ALL CMEM UNIT TESTS PASSED SUCCESSFULLY!`

- [ ] **Step 8: Commit**

```bash
git add src/cmem_slab.c
git commit -m "feat(arena): implement per-thread TLSF arena init/alloc/free/drain"
```

---

### Task 3: Integrate arena into allocation path

**Files:**
- Modify: `src/cmem_event.c:2196-2260` (`mp_alloc_internal` TLSF branch)

- [ ] **Step 1: Insert arena hit after TLSF cache miss**

In `src/cmem_event.c`, in the `mp_alloc_internal` function, after the existing TLSF cache hit block (around line 2218), before the `else { tlsf_free ... }` or the shared pool chain, add:

Find this block (around line 2215-2225):
```c
            tlsf_block_t *block = (tlsf_block_t *)entry->block;
            tlsf_pool_t *tpool = (tlsf_pool_t *)entry->tpool;
            mp_block_header_t *header =
                (mp_block_header_t *)((uint8_t *)block + sizeof(tlsf_block_t));
            block->size_and_flags = total_needed | (block->size_and_flags & BLOCK_STATE_PREV_FREE);
            header->magic = MP_MAGIC_HEAD;
            header->alloc_type = ALLOC_TYPE_TLSF;
            header->slab_class = 0;
            header->flags = 0;
            header->requested_size = size;
            header->usable_size = total_needed - sizeof(tlsf_block_t) - sizeof(mp_block_header_t);
            header->raw_base = block;
            header->subpool = tpool;
            header->alloc_file = NULL;
            header->alloc_line = 0;
            header->alloc_func = NULL;
            header->backtrace_depth = 0;
            ptr = (void *)((uint8_t *)header + sizeof(mp_block_header_t));
```

After this block (before the closing `} else if (size <= TLSF_MAX_SIZE ...)` else), add:
```c
        /* Per-thread arena hit: lock-free allocation from local arena. */
        if (!ptr) {
            if (!tls_cache.tlsf_arena_free) {
                /* Arena not initialized or exhausted — try to create/reinit. */
                if (tls_cache.tlsf_arena_raw_mem == NULL) {
                    tlsf_arena_init(pool);
                }
            }
            if (tls_cache.tlsf_arena_free) {
                void *arena_block = tlsf_arena_alloc(total_needed);
                if (arena_block) {
                    tlsf_block_t *block = (tlsf_block_t *)arena_block;
                    mp_block_header_t *header =
                        (mp_block_header_t *)((uint8_t *)block + sizeof(tlsf_block_t));
                    block->size_and_flags = total_needed | BLOCK_STATE_PREV_FREE;
                    header->magic = MP_MAGIC_HEAD;
                    header->alloc_type = ALLOC_TYPE_TLSF;
                    header->slab_class = 0;
                    header->flags = 0;
                    header->requested_size = size;
                    header->usable_size = total_needed - sizeof(tlsf_block_t) - sizeof(mp_block_header_t);
                    header->raw_base = block;
                    header->subpool = NULL;  /* Arena-owned, no tpool */
                    header->alloc_file = NULL;
                    header->alloc_line = 0;
                    header->alloc_func = NULL;
                    header->backtrace_depth = 0;
                    ptr = (void *)((uint8_t *)header + sizeof(mp_block_header_t));
                    if (pool->flags & MP_FLAG_DEBUG_CANARY) {
                        uint8_t *canary = (uint8_t *)ptr + size;
                        *canary = MP_CANARY_BYTE;
                    }
                    if (pool->flags & MP_FLAG_ZERO_ON_ALLOC) {
                        memset(ptr, 0, size);
                    }
                }
            }
        }
```

- [ ] **Step 2: Build to verify**

Run: `make clean && make all 2>&1 | grep -E 'error|warning' | head -10`
Expected: no errors (warnings OK for now).

- [ ] **Step 3: Run tests**

Run: `make test 2>&1 | tail -3`
Expected: `ALL CMEM UNIT TESTS PASSED SUCCESSFULLY!`

- [ ] **Step 4: Commit**

```bash
git add src/cmem_event.c
git commit -m "feat(arena): integrate per-thread TLSF arena into alloc path"
```

---

### Task 4: Integrate arena into free path

**Files:**
- Modify: `src/cmem_event.c:3192-3215` (`mp_free` TLSF branch)

- [ ] **Step 1: Insert arena free after TLSF cache hit check**

In `src/cmem_event.c`, in the `mp_free` function, find this block (around line 3187-3215):
```c
    } else if (alloc_type == ALLOC_TYPE_TLSF) {
        /* TLSF cache push: stash the block in the per-thread cache to avoid
         * locking on the next allocation of a similar size. */
        tls_cache_validate_owner(pool);
        tlsf_block_t *block = (tlsf_block_t *)header->raw_base;
        size_t block_size = block->size_and_flags & BLOCK_SIZE_MASK;
        int fl = tlsf_block_size_to_fl(block_size);
        int cache_idx = fl - TLSF_CACHE_MIN_FL;
        if (cache_idx >= 0 && cache_idx < TLSF_CACHE_SIZES &&
            tls_cache.tlsf_counts[(unsigned)cache_idx] < TLSF_CACHE_MAX_SLOTS) {
            /* ... existing cache push ... */
        } else {
            tlsf_free(pool, header);
        }
```

Replace the `else { tlsf_free(pool, header); }` branch with:
```c
        } else {
            /* Check if block belongs to the per-thread arena. */
            if (tls_cache.tlsf_arena_free &&
                (uintptr_t)block >= (uintptr_t)tls_cache.tlsf_arena_raw_mem &&
                (uintptr_t)block < ((uintptr_t)tls_cache.tlsf_arena_raw_mem + TLSF_ARENA_DEFAULT_SIZE)) {
                tlsf_arena_free(block, block_size);
            } else {
                tlsf_free(pool, header);
            }
        }
```

- [ ] **Step 2: Build and test**

Run: `make clean && make test 2>&1 | tail -3`
Expected: `ALL CMEM UNIT TESTS PASSED SUCCESSFULLY!`

- [ ] **Step 3: Commit**

```bash
git commit -am "feat(arena): integrate per-thread TLSF arena into free path"
```

---

### Task 5: Wire drain into pool-switch and thread-exit

**Files:**
- Modify: `src/cmem_slab.c:64-115` (`tls_cache_flush_pool` and `tls_cache_dtor`)

- [ ] **Step 1: Update `tls_cache_flush_pool` to drain arena**

In `src/cmem_slab.c`, in `tls_cache_flush_pool`, before the existing slab/TLSF cache flush loops, add:
```c
void tls_cache_flush_pool(memory_pool_t *pool)
{
    if (tls_cache.owner_pool == pool) {
        tls_cache.owner_pool = NULL;
        /* Drain per-thread TLSF arena back to the shared pool. */
        if (tls_cache.tlsf_arena_free) {
            tlsf_arena_drain(pool);
        }
        /* Existing slab cache flush... */
```

- [ ] **Step 2: Update `tls_cache_dtor` to drain and destroy arena**

In `src/cmem_slab.c`, update `tls_cache_dtor`:
```c
static void tls_cache_dtor(void *arg)
{
    (void)arg;
    if (tls_cache.tlsf_arena_free) {
        /* Drain remaining blocks to owner pool before destroying. */
        if (tls_cache.owner_pool) {
            tlsf_arena_drain(tls_cache.owner_pool);
        }
        tlsf_arena_destroy();
    }
    if (tls_cache.owner_pool) {
        tls_cache_flush_pool(tls_cache.owner_pool);
    }
}
```

- [ ] **Step 3: Build and run all tests**

Run: `make test 2>&1 | tail -3 && make test_advanced 2>&1 | tail -3 && make test_cpp 2>&1 | tail -3`
Expected: all three pass.

- [ ] **Step 4: Commit**

```bash
git commit -m "feat(arena): wire arena drain into pool-switch and thread-exit"
```

---

### Task 6: TSan test and final validation

- [ ] **Step 1: Run TSan build**

Run: `make CONFIG=tsan test 2>&1 | grep -E 'ThreadSanitizer|WARNING|ALL|Error'`
Expected: `ALL CMEM UNIT TESTS PASSED SUCCESSFULLY!` with zero TSan warnings.

- [ ] **Step 2: Run format check**

Run: `make format-check 2>&1`
Expected: clean (exit code 0).

- [ ] **Step 3: Run full test suite**

Run: `make test && make test_advanced && make test_cpp`
Expected: all pass.

- [ ] **Step 4: Final commit (if any formatting fixes needed)**

```bash
git add -A
git commit -m "style: format per-thread TLSF arena code"
```

---

### Verification Checklist

- [ ] `make test` passes (ASan + UBSan)
- [ ] `make CONFIG=tsan test` passes with 0 warnings
- [ ] `make test_advanced` passes
- [ ] `make test_cpp` passes
- [ ] `make format-check` passes
- [ ] Arena is initialized on first TLSF alloc (not at pool create time)
- [ ] Arena is drained when switching pools
- [ ] Arena is destroyed on thread exit
- [ ] Arena free coalesces with next neighbor
- [ ] No double-free possible (arena blocks have correct magic)
