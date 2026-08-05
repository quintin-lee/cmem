#include "cmem-analyze-parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CMEM_SNAPSHOT_MAGIC 0x434D454D
#define CMEM_MAX_TOTAL_POOL_SIZE ((uint64_t)1 << 40)
#define CMEM_MAX_ACTIVE_ALLOCATIONS ((uint64_t)1 << 20)
#define CMEM_MAX_LEAK_RECORDS 1048576
#define CMEM_MAX_ALLOC_LINE 1000000

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t total_pool_size;
    uint64_t active_bytes;
    uint64_t active_allocations;
} cmem_parser_header_t;

typedef struct {
    uint64_t address;
    uint64_t requested_size;
    uint8_t alloc_type;
    uint32_t alloc_line;
    char alloc_file[64];
    char alloc_func[64];
} cmem_parser_record_t;

static char *read_fixed_string(const char buf[64])
{
    size_t len = 0;
    while (len < 64 && buf[len] != '\0') {
        len++;
    }
    if (len == 0) {
        return NULL;
    }
    char *out = malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, buf, len);
    out[len] = '\0';
    return out;
}

static bool parse_header(FILE *fp, cmem_parser_header_t *hdr)
{
    if (fread(hdr, sizeof(*hdr), 1, fp) != 1) {
        return false;
    }
    if (hdr->magic != CMEM_SNAPSHOT_MAGIC) {
        return false;
    }
    if (hdr->version != 1) {
        return false;
    }
    if (hdr->total_pool_size > CMEM_MAX_TOTAL_POOL_SIZE) {
        return false;
    }
    if (hdr->active_bytes > hdr->total_pool_size) {
        return false;
    }
    if (hdr->active_allocations > CMEM_MAX_ACTIVE_ALLOCATIONS) {
        return false;
    }
    return true;
}

bool cmem_analyze_parse_snapshot(const char *path, cmem_snapshot_header_t *header)
{
    if (!path || !header) {
        return false;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }

    cmem_parser_header_t hdr;
    if (!parse_header(fp, &hdr)) {
        fclose(fp);
        return false;
    }

    memset(header, 0, sizeof(*header));
    header->magic = hdr.magic;
    header->version = hdr.version;
    header->timestamp = 0;
    header->total_pool_size = hdr.total_pool_size;
    header->active_bytes = hdr.active_bytes;
    header->active_allocs = hdr.active_allocations;
    header->leak_bytes = hdr.active_bytes;
    header->leak_count = hdr.active_allocations;
    header->pool_name = NULL;

    fclose(fp);
    return true;
}

bool cmem_analyze_read_leaks(const char *path, cmem_leak_entry_t **entries, size_t *count)
{
    if (!path || !entries || !count) {
        return false;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }

    cmem_parser_header_t hdr;
    if (!parse_header(fp, &hdr)) {
        fclose(fp);
        return false;
    }

    size_t max_records = (size_t)hdr.active_allocations;
    if (max_records > CMEM_MAX_LEAK_RECORDS) {
        max_records = CMEM_MAX_LEAK_RECORDS;
    }

    cmem_leak_entry_t *out = calloc(max_records, sizeof(cmem_leak_entry_t));
    if (!out) {
        fclose(fp);
        return false;
    }

    cmem_parser_record_t rec;
    size_t idx = 0;
    while (idx < max_records) {
        size_t got = fread(&rec, sizeof(rec), 1, fp);
        if (got != 1) {
            break;
        }
        if (rec.alloc_line > CMEM_MAX_ALLOC_LINE) {
            rec.alloc_line = 0;
        }
        if (rec.alloc_type > 8) {
            rec.alloc_type = 0;
        }
        cmem_leak_entry_t *entry = &out[idx];
        entry->address = (void *)(uintptr_t)rec.address;
        entry->size = rec.requested_size;
        entry->usable_size = rec.requested_size;
        entry->tier = rec.alloc_type;
        entry->file = read_fixed_string(rec.alloc_file);
        entry->line = (int)rec.alloc_line;
        entry->func = read_fixed_string(rec.alloc_func);
        entry->timestamp = 0;
        entry->backtrace_depth = 0;
        idx++;
    }

    fclose(fp);
    *entries = out;
    *count = idx;
    return true;
}

