#include "cmem-inspect.h"
#include "cmem.h"
#include "cmem-diag-output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(void) {
    fprintf(stderr,
        "Usage: cmem-inspect <subcommand> [options]\n"
        "Subcommands:\n"
        "  leaks      Detect memory leaks\n"
        "  audit      Heap integrity check\n"
        "  stats      Pool statistics\n"
        "  tree       Arena hierarchy\n"
        "  histogram  Allocation size distribution\n"
        "  snapshot   Export binary snapshot\n"
        "  diff       Compare two snapshots\n"
        "  html       Export HTML report\n"
        "Options:\n"
        "  --json     Output in JSON format\n"
        "  --output <path> Write output to file\n"
        "  --quiet    Suppress informational messages\n"
    );
}

static cmem_inspect_result_t result_error(const char *msg) {
    cmem_inspect_result_t result = {0};
    result.success = false;
    result.exit_code = 2;
    result.error_msg = msg;
    return result;
}

static int run_leaks(memory_pool_t *pool, bool json) {
    (void)json;
    char report[4096];
    size_t len = mp_analyze_leaks(pool, report, sizeof(report));
    if (len > 0) {
        cmem_diag_output_text("%s", report);
        return 1;
    }
    cmem_diag_output_text("No leaks detected.");
    return 0;
}

static int run_audit(memory_pool_t *pool) {
    bool healthy = mp_audit_heap(pool);
    if (healthy) {
        cmem_diag_output_text("Heap audit: HEALTHY");
        return 0;
    }
    cmem_diag_output_text("Heap audit: CORRUPTED");
    return 1;
}

static int run_stats(memory_pool_t *pool, bool json) {
    mp_stats_t stats;
    mp_get_stats(pool, &stats);
    if (json) {
        cmem_diag_output_json(
            "{\"active_bytes\": %zu, \"active_allocs\": %zu, "
            "\"total_alloc_ops\": %zu, \"total_free_ops\": %zu, "
            "\"slab_bytes\": %zu, \"tlsf_bytes\": %zu, \"os_bytes\": %zu, "
            "\"fragmentation\": %.2f, \"qps\": %.2f, \"bandwidth_mbps\": %.2f}",
            stats.active_bytes, stats.active_allocations,
            stats.total_alloc_ops, stats.total_free_ops,
            stats.slab_allocated_bytes, stats.tlsf_allocated_bytes,
            stats.os_allocated_bytes,
            stats.fragmentation_ratio, stats.alloc_qps, stats.bandwidth_mbps);
    } else {
        cmem_diag_output_text("Active bytes: %zu", stats.active_bytes);
        cmem_diag_output_text("Active allocations: %zu", stats.active_allocations);
        cmem_diag_output_text("Total alloc ops: %zu", stats.total_alloc_ops);
        cmem_diag_output_text("Total free ops: %zu", stats.total_free_ops);
        cmem_diag_output_text("Fragmentation: %.2f%%", stats.fragmentation_ratio * 100.0);
        cmem_diag_output_text("Alloc QPS: %.2f", stats.alloc_qps);
    }
    return 0;
}

static int run_tree(memory_pool_t *pool, bool json) {
    if (json) {
        cmem_diag_output_json("{\"tree\": \"see text output\"}");
    }
    mp_dump_tree_info(pool);
    return 0;
}

static int run_histogram(memory_pool_t *pool) {
    mp_dump_histogram(pool);
    return 0;
}

static int run_snapshot(memory_pool_t *pool) {
    bool ok = mp_export_binary_snapshot(pool, "cmem_snapshot.cmem_dump");
    if (ok) {
        cmem_diag_output_text("Snapshot exported to cmem_snapshot.cmem_dump");
        return 0;
    }
    cmem_diag_output_error("Failed to export snapshot");
    return 2;
}

static int run_diff(memory_pool_t *pool, const char *path_a, const char *path_b) {
    (void)pool;
    char report[16384];
    bool ok = mp_diff_snapshots(path_a, path_b, report, sizeof(report));
    if (ok) {
        cmem_diag_output_text("%s", report);
        return 0;
    }
    cmem_diag_output_error("Failed to diff snapshots");
    return 2;
}

static int run_html(memory_pool_t *pool) {
    bool ok = mp_export_html_report(pool, "memory_report.html");
    if (ok) {
        cmem_diag_output_text("HTML report exported to memory_report.html");
        return 0;
    }
    cmem_diag_output_error("Failed to export HTML report");
    return 2;
}
cmem_inspect_result_t cmem_inspect_run(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return result_error("No subcommand specified");
    }

    const char *subcommand = NULL;
    bool json = false;
    const char *output_file = NULL;
    bool quiet = false;
    const char *diff_a = NULL;
    const char *diff_b = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            json = true;
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (strcmp(argv[i], "diff") == 0 && i + 2 < argc) {
            subcommand = argv[i];
            diff_a = argv[i + 1];
            diff_b = argv[i + 2];
            i += 2;
        } else if (!subcommand) {
            subcommand = argv[i];
        }
    }

    (void)quiet;
    cmem_diag_output_init(json, output_file);

    if (!subcommand) {
        print_usage();
        return result_error("No subcommand specified");
    }

    memory_pool_t *pool = mp_get_global_pool();
    if (!pool) {
        return result_error("No active memory pool");
    }

    int rc = 0;
    if (strcmp(subcommand, "leaks") == 0) {
        rc = run_leaks(pool, json);
    } else if (strcmp(subcommand, "audit") == 0) {
        rc = run_audit(pool);
    } else if (strcmp(subcommand, "stats") == 0) {
        rc = run_stats(pool, json);
    } else if (strcmp(subcommand, "tree") == 0) {
        rc = run_tree(pool, json);
    } else if (strcmp(subcommand, "histogram") == 0) {
        rc = run_histogram(pool);
    } else if (strcmp(subcommand, "snapshot") == 0) {
        rc = run_snapshot(pool);
    } else if (strcmp(subcommand, "diff") == 0) {
        if (!diff_a || !diff_b) {
            return result_error("diff requires two snapshot paths");
        }
        rc = run_diff(pool, diff_a, diff_b);
    } else if (strcmp(subcommand, "html") == 0) {
        rc = run_html(pool);
    } else {
        print_usage();
        return result_error("Unknown subcommand");
    }

    cmem_inspect_result_t result = {0};
    result.success = true;
    result.exit_code = rc;
    return result;
}

int main(int argc, char **argv) {
    cmem_inspect_result_t result = cmem_inspect_run(argc, argv);
    if (!result.success) {
        fprintf(stderr, "Error: %s\n", result.error_msg);
        return 2;
    }
    return result.exit_code;
}
