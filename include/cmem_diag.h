/**
 * @file cmem_diag.h
 * @brief cmem - Diagnostics, Heap Auditing, and Leak Analysis Subsystem.
 *
 * Dedicated public header for cmem diagnostic and leak analysis subsystem.
 * Move these APIs into a separate library target (libcmem_diag) to allow
 * lightweight embedded deployment of libcmem_core without diagnostic overhead.
 */

#ifndef CMEM_DIAG_H
#define CMEM_DIAG_H

#include "cmem.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Leak severity classification.
 */
typedef enum {
    MP_LEAK_SEVERITY_CRITICAL, /**< > 1MB or frequent allocation pattern */
    MP_LEAK_SEVERITY_WARNING,  /**< Medium-sized or recurring leak */
    MP_LEAK_SEVERITY_INFO      /**< Small or occasional leak */
} mp_leak_severity_t;

/**
 * @brief Leak pattern analysis result.
 */
typedef struct {
    const char *pattern_name; /**< Pattern identifier */
    int confidence;           /**< 0-100 confidence score */
    const char *suggestion;   /**< Fix suggestion */
} mp_leak_pattern_t;

/**
 * @brief Audits heap integrity by checking header magics and canary redzones of all active
 * allocations.
 *
 * @param pool Pointer to the memory pool
 * @return true if heap is healthy, false if corruption detected
 */
bool mp_audit_heap(memory_pool_t *pool);

/**
 * @brief Generates a detailed memory leak analysis report with file/line locations and
 * callstacks.
 *
 * @param pool Pointer to the memory pool
 * @param report_buf Output buffer for the report text
 * @param max_len Maximum length of the output buffer
 * @return Number of bytes written to report_buf
 */
size_t mp_analyze_leaks(memory_pool_t *pool, char *report_buf, size_t max_len);

/**
 * @brief Exports the memory leak analysis report to a text file.
 *
 * @param pool Pointer to the memory pool
 * @param filepath Path to the output text file
 * @return true on success
 */
bool mp_export_leak_report(memory_pool_t *pool, const char *filepath);

/**
 * @brief Gets the severity classification for a leak.
 *
 * @param info Allocation info from mp_get_allocation_info()
 * @return Severity level
 */
mp_leak_severity_t mp_get_leak_severity(const mp_allocation_info_t *info);

/**
 * @brief Analyzes leak pattern and returns analysis result.
 *
 * @param info Allocation info from mp_get_allocation_info()
 * @return Leak pattern analysis
 */
mp_leak_pattern_t mp_analyze_leak_pattern(const mp_allocation_info_t *info);

/**
 * @brief Exports an interactive visual HTML profiler dashboard to a file.
 *
 * The dashboard includes memory usage cards, tier distribution bars,
 * and a table of active allocations with source locations.
 *
 * @param pool Pointer to the memory pool
 * @param filepath Path to the output HTML file
 * @return true on success
 */
bool mp_export_html_report(memory_pool_t *pool, const char *filepath);

/**
 * @brief Checks if there are any un-freed memory allocations and prints a leak report if found.
 *
 * @param pool Pointer to the memory pool
 * @return true if no leaks detected, false otherwise
 */
bool mp_check_leaks(memory_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif // CMEM_DIAG_H
