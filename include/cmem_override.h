/**
 * @file cmem_override.h
 * @brief Global standard library malloc/free override header for cmem.
 * 
 * Including this header in existing C codebases automatically redirects standard
 * malloc(), free(), realloc(), and calloc() function calls to the high-performance
 * cmem memory manager without modifying application logic.
 */

#ifndef CMEM_OVERRIDE_H
#define CMEM_OVERRIDE_H

#include "cmem.h"
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Retrieves or initializes the global cmem memory pool instance.
 */
static inline memory_pool_t* cmem_get_global_pool(void) {
    static memory_pool_t* g_pool = NULL;
    if (!g_pool) {
        g_pool = mp_create(4 * 1024 * 1024, MP_FLAG_THREAD_SAFE | MP_FLAG_THREAD_LOCAL_CACHE);
    }
    return g_pool;
}

static inline void* cmem_malloc(size_t size) {
    return mp_alloc(cmem_get_global_pool(), size);
}

static inline void cmem_free(void* ptr) {
    mp_free(cmem_get_global_pool(), ptr);
}

static inline void* cmem_realloc(void* ptr, size_t new_size) {
    return mp_realloc(cmem_get_global_pool(), ptr, new_size);
}

static inline void* cmem_calloc(size_t num, size_t size) {
    return mp_calloc(cmem_get_global_pool(), num, size);
}

#ifndef CMEM_NO_MALLOC_OVERRIDE
#define malloc(sz)          cmem_malloc(sz)
#define free(ptr)           cmem_free(ptr)
#define realloc(ptr, sz)    cmem_realloc(ptr, sz)
#define calloc(n, sz)       cmem_calloc(n, sz)
#endif

#ifdef __cplusplus
}
#endif

#endif // CMEM_OVERRIDE_H
