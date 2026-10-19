# cmem CLI Diagnostic Tools Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement two CLI tools (`cmem-inspect` and `cmem-analyze`) for diagnosing memory issues in cmem-based applications.

**Architecture:** Two independent tools: (1) `cmem-inspect` links `libcmem` for real-time in-process diagnostics with embedded and IPC modes, (2) `cmem-analyze` is a standalone offline snapshot analyzer with zero `libcmem` dependency.

**Tech Stack:** C11, POSIX, cmem public API, CMake, Ninja

---

## Phase 1: Core Framework

### Task 1.1: Create Directory Structure and Shared Utilities

**Files:**
- Create: `tools/cmem-inspect/cmem-inspect.c`
- Create: `tools/cmem-inspect/cmem-inspect.h`
- Create: `tools/cmem-analyze/cmem-analyze.c`
- Create: `tools/cmem-analyze/cmem-analyze.h`
- Create: `tools/common/cmem-diag-output.c`
- Create: `tools/common/cmem-diag-output.h`

- [ ] **Step 1: Create tools directory structure**

```bash
mkdir -p tools/cmem-inspect
mkdir -p tools/cmem-analyze
mkdir -p tools/common
```

- [ ] **Step 2: Create cmem-inspect header**

Create `tools/cmem-inspect/cmem-inspect.h`:

```c
#ifndef CMEM_INSPECT_H
#define CMEM_INSPECT_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool success;
    char output[4096];
    size_t output_len;
    int exit_code;
    const char *error_msg;
} cmem_inspect_result_t;

cmem_inspect_result_t cmem_inspect_run(int argc, char **argv);
cmem_inspect_result_t cmem_inspect_leaks(memory_pool_t *pool, bool json);
cmem_inspect_result_t cmem_inspect_audit(memory_pool_t *pool);
cmem_inspect_result_t cmem_inspect_stats(memory_pool_t *pool, bool json);
cmem_inspect_result_t cmem_inspect_tree(memory_pool_t *pool, bool json);

#endif
```

- [ ] **Step 3: Create cmem-analyze header**

Create `tools/cmem-analyze/cmem-analyze.h`:

```c
#ifndef CMEM_ANALYZE_H
#define CMEM_ANALYZE_H

#include <stdbool.h>
#include <stddef.h>

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
```

- [ ] **Step 4: Create shared output utilities header**

Create `tools/common/cmem-diag-output.h`:

```c
#ifndef CMEM_DIAG_OUTPUT_H
#define CMEM_DIAG_OUTPUT_H

#include <stdbool.h>

void cmem_diag_output_text(const char *fmt, ...);
void cmem_diag_output_json(const char *fmt, ...);
void cmem_diag_output_error(const char *fmt, ...);
void cmem_diag_output_init(bool json, const char *output_file);

#endif
```

- [ ] **Step 5: Create shared output utilities implementation**

Create `tools/common/cmem-diag-output.c`:

```c
#include "cmem-diag-output.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static bool g_json = false;
static FILE *g_output_file = NULL;

void cmem_diag_output_init(bool json, const char *output_file) {
    g_json = json;
    if (output_file) {
        g_output_file = fopen(output_file, "w");
        if (!g_output_file) {
            fprintf(stderr, "Failed to open output file: %s\n", output_file);
            g_output_file = stdout;
        }
    } else {
        g_output_file = stdout;
    }
}

void cmem_diag_output_text(const char *fmt, ...) {
    if (g_json) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_output_file, fmt, args);
    va_end(args);
    fprintf(g_output_file, "\n");
}

void cmem_diag_output_json(const char *fmt, ...) {
    if (!g_json) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_output_file, fmt, args);
    va_end(args);
    fprintf(g_output_file, "\n");
}

void cmem_diag_output_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}
```

- [ ] **Step 6: Commit**

```bash
git add tools/cmem-inspect/cmem-inspect.h tools/cmem-analyze/cmem-analyze.h tools/common/cmem-diag-output.h tools/common/cmem-diag-output.c
git commit -m "chore(tools): 📦 add CLI diagnostic tool skeleton"
```

