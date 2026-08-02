/**
 * @file cmem_diag.c
 * @brief Extracted module implementation.
 */

#include "cmem.h"
#include "cmem_internal.h"
#include <inttypes.h>
bool mp_audit_heap(memory_pool_t* pool)
{
    if (!pool)
        return true;
    pool_lock(pool);

    bool healthy = true;
    mp_block_header_t* curr = pool->active_head;

    while (curr)
    {
        void* payload = (void*) ((uint8_t*) curr + sizeof(mp_block_header_t));

        if (curr->magic != MP_MAGIC_HEAD)
        {
            fprintf(
                stderr,
                "[HEAP AUDIT ERROR] Corrupted header magic at %p! (Found: 0x%X, Expected: 0x%X)\n",
                payload, curr->magic, MP_MAGIC_HEAD);
            healthy = false;
        }

        if (pool->flags & MP_FLAG_DEBUG_CANARY)
        {
            uint8_t* canary = (uint8_t*) payload + curr->requested_size;
            if (*canary != MP_CANARY_BYTE)
            {
                fprintf(stderr,
                        "[HEAP AUDIT ERROR] Redzone canary corruption at %p! (Size: %zu, Source: "
                        "%s:%d in %s)\n",
                        payload, curr->requested_size,
                        curr->alloc_file ? curr->alloc_file : "unknown", curr->alloc_line,
                        curr->alloc_func ? curr->alloc_func : "unknown");
                healthy = false;
            }
        }
        curr = curr->next;
    }

    if (healthy)
    {
        printf(
            "[HEAP AUDIT HEALTH] Heap integrity check passed cleanly! All active blocks valid.\n");
    }

    pool_unlock(pool);
    return healthy;
}

size_t mp_analyze_leaks(memory_pool_t* pool, char* report_buf, size_t max_len)
{
    if (!pool || !report_buf || max_len == 0)
        return 0;
    pool_lock(pool);

    size_t offset = 0;
    offset +=
        snprintf(report_buf + offset, max_len - offset,
                 "=================== DETAILED MEMORY LEAK ANALYSIS REPORT ===================\n"
                 "  Total Managed System Memory: %zu bytes (%.2f KB)\n"
                 "  Active Leaked Allocations  : %zu blocks\n"
                 "  Total Leaked Payload Bytes : %zu bytes (%.2f KB)\n"
                 "============================================================================\n",
                 pool->stats.total_pool_size, pool->stats.total_pool_size / 1024.0,
                 pool->stats.active_allocations, pool->stats.active_bytes,
                 pool->stats.active_bytes / 1024.0);

    if (pool->stats.active_allocations == 0)
    {
        offset += snprintf(report_buf + offset, max_len - offset,
                           "  No memory leaks detected! Clean execution.\n");
        pool_unlock(pool);
        return offset;
    }

    mp_block_header_t* curr = pool->active_head;
    size_t idx = 1;

    while (curr && offset < max_len)
    {
        void* payload = (void*) ((uint8_t*) curr + sizeof(mp_block_header_t));
        const char* tier_str = (curr->alloc_type == ALLOC_TYPE_SLAB)
                                 ? "SLAB"
                                 : ((curr->alloc_type == ALLOC_TYPE_TLSF) ? "TLSF" : "DIRECT OS");

        offset += snprintf(report_buf + offset, max_len - offset,
                           "\n[Leak #%zu] Address: %p | Payload Size: %zu bytes | Tier: %s\n",
                           idx++, payload, curr->requested_size, tier_str);

        if (curr->alloc_file)
        {
            offset += snprintf(report_buf + offset, max_len - offset,
                               "  Source Location : %s:%d (function '%s')\n", curr->alloc_file,
                               curr->alloc_line, curr->alloc_func ? curr->alloc_func : "unknown");
        }
        else
        {
            offset += snprintf(report_buf + offset, max_len - offset,
                               "  Source Location : (Location tracking disabled, enable "
                               "MP_FLAG_TRACK_LOCATIONS)\n");
        }

        if (curr->backtrace_depth > 0)
        {
#ifdef CMEM_HAS_EXECINFO
            char** symbols = backtrace_symbols(curr->backtrace_addrs, curr->backtrace_depth);
#else
            char** symbols = NULL;
#endif
            offset += snprintf(report_buf + offset, max_len - offset, "  Callstack Frames:\n");
            for (int f = 0; f < curr->backtrace_depth && offset < max_len; f++)
            {
                offset += snprintf(report_buf + offset, max_len - offset, "    #%d %s\n", f,
                                   symbols ? symbols[f] : "unknown");
            }
            if (symbols)
                free(symbols);
        }

        curr = curr->next;
    }

    pool_unlock(pool);
    return offset;
}

