#ifndef CMEM_ANALYZE_PARSER_H
#define CMEM_ANALYZE_PARSER_H

#include "cmem-analyze.h"
#include <stdbool.h>

bool cmem_analyze_parse_snapshot(const char *path, cmem_snapshot_header_t *header);
bool cmem_analyze_read_leaks(const char *path, cmem_leak_entry_t **entries, size_t *count);
void cmem_analyze_free_leaks(cmem_leak_entry_t *entries, size_t count);
bool cmem_analyze_diff(const char *path_a, const char *path_b, cmem_diff_result_t *result);
bool cmem_analyze_top_leaks(const char *path, int n, cmem_leak_entry_t **entries, size_t *count);

#endif
