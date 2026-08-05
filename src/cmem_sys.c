/**
 * @file cmem_sys.c
 * @brief System memory allocator and platform-specific helpers.
 *
 * This module is the thin OS-facing layer of cmem. It owns every
 * interaction with the kernel's memory facilities: mmap/munmap, huge
 * pages, guard pages, NUMA binding, mlock, madvise, and secure zeroing.
 * Everything above this layer (slab, TLSF, block headers) treats memory
 * as opaque byte ranges obtained through sys_mem_alloc()/sys_mem_free().
 *
 * Responsibilities:
 *  - Raw backing allocation with configurable flags (HUGE_PAGES, GUARD_PAGES)
 *  - Platform quirks: Windows VirtualAlloc vs POSIX mmap
 *  - Optional custom sys-allocator vtable override (mp_sys_allocator_t)
 *  - NUMA policy application on freshly mapped memory
 *  - Pool expansion, compaction, lazy RSS purge and madvise hints
 *  - Memory security: mlock/munlock, MADV_DONTDUMP, secure zeroing
 */

#include "cmem.h"
#include "cmem_internal.h"
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/**
 * @brief Return the ID of the CPU the calling thread is running on.
 *
 * Used by the per-CPU freelist logic to pick the right lock-free list.
 * The value is cached in thread-local storage and refreshed every
 * CMEM_CPU_CACHE_REFRESH calls. A stale index is benign: per-CPU lists
 * are guarded by atomic CAS, so popping from a previous CPU's list is
 * still correct — only cache affinity is slightly reduced. This avoids
 * the sched_getcpu() syscall on every allocation.
 *
 * macOS/Windows have no sched_getcpu(); a constant is returned there
 * since the per-CPU lists are treated as a single logical list.
 *
 * @return CPU index (>= 0), or 0 on platforms without sched_getcpu().
 */
#define CMEM_CPU_CACHE_REFRESH 1024

int cmem_current_cpu(void)
{
#ifdef __APPLE__
    return 0;
#elif defined(_WIN32)
    return 0;
#else
    static MP_THREAD_LOCAL int cpu_cache = -1;
    static MP_THREAD_LOCAL int refresh_countdown = 0;

    if (cpu_cache < 0 || refresh_countdown == 0) {
        int cpu = sched_getcpu();
        cpu_cache = cpu < 0 ? 0 : cpu;
        refresh_countdown = CMEM_CPU_CACHE_REFRESH;
    }
    refresh_countdown--;
    return cpu_cache;
#endif
}

#define CMEM_MAX_NUMA_NODES 64
#define CMEM_MAX_CPUS 1024
#define CMEM_DECIMAL_BASE 10

static cmem_numa_topology_t g_numa_topo;
static atomic_flag g_numa_topo_init = ATOMIC_FLAG_INIT;

/**
 * @brief Expand a sysfs list like "0-3,8" or "0" into an integer array.
 *
 * @param list    NUL-terminated sysfs list string.
 * @param ids     Output array (at least max_ids entries).
 * @param max_ids Capacity of the output array.
 * @return Number of IDs written, or -1 on malformed input.
 */
static int cmem_parse_idlist(const char *list, int *ids, int max_ids)
{
    int count = 0;
    const char *cursor = list;
    while (*cursor != '\0' && count < max_ids) {
        char *end = NULL;
        long first = strtol(cursor, &end, CMEM_DECIMAL_BASE);
        if (end == cursor) {
            return -1;
        }
        long last = first;
        if (*end == '-') {
            cursor = end + 1;
            last = strtol(cursor, &end, CMEM_DECIMAL_BASE);
            if (end == cursor) {
                return -1;
            }
        }
        if (first < 0 || last < first) {
            return -1;
        }
        while (first <= last && count < max_ids) {
            ids[count++] = (int)first++;
        }
        if (*end == ',') {
            cursor = end + 1;
        } else if (*end != '\0') {
            return -1;
        }
    }
    return count;
}

/**
 * @brief Probe the system NUMA topology once, lazily.
 *
 * Reads /sys/devices/system/node/online and /sys/devices/system/cpu/possible,
 * then maps each CPU to its owning node via per-node cpulist files.  On any
 * failure (or on non-Linux) the topology falls back to a single node with no
 * cpu->node map, which makes the query APIs return conservative defaults.
 */
