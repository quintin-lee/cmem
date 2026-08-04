#ifndef CMEM_ANALYZE_H
#define CMEM_ANALYZE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool json_output;
    bool html_output;
    int top_n;
    const char *output_file;
    bool quiet;
} cmem_analyze_config_t;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint64_t timestamp;
    size_t total_pool_size;
    size_t active_bytes;
    size_t active_allocs;
    size_t leak_bytes;
    size_t leak_count;
    const char *pool_name;
} cmem_snapshot_header_t;

typedef struct {
    void *address;
    size_t size;
    size_t usable_size;
    int tier;
    const char *file;
    int line;
    const char *func;
    uint64_t timestamp;
    size_t backtrace_depth;
    void *backtrace_addrs[8];
} cmem_leak_entry_t;

typedef struct {
    size_t new_leaks;
    size_t fixed_leaks;
    size_t net_leak_bytes;
    size_t total_new_bytes;
    size_t total_freed_bytes;
    cmem_leak_entry_t **new_leak_entries;
    size_t new_leak_count;
    cmem_leak_entry_t **fixed_leak_entries;
    size_t fixed_leak_count;
} cmem_diff_result_t;

int cmem_analyze_run(int argc, char **argv);
bool cmem_analyze_parse_snapshot(const char *path, cmem_snapshot_header_t *header);
bool cmem_analyze_diff(const char *path_a, const char *path_b, cmem_diff_result_t *result);
bool cmem_analyze_top_leaks(const char *path, int n, cmem_leak_entry_t **entries, size_t *count);

#endif