---

### Task 1.2: Implement cmem-inspect Main Entry and Subcommand Dispatch

**Files:**
- Create: `tools/cmem-inspect/cmem-inspect.c`
- Test: Manual build test

- [ ] **Step 1: Write cmem-inspect main implementation**

Create `tools/cmem-inspect/cmem-inspect.c`:

```c
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
        "  leaks   Detect memory leaks\n"
        "  audit   Heap integrity check\n"
        "  stats   Pool statistics\n"
        "  tree    Arena hierarchy\n"
        "  histogram Allocation size distribution\n"
        "  snapshot Export binary snapshot\n"
        "  diff    Compare two snapshots\n"
        "  html    Export HTML report\n"
        "Options:\n"
        "  --json  Output in JSON format\n"
        "  --output <path> Write output to file\n"
        "  --quiet Suppress informational messages\n"
    );
}

static cmem_inspect_result_t result_error(const char *msg) {
    cmem_inspect_result_t r = {0};
    r.success = false;
    r.exit_code = 2;
    r.error_msg = msg;
    return r;
}

static cmem_inspect_result_t result_ok(const char *out, size_t len) {
    cmem_inspect_result_t r = {0};
    r.success = true;
    r.exit_code = 0;
    memcpy(r.output, out, len < sizeof(r.output) ? len : sizeof(r.output));
    r.output_len = len < sizeof(r.output) ? len : sizeof(r.output);
    return r;
}

static int run_leaks(memory_pool_t *pool, bool json) {
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
            "\"slab_bytes\": %zu, \"tlsf_bytes\": %zu, \"os_bytes\": %zu}",
            stats.active_bytes, stats.active_allocations,
            stats.total_alloc_ops, stats.total_free_ops,
            stats.slab_allocated_bytes, stats.tlsf_allocated_bytes,
            stats.os_allocated_bytes);
    } else {
        cmem_diag_output_text("Active bytes: %zu", stats.active_bytes);
        cmem_diag_output_text("Active allocations: %zu", stats.active_allocations);
        cmem_diag_output_text("Total alloc ops: %zu", stats.total_alloc_ops);
        cmem_diag_output_text("Total free ops: %zu", stats.total_free_ops);
    }
    return 0;
}

static int run_tree(memory_pool_t *pool, bool json) {
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
    bool ok = mp_diff_snapshots(path_a, path_b, "snapshot_diff.txt");
    if (ok) {
        cmem_diag_output_text("Diff report written to snapshot_diff.txt");
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

cmem_inspect_result_t cmem_inspect_run(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return result_error("No subcommand specified");
    }

    const char *subcommand = argv[1];
    bool json = false;
    const char *output_file = NULL;
    bool quiet = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            json = true;
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        }
    }

    cmem_diag_output_init(json, output_file);

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
        if (argc < 4) {
            return result_error("diff requires two snapshot paths");
        }
        rc = run_diff(pool, argv[2], argv[3]);
    } else if (strcmp(subcommand, "html") == 0) {
        rc = run_html(pool);
    } else {
        print_usage();
        return result_error("Unknown subcommand");
    }

    cmem_inspect_result_t r = {0};
    r.success = true;
    r.exit_code = rc;
    return r;
}

int main(int argc, char **argv) {
    cmem_inspect_result_t result = cmem_inspect_run(argc, argv);
    if (!result.success) {
        fprintf(stderr, "Error: %s\n", result.error_msg);
        return 2;
    }
    return result.exit_code;
}
```

- [ ] **Step 2: Build test**

```bash
gcc -std=c11 -I./include -c tools/cmem-inspect/cmem-inspect.c -o /tmp/cmem-inspect.o
```

Expected: Compiles successfully

- [ ] **Step 3: Commit**

```bash
git add tools/cmem-inspect/cmem-inspect.c
git commit -m "feat(tools): 📝 add cmem-inspect main entry and dispatch"
```

