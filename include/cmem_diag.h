/**
 * @file cmem_diag.h
 * @brief cmem - Diagnostics, Inspection, Leak Analysis, and Telemetry Exporters.
 *
 * Dedicated public header for cmem diagnostic and observability subsystem.
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
 * @brief Formats and dumps memory pool metrics into Prometheus text exposition format.
 *
 * Output follows the Prometheus exposition format with HELP/TYPE headers.
 *
 * @param pool Pointer to the memory pool
 * @param out_buf Output buffer for Prometheus metrics text
 * @param max_len Maximum length of the output buffer
 * @return Number of bytes written to out_buf
 */
size_t mp_export_prometheus_metrics(memory_pool_t *pool, char *out_buf, size_t max_len);

/**
 * @brief Exports a binary post-mortem memory crash snapshot dump to file.
 *
 * The snapshot can later be parsed with mp_parse_binary_snapshot()
 * or compared with mp_diff_snapshots().
 *
 * @param pool Pointer to the memory pool
 * @param filepath Path to the output binary snapshot file
 * @return true on success
 */
bool mp_export_binary_snapshot(memory_pool_t *pool, const char *filepath);

/**
 * @brief Parses a binary post-mortem memory snapshot file into readable text report.
 *
 * @param filepath Path to the binary snapshot file
 * @param out_report Output buffer for the text report
 * @param max_len Maximum length of the output buffer
 * @return true on success
 */
bool mp_parse_binary_snapshot(const char *filepath, char *out_report, size_t max_len);

/**
 * @brief Compares two binary snapshot files and generates an incremental leak diff report.
 *
 * Identifies allocations present in snapshot B but not in snapshot A.
 *
 * @param snapshot_a_path Path to the baseline snapshot
 * @param snapshot_b_path Path to the target snapshot
 * @param out_report Output buffer for the diff report
 * @param max_len Maximum length of the output buffer
 * @return true on success
 */
bool mp_diff_snapshots(const char *snapshot_a_path,
                       const char *snapshot_b_path,
                       char *out_report,
                       size_t max_len);

/**
 * @brief Prints ASCII allocation size distribution histogram chart to stdout.
 *
 * @param pool Pointer to the memory pool
 */
void mp_dump_histogram(memory_pool_t *pool);

/**
 * @brief Retrieves current statistical metrics of the memory pool.
 *
 * @param pool Pointer to the memory pool
 * @param stats Output statistics structure
 */
void mp_get_stats(memory_pool_t *pool, mp_stats_t *stats);

/**
 * @brief Prints detailed summary and health status of the memory pool to stdout.
 *
 * @param pool Pointer to the memory pool
 */
void mp_dump_info(memory_pool_t *pool);

/**
 * @brief Dumps memory pool tree hierarchy to stdout.
 *
 * Recursively prints all child arenas with their active bytes and allocation counts.
 *
 * @param pool Pointer to the root memory pool
 */
void mp_dump_tree_info(memory_pool_t *pool);

/**
 * @brief Dumps memory pool stats into JSON format buffer for telemetry monitoring.
 *
 * @param pool Pointer to the memory pool
 * @param buf Output buffer for JSON text
 * @param max_len Maximum length of the output buffer
 * @return Number of bytes written to buf
 */
size_t mp_dump_json_stats(memory_pool_t *pool, char *buf, size_t max_len);

/**
 * @brief Checks if there are any un-freed memory allocations and prints a leak report if found.
 *
 * @param pool Pointer to the memory pool
 * @return true if no leaks detected, false otherwise
 */
bool mp_check_leaks(memory_pool_t *pool);

/**
 * @brief Records allocation latency sample for P99/avg profiling.
 */
void mp_record_latency(memory_pool_t *pool, uint64_t latency_ns);

/**
 * @brief Retrieves estimated P99 allocation latency in nanoseconds.
 */
uint64_t mp_get_latency_p99(memory_pool_t *pool);

/**
 * @brief Retrieves average allocation latency in nanoseconds.
 */
uint64_t mp_get_latency_avg(memory_pool_t *pool);

/**
 * @brief Resets allocation latency histogram counters.
 */
void mp_reset_latency_stats(memory_pool_t *pool);

/**
 * @brief Exports allocation events in pprof-compatible text format.
 *
 * Output can be processed by pprof tools for flame graph generation.
 *
 * @param pool Pointer to the memory pool
 * @param out_buf Output buffer for pprof text
 * @param max_len Maximum length of the output buffer
 * @return Number of bytes written to out_buf
 */
size_t mp_export_pprof(memory_pool_t *pool, char *out_buf, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif // CMEM_DIAG_H
