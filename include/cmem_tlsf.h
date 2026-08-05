/**
 * @file cmem_tlsf.h
 * @brief Two-Level Segregated Fit (TLSF) allocator subsystem interfaces for cmem.
 */

#ifndef CMEM_TLSF_H
#define CMEM_TLSF_H

#include "cmem.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle to a TLSF pool instance.
 */
typedef struct tlsf_pool tlsf_pool_t;

#ifdef __cplusplus
}
#endif

#endif // CMEM_TLSF_H