bool mp_export_leak_report(memory_pool_t* pool, const char* filepath)
{
    if (!pool || !filepath)
        return false;
    char buffer[16384];
    size_t report_len = mp_analyze_leaks(pool, buffer, sizeof(buffer));

    FILE* f = fopen(filepath, "w");
    if (!f)
        return false;

    fwrite(buffer, 1, report_len, f);
    fclose(f);
    printf("[CMEM DIAGNOSTICS] Detailed memory leak report exported to: %s\n", filepath);
    return true;
}

bool mp_export_html_report(memory_pool_t* pool, const char* filepath)
{
    if (!pool || !filepath)
        return false;
    FILE* f = fopen(filepath, "w");
    if (!f)
        return false;

    mp_stats_t stats;
    mp_get_stats(pool, &stats);

    fprintf(
        f,
        "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
        "<meta charset=\"UTF-8\">\n"
        "<title>cmem Profile & Leak Analysis Dashboard</title>\n"
        "<style>\n"
        "  body { font-family: 'Inter', system-ui, sans-serif; background: #0f172a; color: "
        "#f8fafc; margin: 0; padding: 2rem; }\n"
        "  .container { max-width: 1100px; margin: 0 auto; }\n"
        "  h1 { color: #38bdf8; font-size: 2rem; border-bottom: 2px solid #334155; padding-bottom: "
        "0.5rem; }\n"
        "  .cards { display: grid; grid-template-columns: repeat(4, 1fr); gap: 1rem; margin: "
        "1.5rem 0; }\n"
        "  .card { background: #1e293b; padding: 1.2rem; border-radius: 10px; border: 1px solid "
        "#334155; box-shadow: 0 4px 6px -1px rgba(0,0,0,0.3); }\n"
        "  .card h3 { margin: 0; font-size: 0.85rem; color: #94a3b8; text-transform: uppercase; }\n"
        "  .card .val { font-size: 1.6rem; font-weight: bold; color: #38bdf8; margin-top: 0.4rem; "
        "}\n"
        "  .progress-bar { background: #334155; height: 24px; border-radius: 12px; overflow: "
        "hidden; display: flex; margin: 1.5rem 0; }\n"
        "  .bar-slab { background: #10b981; text-align: center; font-size: 0.8rem; line-height: "
        "24px; color: #fff; }\n"
        "  .bar-tlsf { background: #6366f1; text-align: center; font-size: 0.8rem; line-height: "
        "24px; color: #fff; }\n"
        "  .bar-os { background: #f59e0b; text-align: center; font-size: 0.8rem; line-height: "
        "24px; color: #fff; }\n"
        "  table { width: 100%%; border-collapse: collapse; background: #1e293b; border-radius: "
        "8px; overflow: hidden; margin-top: 1rem; }\n"
        "  th, td { padding: 0.8rem 1rem; text-align: left; border-bottom: 1px solid #334155; }\n"
        "  th { background: #334155; color: #cbd5e1; font-weight: 600; }\n"
        "  tr:hover { background: #334155; }\n"
        "  .badge { padding: 0.25rem 0.5rem; border-radius: 4px; font-size: 0.75rem; font-weight: "
        "bold; }\n"
        "  .badge-slab { background: #064e3b; color: #34d399; }\n"
        "  .badge-tlsf { background: #312e81; color: #818cf8; }\n"
        "  .badge-os { background: #78350f; color: #fbbf24; }\n"
        "</style>\n</head>\n<body>\n"
        "<div class=\"container\">\n"
        "  <h1>cmem Visual Profiler & Leak Analysis Dashboard</h1>\n"
        "  <div class=\"cards\">\n"
        "    <div class=\"card\"><h3>Total Reserved</h3><div class=\"val\">%.2f KB</div></div>\n"
        "    <div class=\"card\"><h3>Active Payload</h3><div class=\"val\">%.2f KB</div></div>\n"
        "    <div class=\"card\"><h3>Active Blocks</h3><div class=\"val\">%zu</div></div>\n"
        "    <div class=\"card\"><h3>Fragmentation</h3><div class=\"val\">%.1f%%</div></div>\n"
        "  </div>\n",
        stats.total_pool_size / 1024.0, stats.active_bytes / 1024.0, stats.active_allocations,
        stats.fragmentation_ratio * 100.0);

    size_t total_alloc =
        stats.slab_allocated_bytes + stats.tlsf_allocated_bytes + stats.os_allocated_bytes;
    size_t tot = (total_alloc > 0) ? total_alloc : 1;
    double p_slab = (stats.slab_allocated_bytes * 100.0) / tot;
    double p_tlsf = (stats.tlsf_allocated_bytes * 100.0) / tot;
    double p_os = (stats.os_allocated_bytes * 100.0) / tot;

    fprintf(f,
            "  <h2>Allocation Tier Distribution</h2>\n"
            "  <div class=\"progress-bar\">\n"
            "    <div class=\"bar-slab\" style=\"width: %.1f%%;\">Slab (%.1f%%)</div>\n"
            "    <div class=\"bar-tlsf\" style=\"width: %.1f%%;\">TLSF (%.1f%%)</div>\n"
            "    <div class=\"bar-os\" style=\"width: %.1f%%;\">OS (%.1f%%)</div>\n"
            "  </div>\n",
            p_slab, p_slab, p_tlsf, p_tlsf, p_os, p_os);

    fprintf(f,
            "  <h2>Active Memory Allocations & Leak Inventory (%zu Blocks)</h2>\n"
            "  <table>\n"
            "    <thead><tr><th>#</th><th>Address</th><th>Size</th><th>Tier</th><th>Source "
            "Location</th><th>Function</th></tr></thead>\n"
            "    <tbody>\n",
            stats.active_allocations);

    pool_lock(pool);
    mp_block_header_t* curr = pool->active_head;
    size_t idx = 1;

    while (curr)
    {
        void* payload = (void*) ((uint8_t*) curr + sizeof(mp_block_header_t));
        const char* badge_cls =
            (curr->alloc_type == ALLOC_TYPE_SLAB)
                ? "badge-slab"
                : ((curr->alloc_type == ALLOC_TYPE_TLSF) ? "badge-tlsf" : "badge-os");
        const char* tier_name = (curr->alloc_type == ALLOC_TYPE_SLAB)
                                  ? "SLAB"
                                  : ((curr->alloc_type == ALLOC_TYPE_TLSF) ? "TLSF" : "OS");

        fprintf(f,
                "      <tr><td>%zu</td><td><code>%p</code></td><td>%zu B</td>"
                "<td><span class=\"badge "
                "%s\">%s</span></td><td>%s:%d</td><td><code>%s</code></td></tr>\n",
                idx++, payload, curr->requested_size, badge_cls, tier_name,
                curr->alloc_file ? curr->alloc_file : "-", curr->alloc_line,
                curr->alloc_func ? curr->alloc_func : "-");
        curr = curr->next;
    }
    pool_unlock(pool);

    fprintf(f, "    </tbody>\n  </table>\n</div>\n</body>\n</html>\n");
    fclose(f);

    printf("[CMEM DIAGNOSTICS] Interactive HTML Profiler Report exported to: %s\n", filepath);
    return true;
}

