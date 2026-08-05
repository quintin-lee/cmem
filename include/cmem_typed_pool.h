/**
 * @file cmem_typed_pool.h
 * @brief Zero-overhead type-safe object pool allocator for cmem.
 */

#ifndef CMEM_TYPED_POOL_H
#define CMEM_TYPED_POOL_H

#include "cmem.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle to a typed object pool allocator.
 */
typedef struct mp_typed_pool mp_typed_pool_t;

/**
 * @brief Creates a type-safe fixed-element object pool.
 *
 * @param elem_size Size of each object element in bytes
 * @param capacity Maximum number of elements the pool can hold
 * @return Pointer to typed pool handle, or NULL on failure
 */
mp_typed_pool_t *mp_typed_pool_create(size_t elem_size, size_t capacity);

/**
 * @brief Allocates one object element from the typed pool.
 *
 * @param tpool Pointer to the typed pool
 * @return Pointer to element payload, or NULL if pool is exhausted
 */
void *mp_typed_alloc(mp_typed_pool_t *tpool);

/**
 * @brief Frees one object element back to the typed pool.
 *
 * @param tpool Pointer to the typed pool
 * @param ptr Pointer to element payload to free
 */
void mp_typed_free(mp_typed_pool_t *tpool, void *ptr);

/**
 * @brief Destroys the typed object pool and releases its memory.
 *
 * @param tpool Pointer to the typed pool
 */
void mp_typed_pool_destroy(mp_typed_pool_t *tpool);

/**
 * @brief Convenience macro to define a type-safe object pool wrapper.
 */
#define MP_TYPED_POOL_DEFINE(type, name)                                                           \
    typedef struct {                                                                               \
        mp_typed_pool_t *raw_pool;                                                                 \
    } name##_pool_t;                                                                               \
    static inline name##_pool_t *name##_pool_create(size_t cap)                                    \
    {                                                                                              \
        name##_pool_t *p = (name##_pool_t *)malloc(sizeof(name##_pool_t));                         \
        if (p)                                                                                     \
            p->raw_pool = mp_typed_pool_create(sizeof(type), cap);                                 \
        return p;                                                                                  \
    }                                                                                              \
    static inline(type) * name##_pool_alloc(name##_pool_t *p)                                      \
    {                                                                                              \
        return (type *)mp_typed_alloc(p->raw_pool);                                                \
    }                                                                                              \
    static inline void name##_pool_free(name##_pool_t *p, (type) * ptr)                            \
    {                                                                                              \
        mp_typed_free(p->raw_pool, (void *)ptr);                                                   \
    }                                                                                              \
    static inline void name##_pool_destroy(name##_pool_t *p)                                       \
    {                                                                                              \
        if (p) {                                                                                   \
            mp_typed_pool_destroy(p->raw_pool);                                                    \
            free(p);                                                                               \
        }                                                                                          \
    }

#ifdef __cplusplus
}
#endif

#endif // CMEM_TYPED_POOL_H
