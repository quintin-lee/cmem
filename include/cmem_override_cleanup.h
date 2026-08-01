/**
 * @file cmem_override_cleanup.h
 * @brief Cleanup header to restore standard malloc/free after cmem_override.h.
 *
 * Include this at the end of translation units that used cmem_override.h
 * and need to call the real system malloc/free again.
 */

#ifndef CMEM_OVERRIDE_CLEANUP_H
#define CMEM_OVERRIDE_CLEANUP_H

#ifndef CMEM_NO_MALLOC_OVERRIDE
#if defined(__has_extension) && __has_extension(pragma_pop_macro)
#pragma pop_macro("calloc")
#pragma pop_macro("realloc")
#pragma pop_macro("free")
#pragma pop_macro("malloc")
#else
#undef malloc
#undef free
#undef realloc
#undef calloc
#endif
#endif

#endif // CMEM_OVERRIDE_CLEANUP_H