static void cmem_numa_probe(void)
{
    int node_ids[CMEM_MAX_NUMA_NODES];
    int cpu_ids[CMEM_MAX_CPUS];
    FILE *node_file = fopen("/sys/devices/system/node/online", "r");
    if (node_file == NULL) {
        g_numa_topo.node_count = 1;
        g_numa_topo.cpu_count = 0;
        g_numa_topo.cpu_to_node = NULL;
        return;
    }
    char node_list[256];
    if (fgets(node_list, sizeof(node_list), node_file) == NULL) {
        (void)fclose(node_file);
        g_numa_topo.node_count = 1;
        g_numa_topo.cpu_count = 0;
        g_numa_topo.cpu_to_node = NULL;
        return;
    }
    (void)fclose(node_file);

    int node_count = cmem_parse_idlist(node_list, node_ids, CMEM_MAX_NUMA_NODES);
    if (node_count <= 0) {
        g_numa_topo.node_count = 1;
        g_numa_topo.cpu_count = 0;
        g_numa_topo.cpu_to_node = NULL;
        return;
    }

    FILE *cpu_file = fopen("/sys/devices/system/cpu/possible", "r");
    if (cpu_file == NULL) {
        g_numa_topo.node_count = node_count;
        g_numa_topo.cpu_count = 0;
        g_numa_topo.cpu_to_node = NULL;
        return;
    }
    char cpu_list[1024];
    if (fgets(cpu_list, sizeof(cpu_list), cpu_file) == NULL) {
        (void)fclose(cpu_file);
        g_numa_topo.node_count = node_count;
        g_numa_topo.cpu_count = 0;
        g_numa_topo.cpu_to_node = NULL;
        return;
    }
    (void)fclose(cpu_file);

    int cpu_count = cmem_parse_idlist(cpu_list, cpu_ids, CMEM_MAX_CPUS);
    if (cpu_count <= 0) {
        g_numa_topo.node_count = node_count;
        g_numa_topo.cpu_count = 0;
        g_numa_topo.cpu_to_node = NULL;
        return;
    }

    int *cpu_to_node = (int *)calloc((size_t)cpu_count, sizeof(int));
    if (cpu_to_node == NULL) {
        g_numa_topo.node_count = node_count;
        g_numa_topo.cpu_count = 0;
        g_numa_topo.cpu_to_node = NULL;
        return;
    }
    for (int i = 0; i < cpu_count; i++) {
        cpu_to_node[i] = 0;
    }

    for (int node_idx = 0; node_idx < node_count; node_idx++) {
        char cpulist_path[64];
        int written = snprintf(cpulist_path,
                               sizeof(cpulist_path),
                               "/sys/devices/system/node/node%d/cpulist",
                               node_ids[node_idx]);
        if (written <= 0 || (size_t)written >= sizeof(cpulist_path)) {
            continue;
        }
        FILE *list_file = fopen(cpulist_path, "r");
        if (list_file == NULL) {
            continue;
        }
        char line[1024];
        if (fgets(line, sizeof(line), list_file) == NULL) {
            (void)fclose(list_file);
            continue;
        }
        (void)fclose(list_file);
        int node_cpus[CMEM_MAX_CPUS];
        int node_cpu_count = cmem_parse_idlist(line, node_cpus, CMEM_MAX_CPUS);
        for (int cpu_idx = 0; cpu_idx < node_cpu_count; cpu_idx++) {
            int cpu = node_cpus[cpu_idx];
            if (cpu >= 0 && cpu < cpu_count) {
                cpu_to_node[cpu] = node_ids[node_idx];
            }
        }
    }

    g_numa_topo.node_count = node_count;
    g_numa_topo.cpu_count = cpu_count;
    g_numa_topo.cpu_to_node = cpu_to_node;
}

/**
 * @brief Return the number of NUMA nodes on this system.
 *
 * @return Node count (>= 1); 1 when unknown or unsupported.
 */
int cmem_numa_node_count(void)
{
    if (!atomic_flag_test_and_set_explicit(&g_numa_topo_init, memory_order_acquire)) {
        cmem_numa_probe();
    }
    return g_numa_topo.node_count > 0 ? g_numa_topo.node_count : 1;
}

/**
 * @brief Return the NUMA node owning the given CPU index.
 *
 * @param cpu CPU index (>= 0)
 * @return Node ID, or 0 when unknown/unsupported/out of range.
 */