bool mp_export_binary_snapshot(memory_pool_t* pool, const char* filepath)
{
    if (!pool || !filepath)
        return false;
    FILE* f = fopen(filepath, "wb");
    if (!f)
        return false;

    pool_lock(pool);

    cmem_snapshot_header_t hdr;
    hdr.magic = 0x434D454D;
    hdr.version = 1;
    hdr.total_pool_size = (uint64_t) pool->stats.total_pool_size;
    hdr.active_bytes = (uint64_t) pool->stats.active_bytes;
    hdr.active_allocations = (uint64_t) pool->stats.active_allocations;

    fwrite(&hdr, sizeof(hdr), 1, f);

    mp_block_header_t* curr = pool->active_head;
    while (curr)
    {
        cmem_snapshot_record_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.address = (uint64_t) (uintptr_t) ((uint8_t*) curr + sizeof(mp_block_header_t));
        rec.requested_size = (uint64_t) curr->requested_size;
        rec.alloc_type = curr->alloc_type;
        rec.alloc_line = (uint32_t) curr->alloc_line;
        if (curr->alloc_file)
            snprintf(rec.alloc_file, sizeof(rec.alloc_file), "%s", curr->alloc_file);
        if (curr->alloc_func)
            snprintf(rec.alloc_func, sizeof(rec.alloc_func), "%s", curr->alloc_func);

        fwrite(&rec, sizeof(rec), 1, f);
        curr = curr->next;
    }

    pool_unlock(pool);
    fclose(f);

    printf("[CMEM DIAGNOSTICS] Binary Crash Snapshot Dump exported to: %s\n", filepath);
    return true;
}

