#!/usr/bin/env python3
"""Extract functions from cmem.c into module files and remove them from cmem.c."""

import re
import subprocess

def get_original_cmem():
    result = subprocess.run(['git', 'show', 'HEAD:src/cmem.c'], capture_output=True, text=True, cwd='/home/quintin/Data/source/c_cpp/memory_pool')
    return result.stdout

def is_function_sig(line):
    stripped = line.strip()
    if not stripped or stripped.startswith('#') or stripped.startswith('//'):
        return False
    if stripped.startswith('/*'):
        return False
    if stripped.endswith(')'):
        if any(stripped.startswith(p) for p in ['if ', 'if(', 'while ', 'while(', 'for ', 'for(', 'switch ', 'switch(', 'return ', 'return(', 'sizeof ', 'sizeof(', 'typedef ', 'typedef(', 'struct ', 'struct(', 'union ', 'union(', 'enum ', 'enum(', 'do ']):
            return False
        return True
    return False

def find_top_level_functions(content):
    lines = content.splitlines(keepends=True)
    functions = []
    i = 0
    n = len(lines)
    
    while i < n:
        line = lines[i]
        stripped = line.strip()
        
        if not stripped or stripped.startswith('#') or stripped.startswith('//'):
            i += 1
            continue
        
        if stripped.startswith('/*'):
            j = i + 1
            while j < n and '*/' not in lines[j]:
                j += 1
            i = j + 1
            continue
        
        if is_function_sig(line):
            j = i + 1
            found_brace = False
            while j < n and j < i + 20:
                next_stripped = lines[j].strip()
                if not next_stripped or next_stripped.startswith('#') or next_stripped.startswith('//'):
                    j += 1
                    continue
                if next_stripped.startswith('/*'):
                    k = j + 1
                    while k < n and '*/' not in lines[k]:
                        k += 1
                    j = k + 1
                    continue
                if next_stripped == '{':
                    found_brace = True
                    break
                break
            
            if found_brace:
                func_match = re.search(r'([a-zA-Z_][a-zA-Z0-9_]*)\s*\([^)]*\)\s*$', stripped)
                if func_match:
                    func_name = func_match.group(1)
                    if func_name not in ['if', 'while', 'for', 'switch', 'return', 'sizeof', 'typedef', 'struct', 'union', 'enum', 'do']:
                        start = i
                        depth = 0
                        k = j
                        while k < n:
                            for ch in lines[k]:
                                if ch == '{':
                                    depth += 1
                                elif ch == '}':
                                    depth -= 1
                                    if depth == 0:
                                        functions.append((func_name, start, k + 1))
                                        i = k + 1
                                        break
                            if depth == 0 and k >= j:
                                break
                            k += 1
                        continue
        
        i += 1
    
    return functions

def get_comment_range(lines, func_start):
    comment_start = func_start - 1
    while comment_start > 0:
        idx = comment_start - 1
        if idx < 0:
            break
        prev_line = lines[idx].strip()
        if prev_line.startswith('*') or prev_line.startswith('//') or prev_line == '':
            comment_start -= 1
        elif prev_line.startswith('/*'):
            comment_start -= 1
            break
        else:
            break
    return max(0, comment_start)

