/**
 * @file cmem_override.h
 * @brief Global standard library malloc/free override header for cmem.
 *
 * Including this header in existing C or C++ codebases automatically redirects
 * standard malloc(), free(), realloc(), and calloc() function calls to the
 * high-performance cmem memory manager without modifying application logic.
 *
 * This is useful for:
 *  - Zero-effort migration of existing code to cmem
 *  - Drop-in replacement for system malloc in applications
 *  - Global memory tracking and debugging without code changes
 *
 * @warning This header uses preprocessor macros to override standard symbols.
 *          Define CMEM_NO_MALLOC_OVERRIDE before including to disable the override.
 *
 * @warning Include this header AFTER all system/library headers to avoid
 *          replacing malloc/free inside system internals. Use
 *          #include "cmem_override_cleanup.h" at the end of translation units
 *          that need to call real system malloc again.
 *
 * Example usage:
 * @code
 *   // In your main header or first .c file:
 *   #include "cmem_override.h"
 *
 *   // All subsequent malloc/free calls use cmem transparently
 *   int main() {
 *       char* p = malloc(100);  // actually calls cmem_malloc -> mp_alloc
 *       strcpy(p, "Hello cmem!");
 *       free(p);                // actually calls cmem_free -> mp_free
 *       return 0;
 *   }
 * @endcode
 */

#ifndef CMEM_OVERRIDE_H
#define CMEM_OVERRIDE_H

#include "cmem.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Retrieves or initializes the global cmem memory pool instance.
 *
 * Creates a 4MB thread-safe pool with thread-local cache on first call.
 * Subsequent calls return the same pool instance.
 *
 * @return Pointer to the global cmem memory pool
 */
static inline memory_pool_t *cmem_get_global_pool(void)
{
    static memory_pool_t *g_pool = NULL;
    if (!g_pool) {
        // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
        g_pool = mp_create(4 * 1024 * 1024, MP_FLAG_THREAD_SAFE);
        // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
    }
    return g_pool;
}

/**
 * @brief Global malloc replacement using cmem.
 *
 * @param size Number of bytes to allocate
 * @return Pointer to the allocated memory, or NULL on failure
 */
static inline void *cmem_malloc(size_t size)
{
    return mp_alloc(cmem_get_global_pool(), size);
}

/**
 * @brief Global free replacement using cmem.
 *
 * @param ptr Pointer to the memory to free
 */
static inline void cmem_free(void *ptr)
{
    mp_free(cmem_get_global_pool(), ptr);
}

/**
 * @brief Global realloc replacement using cmem.
 *
 * @param ptr Existing allocation pointer (or NULL for new allocation)
 * @param new_size New requested size in bytes
 * @return Pointer to the reallocated memory, or NULL on failure
 */
static inline void *cmem_realloc(void *__ptr, size_t __size)
{
    return mp_realloc(cmem_get_global_pool(), __ptr, __size);
}

/**
 * @brief Global calloc replacement using cmem.
 *
 * @param num Number of elements
 * @param size Size of each element in bytes
 * @return Pointer to the zero-initialized memory, or NULL on failure
 */
static inline void *cmem_calloc(size_t __nmemb, size_t __size)
{
    return mp_calloc(cmem_get_global_pool(), __nmemb, __size);
}

/**
 * @brief Override standard malloc with cmem_malloc.
 *
 * Define CMEM_NO_MALLOC_OVERRIDE before including this header to disable.
 */
#ifndef CMEM_NO_MALLOC_OVERRIDE
#if defined(__has_extension)
#if __has_extension(pragma_push_macro)
#pragma push_macro("malloc")
#pragma push_macro("free")
#pragma push_macro("realloc")
#pragma push_macro("calloc")
#endif
#endif
#define malloc(sz) cmem_malloc(sz)
#define free(ptr) cmem_free(ptr)
#define realloc(ptr, sz) cmem_realloc(ptr, sz)
#define calloc(n, sz) cmem_calloc(n, sz)
#endif

#ifdef __cplusplus
}
#endif

#endif // CMEM_OVERRIDE_H