bool mp_parse_binary_snapshot(const char* filepath, char* out_report, size_t max_len)
{
    if (!filepath || !out_report || max_len == 0)
        return false;
    FILE* f = fopen(filepath, "rb");
    if (!f)
        return false;

    cmem_snapshot_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != 0x434D454D)
    {
        fclose(f);
        return false;
    }

    size_t offset = 0;
    offset += snprintf(out_report + offset, max_len - offset,
                       "=================== CMEM BINARY SNAPSHOT DUMP PARSER ===================\n"
                       "  Format Version     : %u\n"
                       "  Total Pool Size    : %" PRIu64 " bytes\n"
                       "  Active Payload B   : %" PRIu64 " bytes\n"
                       "  Active Allocations : %" PRIu64 " blocks\n"
                       "========================================================================\n",
                       hdr.version, hdr.total_pool_size, hdr.active_bytes, hdr.active_allocations);

    cmem_snapshot_record_t rec;
    size_t idx = 1;

    while (fread(&rec, sizeof(rec), 1, f) == 1 && offset < max_len)
    {
        const char* tier_str = (rec.alloc_type == ALLOC_TYPE_SLAB)
                                 ? "SLAB"
                                 : ((rec.alloc_type == ALLOC_TYPE_TLSF) ? "TLSF" : "DIRECT OS");
        offset += snprintf(out_report + offset, max_len - offset,
                           "[Record #%zu] Addr: 0x%" PRIx64 " | Size: %" PRIu64
                           " B | Tier: %s | Location: %s:%u (%s)\n",
                           idx++, rec.address, rec.requested_size, tier_str,
                           rec.alloc_file[0] ? rec.alloc_file : "unknown", rec.alloc_line,
                           rec.alloc_func[0] ? rec.alloc_func : "unknown");
    }

    fclose(f);
    return true;
}

void mp_get_stats(memory_pool_t* pool, mp_stats_t* stats)
{
    if (!pool || !stats)
        return;
    pool_rdlock(pool);
    *stats = pool->stats;
    size_t total_sys = pool->stats.total_pool_size > 0 ? pool->stats.total_pool_size : 1;
    stats->fragmentation_ratio = 1.0 - ((double) pool->stats.active_bytes / (double) total_sys);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (pool->window_start_time.tv_sec == 0)
    {
        pool->window_start_time = now;
    }
    double elapsed = (now.tv_sec - pool->window_start_time.tv_sec) +
                     (now.tv_nsec - pool->window_start_time.tv_nsec) / 1e9;
    size_t ops = pool->stats.total_alloc_ops;
    size_t active = pool->stats.active_bytes;
    if (elapsed > 0.000001 && ops > 0)
    {
        stats->alloc_qps = (double) ops / elapsed;
        stats->bandwidth_mbps = ((double) active / (1024.0 * 1024.0)) / elapsed;
    }
    else
    {
        stats->alloc_qps = (double) ops;
        stats->bandwidth_mbps = (double) active / (1024.0 * 1024.0);
    }
    pool_rdunlock(pool);
}