def main():
    original = get_original_cmem()
    original_lines = original.splitlines(keepends=True)
    functions = find_top_level_functions(original)
    
    extract_names = {
        'slab_init', 'slab_create_page', 'slab_alloc', 'slab_free',
        'tls_cache_refill', 'percpu_init', 'percpu_destroy', 'percpu_cpu_index',
        'percpu_pop', 'percpu_push', 'percpu_refill',
        'mp_set_percpu_freelist', 'mp_get_percpu_freelist', 'mp_get_percpu_cpu_count',
        'mp_mark_page_hot', 'mp_mark_page_cold', 'mp_get_hot_page_count',
        'mp_get_cold_page_count', 'mp_separate_hot_cold_pages',
        'tlsf_fls', 'tlsf_ffs', 'tlsf_mapping_insert', 'tlsf_mapping_search',
        'tlsf_create_pool_custom', 'tlsf_insert_free_block', 'tlsf_remove_free_block',
        'tlsf_find_suitable_block', 'tlsf_alloc', 'tlsf_free', 'tlsf_try_inplace_expand',
        'sys_mem_alloc', 'sys_mem_free', 'cmem_sched_getcpu', 'cmem_munmap',
        'cmem_aligned_malloc', 'cmem_aligned_free',
        'mp_expand_pool', 'mp_can_expand', 'mp_get_expandable_size',
        'mp_lock_memory', 'mp_unlock_memory', 'mp_protect_from_dump',
        'mp_secure_zero', 'mp_set_encrypted_memory',
        'mp_madvise', 'mp_trim', 'mp_purge_lazy', 'mp_compact', 'mp_set_memory_limit',
        'mp_audit_heap', 'mp_analyze_leaks', 'mp_export_leak_report',
        'mp_export_html_report', 'mp_export_binary_snapshot', 'mp_parse_binary_snapshot',
        'mp_diff_snapshots', 'mp_get_stats', 'mp_dump_info', 'mp_dump_histogram',
        'print_arena_node', 'mp_dump_tree_info', 'mp_dump_json_stats',
        'mp_export_prometheus_metrics', 'mp_check_leaks',
        'mp_event_log_create', 'mp_event_log_destroy', 'mp_event_log_record',
        'mp_event_log_consume', 'mp_event_log_pending', 'mp_event_log_clear',
        'mp_export_pprof',
        'mp_typed_pool_create', 'mp_typed_alloc', 'mp_typed_free', 'mp_typed_pool_destroy',
        'mp_mark_pool_dirty', 'mp_clear_pool_dirty', 'mp_is_pool_dirty',
        'mp_isolate_bad_block', 'mp_set_thread_quota', 'mp_set_circuit_breaker',
        'mp_get_thread_allocated_bytes', 'mp_reset_thread_quota',
        'mp_is_circuit_breaker_tripped', 'mp_abi_version', 'mp_set_cgroup_aware',
        'mp_get_cgroup_mem_limit', 'mp_asan_is_enabled', 'mp_asan_report_error',
        'mp_asan_check_memory', 'mp_set_asan_integration',
        'mp_ring_create', 'mp_ring_alloc', 'mp_ring_free', 'mp_ring_destroy',
        'check_watermark_after_change', 'active_list_add', 'active_list_remove',
        'mp_set_event_callback', 'mp_set_watermark_callback', 'mp_set_arena_quota',
        'mp_check_arena_quota', 'mp_record_latency', 'mp_reset_latency_stats',
        'mp_get_latency_avg', 'mp_get_latency_p99', 'mp_set_auto_compact',
        'mp_auto_compact_check', 'mp_set_fallback_on_oom', 'mp_set_gc_callback',
        'mp_set_eviction_callback', 'mp_enable_emergency_reserve',
        'mp_set_error_recovery_callback', 'mp_reparse_env_flags', 'mp_get_env_generation',
        'mp_frame_arena_create', 'mp_frame_alloc', 'mp_frame_end', 'mp_frame_arena_destroy',
        'get_slab_class_index', 'percpu_push',
        'mp_aligned_alloc', 'mp_alloc', 'mp_alloc_batch', 'mp_alloc_internal',
        'mp_alloc_loc', 'mp_alloc_size', 'mp_asprintf', 'mp_asprintf_loc',
        'mp_calloc', 'mp_calloc_loc', 'mp_create', 'mp_create_child',
        'mp_create_custom', 'mp_create_from_buffer', 'mp_create_shared',
        'mp_destroy', 'mp_destroy_shared', 'mp_diff_snapshots',
        'mp_enumerate_regions', 'mp_free', 'mp_freeable', 'mp_free_batch',
        'mp_get_allocation_info', 'mp_get_child_count', 'mp_get_name',
        'mp_get_parent', 'mp_get_slab_class_count', 'mp_get_slab_classes',
        'mp_memdup', 'mp_memdup_loc', 'mp_parse_env_flags',
        'mp_preferred_size', 'mp_preferred_size_for_pool', 'mp_pressure',
        'mp_ptr_valid', 'mp_realloc', 'mp_reallocarray', 'mp_reallocarray_loc',
        'mp_realloc_loc', 'mp_reset', 'mp_reset_stats', 'mp_resident',
        'mp_set_arena_quota', 'mp_set_auto_compact', 'mp_set_error_recovery_callback',
        'mp_set_name', 'mp_set_numa_node', 'mp_set_slab_classes',
        'mp_set_watermark_callback', 'mp_strdup', 'mp_strdup_loc', 'mp_usable_size',
    }
    
    modules = {}
    for name, start, end in functions:
        if name in extract_names:
            if name in {'slab_init', 'slab_create_page', 'slab_alloc', 'slab_free',
                        'tls_cache_refill', 'percpu_init', 'percpu_destroy', 'percpu_cpu_index',
                        'percpu_pop', 'percpu_push', 'percpu_refill',
                        'mp_set_percpu_freelist', 'mp_get_percpu_freelist', 'mp_get_percpu_cpu_count',
                        'mp_mark_page_hot', 'mp_mark_page_cold', 'mp_get_hot_page_count',
                        'mp_get_cold_page_count', 'mp_separate_hot_cold_pages',
                        'get_slab_class_index'}:
                module = 'cmem_slab.c'
            elif name in {'tlsf_fls', 'tlsf_ffs', 'tlsf_mapping_insert', 'tlsf_mapping_search',
                          'tlsf_create_pool_custom', 'tlsf_insert_free_block', 'tlsf_remove_free_block',
                          'tlsf_find_suitable_block', 'tlsf_alloc', 'tlsf_free', 'tlsf_try_inplace_expand'}:
                module = 'cmem_tlsf.c'
            elif name in {'sys_mem_alloc', 'sys_mem_free', 'cmem_sched_getcpu', 'cmem_munmap',
                          'cmem_aligned_malloc', 'cmem_aligned_free',
                          'mp_expand_pool', 'mp_can_expand', 'mp_get_expandable_size',
                          'mp_lock_memory', 'mp_unlock_memory', 'mp_protect_from_dump',
                          'mp_secure_zero', 'mp_set_encrypted_memory',
                          'mp_madvise', 'mp_trim', 'mp_purge_lazy', 'mp_compact', 'mp_set_memory_limit'}:
                module = 'cmem_sys.c'
            elif name in {'mp_audit_heap', 'mp_analyze_leaks', 'mp_export_leak_report',
                          'mp_export_html_report', 'mp_export_binary_snapshot', 'mp_parse_binary_snapshot',
                          'mp_diff_snapshots', 'mp_get_stats', 'mp_dump_info', 'mp_dump_histogram',
                          'print_arena_node', 'mp_dump_tree_info', 'mp_dump_json_stats',
                          'mp_export_prometheus_metrics', 'mp_check_leaks'}:
                module = 'cmem_diag.c'
            else:
                module = 'cmem_event.c'
            
            if module not in modules:
                modules[module] = []
            modules[module].append((name, start, end))
    
    # Build set of line ranges to remove (0-indexed)
    remove_ranges = set()
    for module, funcs in modules.items():
        for name, start, end in funcs:
            comment_start = get_comment_range(original_lines, start)
            for ln in range(comment_start, end):
                remove_ranges.add(ln)
    
    # Build new cmem.c by keeping only lines not in remove_ranges
    new_lines = []
    for i, line in enumerate(original_lines):
        if i not in remove_ranges:
            new_lines.append(line)
    
    with open('src/cmem.c', 'w') as f:
        f.writelines(new_lines)
    print(f"Updated cmem.c: {len(new_lines)} lines (was {len(original_lines)})")
    
    for filename, funcs in modules.items():
        content = []
        content.append('/**\n')
        content.append(f' * @file {filename}\n')
        content.append(' * @brief Extracted module implementation.\n')
        content.append(' */\n')
        content.append('\n')
        content.append('#include "cmem.h"\n')
        content.append('#include "cmem_internal.h"\n')
        
        if filename == 'cmem_tlsf.c':
            content.append('#include <string.h>\n')
        elif filename == 'cmem_event.c':
            content.append('#include <stdlib.h>\n')
        
        for name, start, end in funcs:
            func_lines = original_lines[start:end]
            content.extend(func_lines)
            content.append('\n')
        
        with open(f'src/{filename}', 'w') as f:
            f.writelines(content)
        print(f"Wrote {filename} ({len(funcs)} functions)")

if __name__ == '__main__':
    main()
