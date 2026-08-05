/**
 * @file cmem_arena.h
 * @brief Hierarchical child arenas, multi-arena partitioning, and quota management for cmem.
 */

#ifndef CMEM_ARENA_H
#define CMEM_ARENA_H

#include "cmem.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Creates a hierarchical child memory pool attached to a parent pool.
 *
 * Child pools inherit configuration flags from parent unless overridden.
 * Destroying parent pool automatically destroys all child pools recursively.
 *
 * @param parent Pointer to the parent memory pool
 * @param flags Operational flags for the child pool
 * @return Pointer to the newly created child pool, or NULL on failure
 */
memory_pool_t *mp_create_child_pool(memory_pool_t *parent, mp_flags_t flags);

/**
 * @brief Creates a multi-arena partitioned memory pool for NUMA / CPU core scaling.
 *
 * @param initial_capacity Initial capacity per arena
 * @param num_arenas Number of sub-arenas to create (0 = auto-detect CPUs)
 * @param flags Operational flags for all arenas
 * @return Pointer to master memory pool handle, or NULL on failure
 */
memory_pool_t *mp_create_arena(size_t initial_capacity, int num_arenas, mp_flags_t flags);

/**
 * @brief Sets a per-arena byte allocation quota and optional overflow callback.
 *
 * @param pool Pointer to the memory pool
 * @param quota_bytes Maximum allowed bytes (0 = unlimited)
 * @param cb Callback triggered when quota is exceeded
 * @param user_data User data passed to callback
 */
void mp_set_arena_quota(memory_pool_t *pool,
                        size_t quota_bytes,
                        mp_watermark_callback_t cb,
                        void *user_data);

/**
 * @brief Checks if the arena is within its quota limit.
 *
 * @param pool Pointer to the memory pool
 * @return true if within quota or no quota set, false if over quota
 */
bool mp_check_arena_quota(memory_pool_t *pool);

/**
 * @brief Dumps memory pool tree hierarchy to stdout.
 *
 * Recursively prints all child arenas with their active bytes and allocation counts.
 *
 * @param pool Pointer to the root memory pool
 */
void mp_dump_tree_info(memory_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif // CMEM_ARENA_H
