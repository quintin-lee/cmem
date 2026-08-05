/**
 * @file cmem_frame.h
 * @brief Dual ping-pong frame arena allocator for game and graphics rendering pipelines in cmem.
 */

#ifndef CMEM_FRAME_H
#define CMEM_FRAME_H

#include "cmem.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle to a dual ping-pong frame arena.
 */
typedef struct cmem_frame_arena cmem_frame_arena_t;

/**
 * @brief Creates a dual ping-pong frame arena for game/graphics pipelines.
 *
 * Allocates two backing pools; while frame N allocates from pool A,
 * pool B is reset in O(1) in the background.
 *
 * @param frame_capacity Capacity in bytes for each frame backing pool
 * @return Pointer to frame arena handle, or NULL on failure
 */
cmem_frame_arena_t *mp_frame_arena_create(size_t frame_capacity);

/**
 * @brief Allocates frame-scoped memory from the current active frame pool.
 *
 * @param farena Pointer to the frame arena
 * @param size Allocation size in bytes
 * @return Payload pointer, or NULL on allocation failure
 */
void *mp_frame_alloc(cmem_frame_arena_t *farena, size_t size);

/**
 * @brief Swaps frame pools and resets the inactive pool for the next frame.
 *
 * Call this at the end of each game frame loop iteration.
 *
 * @param farena Pointer to the frame arena
 */
void mp_frame_end(cmem_frame_arena_t *farena);

/**
 * @brief Destroys the dual frame arena and frees both backing pools.
 *
 * @param farena Pointer to the frame arena
 */
void mp_frame_arena_destroy(cmem_frame_arena_t *farena);

#ifdef __cplusplus
}
#endif

#endif // CMEM_FRAME_H