---

## Phase 2: cmem-inspect Subcommands

### Task 2.1: Implement JSON Output Support for Stats and Tree

**Files:**
- Modify: `tools/cmem-inspect/cmem-inspect.c`

- [ ] **Step 1: Enhance stats JSON output**

Replace `run_stats()` in `tools/cmem-inspect/cmem-inspect.c`:

```c
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
```

- [ ] **Step 2: Enhance tree JSON output**

Replace `run_tree()` in `tools/cmem-inspect/cmem-inspect.c`:

```c
static int run_tree(memory_pool_t *pool, bool json) {
    if (json) {
        cmem_diag_output_json("{\"tree\": \"see text output\"}");
    }
    mp_dump_tree_info(pool);
    return 0;
}
```

- [ ] **Step 3: Commit**

```bash
git add tools/cmem-inspect/cmem-inspect.c
git commit -m "feat(tools): 📝 add JSON output for stats and tree"
```

---

### Task 2.2: Implement IPC Server Stub

**Files:**
- Modify: `include/cmem.h` (add IPC API declarations)
- Modify: `src/cmem_event.c` (add IPC server stub)
- Create: `tools/cmem-inspect/cmem-inspect-ipc.c`

- [ ] **Step 1: Add IPC API declarations to cmem.h**

Add after `mp_export_html_report` declaration in `include/cmem.h`:

```c
/**
 * @brief Starts a lightweight diagnostic IPC server.
 *
 * @param pool Memory pool to expose for diagnostics
 * @param socket_path Unix domain socket path or named pipe name
 * @return true on success, false on failure
 */
bool mp_start_diag_server(memory_pool_t *pool, const char *socket_path);

/**
 * @brief Stops the diagnostic IPC server.
 */
void mp_stop_diag_server(void);
```

- [ ] **Step 2: Add IPC server stub to cmem_event.c**

Add at end of `src/cmem_event.c`:

```c
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

static memory_pool_t *g_diag_pool = NULL;
static const char *g_diag_socket_path = NULL;
static volatile bool g_diag_server_running = false;

bool mp_start_diag_server(memory_pool_t *pool, const char *socket_path) {
    if (g_diag_server_running) {
        return false;
    }
    g_diag_pool = pool;
    g_diag_socket_path = socket_path;
    g_diag_server_running = true;
    return true;
}

void mp_stop_diag_server(void) {
    g_diag_server_running = false;
    g_diag_pool = NULL;
    g_diag_socket_path = NULL;
}
```

- [ ] **Step 3: Create IPC client stub**

Create `tools/cmem-inspect/cmem-inspect-ipc.c`:

```c
#include "cmem-inspect.h"
#include "cmem.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

cmem_inspect_result_t cmem_inspect_run_ipc(const char *socket_path, const char *subcommand) {
    cmem_inspect_result_t r = {0};
    r.success = false;
    r.exit_code = 2;
    r.error_msg = "IPC mode not yet implemented";

    (void)socket_path;
    (void)subcommand;

    return r;
}
```

- [ ] **Step 4: Commit**

```bash
git add include/cmem.h src/cmem_event.c tools/cmem-inspect/cmem-inspect-ipc.c
git commit -m "feat(tools): 📝 add IPC server stub for cmem-inspect"
```

---

## Phase 3: cmem-analyze Core

### Task 3.1: Implement Binary Snapshot Parser

**Files:**
- Create: `tools/cmem-analyze/cmem-analyze-parser.c`
- Create: `tools/cmem-analyze/cmem-analyze-parser.h`

- [ ] **Step 1: Create snapshot parser header**

Create `tools/cmem-analyze/cmem-analyze-parser.h`:

```c
#ifndef CMEM_ANALYZE_PARSER_H
#define CMEM_ANALYZE_PARSER_H

#include "cmem-analyze.h"
#include <stdbool.h>

bool cmem_analyze_parse_snapshot(const char *path, cmem_snapshot_header_t *header);
bool cmem_analyze_read_leaks(const char *path, cmem_leak_entry_t **entries, size_t *count);
void cmem_analyze_free_leaks(cmem_leak_entry_t *entries, size_t count);

#endif
```