void cmem_analyze_free_leaks(cmem_leak_entry_t *entries, size_t count)
{
    if (!entries) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free((void *)(uintptr_t)entries[i].file);
        free((void *)(uintptr_t)entries[i].func);
    }
    free(entries);
}

static int compare_leaks_by_size_desc(const void *left, const void *right)
{
    const cmem_leak_entry_t *ea = (const cmem_leak_entry_t *)left;
    const cmem_leak_entry_t *eb = (const cmem_leak_entry_t *)right;
    if (ea->size > eb->size) {
        return -1;
    }
    if (ea->size < eb->size) {
        return 1;
    }
    return 0;
}

bool cmem_analyze_top_leaks(const char *path, int n, cmem_leak_entry_t **entries, size_t *count)
{
    if (!path || !entries || !count) {
        return false;
    }

    cmem_leak_entry_t *all = NULL;
    size_t total = 0;
    if (!cmem_analyze_read_leaks(path, &all, &total)) {
        return false;
    }

    if (total == 0) {
        *entries = NULL;
        *count = 0;
        return true;
    }

    qsort(all, total, sizeof(cmem_leak_entry_t), compare_leaks_by_size_desc);

    size_t show = n > 0 && (size_t)n < total ? (size_t)n : total;
    cmem_leak_entry_t *top = calloc(show, sizeof(cmem_leak_entry_t));
    if (!top) {
        cmem_analyze_free_leaks(all, total);
        return false;
    }
    memcpy(top, all, show * sizeof(cmem_leak_entry_t));
    cmem_analyze_free_leaks(all, total);

    *entries = top;
    *count = show;
    return true;
}

bool cmem_analyze_diff(const char *path_a, const char *path_b, cmem_diff_result_t *result)
{
    if (!path_a || !path_b || !result) {
        return false;
    }

    memset(result, 0, sizeof(*result));

    cmem_snapshot_header_t hdra, hdrb;
    if (!cmem_analyze_parse_snapshot(path_a, &hdra)) {
        return false;
    }
    if (!cmem_analyze_parse_snapshot(path_b, &hdrb)) {
        return false;
    }

    cmem_leak_entry_t *entries_a = NULL;
    size_t count_a = 0;
    cmem_leak_entry_t *entries_b = NULL;
    size_t count_b = 0;

    if (!cmem_analyze_read_leaks(path_a, &entries_a, &count_a)) {
        return false;
    }
    if (!cmem_analyze_read_leaks(path_b, &entries_b, &count_b)) {
        cmem_analyze_free_leaks(entries_a, count_a);
        return false;
    }

    for (size_t i = 0; i < count_b; i++) {
        bool found = false;
        for (size_t j = 0; j < count_a; j++) {
            if (entries_b[i].address == entries_a[j].address &&
                entries_b[i].size == entries_a[j].size) {
                found = true;
                break;
            }
        }
        if (!found) {
            result->new_leaks++;
            result->total_new_bytes += entries_b[i].size;
        }
    }

    for (size_t i = 0; i < count_a; i++) {
        bool found = false;
        for (size_t j = 0; j < count_b; j++) {
            if (entries_a[i].address == entries_b[j].address &&
                entries_a[i].size == entries_b[j].size) {
                found = true;
                break;
            }
        }
        if (!found) {
            result->fixed_leaks++;
            result->total_freed_bytes += entries_a[i].size;
        }
    }

    result->net_leak_bytes = (ssize_t)result->total_new_bytes - (ssize_t)result->total_freed_bytes;

    cmem_analyze_free_leaks(entries_a, count_a);
    cmem_analyze_free_leaks(entries_b, count_b);
    return true;
}