int cmem_cpu_to_node(int cpu)
{
    if (cpu < 0) {
        return 0;
    }
    (void)cmem_numa_node_count();
    if (g_numa_topo.cpu_to_node == NULL || cpu >= g_numa_topo.cpu_count) {
        return 0;
    }
    return g_numa_topo.cpu_to_node[cpu];
}

/**
 * @brief Return the NUMA node the calling thread is currently running on.
 *
 * @return Node ID, or -1 when unavailable (non-Linux or unknown CPU).
 */
int cmem_numa_current_node(void)
{
#if defined(__linux__) && !defined(__APPLE__)
    int cpu = sched_getcpu();
    if (cpu < 0) {
        return -1;
    }
    return cmem_cpu_to_node(cpu);
#else
    (void)cmem_numa_node_count();
    return -1;
#endif
}

#ifdef _WIN32
/** @brief Release a mapping previously obtained via sys_mem_alloc() (Windows). */
void cmem_munmap(void *ptr, size_t size)
{
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
}

/**
 * @brief Over-aligned malloc that keeps the original pointer for free().
 *
 * The raw allocation is stored in the word immediately before the aligned
 * pointer so cmem_aligned_free() can recover it.  `alignment` must be a
 * power of two.
 *
 * @return Aligned pointer, or NULL on allocation failure.
 */
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

/** @brief Free a pointer returned by cmem_aligned_malloc(). */
void cmem_aligned_free(void *ptr)
{
    if (ptr) {
        free(((void **)ptr)[-1]);
    }
}
#else
/** @brief Release a mapping previously obtained via sys_mem_alloc() (POSIX). */
void cmem_munmap(void *ptr, size_t size)
{
    munmap(ptr, size);
}
#endif

/**
 * @brief Allocate `size` bytes of backing memory honouring pool flags.
 *
 * Central allocation entry point for every cmem backend.  The chosen path
 * depends on pool configuration, in priority order:
 *  1. Custom sys-allocator vtable, if installed (bypasses the OS entirely);
 *  2. MP_FLAG_HUGE_PAGES  -> mmap with MAP_HUGETLB, falling back to plain
 *     mmap + MADV_HUGEPAGE when the kernel has no huge pages available;
 *  3. MP_FLAG_GUARD_PAGES -> mmap payload surrounded by PROT_NONE pages to
 *     catch out-of-bounds reads/writes;
 *  4. otherwise -> aligned malloc (posix_memalign / cmem_aligned_malloc).
 * On Linux, MPOL_BIND is applied afterwards to pin the pages to the pool's
 * configured NUMA node.
 *
 * @param pool      Owning pool (may be NULL for raw OS allocation).
 * @param size      Number of bytes requested.
 * @param alignment Alignment for the returned pointer.
 * @return Pointer to usable memory, or NULL on failure.
 */
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
        size_t page_sz = si.dwPageSize;
        size_t aligned_payload = (size + page_sz - 1) & ~(page_sz - 1);
        size_t total_map = page_sz + aligned_payload + page_sz;
        void *raw_map = VirtualAlloc(NULL, total_map, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!raw_map) {
            return NULL;
        }

        uint8_t *base = (uint8_t *)raw_map;
        DWORD old_prot;
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
        long pg = sysconf(_SC_PAGESIZE);
        size_t page_sz = (pg > 0) ? (size_t)pg : 4096;
        size_t aligned_payload = (size + page_sz - 1) & ~(page_sz - 1);
        size_t total_map = page_sz + aligned_payload + page_sz;
        void *raw_map =
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
    if (pool && ptr) {
        int numa_node = pool->numa_node; /* Explicit manual binding wins. */
        if (numa_node < 0 && (pool->flags & MP_FLAG_AUTO_NUMA)) {
            numa_node = cmem_numa_current_node();
        }
        if (numa_node >= 0) {
            unsigned long nodemask = (1UL << numa_node);
            syscall(SYS_mbind, ptr, size, CMEM_MPOL_BIND, &nodemask, sizeof(nodemask) * 8, 0);
        }
    }
#endif
#endif

    return ptr;
}