- [ ] **Step 2: Implement snapshot parser**

Create `tools/cmem-analyze/cmem-analyze-parser.c`:

```c
#include "cmem-analyze-parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CMEM_SNAPSHOT_MAGIC 0x4D454D434D454D50ULL

bool cmem_analyze_parse_snapshot(const char *path, cmem_snapshot_header_t *header) {
    if (!path || !header) {
        return false;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }

    uint64_t magic = 0;
    if (fread(&magic, sizeof(magic), 1, fp) != 1) {
        fclose(fp);
        return false;
    }

    if (magic != CMEM_SNAPSHOT_MAGIC) {
        fclose(fp);
        return false;
    }

    uint32_t version = 0;
    if (fread(&version, sizeof(version), 1, fp) != 1) {
        fclose(fp);
        return false;
    }

    if (version != 1) {
        fclose(fp);
        return false;
    }

    memset(header, 0, sizeof(*header));
    header->magic = magic;
    header->version = version;

    if (fread(&header->timestamp, sizeof(header->timestamp), 1, fp) != 1) {
        fclose(fp);
        return false;
    }

    char pool_name[256] = {0};
    if (fread(pool_name, sizeof(pool_name), 1, fp) != 1) {
        fclose(fp);
        return false;
    }
    header->pool_name = strdup(pool_name);

    if (fread(&header->total_pool_size, sizeof(header->total_pool_size), 1, fp) != 1) {
        fclose(fp);
        return false;
    }
    if (fread(&header->active_bytes, sizeof(header->active_bytes), 1, fp) != 1) {
        fclose(fp);
        return false;
    }
    if (fread(&header->active_allocs, sizeof(header->active_allocs), 1, fp) != 1) {
        fclose(fp);
        return false;
    }
    if (fread(&header->leak_count, sizeof(header->leak_count), 1, fp) != 1) {
        fclose(fp);
        return false;
    }

    fclose(fp);
    return true;
}

static char *read_string(FILE *fp) {
    char buf[256];
    size_t len = 0;
    int c;
    while ((c = fgetc(fp)) != EOF && len < sizeof(buf) - 1) {
        buf[len++] = (char)c;
        if (c == '\0') break;
    }
    buf[len] = '\0';
    return strdup(buf);
}

bool cmem_analyze_read_leaks(const char *path, cmem_leak_entry_t **entries, size_t *count) {
    if (!path || !entries || !count) {
        return false;
    }

    cmem_snapshot_header_t header;
    if (!cmem_analyze_parse_snapshot(path, &header)) {
        return false;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }

    fseek(fp, sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint64_t) + 256 + sizeof(size_t) * 4, SEEK_SET);

    *entries = calloc(header.leak_count, sizeof(cmem_leak_entry_t));
    if (!*entries) {
        fclose(fp);
        return false;
    }

    *count = header.leak_count;
    for (size_t i = 0; i < header.leak_count; i++) {
        cmem_leak_entry_t *e = &(*entries)[i];
        fread(&e->address, sizeof(e->address), 1, fp);
        fread(&e->size, sizeof(e->size), 1, fp);
        fread(&e->usable_size, sizeof(e->usable_size), 1, fp);
        uint32_t tier = 0;
        fread(&tier, sizeof(tier), 1, fp);
        e->tier = (int)tier;
        e->file = read_string(fp);
        fread(&e->line, sizeof(e->line), 1, fp);
        e->func = read_string(fp);
        fread(&e->timestamp, sizeof(e->timestamp), 1, fp);
        uint16_t depth = 0;
        fread(&depth, sizeof(depth), 1, fp);
        e->backtrace_depth = depth < 8 ? depth : 8;
        for (size_t j = 0; j < e->backtrace_depth; j++) {
            fread(&e->backtrace_addrs[j], sizeof(void *), 1, fp);
        }
    }

    fclose(fp);
    return true;
}

void cmem_analyze_free_leaks(cmem_leak_entry_t *entries, size_t count) {
    if (!entries) return;
    for (size_t i = 0; i < count; i++) {
        free((void *)(uintptr_t)(uintptr_t)entries[i].file);
        free((void *)(uintptr_t)entries[i].func);
    }
    free(entries);
}
```

