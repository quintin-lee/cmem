# cmem CLI Diagnostic Tools Design Specification

## 1. Overview

This specification defines two CLI tools for diagnosing memory issues in applications using cmem:

- **`cmem-inspect`**: In-process diagnostic tool that links against `libcmem` and calls its public APIs directly.
- **`cmem-analyze`**: Offline analysis tool that parses cmem-exported snapshot files without linking to `libcmem`.

Both tools are designed to help upper-layer applications identify, analyze, and resolve memory problems such as leaks, heap corruption, and fragmentation.

## 2. cmem-inspect Design

### 2.1 Purpose

`cmem-inspect` provides real-time, in-process memory diagnostics by directly invoking cmem's public API. It is suitable for:

- Debug builds where the application can embed a diagnostic call
- Development environments where source-level location tracking is available
- Interactive debugging sessions where immediate feedback is required

### 2.2 Interface

```c
typedef struct {
    bool success;
    char output[4096];
    size_t output_len;
    int exit_code;          // 0=healthy, 1=issues found, 2=error
    const char *error_msg;
} cmem_inspect_result_t;

cmem_inspect_result_t cmem_inspect_run(int argc, char **argv);
cmem_inspect_result_t cmem_inspect_leaks(memory_pool_t *pool, bool json);
cmem_inspect_result_t cmem_inspect_audit(memory_pool_t *pool);
cmem_inspect_result_t cmem_inspect_stats(memory_pool_t *pool, bool json);
cmem_inspect_result_t cmem_inspect_tree(memory_pool_t *pool, bool json);
```

### 2.3 Subcommands

| Subcommand | Function | API Used | Output Formats |
|---|---|---|---|
| `leaks` | Detect memory leaks | `mp_analyze_leaks()`, `mp_export_leak_report()` | text, JSON |
| `audit` | Heap integrity check | `mp_audit_heap()` | text |
| `stats` | Pool statistics | `mp_get_stats()` | text, JSON, Prometheus |
| `tree` | Arena hierarchy | `mp_dump_tree_info()` | text, JSON |
| `histogram` | Allocation size distribution | `mp_dump_histogram()` | text |
| `snapshot` | Export binary snapshot | `mp_export_binary_snapshot()` | binary file |
| `diff` | Compare two snapshots | `mp_diff_snapshots()` | text, HTML |
| `html` | Export HTML report | `mp_export_html_report()` | HTML file |

### 2.4 Data Sources

#### Direct Pointer (Embedded Mode)

Application passes `memory_pool_t*` directly:

```c
memory_pool_t *pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);
cmem_inspect_result_t r = cmem_inspect_leaks(pool, false);
printf("%.*s", (int)r.output_len, r.output);
```

#### IPC (Standalone Mode)

Application starts a lightweight diagnostic server; `cmem-inspect` connects via IPC:

```c
// Application side
mp_start_diag_server(pool, "/tmp/cmem-diag.sock");

// CLI side
// cmem-inspect leaks --source unix:/tmp/cmem-diag.sock
// cmem-inspect stats --source namedpipe:\\cmem-diag-pipe
```

IPC protocol:
- Request: JSON or simple text commands
- Response: JSON (structured) or text
- Supported commands: `leaks`, `audit`, `stats`, `tree`, `histogram`, `snapshot`

### 2.5 Output Formats

Default is human-readable text. Use `--json` for JSON output.

Text example:
```
[LEAK REPORT] Pool: RootArena
  Total Managed: 65536 bytes
  Active Leaked: 128 bytes (1 blocks)

  [Leak #1] Address: 0x7f9aa94f00c8 | Size: 128 bytes | Tier: SLAB
```

JSON example:
```json
{
  "pool": "RootArena",
  "total_managed": 65536,
  "active_leaked_bytes": 128,
  "active_leaked_blocks": 1,
  "leaks": [
    { "address": "0x7f9aa94f00c8", "size": 128, "tier": "SLAB" }
  ]
}
```

### 2.6 Error Handling

- API failures map to `exit_code=2` with `error_msg` set
- Leak detection returns `exit_code=1` with details in `output`
- Heap corruption returns `exit_code=1` with audit errors in `output`
- Healthy pool returns `exit_code=0`

## 3. cmem-analyze Design

### 3.1 Purpose

`cmem-analyze` performs offline analysis of cmem snapshot files without linking to `libmem`. It is suitable for:

- Post-mortem analysis of production crashes
- CI/CD integration where the application binary is unavailable
- Historical trend analysis across multiple snapshots
- Environments where running the application is not feasible

### 3.2 Interface

```c
typedef struct {
    bool json_output;
    bool html_output;
    int top_n;              // 0 = all
    const char *output_file; // NULL = stdout
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
    mp_alloc_type_t tier;
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
    cmem_leak_entry_t **fixed_leak_entries;
} cmem_diff_result_t;

int cmem_analyze_run(int argc, char **argv);
bool cmem_analyze_parse_snapshot(const char *path, cmem_snapshot_header_t *header);
bool cmem_analyze_diff(const char *path_a, const char *path_b, cmem_diff_result_t *result);
bool cmem_analyze_top_leaks(const char *path, int n, cmem_leak_entry_t **entries, size_t *count);
```

### 3.3 Subcommands

