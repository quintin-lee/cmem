#include "cmem-analyze.h"
#include "cmem-analyze-parser.h"
#include "cmem-diag-output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define CMEM_KB 1024.0
#define CMEM_DEFAULT_TOP_N 10
#define CMEM_STRTOL_BASE 10

static void print_usage(void)
{
    fprintf(stderr,
        "Usage: cmem-analyze <subcommand> [options] <input>\n"
        "Subcommands:\n"
        "  report     Full analysis report\n"
        "  top        Top-N leaks by size\n"
        "  summary    Summary statistics\n"
        "  validate   Verify snapshot integrity\n"
        "  diff       Compare two snapshots\n"
        "Options:\n"
        "  --json     Output in JSON format\n"
        "  --html     Output in HTML format\n"
        "  --output <path> Write output to file\n"
        "  --quiet    Suppress informational messages\n"
        "  --top <n>  Number of top leaks to show (default: 10)\n"
    );
}

static const char *tier_name(int tier)
{
    switch (tier) {
        case 1:
            return "SLAB";
        case 2:
            return "TLSF";
        case 3:
            return "DIRECT_OS";
        case 4:
            return "EMERGENCY";
        default:
            return "UNKNOWN";
    }
}

static int cmd_report(const char *path, bool json, bool html, bool quiet)
{
    cmem_snapshot_header_t header;
    if (!cmem_analyze_parse_snapshot(path, &header)) {
        cmem_diag_output_error("Failed to parse snapshot: %s", path);
        return 2;
    }

    if (json) {
        cmem_diag_output_json(
            "{\"pool\": \"%s\", \"timestamp\": %llu, "
            "\"total_pool_size\": %zu, \"active_bytes\": %zu, "
            "\"active_allocs\": %zu, \"leak_count\": %zu, \"leak_bytes\": %zu}",
            header.pool_name ? header.pool_name : "unknown",
            (unsigned long long)header.timestamp,
            header.total_pool_size,
            header.active_bytes,
            header.active_allocs,
            header.leak_count,
            header.leak_bytes);
    } else if (html) {
        cmem_diag_output_text("HTML report generation not yet implemented");
        return 2;
    } else {
        if (!quiet) {
            cmem_diag_output_text("=================== CMEM SNAPSHOT ANALYSIS REPORT ===================");
            cmem_diag_output_text("  Snapshot File  : %s", path);
            cmem_diag_output_text("  Pool Name      : %s", header.pool_name ? header.pool_name : "unknown");
            cmem_diag_output_text("  Total Pool Size: %zu bytes (%.2f KB)",
                header.total_pool_size,
                (double)header.total_pool_size / CMEM_KB);
            cmem_diag_output_text("  Active Bytes   : %zu bytes (%.2f KB)",
                header.active_bytes,
                (double)header.active_bytes / CMEM_KB);
            cmem_diag_output_text("  Active Allocs  : %zu blocks", header.active_allocs);
            cmem_diag_output_text("  Leak Count     : %zu blocks", header.leak_count);
            cmem_diag_output_text("  Leak Bytes     : %zu bytes (%.2f KB)",
                header.leak_bytes,
                (double)header.leak_bytes / CMEM_KB);
            cmem_diag_output_text("====================================================================");
        }

        cmem_leak_entry_t *entries = NULL;
        size_t count = 0;
        if (cmem_analyze_read_leaks(path, &entries, &count) && count > 0) {
            cmem_diag_output_text("\nLeak Details:");
            for (size_t i = 0; i < count; i++) {
                cmem_diag_output_text(
                    "  [%zu] %p | %zu bytes | %s | %s:%d (%s)",
                    i + 1,
                    entries[i].address,
                    entries[i].size,
                    tier_name(entries[i].tier),
                    entries[i].file ? entries[i].file : "unknown",
                    entries[i].line,
                    entries[i].func ? entries[i].func : "unknown");
            }
        }
        cmem_analyze_free_leaks(entries, count);
    }

    return header.leak_count > 0 ? 1 : 0;
}