- [ ] **Step 3: Commit**

```bash
git add tools/cmem-analyze/cmem-analyze-parser.c tools/cmem-analyze/cmem-analyze-parser.h
git commit -m "feat(tools): 📝 add binary snapshot parser for cmem-analyze"
```

---

### Task 3.2: Implement cmem-analyze Main Entry and Subcommands

**Files:**
- Create: `tools/cmem-analyze/cmem-analyze.c`

- [ ] **Step 1: Write cmem-analyze main implementation**

Create `tools/cmem-analyze/cmem-analyze.c`:

```c
#include "cmem-analyze.h"
#include "cmem-analyze-parser.h"
#include "cmem-diag-output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(void) {
    fprintf(stderr,
        "Usage: cmem-analyze <subcommand> [options] <input>\n"
        "Subcommands:\n"
        "  report   Full analysis report\n"
        "  top      Top-N leaks by size\n"
        "  summary  Summary statistics\n"
        "  validate Verify snapshot integrity\n"
        "  convert  Format conversion\n"
        "Options:\n"
        "  --json  Output in JSON format\n"
        "  --html  Output in HTML format\n"
        "  --output <path> Write output to file\n"
        "  --quiet Suppress informational messages\n"
    );
}

static int cmd_report(const char *path, bool json, bool html) {
    cmem_snapshot_header_t header;
    if (!cmem_analyze_parse_snapshot(path, &header)) {
        cmem_diag_output_error("Failed to parse snapshot: %s", path);
        return 2;
    }

    if (json) {
        cmem_diag_output_json(
            "{\"pool\": \"%s\", \"timestamp\": %llu, "
            "\"leaks\": %zu, \"bytes\": %zu}",
            header.pool_name, (unsigned long long)header.timestamp,
            header.leak_count, header.leak_bytes);
    } else {
        cmem_diag_output_text("Pool: %s", header.pool_name);
        cmem_diag_output_text("Leaks: %zu blocks, %zu bytes",
            header.leak_count, header.leak_bytes);
    }

    if (html) {
        cmem_diag_output_text("HTML report generation not yet implemented");
    }

    return header.leak_count > 0 ? 1 : 0;
}

static int cmd_top(const char *path, int n, bool json) {
    cmem_leak_entry_t *entries = NULL;
    size_t count = 0;
    if (!cmem_analyze_read_leaks(path, &entries, &count)) {
        cmem_diag_output_error("Failed to read leaks from: %s", path);
        return 2;
    }

    size_t show = n > 0 && n < (int)count ? (size_t)n : count;
    if (json) {
        cmem_diag_output_json("[");
        for (size_t i = 0; i < show; i++) {
            cmem_diag_output_json(
                "{\"address\": \"%p\", \"size\": %zu, \"tier\": %d, "
                "\"file\": \"%s\", \"line\": %d}",
                entries[i].address, entries[i].size, entries[i].tier,
                entries[i].file ? entries[i].file : "", entries[i].line);
            if (i + 1 < show) cmem_diag_output_json(",");
        }
        cmem_diag_output_json("]");
    } else {
        for (size_t i = 0; i < show; i++) {
            cmem_diag_output_text("[%zu] %p | %zu bytes | tier=%d | %s:%d",
                i + 1, entries[i].address, entries[i].size, entries[i].tier,
                entries[i].file ? entries[i].file : "unknown",
                entries[i].line);
        }
    }

    cmem_analyze_free_leaks(entries, count);
    return show > 0 ? 1 : 0;
}

static int cmd_summary(const char *path, bool json) {
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
            header.pool_name, header.total_pool_size,
            header.active_bytes, header.active_allocs,
            header.leak_count, header.leak_bytes);
    } else {
        cmem_diag_output_text("Pool: %s", header.pool_name);
        cmem_diag_output_text("Total pool size: %zu bytes", header.total_pool_size);
        cmem_diag_output_text("Active bytes: %zu", header.active_bytes);
        cmem_diag_output_text("Active allocations: %zu", header.active_allocs);
        cmem_diag_output_text("Leak count: %zu", header.leak_count);
        cmem_diag_output_text("Leak bytes: %zu", header.leak_bytes);
    }

    return header.leak_count > 0 ? 1 : 0;
}

static int cmd_validate(const char *path) {
    cmem_snapshot_header_t header;
    bool ok = cmem_analyze_parse_snapshot(path, &header);
    if (ok) {
        cmem_diag_output_text("Snapshot valid: %s", path);
        cmem_diag_output_text("Pool: %s, leaks: %zu, bytes: %zu",
            header.pool_name, header.leak_count, header.leak_bytes);
        return 0;
    }
    cmem_diag_output_error("Invalid snapshot: %s", path);
    return 2;
}

static int cmd_convert(const char *path, bool json, bool html) {
    if (html) {
        cmem_diag_output_text("HTML conversion not yet implemented");
        return 2;
    }
    if (json) {
        cmem_snapshot_header_t header;
        if (!cmem_analyze_parse_snapshot(path, &header)) {
            cmem_diag_output_error("Failed to parse snapshot: %s", path);
            return 2;
        }
        cmem_diag_output_json(
            "{\"pool\": \"%s\", \"timestamp\": %llu, "
            "\"total_pool_size\": %zu, \"active_bytes\": %zu, "
            "\"active_allocs\": %zu, \"leak_count\": %zu, \"leak_bytes\": %zu}",
            header.pool_name, (unsigned long long)header.timestamp,
            header.total_pool_size, header.active_bytes,
            header.active_allocs, header.leak_count, header.leak_bytes);
        return 0;
    }
    cmem_diag_output_error("No output format specified for convert");
    return 2;
}

int cmem_analyze_run(int argc, char **argv) {
    if (argc < 3) {
        print_usage();
        return 2;
    }

    const char *subcommand = argv[1];
    bool json = false;
    bool html = false;
    const char *output_file = NULL;
    bool quiet = false;
    const char *input = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            json = true;
        } else if (strcmp(argv[i], "--html") == 0) {
            html = true;
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (!input) {
            input = argv[i];
        }
    }

    cmem_diag_output_init(json, output_file);

    if (!input) {
        print_usage();
        return 2;
    }

    int rc = 0;
    if (strcmp(subcommand, "report") == 0) {
        rc = cmd_report(input, json, html);
    } else if (strcmp(subcommand, "top") == 0) {
        int n = 0;
        for (int i = 2; i < argc; i++) {
            if (strncmp(argv[i], "--top=", 6) == 0) {
                n = atoi(argv[i] + 6);
                break;
            }
        }
        rc = cmd_top(input, n, json);
    } else if (strcmp(subcommand, "summary") == 0) {
        rc = cmd_summary(input, json);
    } else if (strcmp(subcommand, "validate") == 0) {
        rc = cmd_validate(input);
    } else if (strcmp(subcommand, "convert") == 0) {
        rc = cmd_convert(input, json, html);
    } else {
        print_usage();
        return 2;
    }

    return rc;
}

int main(int argc, char **argv) {
    return cmem_analyze_run(argc, argv);
}
```

