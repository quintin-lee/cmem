/**
 * @file cmem_sys.c
 * @brief System memory allocator and platform-specific helpers.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cmem.h"
#include "cmem_internal.h"
#include <sched.h>

int cmem_sched_getcpu(void)
{
#ifdef __APPLE__
    return 0;
#else
    return sched_getcpu();
#endif
}

#ifdef _WIN32
void cmem_munmap(void *ptr, size_t size)
{
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
}

void *cmem_aligned_malloc(size_t size, size_t alignment)
{
    void *ptr = malloc(size + alignment - 1 + sizeof(void *));
    if (!ptr) {
        return NULL;
    }
    uint8_t *aligned =
        (uint8_t *)(((uintptr_t)ptr + sizeof(void *) + alignment - 1) & ~(alignment - 1));
    ((void **)aligned)[-1] = ptr;
    return aligned;
}

void cmem_aligned_free(void *ptr)
{
    if (ptr) {
        free(((void **)ptr)[-1]);
    }
}
#else
void cmem_munmap(void *ptr, size_t size)
{
    munmap(ptr, size);
}
#endif

void *sys_mem_alloc(memory_pool_t *pool, size_t size, size_t alignment)
{
    void *ptr = NULL;
#ifdef _WIN32
    if (pool && pool->has_custom_sys_alloc && pool->sys_allocator.sys_alloc) {
        return pool->sys_allocator.sys_alloc(size, pool->sys_allocator.user_data);
    }
    if (pool && (pool->flags & MP_FLAG_HUGE_PAGES)) {
        return NULL;
    } else if (pool && (pool->flags & MP_FLAG_GUARD_PAGES)) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        size_t page_sz         = si.dwPageSize;
        size_t aligned_payload = (size + page_sz - 1) & ~(page_sz - 1);
        size_t total_map       = page_sz + aligned_payload + page_sz;
        void  *raw_map = VirtualAlloc(NULL, total_map, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!raw_map) {
            return NULL;
        }

        uint8_t *base = (uint8_t *)raw_map;
        DWORD    old_prot;
        VirtualProtect(base, page_sz, PAGE_NOACCESS, &old_prot);
        VirtualProtect(base + page_sz + aligned_payload, page_sz, PAGE_NOACCESS, &old_prot);

        ptr = base + page_sz;
    } else if (alignment > sizeof(void *)) {
        ptr = cmem_aligned_malloc(size, alignment);
    } else {
        ptr = cmem_aligned_malloc(size, sizeof(void *));
    }
#else
    if (pool && pool->has_custom_sys_alloc && pool->sys_allocator.sys_alloc) {
        return pool->sys_allocator.sys_alloc(size, pool->sys_allocator.user_data);
    }
    if (pool && (pool->flags & MP_FLAG_HUGE_PAGES)) {
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
        ptr = mmap(
            NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (ptr == MAP_FAILED) {
            ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#ifdef MADV_HUGEPAGE
            if (ptr != MAP_FAILED) {
                madvise(ptr, size, MADV_HUGEPAGE);
            }
#endif
        }
        if (ptr == MAP_FAILED) {
            return NULL;
        }
    } else if (pool && (pool->flags & MP_FLAG_GUARD_PAGES)) {
        long   pg              = sysconf(_SC_PAGESIZE);
        size_t page_sz         = (pg > 0) ? (size_t)pg : 4096;
        size_t aligned_payload = (size + page_sz - 1) & ~(page_sz - 1);
        size_t total_map       = page_sz + aligned_payload + page_sz;
        void  *raw_map =
            mmap(NULL, total_map, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw_map == MAP_FAILED) {
            return NULL;
        }

        uint8_t *base = (uint8_t *)raw_map;
        mprotect(base, page_sz, PROT_NONE);
        mprotect(base + page_sz + aligned_payload, page_sz, PROT_NONE);

        ptr = base + page_sz;
    } else if (alignment > sizeof(void *)) {
        if (posix_memalign(&ptr, alignment, size) != 0) {
            return NULL;
        }
    } else {
        ptr = malloc(size);
    }

#if defined(__linux__) && defined(SYS_mbind)
    if (pool && pool->numa_node >= 0 && ptr) {
        unsigned long nodemask = (1UL << pool->numa_node);
        syscall(SYS_mbind, ptr, size, CMEM_MPOL_BIND, &nodemask, sizeof(nodemask) * 8, 0);
    }
#endif
#endif

    return ptr;
}

void sys_mem_free(memory_pool_t *pool, void *ptr, size_t size)
{
    if (pool->flags & MP_FLAG_STATIC_BUFFER) {
        return;
    }
    if (pool->has_custom_sys_alloc && pool->sys_allocator.sys_free) {
        pool->sys_allocator.sys_free(ptr, size, pool->sys_allocator.user_data);
        return;
    }
#ifdef _WIN32
    if (pool->flags & MP_FLAG_HUGE_PAGES || pool->flags & MP_FLAG_GUARD_PAGES) {
        if (pool->flags & MP_FLAG_GUARD_PAGES) {
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            size_t   page_sz = si.dwPageSize;
            uint8_t *base    = (uint8_t *)ptr - page_sz;
            VirtualFree(base, 0, MEM_RELEASE);
        } else if (pool->flags & MP_FLAG_HUGE_PAGES) {
            cmem_munmap(ptr, size);
        }
        return;
    }
    cmem_aligned_free(ptr);
#else
    if (pool->flags & MP_FLAG_HUGE_PAGES) {
        munmap(ptr, size);
        return;
    }
    if (pool->flags & MP_FLAG_GUARD_PAGES) {
        long     pg              = sysconf(_SC_PAGESIZE);
        size_t   page_sz         = (pg > 0) ? (size_t)pg : 4096;
        size_t   aligned_payload = (size + page_sz - 1) & ~(page_sz - 1);
        size_t   total_map       = page_sz + aligned_payload + page_sz;
        uint8_t *raw_map         = (uint8_t *)ptr - page_sz;
        munmap(raw_map, total_map);
        return;
    }
    free(ptr);
#endif
}

bool mp_expand_pool(memory_pool_t *pool, size_t additional_bytes)
{
    if (!pool || additional_bytes == 0) {
        return false;
    }
    if (pool->flags & MP_FLAG_STATIC_BUFFER) {
        return false;
    }

    pool_lock(pool);
    tlsf_pool_t *new_tlsf = tlsf_create_pool_custom(pool, additional_bytes, NULL);
    if (!new_tlsf) {
        pool_unlock(pool);
        return false;
    }

    new_tlsf->next  = pool->tlsf_root;
    pool->tlsf_root = new_tlsf;
    pool->stats.total_pool_size += additional_bytes + sizeof(tlsf_pool_t);
    pool_unlock(pool);

    trigger_event(pool, MP_EVENT_ALLOC, NULL, additional_bytes);
    return true;
}

bool mp_can_expand(memory_pool_t *pool)
{
    if (!pool) {
        return false;
    }
    if (pool->flags & MP_FLAG_STATIC_BUFFER) {
        return false;
    }
    return true;
}

size_t mp_get_expandable_size(memory_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    pool_rdlock(pool);
    size_t expandable = pool->stats.max_memory_limit > 0
                            ? (pool->stats.max_memory_limit - pool->stats.total_pool_size)
                            : 0;
    pool_rdunlock(pool);
    return expandable > 0 ? expandable : 0;
}

int mp_lock_memory(memory_pool_t *pool, void *addr, size_t length)
{
    if (!pool || !addr || length == 0) {
        return -1;
    }
    (void)pool;
#ifdef __linux__
    if (mlock(addr, length) != 0) {
        return -1;
    }
#endif
    return 0;
}

int mp_unlock_memory(memory_pool_t *pool, void *addr, size_t length)
{
    if (!pool || !addr || length == 0) {
        return -1;
    }
    (void)pool;
#ifdef __linux__
    if (munlock(addr, length) != 0) {
        return -1;
    }
#endif
    return 0;
}

int mp_protect_from_dump(memory_pool_t *pool, void *addr, size_t length)
{
    if (!pool || !addr || length == 0) {
        return -1;
    }
    (void)pool;
#ifdef __linux__
    if (madvise(addr, length, MADV_DONTDUMP) != 0) {
        return -1;
    }
#endif
    return 0;
}

void mp_secure_zero(memory_pool_t *pool, void *ptr, size_t length)
{
    if (!pool || !ptr || length == 0) {
        return;
    }
#if defined(__GNUC__) || defined(__clang__)
    __builtin_memset(ptr, 0, length);
    __asm__ __volatile__("" : : "r"(ptr) : "memory");
#else
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (length--) {
        *p++ = 0;
    }
#endif
}

void mp_set_encrypted_memory(memory_pool_t *pool, bool enable)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    if (enable) {
        pool->flags = (mp_flags_t)(pool->flags | MP_FLAG_ENCRYPTED_MEMORY);
    } else {
        pool->flags = (mp_flags_t)(pool->flags & ~MP_FLAG_ENCRYPTED_MEMORY);
    }
    pool_unlock(pool);
}

void mp_set_memory_limit(memory_pool_t *pool, size_t max_bytes)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    pool->stats.max_memory_limit = max_bytes;
    pool_unlock(pool);
}

size_t mp_compact(memory_pool_t *pool)
{
    if (!pool || (pool->flags & MP_FLAG_STATIC_BUFFER)) {
        return 0;
    }
    pool_lock(pool);

    size_t freed_bytes = 0;

    for (int c = 0; c < SLAB_CLASS_COUNT; c++) {
        mp_slab_class_t *sc   = &pool->slab_classes[c];
        mp_slab_page_t  *curr = sc->partial_pages;

        while (curr) {
            mp_slab_page_t *next = curr->next;
            if (curr->free_count == curr->total_slots) {
                if (curr->prev) {
                    curr->prev->next = curr->next;
                } else {
                    sc->partial_pages = curr->next;
                }
                if (curr->next) {
                    curr->next->prev = curr->prev;
                }

#ifdef _WIN32
                cmem_aligned_free(curr->page_raw_mem);
#else
                cmem_munmap(curr->page_raw_mem, SLAB_PAGE_SIZE);
#endif
                freed_bytes += SLAB_PAGE_SIZE;
                if (pool->stats.total_pool_size >= SLAB_PAGE_SIZE) {
                    pool->stats.total_pool_size -= SLAB_PAGE_SIZE;
                }
            }
            curr = next;
        }
    }

    trigger_event(pool, MP_EVENT_COMPACT, NULL, freed_bytes);
    pool_unlock(pool);
    return freed_bytes;
}

size_t mp_purge_lazy(memory_pool_t *pool)
{
    if (!pool || (pool->flags & MP_FLAG_STATIC_BUFFER)) {
        return 0;
    }
    pool_lock(pool);

    size_t purged_bytes = 0;
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    long page_sz = (long)si.dwPageSize;
#else
    long page_sz = sysconf(_SC_PAGESIZE);
#endif
    if (page_sz <= 0) {
        page_sz = 4096;
    }

    for (int c = 0; c < SLAB_CLASS_COUNT; c++) {
        mp_slab_class_t *sc   = &pool->slab_classes[c];
        mp_slab_page_t  *curr = sc->partial_pages;
        while (curr) {
            if (curr->free_count == curr->total_slots && curr->page_raw_mem) {
                uintptr_t start         = (uintptr_t)curr->page_raw_mem + sizeof(mp_slab_page_t);
                uintptr_t aligned_start = (start + page_sz - 1) & ~((uintptr_t)page_sz - 1);
                uintptr_t end           = (uintptr_t)curr->page_raw_mem + SLAB_PAGE_SIZE;
                uintptr_t aligned_end   = end & ~((uintptr_t)page_sz - 1);

                if (aligned_end > aligned_start) {
                    size_t purge_sz = aligned_end - aligned_start;
#ifdef MADV_DONTNEED
                    madvise((void *)aligned_start, purge_sz, MADV_DONTNEED);
                    purged_bytes += purge_sz;
#else
                    (void)purge_sz;
#endif
                }
            }
            curr = curr->next;
        }
    }

    pool_unlock(pool);
    printf("[CMEM PERF] Lazy RSS physical memory purge completed: %zu bytes released\n",
           purged_bytes);
    return purged_bytes;
}

int mp_madvise(memory_pool_t *pool, void *addr, size_t length, int advice)
{
    if (!addr || length == 0) {
        return -1;
    }
    (void)pool;

#ifdef _WIN32
    (void)pool;
    (void)advice;
    (void)addr;
    (void)length;
    return 0;
#else
    long   pg      = sysconf(_SC_PAGESIZE);
    size_t page_sz = (pg > 0) ? (size_t)pg : 4096;

    uintptr_t start         = (uintptr_t)addr;
    uintptr_t aligned_start = (start + page_sz - 1) & ~(page_sz - 1);
    uintptr_t end           = start + length;
    uintptr_t aligned_end   = end & ~(page_sz - 1);

    if (aligned_end <= aligned_start) {
        return 0;
    }

    size_t aligned_len = aligned_end - aligned_start;

#ifdef MADV_DONTNEED
    return madvise((void *)aligned_start, aligned_len, advice);
#else
    (void)advice;
    (void)aligned_len;
    return 0;
#endif
#endif
}

size_t mp_trim(memory_pool_t *pool, size_t pad)
{
    if (!pool) {
        return 0;
    }

    size_t total_reclaimed = 0;

    total_reclaimed += mp_compact(pool);
    total_reclaimed += mp_purge_lazy(pool);

    pool_lock(pool);
    memory_pool_t *child = pool->first_child;
    while (child) {
        memory_pool_t *next = child->next_sibling;
        pool_unlock(pool);
        total_reclaimed += mp_trim(child, pad);
        pool_lock(pool);
        child = next;
    }
    pool_unlock(pool);

    return total_reclaimed;
}