/**
 * @brief Release memory previously handed out by sys_mem_alloc().
 *
 * Mirrors the allocation paths: static-buffer pools never free (their
 * backing is owned by the caller); custom allocators get the matching
 * sys_free call; huge pages and guard pages are unmapped with their full
 * original size (including the guard page margins); everything else is
 * passed to free().
 *
 * @param pool Owning pool (must not be NULL).
 * @param ptr  Pointer returned by sys_mem_alloc().
 * @param size Original allocation size.
 */
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
            size_t page_sz = si.dwPageSize;
            uint8_t *base = (uint8_t *)ptr - page_sz;
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
        long pg = sysconf(_SC_PAGESIZE);
        size_t page_sz = (pg > 0) ? (size_t)pg : 4096;
        size_t aligned_payload = (size + page_sz - 1) & ~(page_sz - 1);
        size_t total_map = page_sz + aligned_payload + page_sz;
        uint8_t *raw_map = (uint8_t *)ptr - page_sz;
        munmap(raw_map, total_map);
        return;
    }
    free(ptr);
#endif
}

/**
 * @brief Grow the pool's TLSF arena by `additional_bytes`.
 *
 * Allocates a fresh TLSF arena of the requested size and prepends it to the
 * pool's arena chain, immediately making the bytes available for
 * mid-size (TLSF tier) allocations.  Rejected for static-buffer pools.
 *
 * @param pool             Pool to expand.
 * @param additional_bytes Size of the new TLSF arena.
 * @return true on success, false if the arena could not be created.
 */
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

    new_tlsf->next = pool->tlsf_root;
    pool->tlsf_root = new_tlsf;
    pool->stats.total_pool_size += additional_bytes + sizeof(tlsf_pool_t);
    pool_unlock(pool);

    trigger_event(pool, MP_EVENT_ALLOC, NULL, additional_bytes);
    return true;
}

/**
 * @brief Report whether the pool can still be expanded at runtime.
 *
 * @param pool Pool to query.
 * @return true unless the pool is NULL or backed by a static buffer.
 */
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

/**
 * @brief Return how many more bytes the pool may grow before its limit.
 *
 * Computed from the configured max_memory_limit minus the currently
 * committed pool size.
 *
 * @param pool Pool to query.
 * @return Remaining expandable bytes (0 if unbounded or exhausted).
 */
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

/**
 * @brief Pin a range of the pool's memory into physical RAM (mlock).
 *
 * Prevents the kernel from swapping the given range; useful for
 * latency-critical or security-sensitive buffers.
 *
 * @param pool   Owning pool (only used for validation).
 * @param addr   Start of the range to lock.
 * @param length Length of the range in bytes.
 * @return 0 on success, -1 on invalid input or mlock failure.
 */
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

/**
 * @brief Unlock a range previously locked with mp_lock_memory().
 *
 * @param pool   Owning pool (only used for validation).
 * @param addr   Start of the range to unlock.
 * @param length Length of the range in bytes.
 * @return 0 on success, -1 on invalid input or munlock failure.
 */
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

/**
 * @brief Exclude a range from core dumps (MADV_DONTDUMP).
 *
 * Keeps sensitive pool contents (keys, plaintext, passwords) out of
 * crash dumps.
 *
 * @param pool   Owning pool (only used for validation).
 * @param addr   Start of the range to protect.
 * @param length Length of the range in bytes.
 * @return 0 on success, -1 on invalid input or madvise failure.
 */
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

/**
 * @brief Zero a memory range in a way the compiler cannot optimise away.
 *
 * Uses an inline asm memory clobber (GCC/Clang) or a volatile byte loop
 * elsewhere so the zeroing actually reaches memory even if the buffer is
 * never read again.  Used to wipe secrets and for sanitisation before
 * returning memory to the OS.
 *
 * @param pool   Owning pool (only used for validation).
 * @param ptr    Start of the range to zero.
 * @param length Number of bytes to zero.
 */
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

/**
 * @brief Toggle the pool's encrypted-memory flag.
 *
 * Merely sets/clears MP_FLAG_ENCRYPTED_MEMORY; the flag is advisory for
 * downstream backends that want to keep allocations from being swapped.
 *
 * @param pool   Pool whose flag to change.
 * @param enable true to set the flag, false to clear it.
 */
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

/**
 * @brief Set the pool's maximum committed size (hard memory limit).
 *
 * @param pool      Pool to configure.
 * @param max_bytes New cap on total committed bytes (0 = unlimited).
 */
void mp_set_memory_limit(memory_pool_t *pool, size_t max_bytes)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    pool->stats.max_memory_limit = max_bytes;
    pool_unlock(pool);
}