- [ ] **Step 2: Build test**

```bash
gcc -std=c11 -I./include -c tools/cmem-analyze/cmem-analyze.c -o /tmp/cmem-analyze.o
```

Expected: Compiles successfully

- [ ] **Step 3: Commit**

```bash
git add tools/cmem-analyze/cmem-analyze.c
git commit -m "feat(tools): 📝 add cmem-analyze main entry and subcommands"
```

---

## Phase 4: Build System Integration

### Task 4.1: Add CMake Targets

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add cmem-inspect target**

Add after `add_executable(example_embedded ...)` in `CMakeLists.txt`:

```cmake
add_executable(cmem-inspect tools/cmem-inspect/cmem-inspect.c)
target_link_libraries(cmem-inspect PRIVATE cmem pthread ${RT_LIBRARY})
add_dependencies(cmem-inspect format-check check-mermaid)
```

- [ ] **Step 2: Add cmem-analyze target**

Add after `cmem-inspect` target:

```cmake
add_executable(cmem-analyze tools/cmem-analyze/cmem-analyze.c tools/common/cmem-diag-output.c)
target_link_libraries(cmem-analyze PRIVATE pthread ${RT_LIBRARY})
add_dependencies(cmem-analyze format-check check-mermaid)
```

- [ ] **Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: 📦 add cmem-inspect and cmem-analyze targets"
```

---

### Task 4.2: Add Makefile Targets

**Files:**
- Modify: `Makefile`

- [ ] **Step 1: Add tools target**

Add after `examples:` target in `Makefile`:

```makefile
# 构建诊断工具
tools: cmem-inspect cmem-analyze