static int cmd_top(const char *path, int n, bool json, bool quiet)
{
    cmem_leak_entry_t *entries = NULL;
    size_t count = 0;
    if (!cmem_analyze_top_leaks(path, n, &entries, &count)) {
        cmem_diag_output_error("Failed to read leaks from: %s", path);
        return 2;
    }

    if (count == 0) {
        if (!quiet && !json) {
            cmem_diag_output_text("No leaks found.");
        }
        return 0;
    }

    if (json) {
        cmem_diag_output_json("[");
        for (size_t i = 0; i < count; i++) {
            cmem_diag_output_json(
                "{\"rank\": %zu, \"address\": \"%p\", \"size\": %zu, "
                "\"tier\": \"%s\", \"file\": \"%s\", \"line\": %d, \"func\": \"%s\"}",
                i + 1,
                entries[i].address,
                entries[i].size,
                tier_name(entries[i].tier),
                entries[i].file ? entries[i].file : "",
                entries[i].line,
                entries[i].func ? entries[i].func : "");
            if (i + 1 < count) {
                cmem_diag_output_json(",");
            }
        }
        cmem_diag_output_json("]");
    } else {
        if (!quiet) {
            cmem_diag_output_text("=================== TOP %zu LEAKS BY SIZE ===================", count);
        }
        for (size_t i = 0; i < count; i++) {
            cmem_diag_output_text(
                "  [%zu] %p | %zu bytes (%.2f KB) | %s | %s:%d (%s)",
                i + 1,
                entries[i].address,
                entries[i].size,
                (double)entries[i].size / CMEM_KB,
                tier_name(entries[i].tier),
                entries[i].file ? entries[i].file : "unknown",
                entries[i].line,
                entries[i].func ? entries[i].func : "unknown");
        }
        if (!quiet) {
            cmem_diag_output_text("=================================================================");
        }
    }

    cmem_analyze_free_leaks(entries, count);
    return (int)count > 0 ? 1 : 0;
}

static int cmd_summary(const char *path, bool json, bool quiet)
{
    cmem_snapshot_header_t header;
    if (!cmem_analyze_parse_snapshot(path, &header)) {
        cmem_diag_output_error("Failed to parse snapshot: %s", path);
        return 2;
    }

    if (json) {
        cmem_diag_output_json(
            "{\"pool\": \"%s\", \"total_pool_size\": %zu, "
            "\"active_bytes\": %zu, \"active_allocs\": %zu, "
            "\"leak_count\": %zu, \"leak_bytes\": %zu}",
            header.pool_name ? header.pool_name : "unknown",
            header.total_pool_size,
            header.active_bytes,
            header.active_allocs,
            header.leak_count,
            header.leak_bytes);
    } else {
        if (!quiet) {
            cmem_diag_output_text("=================== SNAPSHOT SUMMARY ===================");
            cmem_diag_output_text("  Pool            : %s", header.pool_name ? header.pool_name : "unknown");
            cmem_diag_output_text("  Total Pool Size : %zu bytes (%.2f KB)",
                header.total_pool_size,
                (double)header.total_pool_size / CMEM_KB);
            cmem_diag_output_text("  Active Bytes    : %zu bytes (%.2f KB)",
                header.active_bytes,
                (double)header.active_bytes / CMEM_KB);
            cmem_diag_output_text("  Active Allocs   : %zu blocks", header.active_allocs);
            cmem_diag_output_text("  Leak Count      : %zu blocks", header.leak_count);
            cmem_diag_output_text("  Leak Bytes      : %zu bytes (%.2f KB)",
                header.leak_bytes,
                (double)header.leak_bytes / CMEM_KB);
            cmem_diag_output_text("=========================================================");
        }
    }

    return header.leak_count > 0 ? 1 : 0;
}

static int cmd_validate(const char *path, bool quiet)
{
    cmem_snapshot_header_t header;
    bool ok = cmem_analyze_parse_snapshot(path, &header);
    if (ok) {
        if (!quiet) {
            cmem_diag_output_text("Snapshot valid: %s", path);
            cmem_diag_output_text("  Pool: %s", header.pool_name ? header.pool_name : "unknown");
            cmem_diag_output_text("  Leaks: %zu blocks, %zu bytes",
                header.leak_count, header.leak_bytes);
        }
        return 0;
    }
    cmem_diag_output_error("Invalid snapshot: %s", path);
    return 2;
}