void mp_dump_info(memory_pool_t* pool)
{
    if (!pool)
        return;
    mp_stats_t stats;
    mp_get_stats(pool, &stats);

    printf("\n================ CMEM DIAGNOSTICS DUMP [%s] ================\n", pool->arena_name);
    printf("  Total System Reserved Memory: %zu bytes (%.2f KB)\n", stats.total_pool_size,
           stats.total_pool_size / 1024.0);
    printf("  Current Active Allocations  : %zu blocks, %zu bytes (%.2f KB)\n",
           stats.active_allocations, stats.active_bytes, stats.active_bytes / 1024.0);
    printf("  Peak Memory Allocation      : %zu bytes (%.2f KB)\n", stats.peak_bytes,
           stats.peak_bytes / 1024.0);
    printf("  Max Memory Budget Limit     : %zu bytes (%s)\n", stats.max_memory_limit,
           stats.max_memory_limit > 0 ? "Enforced" : "Unlimited");
    printf("  Estimated Fragmentation     : %.2f%%\n", stats.fragmentation_ratio * 100.0);
    printf("  Real-time Alloc Rate (QPS)  : %.2f ops/sec\n", stats.alloc_qps);
    printf("  Real-time Bandwidth         : %.2f MB/sec\n", stats.bandwidth_mbps);
    printf("  Cumulative Stats            : %zu Allocations, %zu Frees\n", stats.total_alloc_ops,
           stats.total_free_ops);
    printf("  Allocation Tier Breakdown   :\n");
    printf("    - Slab Pool (Small <=512B): %zu bytes\n", stats.slab_allocated_bytes);
    printf("    - TLSF Pool (Med <=4MB)   : %zu bytes\n", stats.tlsf_allocated_bytes);
    printf("    - Direct OS (Large >4MB)  : %zu bytes\n", stats.os_allocated_bytes);
    printf("==============================================================\n\n");
}

void mp_dump_histogram(memory_pool_t* pool)
{
    if (!pool)
        return;
    mp_stats_t stats;
    mp_get_stats(pool, &stats);

    static const char* labels[CMEM_HISTOGRAM_BUCKETS] = {
        "<= 16 B       ",  "17 B - 32 B    ", "33 B - 64 B    ", "65 B - 128 B   ",
        "129 B - 256 B  ", "257 B - 512 B  ", "513 B - 1 KB   ", "1 KB - 2 KB    ",
        "2 KB - 4 KB    ", "4 KB - 8 KB    ", "8 KB - 16 KB   ", "16 KB - 32 KB  ",
        "32 KB - 64 KB  ", "64 KB - 512 KB ", "512 KB - 4 MB  ", "> 4 MB         "};

    size_t max_count = 0;
    for (int i = 0; i < CMEM_HISTOGRAM_BUCKETS; i++)
    {
        if (stats.size_histogram[i] > max_count)
            max_count = stats.size_histogram[i];
    }

    printf("\n================ ALLOCATION SIZE HISTOGRAM [%s] ================\n",
           pool->arena_name);
    for (int i = 0; i < CMEM_HISTOGRAM_BUCKETS; i++)
    {
        if (stats.size_histogram[i] == 0)
            continue;
        int bar_len = (max_count > 0) ? (int) ((stats.size_histogram[i] * 20) / max_count) : 0;
        char bar_str[21];
        memset(bar_str, '*', bar_len);
        bar_str[bar_len] = '\0';

        printf("  Bucket %-2d [%s] : %-8zu [%-20s]\n", i, labels[i], stats.size_histogram[i],
               bar_str);
    }
    printf("=========================================================================\n\n");
}

void print_arena_node(memory_pool_t* pool, int indent)
{
    if (!pool)
        return;
    for (int i = 0; i < indent; i++)
        printf("  ");
    printf("|- [Arena: %s] Active Bytes: %zu B, Active Allocations: %zu\n", pool->arena_name,
           pool->stats.active_bytes, pool->stats.active_allocations);

    memory_pool_t* child = pool->first_child;
    while (child)
    {
        print_arena_node(child, indent + 1);
        child = child->next_sibling;
    }
}

void mp_dump_tree_info(memory_pool_t* pool)
{
    if (!pool)
        return;
    pool_lock(pool);
    printf("\n================ CMEM ARENA TREE DUMP ================\n");
    print_arena_node(pool, 0);
    printf("======================================================\n\n");
    pool_unlock(pool);
}