cmem-inspect: lib
	$(CC) $(CFLAGS) -I./include tools/cmem-inspect/cmem-inspect.c -o build/cmem-inspect -L./build -lcmem -lpthread $(LDFLAGS)

cmem-analyze: tools/common/cmem-diag-output.c
	$(CC) $(CFLAGS) -I./include tools/cmem-analyze/cmem-analyze.c tools/common/cmem-diag-output.c -o build/cmem-analyze -lpthread $(LDFLAGS)
```

- [ ] **Step 2: Add tools to help text**

Update `help:` target:

```makefile
	@echo "  tools        - Build diagnostic tools (cmem-inspect, cmem-analyze)"
```

- [ ] **Step 3: Commit**

```bash
git add Makefile
git commit -m "build: 📦 add tools target for diagnostic CLIs"
```

---

## Phase 5: Testing and CI

### Task 5.1: Add Smoke Tests

**Files:**
- Create: `tests/test_cli_tools.c`

- [ ] **Step 1: Create smoke test file**

Create `tests/test_cli_tools.c`:

```c
#include "cmem.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

static void test_inspect_compiles(void) {
    int rc = system("make cmem-inspect >/dev/null 2>&1");
    assert(rc == 0 || !"cmem-inspect build failed");
}

static void test_analyze_compiles(void) {
    int rc = system("make cmem-analyze >/dev/null 2>&1");
    assert(rc == 0 || !"cmem-analyze build failed");
}

int main() {
    printf("=== CLI Tools Smoke Tests ===\n");
    test_inspect_compiles();
    printf("[PASS] cmem-inspect builds\n");
    test_analyze_compiles();
    printf("[PASS] cmem-analyze builds\n");
    printf("All CLI tool smoke tests passed!\n");
    return 0;
}
```

- [ ] **Step 2: Add CMake target**

Add to `CMakeLists.txt`:

```cmake
add_executable(cli_tools tests/test_cli_tools.c)
target_link_libraries(cli_tools PRIVATE cmem pthread ${RT_LIBRARY})
add_dependencies(cli_tools format-check check-mermaid)
```

- [ ] **Step 3: Commit**

```bash
git add tests/test_cli_tools.c
git commit -m "test: ✅ add CLI tools smoke tests"
```

---

### Task 5.2: Add CI Steps

**Files:**
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 1: Add tools build step**

Add after `make test_cpp` in `build-and-test` job:

```yaml
      - name: Build Diagnostic CLI Tools
        run: make tools
```

- [ ] **Step 2: Add tools test step**

Add after `make test_cpp` in `build-and-test` job:

```yaml
      - name: Run CLI Tools Smoke Tests
        run: ./build/cli_tools
```

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: 👷 add diagnostic CLI tools to CI"
```

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-08-04-cmem-cli-diag-tools.md`.

**Two execution options:**

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**
