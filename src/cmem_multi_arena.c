/**
 * @file cmem_multi_arena.c
 * @brief Per-thread TLSF arena binding for cmem (enable/bind/purge/count).
 */
#include "cmem.h"
#include "cmem_internal.h"

/* ========================================================================== */
/*  Multi-Arena Thread Binding Architecture                                   */
/* ========================================================================== */

/**
 * @brief Drop the calling thread's TLS references to @p pool.
 *
 * mp_destroy calls this BEFORE releasing pool memory: a later mp_create
 * can reuse the freed addresses, and stale tls_cache.bound_arena or
 * arena_bindings entries would hand dangling arena pointers back to the
 * allocation fast path. Matching is by pointer identity; the recursive
 * child teardown in mp_destroy purges each child as well.
 */
void mp_purge_thread_arena_bindings(memory_pool_t *pool)
{
    if (tls_cache.bound_arena == pool) {
        tls_cache.bound_arena = NULL;
    }

    for (int i = tls_cache.arena_binding_count - 1; i >= 0; i--) {
        if (tls_cache.arena_bindings[i].master_pool == pool ||
            tls_cache.arena_bindings[i].bound_arena == pool) {
            tls_cache.arena_bindings[i] =
                tls_cache.arena_bindings[tls_cache.arena_binding_count - 1];
            tls_cache.arena_binding_count--;
        }
    }
}

memory_pool_t *mp_get_thread_bound_arena(memory_pool_t *pool)
{
    if (!pool || pool->num_arenas <= 0) {
        return pool;
    }

    for (int i = 0; i < tls_cache.arena_binding_count; i++) {
        if (tls_cache.arena_bindings[i].master_pool == pool) {
            return tls_cache.arena_bindings[i].bound_arena;
        }
    }

    int idx = 0;
#ifndef _WIN32
    int cpu = cmem_current_cpu();
    if (cpu >= 0) {
        idx = cpu % pool->num_arenas;
    } else {
        idx = (int)(CMEM_ATOMIC_FETCH_ADD(&pool->arena_rr_index, 1, CMEM_ORDER_RELAXED) %
                    (size_t)pool->num_arenas);
    }
#else
    idx = (int)(CMEM_ATOMIC_FETCH_ADD(&pool->arena_rr_index, 1, CMEM_ORDER_RELAXED) %
                (size_t)pool->num_arenas);
#endif

    memory_pool_t *bound_arena = pool->arenas[idx];
    if (tls_cache.arena_binding_count < CMEM_MAX_TLS_BOUND_POOLS) {
        tls_cache.arena_bindings[tls_cache.arena_binding_count].master_pool = pool;
        tls_cache.arena_bindings[tls_cache.arena_binding_count].bound_arena = bound_arena;
        tls_cache.arena_bindings[tls_cache.arena_binding_count].arena_index = idx;
        tls_cache.arena_binding_count++;
    }
    return bound_arena;
}

bool mp_enable_multi_arena(memory_pool_t *pool, int num_arenas)
{
    if (!pool || pool->is_multi_arena_child) {
        return false;
    }
    if (pool->num_arenas > 0) {
        return true;
    }

    if (num_arenas <= 0) {
#ifdef _WIN32
        num_arenas = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
        num_arenas = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
        if (num_arenas <= 0) {
            num_arenas = 4;
        }
    }
    if (num_arenas > 64) {
        num_arenas = 64;
    }

    memory_pool_t **arenas = (memory_pool_t **)calloc((size_t)num_arenas, sizeof(memory_pool_t *));
    if (!arenas) {
        return false;
    }

    int numa_nodes = mp_numa_node_count();

    /* mp_create_child() takes the parent lock itself; holding it across the
     * loop would recursively write-lock the same rwlock and corrupt it. */
    for (int i = 0; i < num_arenas; i++) {
        char name[64];
        (void)snprintf(name, sizeof(name), "%.48s_Arena%d", pool->arena_name, i);
        memory_pool_t *child =
            mp_create_child_verbatim(pool, 0, pool->flags & ~MP_FLAG_MULTI_ARENA, name);
        if (!child) {
            child = pool;
        } else {
            child->is_multi_arena_child = true;
            child->master_pool = pool;
            if (numa_nodes > 1) {
                child->numa_node = i % numa_nodes;
            }
        }
        arenas[i] = child;
    }

    pool_lock(pool);
    pool->arenas = arenas;
    pool->num_arenas = num_arenas;
    pool->flags |= MP_FLAG_MULTI_ARENA;
    pool_unlock(pool);
    return true;
}

bool mp_bind_thread_to_arena(memory_pool_t *pool, int arena_index)
{
    if (!pool || pool->num_arenas <= 0 || arena_index < 0 || arena_index >= pool->num_arenas) {
        return false;
    }
    memory_pool_t *bound_arena = pool->arenas[arena_index];

    for (int i = 0; i < tls_cache.arena_binding_count; i++) {
        if (tls_cache.arena_bindings[i].master_pool == pool) {
            tls_cache.arena_bindings[i].bound_arena = bound_arena;
            tls_cache.arena_bindings[i].arena_index = arena_index;
            return true;
        }
    }

    if (tls_cache.arena_binding_count < CMEM_MAX_TLS_BOUND_POOLS) {
        tls_cache.arena_bindings[tls_cache.arena_binding_count].master_pool = pool;
        tls_cache.arena_bindings[tls_cache.arena_binding_count].bound_arena = bound_arena;
        tls_cache.arena_bindings[tls_cache.arena_binding_count].arena_index = arena_index;
        tls_cache.arena_binding_count++;
        return true;
    }
    return false;
}

memory_pool_t *mp_get_thread_arena(memory_pool_t *pool)
{
    return mp_get_thread_bound_arena(pool);
}

int mp_get_arena_count(memory_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    return pool->num_arenas;
}

memory_pool_t *mp_get_arena(memory_pool_t *pool, int arena_index)
{
    if (!pool || pool->num_arenas <= 0 || arena_index < 0 || arena_index >= pool->num_arenas) {
        return NULL;
    }
    return pool->arenas[arena_index];
}
