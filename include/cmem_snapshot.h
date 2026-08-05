/**
 * @file cmem_snapshot.h
 * @brief Binary post-mortem crash snapshot dump and diffing tools for cmem.
 */

#ifndef CMEM_SNAPSHOT_H
#define CMEM_SNAPSHOT_H

#include "cmem.h"

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif // CMEM_SNAPSHOT_H
