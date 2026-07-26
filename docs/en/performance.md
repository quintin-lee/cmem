# Performance Tuning Guide

## Table of Contents

1. [Performance Model](#1-performance-model)
2. [Pool Size Tuning](#2-pool-size-tuning)
3. [Flag Selection Strategy](#3-flag-selection-strategy)
4. [Slab Class Tuning](#4-slab-class-tuning)
5. [Concurrency Optimization](#5-concurrency-optimization)
6. [Memory Reclamation Strategy](#6-memory-reclamation-strategy)
7. [NUMA Optimization](#7-numa-optimization)
8. [HugePages Optimization](#8-hugepages-optimization)
9. [Monitoring Metrics](#9-monitoring-metrics)
10. [Performance Pitfalls](#10-performance-pitfalls)

---

## 1. Performance Model

cmem adopts a three-tier hybrid architecture, each tier having different performance characteristics:

| Tier | Size Range | Allocation Complexity | Typical Throughput | Applicable Scenario |
| :--- | :--- | :--- | :--- | :--- |
| Slab | <= 512B | O(1) | ~1.8 Gops/sec | Small objects, high-frequency allocation |
| TLSF | 512B ~ 4MB | O(1) | ~400 Mops/sec | Medium objects, variable-length allocation |
| OS Fallback | > 4MB | O(n) | ~50 Mops/sec | Large objects, low-frequency allocation |

### 1.1 Allocation Path

```
mp_alloc(pool, size)
  │
  ├─ size > 4MB ──────────────► Direct OS Fallback
  │
  ├─ 512B < size <= 4MB ──────► TLSF Allocator
  │     ├─ Lookup FL/SL bitmap O(1)
  │     └─ Split/merge blocks
  │
  └─ size <= 512B ────────────► Slab Pool
        ├─ [PERCPU_FREELIST] lock-free CAS
        ├─ [TLS_CACHE]       lock-free
        └─ [SLAB_CLASS_LOCK] fine-grained lock
```

---

## 2. Pool Size Tuning

### 2.1 Initial Capacity Selection

```c
// Auto-calculated (recommended)
memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);

// Manually specified
memory_pool_t* pool = mp_create(64 * 1024 * 1024, MP_FLAG_DEFAULT);  // 64MB
```

**Recommendations:**
- Small applications (< 1GB memory): `0` (auto) or `16MB`
- Medium applications (1GB~8GB): `64MB` ~ `256MB`
- Large applications (> 8GB): `256MB` ~ `1GB`

### 2.2 Online Expansion

```c
// Monitor expandable space
size_t expandable = mp_get_expandable_size(pool);

// Dynamic expansion
if (expandable < 64 * 1024 * 1024) {  // Remaining < 64MB
    mp_expand_pool(pool, 64 * 1024 * 1024);  // Append 64MB
}
```

### 2.3 Memory Limit

```c
// Set hard upper limit
mp_set_memory_limit(pool, 512 * 1024 * 1024);  // 512MB

// With watermark callback
mp_set_watermark_callback(pool, 0.8, 0.5, watermark_cb, NULL);
```

---

## 3. Flag Selection Strategy

### 3.1 Production Environment Recommendations

```c
// General production environment
mp_flags_t flags = MP_FLAG_THREAD_SAFE | MP_FLAG_THREAD_LOCAL_CACHE;

// High concurrency scenario
mp_flags_t flags = MP_FLAG_THREAD_SAFE | MP_FLAG_PERCPU_FREELIST;

// Security-sensitive scenario
mp_flags_t flags = MP_FLAG_THREAD_SAFE | MP_FLAG_DEBUG_CANARY |
                   MP_FLAG_POISON_ON_FREE | MP_FLAG_ENCRYPTED_MEMORY;

// Large memory / low latency scenario
mp_flags_t flags = MP_FLAG_THREAD_SAFE | MP_FLAG_HUGE_PAGES |
                   MP_FLAG_HOT_COLD_SEPARATION;
```

### 3.2 Flag Overhead Comparison

| Flag | Memory Overhead | Performance Overhead | Recommended Scenario |
| :--- | :--- | :--- | :--- |
| `MP_FLAG_THREAD_SAFE` | None | ~5-10% | Required for multi-thread |
| `MP_FLAG_THREAD_LOCAL_CACHE` | ~1KB per thread | < 1% | High-frequency small objects |
| `MP_FLAG_PERCPU_FREELIST` | ~16 slots per CPU per Class | < 2% | High-concurrency small objects |
| `MP_FLAG_DEBUG_CANARY` | +1B per block | ~3% | Development/debugging |
| `MP_FLAG_TRACK_LOCATIONS` | ~80B per block | ~5% | Leak analysis |
| `MP_FLAG_POISON_ON_FREE` | None | < 1% | UAF detection |
| `MP_FLAG_CACHE_ALIGNED` | Possible waste | < 1% | High-performance scenarios |
| `MP_FLAG_GUARD_PAGES` | +2 pages per block | ~2-5% | Debugging/security |
| `MP_FLAG_HUGE_PAGES` | None | ~1% | Large memory |
| `MP_FLAG_ENCRYPTED_MEMORY` | None | ~2-3% | Security-sensitive |
| `MP_FLAG_ASAN_INTEGRATION` | None | Compile-time dependent | ASan environment |

---

## 4. Slab Class Tuning

### 4.1 Default Class Table

```c
static const size_t kSlabSizes[SLAB_CLASS_COUNT] = {
    8, 16, 32, 64, 128, 256, 512
};
```

### 4.2 Custom Class Table

```c
// Optimize for specific workload
size_t custom_sizes[] = {16, 32, 64, 128, 256, 512, 1024};
mp_set_slab_classes(pool, custom_sizes, 7);
```

**Recommendations:**
- Adjust based on actual allocation size distribution
- Avoid too many size classes (recommended <= 16)
- Keep size classes as powers of 2

### 4.3 Monitor Size Distribution

```c
mp_dump_histogram(pool);
```

**Output Example:**
```
Bucket 0  [1 B - 2 B    ] : 10
Bucket 1  [3 B - 4 B    ] : 5
Bucket 3  [9 B - 16 B   ] : 50   <-- concentrated at 16B
Bucket 5  [33 B - 64 B  ] : 20
```

Adjust Slab Classes based on the distribution.

---

## 5. Concurrency Optimization

### 5.1 TLS Cache vs Per-CPU Freelist

| Feature | TLS Cache | Per-CPU Freelist |
| :--- | :--- | :--- |
| Implementation Complexity | Simple | Moderate |
| Memory Overhead | ~1KB per thread | ~256B per CPU per Class |
| Lock Contention | Low | None |
| Cache Friendliness | High | Medium |
| Applicable Scenario | General | High-concurrency small objects |

### 5.2 Recommended Configuration

```c
// General multi-thread
MP_FLAG_THREAD_SAFE | MP_FLAG_THREAD_LOCAL_CACHE

// High concurrency (> 8 threads, small objects dominant)
MP_FLAG_THREAD_SAFE | MP_FLAG_PERCPU_FREELIST
```

### 5.3 Batch Operations

```c
// Reduce lock acquisition count
void* ptrs[100];
size_t n = mp_alloc_batch(pool, 64, ptrs, 100);
// ... use ptrs ...
mp_free_batch(pool, ptrs, n);
```

### 5.4 Frame Arena

```c
// Game/rendering pipeline: O(1) batch reset
cmem_frame_arena_t* farena = mp_frame_arena_create(512 * 1024);

// Frame 1
void* p1 = mp_frame_alloc(farena, 1024);
// ... use p1 ...
mp_frame_end(farena);  // O(1) reset

// Frame 2
void* p2 = mp_frame_alloc(farena, 2048);
// ... use p2 ...
mp_frame_end(farena);
```

---

## 6. Memory Reclamation Strategy

### 6.1 Auto-Compact

```c
// Configure auto-compact
mp_set_auto_compact(pool, true, 0.8, 0.3);
// Trigger when pressure > 80% or fragmentation > 30%
```

### 6.2 Manual Reclamation

```c
// Full reclamation flow
size_t freed = mp_trim(pool, 0);  // Deep reclamation
printf("Reclaimed %zu bytes\n", freed);
```

### 6.3 Lazy Reclamation

```c
// Only reclaim RSS, do not release pages
size_t purged = mp_purge_lazy(pool);
```

### 6.4 Reclamation Strategy Comparison

| Strategy | Operation | Performance | Reclamation Rate |
| :--- | :--- | :--- | :--- |
| `mp_compact` | Release free Pages | Medium | High |
| `mp_purge_lazy` | madvise(DONTNEED) | Low | Medium |
| `mp_trim` | compact + purge | High | Highest |

---

## 7. NUMA Optimization

### 7.1 NUMA Binding

```c
// Bind to NUMA node 0
mp_set_numa_node(pool, 0);
```

### 7.2 Detect NUMA Topology

```bash
# View NUMA topology
numactl --hardware

# View memory allocation policy
numastat
```

### 7.3 Multi-Node Scenario

```c
// Create independent pools for each NUMA node
for (int node = 0; node < numa_max_node(); node++) {
    memory_pool_t* node_pool = mp_create(64*1024*1024, MP_FLAG_THREAD_SAFE);
    mp_set_numa_node(node_pool, node);
    // Assign node_pool to threads on that node
}
```

---

## 8. HugePages Optimization

### 8.1 Enable HugePages

```c
mp_flags_t flags = MP_FLAG_HUGE_PAGES;
memory_pool_t* pool = mp_create(256 * 1024 * 1024, flags);
```

### 8.2 System Configuration

```bash
# View HugePages configuration
cat /proc/meminfo | grep HugePages

# Reserve HugePages
echo 1024 | sudo tee /proc/sys/vm/nr_hugepages

# Mount hugetlbfs
sudo mount -t hugetlbfs nodev /dev/hugepages
```

### 8.3 Applicable Scenarios

- Memory pool > 1GB
- Reduce TLB Miss
- Large memory workloads (databases, caching)

---

## 9. Monitoring Metrics

### 9.1 Key Metrics

```c
mp_stats_t stats;
mp_get_stats(pool, &stats);

// Memory usage
printf("Active: %.2f MB\n", stats.active_bytes / 1024.0 / 1024.0);
printf("Peak: %.2f MB\n", stats.peak_bytes / 1024.0 / 1024.0);
printf("Pressure: %.2f%%\n", mp_pressure(pool) * 100);

// Throughput
printf("QPS: %.2f\n", stats.alloc_qps);
printf("Bandwidth: %.2f MB/s\n", stats.bandwidth_mbps);

// Fragmentation
printf("Fragmentation: %.2f%%\n", stats.fragmentation_ratio * 100);

// Latency
printf("P99 Latency: %zu ns\n", mp_get_latency_p99(pool));
printf("Avg Latency: %zu ns\n", mp_get_latency_avg(pool));
```

### 9.2 Prometheus Metrics

```c
char buf[4096];
mp_export_prometheus_metrics(pool, buf, sizeof(buf));
// Expose to Prometheus for scraping
```

### 9.3 JSON Telemetry

```c
char buf[4096];
mp_dump_json_stats(pool, buf, sizeof(buf));
// Send to monitoring system
```

---

## 10. Performance Pitfalls

### 10.1 Common Mistakes

| Mistake | Impact | Solution |
| :--- | :--- | :--- |
| Pool too small | Frequent OS allocations | Increase initial capacity or enable expansion |
| No TLS Cache | High concurrency lock contention | Enable `MP_FLAG_THREAD_LOCAL_CACHE` |
| Excessive Guard Pages | Address space waste | Enable only during debugging |
| Not calling `mp_trim` | RSS not released | Call regularly or enable auto-compact |
| Large objects via Slab | Memory waste | Ensure size thresholds are correct |

### 10.2 Performance Troubleshooting Flow

```c
// 1. Check fragmentation
double frag = mp_pressure(pool);
if (frag > 0.5) {
    printf("High fragmentation: %.2f\n", frag);
}

// 2. Check reclaimable memory
size_t freeable = mp_freeable(pool);
if (freeable > 1024 * 1024) {
    printf("Consider mp_trim: %zu bytes freeable\n", freeable);
}

// 3. Check size distribution
mp_dump_histogram(pool);

// 4. Check latency
uint64_t p99 = mp_get_latency_p99(pool);
if (p99 > 1000000) {  // > 1ms
    printf("High P99 latency: %zu ns\n", p99);
}
```

### 10.3 Benchmarking Recommendations

```bash
# 1. Establish performance baseline
make bench > baseline.txt

# 2. Compare after each change
make bench > after_change.txt

# 3. Use perf to analyze hotspots
perf record -g ./build/benchmark
perf report
```

---

## Appendix: Performance Tuning Checklist

- [ ] Choose appropriate initial capacity based on workload
- [ ] Enable `MP_FLAG_THREAD_LOCAL_CACHE` or `MP_FLAG_PERCPU_FREELIST`
- [ ] Configure `mp_set_auto_compact` for automatic reclamation
- [ ] Regularly call `mp_trim` or `mp_purge_lazy`
- [ ] Monitor `fragmentation_ratio` and `pressure`
- [ ] Use `mp_dump_histogram` to analyze size distribution
- [ ] Enable `MP_FLAG_HUGE_PAGES` for large memory scenarios
- [ ] Bind to correct NUMA node on NUMA systems
- [ ] Disable debug Flags in production environment
- [ ] Regularly run benchmarks to establish performance baseline