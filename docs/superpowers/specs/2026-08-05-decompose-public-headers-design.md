# Spec: Modular Public Header Decomposition for cmem

## Overview

This specification defines the modular decomposition of the main public header `include/cmem.h` (~1,900 lines) into dedicated subsystem headers (`cmem_ring.h`, `cmem_tlsf.h`, `cmem_diag.h`, `cmem_snapshot.h`, `cmem_metrics.h`, `cmem_arena.h`, `cmem_frame.h`, `cmem_typed_pool.h`). `include/cmem.h` will serve as the primary entry point, automatically including all subsystem headers to guarantee 100% backward compatibility for existing codebases while giving developers clean, single-subsystem include options for embedded or modular projects.

---

## Subsystem Header Division

### 1. `include/cmem_ring.h`
- DPDK-style SPSC lock-free ring buffer allocator (`cmem_ring_buffer_t`, `mp_ring_create`, `mp_ring_alloc`, `mp_ring_free`, `mp_ring_destroy`).
- Structured event log ring buffer (`mp_event_log_t`, `mp_event_type_t`, `mp_event_log_create`, `mp_event_log_record`, `mp_event_log_consume`, `mp_event_log_pending`, `mp_event_log_clear`).

### 2. `include/cmem_tlsf.h`
- TLSF allocator specific structures and fine-tuning parameters (`tlsf_pool_t`, TLSF block size constants, and TLSF control helpers).

### 3. `include/cmem_diag.h`
- Heap auditing, leak analysis, and HTML profiling (`mp_audit_heap`, `mp_check_leaks`, `mp_analyze_leaks`, `mp_export_leak_report`, `mp_export_html_report`, `mp_get_leak_severity`, `mp_analyze_leak_pattern`, `mp_leak_severity_t`, `mp_leak_pattern_t`).

### 4. `include/cmem_snapshot.h`
- Post-mortem binary crash dumps and diffing (`mp_export_binary_snapshot`, `mp_parse_binary_snapshot`, `mp_diff_snapshots`).

### 5. `include/cmem_metrics.h`
- Prometheus exporters, latency histogram, JSON stats dumps, and pprof profiling (`mp_export_prometheus_metrics`, `mp_dump_json_stats`, `mp_dump_histogram`, `mp_get_stats`, `mp_record_latency`, `mp_get_latency_p99`, `mp_get_latency_avg`, `mp_reset_latency_stats`, `mp_export_pprof`).

### 6. `include/cmem_arena.h`
- Hierarchical child pools, multi-arena partitioning, and per-arena memory quota (`mp_create_arena`, `mp_create_child_pool`, `mp_set_arena_quota`, `mp_dump_tree_info`).

### 7. `include/cmem_frame.h`
- Game & graphics dual ping-pong frame arena (`cmem_frame_arena_t`, `mp_frame_arena_create`, `mp_frame_alloc`, `mp_frame_end`, `mp_frame_arena_destroy`).

### 8. `include/cmem_typed_pool.h`
- Zero-overhead typed object pool allocator (`mp_typed_pool_t`, `mp_typed_pool_create`, `mp_typed_alloc`, `mp_typed_free`, `mp_typed_pool_destroy`, `MP_TYPED_POOL_DEFINE`).

### 9. `include/cmem.h` (Main Header)
- Retain core pool creation (`mp_create`, `mp_destroy`, `mp_reset`, `mp_expand_pool`), allocation (`mp_alloc`, `mp_free`, `mp_calloc`, `mp_realloc`, `mp_alloc_fast`, `mp_free_fast`, `mp_alloc_batch`, `mp_free_batch`), flags (`mp_flags_t`), and memory compaction (`mp_compact`, `mp_trim`, `mp_madvise`, `mp_purge_lazy`).
- Include all subsystem headers at the end of `cmem.h` (guaranteeing 100% backward compatibility).

---

## Build System & Tooling Updates

- Update `CMakeLists.txt` `format-check` file list and `install(FILES ... DESTINATION include)` list with all new header files.
- Update `Makefile` `install` and `uninstall` rules with all new header files.

---

## Verification Plan

1. **Format & Mermaid Validation**:
   - `make format-check` and `make check-mermaid` pass cleanly.
2. **Build System Tests**:
   - `make lib` builds static and shared libraries cleanly.
   - `cmake -B build_cmake -G Ninja -DCMEM_ENABLE_DIAGNOSTICS=OFF && cmake --build build_cmake` builds successfully.
   - `cmake -B build_cmake -G Ninja -DCMEM_ENABLE_DIAGNOSTICS=ON && cmake --build build_cmake && ctest --test-dir build_cmake` passes 100%.