static int cmd_diff(const char *path_a, const char *path_b, bool json, bool quiet)
{
    cmem_diff_result_t result;
    if (!cmem_analyze_diff(path_a, path_b, &result)) {
        cmem_diag_output_error("Failed to diff snapshots");
        return 2;
    }

    if (json) {
        cmem_diag_output_json(
            "{\"new_leaks\": %zu, \"fixed_leaks\": %zu, "
            "\"net_leak_bytes\": %zd, \"total_new_bytes\": %zu, \"total_freed_bytes\": %zu}",
            result.new_leaks,
            result.fixed_leaks,
            result.net_leak_bytes,
            result.total_new_bytes,
            result.total_freed_bytes);
    } else {
        if (!quiet) {
            cmem_diag_output_text("=================== SNAPSHOT DIFF REPORT ===================");
            cmem_diag_output_text("  Baseline        : %s", path_a);
            cmem_diag_output_text("  Target          : %s", path_b);
            cmem_diag_output_text("  New Leaks       : %zu blocks", result.new_leaks);
            cmem_diag_output_text("  Fixed Leaks     : %zu blocks", result.fixed_leaks);
            cmem_diag_output_text("  Net Leak Bytes  : %zd bytes (%.2f KB)",
                result.net_leak_bytes,
                (double)result.net_leak_bytes / CMEM_KB);
            cmem_diag_output_text("  Total New Bytes : %zu bytes (%.2f KB)",
                result.total_new_bytes,
                (double)result.total_new_bytes / CMEM_KB);
            cmem_diag_output_text("  Total Freed Bytes: %zu bytes (%.2f KB)",
                result.total_freed_bytes,
                (double)result.total_freed_bytes / CMEM_KB);
            cmem_diag_output_text("=============================================================");
        }
    }

    return result.new_leaks > 0 ? 1 : 0;
}

int cmem_analyze_run(int argc, char **argv)
{
    if (argc < 3) {
        print_usage();
        return 2;
    }

    const char *subcommand = NULL;
    bool json = false;
    bool html = false;
    const char *output_file = NULL;
    bool quiet = false;
    int top_n = CMEM_DEFAULT_TOP_N;
    const char *input_a = NULL;
    const char *input_b = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            json = true;
        } else if (strcmp(argv[i], "--html") == 0) {
            html = true;
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            char *endptr = NULL;
            errno = 0;
            long val = strtol(argv[++i], &endptr, CMEM_STRTOL_BASE);
            if (errno == 0 && endptr && *endptr == '\0' && val > 0) {
                top_n = (int)val;
            }
        } else if (!subcommand) {
            subcommand = argv[i];
        } else if (!input_a) {
            input_a = argv[i];
        } else if (!input_b) {
            input_b = argv[i];
        }
    }

    cmem_diag_output_init(json, output_file);

    if (!subcommand) {
        print_usage();
        return 2;
    }

    if (!input_a && strcmp(subcommand, "validate") != 0) {
        cmem_diag_output_error("Missing input snapshot path");
        return 2;
    }

    int rc = 0;
    if (strcmp(subcommand, "report") == 0) {
        rc = cmd_report(input_a, json, html, quiet);
    } else if (strcmp(subcommand, "top") == 0) {
        rc = cmd_top(input_a, top_n, json, quiet);
    } else if (strcmp(subcommand, "summary") == 0) {
        rc = cmd_summary(input_a, json, quiet);
    } else if (strcmp(subcommand, "validate") == 0) {
        rc = cmd_validate(input_a, quiet);
    } else if (strcmp(subcommand, "diff") == 0) {
        if (!input_b) {
            cmem_diag_output_error("diff requires two snapshot paths");
            return 2;
        }
        rc = cmd_diff(input_a, input_b, json, quiet);
    } else {
        print_usage();
        return 2;
    }

    return rc;
}

int main(int argc, char **argv)
{
    return cmem_analyze_run(argc, argv);
}
