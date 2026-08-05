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

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
static INIT_ONCE g_pool_init_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK cmem_init_global_pool_win(PINIT_ONCE once, PVOID ignored, PVOID *ctx)
{
    (void)once;
    (void)ignored;
    *(memory_pool_t **)ctx = mp_create(4 * 1024 * 1024, MP_FLAG_THREAD_SAFE);
    return TRUE;
}
#elif defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
static pthread_once_t g_pool_init_once = PTHREAD_ONCE_INIT;
#endif

static memory_pool_t *g_cmem_global_pool = NULL;

static inline void cmem_init_global_pool(void)
{
    g_cmem_global_pool = mp_create(4 * 1024 * 1024, MP_FLAG_THREAD_SAFE);
}

static inline memory_pool_t *cmem_get_global_pool(void)
{
    if (!g_cmem_global_pool) {
#if defined(_WIN32) || defined(_WIN64)
        InitOnceExecuteOnce(
            &g_pool_init_once, cmem_init_global_pool_win, NULL, (PVOID *)&g_cmem_global_pool);
#else
        pthread_once(&g_pool_init_once, cmem_init_global_pool);
#endif
    }
    return g_cmem_global_pool;
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
 * @brief Override standard malloc/free/realloc/calloc with cmem equivalents.
 *
 * Uses thread-safe initialization via pthread_once (Unix) or InitOnceExecuteOnce
 * (Windows). The global pool is lazily created on first call and shared across
 * all threads.
 *
 * Define CMEM_NO_MALLOC_OVERRIDE before including this header to disable
 * macro substitution entirely. To exclude specific translation units, define
 * CMEM_NO_MALLOC_OVERRIDE at the top of those files before including this
 * header, or include "cmem_override_cleanup.h" to restore original macros.
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