/**
 * @brief Return completely-free slab pages to the OS.
 *
 * Scans every slab class' partial-page list; any page whose slots are all
 * free is unlinked and unmapped, shrinking the pool's committed size.
 * Static-buffer pools are skipped (their backing is not owned by cmem).
 *
 * When idle reclamation is enabled, pages that have been fully idle longer
 * than the configured timeout are also returned to the OS.
 *
 * @param pool Pool to compact.
 * @return Number of bytes returned to the OS.
 */
size_t mp_compact(memory_pool_t *pool)
{
    if (!pool || (pool->flags & MP_FLAG_STATIC_BUFFER)) {
        return 0;
    }
    pool_lock(pool);

    size_t freed_bytes = 0;
    int64_t now = cmem_now_ms();

    for (int cls = 0; cls < SLAB_CLASS_COUNT; cls++) {
        mp_slab_class_t *sc = &pool->slab_classes[cls];
        mp_slab_page_t *curr = sc->partial_pages;

        while (curr) {
            mp_slab_page_t *next = curr->next;
            bool should_free = false;

            if (curr->free_count == curr->total_slots) {
                should_free = true;
            } else if (pool->idle_reclaim_enabled && curr->idle_since_ts > 0) {
                uint64_t idle_ms = (uint64_t)(now - curr->idle_since_ts);
                if (idle_ms >= pool->idle_reclaim_timeout_ms) {
                    should_free = true;
                }
            }

            if (should_free) {
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

/**
 * @brief Release physical RSS pages of idle slab pages (MADV_DONTNEED).
 *
 * For each completely-free slab page the payload region is passed to
 * madvise(MADV_DONTNEED), letting the kernel reclaim the backing physical
 * pages while the mapping itself stays valid for reuse.
 *
 * @param pool Pool to purge.
 * @return Number of bytes handed back to the kernel.
 */
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

    for (int cls = 0; cls < SLAB_CLASS_COUNT; cls++) {
        mp_slab_class_t *sc = &pool->slab_classes[cls];
        mp_slab_page_t *curr = sc->partial_pages;

        while (curr) {
            if (curr->free_count == curr->total_slots && curr->page_raw_mem) {
                uintptr_t start = (uintptr_t)curr->page_raw_mem + sizeof(mp_slab_page_t);
                uintptr_t aligned_start = (start + page_sz - 1) & ~((uintptr_t)page_sz - 1);
                uintptr_t end = (uintptr_t)curr->page_raw_mem + SLAB_PAGE_SIZE;
                uintptr_t aligned_end = end & ~((uintptr_t)page_sz - 1);

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

/**
 * @brief Apply a kernel memory-advice hint to an aligned sub-range.
 *
 * Rounds the requested [addr, addr+length) range out to whole pages before
 * issuing madvise().  A no-op on Windows.
 *
 * @param pool   Owning pool (unused, kept for API symmetry).
 * @param addr   Start of the range.
 * @param length Length of the range in bytes.
 * @param advice madvise() advice constant (e.g. MADV_DONTNEED).
 * @return 0 on success or when there is nothing to advise, -1 on failure.
 */
int mp_madvise(memory_pool_t *pool,
               void *addr,
               size_t length, // NOLINT(bugprone-easily-swappable-parameters)
               int advice)
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
    long pg = sysconf(_SC_PAGESIZE);
    size_t page_sz = (pg > 0) ? (size_t)pg : 4096;

    uintptr_t start = (uintptr_t)addr;
    uintptr_t aligned_start = (start + page_sz - 1) & ~(page_sz - 1);
    uintptr_t end = start + length;
    uintptr_t aligned_end = end & ~(page_sz - 1);

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

/**
 * @brief Reclaim memory from the pool and all of its child arenas.
 *
 * Combines a full compaction (unmap empty slab pages) with a lazy RSS
 * purge, then recursively does the same for every descendant arena in the
 * child tree.  `pad` is accepted for API compatibility with glibc malloc_trim.
 *
 * @param pool Root pool to trim.
 * @param pad  Padding hint (unused, kept for malloc_trim() symmetry).
 * @return Total bytes reclaimed across the whole arena tree.
 */
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

int mp_numa_node_count(void)
{
    return cmem_numa_node_count();
}

int mp_cpu_to_node(int cpu)
{
    return cmem_cpu_to_node(cpu);
}
