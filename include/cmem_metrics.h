/**
 * @file cmem_metrics.h
 * @brief Telemetry metrics, Prometheus exporter, latency profiling, and JSON dumps for cmem.
 */

#ifndef CMEM_METRICS_H
#define CMEM_METRICS_H

#include "cmem.h"

#ifdef __cplusplus
extern "C" {
#endif

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
 * @brief Prints ASCII allocation size distribution histogram chart to stdout.
 *
 * @param pool Pointer to the memory pool
 */
void mp_dump_histogram(memory_pool_t *pool);

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
 * @brief Formats and dumps memory pool metrics into Prometheus text exposition format.
 *
 * @param pool Pointer to the memory pool
 * @param out_buf Output buffer for Prometheus metrics text
 * @param max_len Maximum length of the output buffer
 * @return Number of bytes written to out_buf
 */
size_t mp_export_prometheus_metrics(memory_pool_t *pool, char *out_buf, size_t max_len);

/**
 * @brief Records allocation latency sample for P99/avg profiling.
 *
 * @param pool Pointer to the memory pool
 * @param latency_ns Latency in nanoseconds
 */
void mp_record_latency(memory_pool_t *pool, uint64_t latency_ns);

/**
 * @brief Retrieves estimated P99 allocation latency in nanoseconds.
 *
 * @param pool Pointer to the memory pool
 * @return Estimated P99 latency
 */
uint64_t mp_get_latency_p99(memory_pool_t *pool);

/**
 * @brief Retrieves average allocation latency in nanoseconds.
 *
 * @param pool Pointer to the memory pool
 * @return Average latency
 */
uint64_t mp_get_latency_avg(memory_pool_t *pool);

/**
 * @brief Resets allocation latency histogram counters.
 *
 * @param pool Pointer to the memory pool
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

#endif // CMEM_METRICS_H