| Subcommand | Function | Input | Output |
|---|---|---|---|
| `diff` | Compare two snapshots | Binary snapshot x2 | text, JSON, HTML |
| `report` | Full analysis report | Binary or JSON snapshot | text, JSON, HTML |
| `top` | Top-N leaks by size/count | Binary or JSON snapshot | text, JSON |
| `summary` | Summary statistics | Binary or JSON snapshot | text, JSON |
| `validate` | Verify snapshot integrity | Binary snapshot | text |
| `convert` | Format conversion | Binary snapshot / JSON | JSON / HTML |

### 3.4 Binary Snapshot Format

```
[Header]
  magic: "CMEMDMP" (8 bytes)
  version: uint32_t
  timestamp: uint64_t (unix ms)
  pool_name: null-terminated string
  total_pool_size: size_t
  active_bytes: size_t
  active_allocs: size_t
  leak_count: size_t

[Block Records] (leak_count entries)
  address: void*
  size: size_t
  usable_size: size_t
  tier: uint32_t
  file: null-terminated string (or empty)
  line: int32_t
  func: null-terminated string (or empty)
  timestamp: uint64_t
  backtrace_depth: uint16_t
  backtrace_addrs: void*[backtrace_depth]
```

Parser requirements:
- Use `fread()` for field-by-field reading
- Handle endianness (snapshot records host byte order at export time)
- Strings are null-terminated variable length
- Backtrace addresses are raw pointers only; symbol resolution requires target platform environment and is out of scope

### 3.5 Diff Algorithm

1. Group leak blocks from both snapshots by `(address, size)` hash
2. Match blocks with identical addresses, mark as `unchanged`
3. Blocks only in snapshot B become `new_leak`
4. Blocks only in snapshot A become `fixed_leak`
5. Sort `new_leak` by size descending for Top-N output

### 3.6 Output Formats

Text example:
```
=== Snapshot Diff Report ===
  Snapshot A: 2026-08-03T10:00:00Z (leaks: 5, bytes: 1024)
  Snapshot B: 2026-08-03T11:00:00Z (leaks: 8, bytes: 2048)

  Summary:
    New leaks:      +3 blocks, +1024 bytes
    Fixed leaks:    -0 blocks, -0 bytes
    Net change:     +1024 bytes

  Top New Leaks:
    [1] 0x7f9aa94f00c8 | 512 bytes | SLAB | test_main.c:45
    [2] 0x7f9aa94f0100 | 256 bytes | TLSF | test_advanced.c:120
    [3] 0x7f9aa94f0200 | 256 bytes | SLAB | cmem_event.c:89
```

JSON example:
```json
{
  "snapshot_a": { "timestamp": "2026-08-03T10:00:00Z", "leaks": 5, "bytes": 1024 },
  "snapshot_b": { "timestamp": "2026-08-03T11:00:00Z", "leaks": 8, "bytes": 2048 },
  "summary": {
    "new_leaks": 3,
    "fixed_leaks": 0,
    "net_leak_bytes": 1024
  },
  "new_leaks": [
    { "address": "0x7f9aa94f00c8", "size": 512, "tier": "SLAB", "file": "test_main.c", "line": 45 }
  ]
}
```

HTML output:
- Self-contained HTML file
- Summary cards (total leaks, bytes, trend)
- Sortable leak entry table
- Collapsible backtrace detail panels

### 3.7 Error Handling

- Missing or unreadable snapshot: `exit_code=2`, stderr message
- Invalid format (bad magic, unsupported version): `exit_code=2`, format error details
- OOM during analysis: `exit_code=2`, OOM message
- Leaks found: `exit_code=1`, stdout contains leak details
- No leaks: `exit_code=0`, stdout confirms clean state

## 4. Shared Considerations

### 4.1 Exit Codes

| Code | Meaning |
|---|---|
| 0 | Healthy / analysis completed successfully |
| 1 | Issues found (leaks, heap corruption, etc.) |
| 2 | Tool error (bad args, missing file, parse error) |

### 4.2 Output Location

- Default: stdout for text/JSON, current directory for HTML/binary files
- `--output <path>`: write to specified file path
- `--quiet`: suppress informational messages, output results only

### 4.3 Integration Points

- `cmem-inspect` links against `libcmem` and includes `cmem.h` / `cmem_internal.h`
- `cmem-analyze` is standalone; it only depends on the C standard library
- Both tools share no code to keep `cmem-analyze` independent
- Snapshot format version is defined in `include/cmem.h` to ensure compatibility

## 5. Implementation Plan

### Phase 1: Core Framework
- Create `tools/cmem-inspect/` and `tools/cmem-analyze/` directories
- Implement argument parsing and subcommand dispatch for both tools
- Implement shared output formatting utilities (text, JSON)

### Phase 2: cmem-inspect Subcommands
- Implement `leaks`, `audit`, `stats`, `tree` subcommands
- Add IPC server stub in `src/cmem_event.c` (`mp_start_diag_server()`)
- Add IPC client in `cmem-inspect`

### Phase 3: cmem-analyze Subcommands
- Implement binary snapshot parser
- Implement `diff`, `report`, `top`, `summary`, `validate`, `convert` subcommands
- Add JSON snapshot import path

### Phase 4: Integration
- Add CMake targets for both tools
- Update `Makefile` with `tools` target
- Add CI steps for tool build and smoke tests

## 6. Open Questions

- Should `cmem-analyze` support symbol resolution via `addr2line` or `dbghelp`? (Initial scope: no)
- Should `cmem-inspect` support remote inspection over TCP? (Initial scope: unix domain socket / named pipe only)
- Should snapshot format include a checksum or signature for integrity verification? (Recommended: yes, add CRC32 in header)