size_t mp_dump_json_stats(memory_pool_t* pool, char* buf, size_t max_len)
{
    if (!pool || !buf || max_len == 0)
        return 0;
    mp_stats_t stats;
    mp_get_stats(pool, &stats);

    int len =
        snprintf(buf, max_len,
                 "{\n"
                 "  \"arena_name\": \"%s\",\n"
                 "  \"total_pool_size\": %zu,\n"
                 "  \"active_bytes\": %zu,\n"
                 "  \"peak_bytes\": %zu,\n"
                 "  \"max_memory_limit\": %zu,\n"
                 "  \"active_allocations\": %zu,\n"
                 "  \"total_alloc_ops\": %zu,\n"
                 "  \"total_free_ops\": %zu,\n"
                 "  \"slab_allocated_bytes\": %zu,\n"
                 "  \"tlsf_allocated_bytes\": %zu,\n"
                 "  \"os_allocated_bytes\": %zu,\n"
                 "  \"fragmentation_ratio\": %.4f\n"
                 "}",
                 pool->arena_name, stats.total_pool_size, stats.active_bytes, stats.peak_bytes,
                 stats.max_memory_limit, stats.active_allocations, stats.total_alloc_ops,
                 stats.total_free_ops, stats.slab_allocated_bytes, stats.tlsf_allocated_bytes,
                 stats.os_allocated_bytes, stats.fragmentation_ratio);

    return (len > 0 && (size_t) len < max_len) ? (size_t) len : max_len - 1;
}

size_t mp_export_prometheus_metrics(memory_pool_t* pool, char* out_buf, size_t max_len)
{
    if (!pool || !out_buf || max_len == 0)
        return 0;
    mp_stats_t stats;
    mp_get_stats(pool, &stats);

    int len =
        snprintf(out_buf, max_len,
                 "# HELP cmem_total_pool_bytes Total reserved bytes from OS.\n"
                 "# TYPE cmem_total_pool_bytes gauge\n"
                 "cmem_total_pool_bytes{arena=\"%s\"} %zu\n\n"
                 "# HELP cmem_active_bytes Currently allocated active payload bytes.\n"
                 "# TYPE cmem_active_bytes gauge\n"
                 "cmem_active_bytes{arena=\"%s\"} %zu\n\n"
                 "# HELP cmem_active_allocations Active outstanding allocation count.\n"
                 "# TYPE cmem_active_allocations gauge\n"
                 "cmem_active_allocations{arena=\"%s\"} %zu\n\n"
                 "# HELP cmem_alloc_ops_total Cumulative count of allocation operations.\n"
                 "# TYPE cmem_alloc_ops_total counter\n"
                 "cmem_alloc_ops_total{arena=\"%s\"} %zu\n\n"
                 "# HELP cmem_alloc_qps Real-time allocation QPS rate.\n"
                 "# TYPE cmem_alloc_qps gauge\n"
                 "cmem_alloc_qps{arena=\"%s\"} %.2f\n\n"
                 "# HELP cmem_bandwidth_mbps Real-time allocation bandwidth throughput in MB/s.\n"
                 "# TYPE cmem_bandwidth_mbps gauge\n"
                 "cmem_bandwidth_mbps{arena=\"%s\"} %.2f\n\n"
                 "# HELP cmem_fragmentation_ratio Memory fragmentation ratio (0.0 to 1.0).\n"
                 "# TYPE cmem_fragmentation_ratio gauge\n"
                 "cmem_fragmentation_ratio{arena=\"%s\"} %.4f\n",
                 pool->arena_name, stats.total_pool_size, pool->arena_name, stats.active_bytes,
                 pool->arena_name, stats.active_allocations, pool->arena_name,
                 stats.total_alloc_ops, pool->arena_name, stats.alloc_qps, pool->arena_name,
                 stats.bandwidth_mbps, pool->arena_name, stats.fragmentation_ratio);

    return (len > 0 && (size_t) len < max_len) ? (size_t) len : max_len - 1;
}

bool mp_check_leaks(memory_pool_t* pool)
{
    if (!pool)
        return true;
    pool_lock(pool);

    bool clean = (pool->stats.active_allocations == 0);
    if (!clean)
    {
        char report[4096];
        pool_unlock(pool);
        mp_analyze_leaks(pool, report, sizeof(report));
        fprintf(stderr, "%s\n", report);
        return false;
    }

    printf("[CMEM HEALTH] No memory leaks detected in [%s]. All memory safely freed!\n",
           pool->arena_name);
    pool_unlock(pool);
    return true;
}
