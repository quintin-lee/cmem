# cmem API 参考文档

本文档提供 cmem 所有公共 API 的详细说明、参数、返回值和使用示例。

## 目录

1. [内存池生命周期](#1-内存池生命周期)
2. [内存分配与释放](#2-内存分配与释放)
3. [内省查询与元数据](#3-内省查询与元数据)
4. [便利辅助函数](#4-便利辅助函数)
5. [内存紧凑与回收](#5-内存紧凑与回收)
6. [统计、诊断与监控](#6-统计诊断与监控)
7. [泄漏检测与堆审计](#7-泄漏检测与堆审计)
8. [高级特性 API](#8-高级特性-api)
9. [配置标志位](#9-配置标志位)
10. [C++ API](#10-c-api)
11. [事件类型](#11-事件类型)

---

## 1. 内存池生命周期

### mp_create

```c
memory_pool_t* mp_create(size_t initial_capacity, mp_flags_t flags);
```

创建并初始化标准内存池实例。

**参数：**
- `initial_capacity`：初始内存容量（字节），0 表示使用默认值
- `flags`：配置标志位，多个 flag 用 `|` 组合

**返回值：**
- 成功：返回 `memory_pool_t*` 指针
- 失败：返回 `NULL`

**示例：**
```c
memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_THREAD_SAFE | MP_FLAG_DEBUG_CANARY);
```

---

### mp_create_child

```c
memory_pool_t* mp_create_child(memory_pool_t* parent, size_t capacity, mp_flags_t flags, const char* name);
```

创建嵌套在父内存池下的子 Arena。

**参数：**
- `parent`：父内存池指针
- `capacity`：子 Arena 初始容量
- `flags`：配置标志位
- `name`：子 Arena 名称（用于诊断输出）

**返回值：**
- 成功：返回子 Arena 指针
- 失败：返回 `NULL`

**特性：**
- 子 Arena 销毁/重置时递归作用于所有后代
- 支持无限层级嵌套

---

### mp_create_from_buffer

```c
memory_pool_t* mp_create_from_buffer(void* buffer, size_t buffer_size, mp_flags_t flags);
```

在预分配静态缓冲区中创建内存池（零 OS malloc 依赖）。

**参数：**
- `buffer`：预分配缓冲区（必须 8 字节对齐）
- `buffer_size`：缓冲区大小
- `flags`：配置标志位

**返回值：**
- 成功：返回内存池指针
- 失败：返回 `NULL`

**适用场景：**
- 嵌入式系统
- 内核模块
- 无动态内存分配环境

---

### mp_create_shared

```c
memory_pool_t* mp_create_shared(const char* shm_name, size_t capacity, mp_flags_t flags);
```

创建 POSIX 共享内存池（`/dev/shm` 零拷贝 IPC）。

**参数：**
- `shm_name`：共享内存对象名称（如 `"/my_ipc_pool"`）
- `capacity`：容量（字节）
- `flags`：配置标志位

**返回值：**
- 成功：返回内存池指针
- 失败：返回 `NULL`

---

### mp_create_custom

```c
memory_pool_t* mp_create_custom(size_t initial_capacity, mp_flags_t flags, const mp_sys_allocator_t* sys_allocator);
```

使用自定义系统分配器创建内存池。

**参数：**
- `initial_capacity`：初始容量
- `flags`：配置标志位
- `sys_allocator`：自定义分配器函数表，`NULL` 使用默认系统分配器

**返回值：**
- 成功：返回内存池指针
- 失败：返回 `NULL`

---

### mp_destroy

```c
void mp_destroy(memory_pool_t* pool);
```

销毁内存池并递归销毁旗下所有子 Context。

**参数：**
- `pool`：内存池指针

**注意：**
- 销毁后 `pool` 指针不可再使用
- 自动释放所有 Slab Pages、TLSF Pools、同步原语

---

### mp_destroy_shared

```c
void mp_destroy_shared(memory_pool_t* pool, const char* shm_name);
```

销毁共享内存池并解除 POSIX 共享内存段链接。

**参数：**
- `pool`：内存池指针
- `shm_name`：共享内存名称

---

### mp_reset

```c
void mp_reset(memory_pool_t* pool);
```

$O(1)$ 批量重置内存池及其旗下所有子 Context。

**特性：**
- 逻辑上释放所有分配块
- 底层内存保留供复用
- 远快于逐个 `mp_free`

---

## 2. 内存分配与释放

### mp_alloc

```c
void* mp_alloc(memory_pool_t* pool, size_t size);
```

从内存池中分配指定字节的内存块。

**参数：**
- `pool`：内存池指针
- `size`：请求字节数

**返回值：**
- 成功：返回指向 payload 的指针
- 失败：返回 `NULL`

**分配策略：**
- `size <= 512B`：Slab Pool
- `512B < size <= 4MB`：TLSF Allocator
- `size > 4MB`：Direct OS Fallback

---

### mp_alloc_fast

```c
static inline void* mp_alloc_fast(memory_pool_t* pool, size_t size);
```

针对配置了 `MP_FLAG_FAST_PATH` 标志的内存池的高性能内联小对象分配接口。直接存取线程局部缓存 `tls_cache` 并通过 $O(1)$ 尺寸映射表直接跳转，彻底消除跨编译单元函数调用延迟。

**参数：**
- `pool`：内存池指针
- `size`：请求字节数（`1 <= size <= 512`）

**返回值：**
- 成功：返回指向 payload 的指针
- 失败/缓存未命中：自动降级调用 `mp_alloc(pool, size)`

---

### mp_free_fast

```c
static inline void mp_free_fast(memory_pool_t* pool, void* ptr);
```

针对配置了 `MP_FLAG_FAST_PATH` 标志的内存池的高性能内联小对象释放接口。在 TLS 线程局部缓存命中时，避免全局互斥锁竞争，提供极速无锁压栈释放。

**参数：**
- `pool`：内存池指针
- `ptr`：指向要释放的 payload 指针

---

### mp_calloc

```c
void* mp_calloc(memory_pool_t* pool, size_t num, size_t size);
```

分配内存块并自动填充零数据。

**参数：**
- `pool`：内存池指针
- `num`：元素数量
- `size`：每个元素大小

**返回值：**
- 成功：返回指向清零内存的指针
- 失败：返回 `NULL`

---

### mp_realloc

```c
void* mp_realloc(memory_pool_t* pool, void* ptr, size_t new_size);
```

重新调整内存块尺寸。

**参数：**
- `pool`：内存池指针
- `ptr`：现有分配指针（`NULL` 表示新分配）
- `new_size`：新请求尺寸

**返回值：**
- 成功：返回新指针
- 失败：返回 `NULL`（原指针保持不变）

**优化：**
- TLSF 块尝试原地 Expand（In-Place Expansion）
- 避免无谓的 `memcpy`

---

### mp_reallocarray

```c
void* mp_reallocarray(memory_pool_t* pool, void* ptr, size_t nmemb, size_t size);
```

溢出安全的数组重新分配。

**安全特性：**
- 显式校验 `nmemb * size > SIZE_MAX` 乘法溢出
- 防止整数溢出导致的分配不足

---

### mp_aligned_alloc

```c
void* mp_aligned_alloc(memory_pool_t* pool, size_t alignment, size_t size);
```

分配指定对齐边界的内存块。

**参数：**
- `pool`：内存池指针
- `alignment`：对齐字节数（必须为 2 的幂，最小 `sizeof(void*)`）
- `size`：请求字节数

**返回值：**
- 成功：返回对齐的指针
- 失败：返回 `NULL`

---

### mp_free

```c
void* mp_free(memory_pool_t* pool, void* ptr);
```

释放内存块归还内存池。

**参数：**
- `pool`：内存池指针
- `ptr`：要释放的指针（`NULL` 直接返回）

**调试检查（若启用）：**
- Canary 越界校验
- Magic 魔数校验
- 双 Free 检测

---

### mp_alloc_batch

```c
size_t mp_alloc_batch(memory_pool_t* pool, size_t size, void** out_ptrs, size_t count);
```

高吞吐单次批量分配 N 个内存块。

**参数：**
- `pool`：内存池指针
- `size`：每块大小
- `out_ptrs`：输出指针数组
- `count`：请求块数

**返回值：**
- 实际成功分配的块数（可能小于 count）

---

### mp_free_batch

```c
void mp_free_batch(memory_pool_t* pool, void** ptrs, size_t count);
```

单次批量释放 N 个内存块。

**参数：**
- `pool`：内存池指针
- `ptrs`：指针数组
- `count`：块数

---

## 3. 内省查询与元数据

### mp_usable_size

```c
size_t mp_usable_size(memory_pool_t* pool, void* ptr);
```

查询已分配内存块的真实可用字节容量。

**返回值：**
- 有效指针：返回可用字节数
- 无效指针：返回 0

---

### mp_alloc_size

```c
size_t mp_alloc_size(memory_pool_t* pool, void* ptr);
```

查询已分配内存块的用户申请字节尺寸。

**返回值：**
- 有效指针：返回申请尺寸
- 无效指针：返回 0

---

### mp_ptr_valid

```c
bool mp_ptr_valid(memory_pool_t* pool, void* ptr);
```

校验指针是否为当前池中活动状态的有效指针。

**返回值：**
- `true`：指针有效且处于活跃分配状态
- `false`：指针无效或已释放

---

### mp_preferred_size

```c
size_t mp_preferred_size(size_t size);
```

计算最佳匹配的 Size Class 对齐尺寸。

**示例：**
```c
mp_preferred_size(12) -> 16
mp_preferred_size(40) -> 64
mp_preferred_size(600) -> 608 (TLSF 对齐)
```

---

### mp_set_name / mp_get_name

```c
void mp_set_name(memory_pool_t* pool, const char* name);
const char* mp_get_name(memory_pool_t* pool);
```

设置/获取内存池的易读名称（用于诊断输出）。

---

### mp_get_parent / mp_get_child_count

```c
memory_pool_t* mp_get_parent(memory_pool_t* pool);
size_t mp_get_child_count(memory_pool_t* pool);
```

获取父 Arena 指针和子 Arena 数量。

---

### mp_get_allocation_info

```c
bool mp_get_allocation_info(memory_pool_t* pool, void* ptr, mp_allocation_info_t* info);
```

获取单个分配的详细信息，包括 tier、尺寸、源码位置和回溯栈。

**参数：**
- `pool`: 内存池指针
- `ptr`: mp_alloc/calloc/realloc 返回的有效指针
- `info`: 输出结构体，填充分配元数据

**返回值：**
- 成功返回 `true`，失败返回 `false`

---

### mp_enumerate_regions

```c
size_t mp_enumerate_regions(memory_pool_t* pool, mp_region_info_t* regions, size_t max_regions);
```

枚举池的所有底层内存区域（Slab 页、TLSF 池、OS 后备映射）。

**参数：**
- `pool`: 内存池指针
- `regions`: 输出 mp_region_info_t 数组
- `max_regions`: 数组最大容量

**返回值：**
- 写入数组的区域数量

---

## 4. 便利辅助函数

### mp_strdup

```c
char* mp_strdup(memory_pool_t* pool, const char* str);
```

在内存池中深拷贝以 `\0` 结尾的字符串。

**返回值：**
- 成功：返回新字符串指针
- 失败：返回 `NULL`

---

### mp_memdup

```c
void* mp_memdup(memory_pool_t* pool, const void* src, size_t n);
```

在内存池中深拷贝 N 字节二进制数据。

---

### mp_asprintf

```c
char* mp_asprintf(memory_pool_t* pool, const char* fmt, ...);
```

格式化字符串并在内存池中分配存储空间。

**示例：**
```c
char* msg = mp_asprintf(pool, "Arena [%s] active: %zu", mp_get_name(pool), count);
```

---

## 5. 内存紧凑与回收

### mp_trim

```c
size_t mp_trim(memory_pool_t* pool, size_t pad);
```

深层内存紧凑与页归还。

**参数：**
- `pad`：保留的最小字节数

**返回值：**
- 实际回收的字节数

**行为：**
- 释放完全空闲的 Slab Page
- 调用 `mp_compact` + `mp_purge_lazy`

---

### mp_compact

```c
size_t mp_compact(memory_pool_t* pool);
```

紧凑内存池，释放空闲 Slab Page 回 OS。

---

### mp_purge_lazy

```c
size_t mp_purge_lazy(memory_pool_t* pool);
```

延迟物理内存页 RSS 清理（Linux `MADV_DONTNEED` / `MADV_FREE`）。

---

### mp_madvise

```c
int mp_madvise(memory_pool_t* pool, void* addr, size_t length, int advice);
```

跨平台内存 Advice 包装。

**平台差异：**
- Linux：调用 `madvise()`
- Windows：调用 `VirtualAlloc(MEM_RESET)`

---

### mp_resident

```c
size_t mp_resident(memory_pool_t* pool);
```

获取当前内存池系统 RSS 物理驻留字节数。

---

### mp_freeable

```c
size_t mp_freeable(memory_pool_t* pool);
```

获取当前池内可被 `mp_trim` 回收的空闲页字节数。

---

## 6. 统计、诊断与监控

### mp_get_stats

```c
void mp_get_stats(memory_pool_t* pool, mp_stats_t* stats);
```

获取统计快照。

---

### mp_pressure

```c
double mp_pressure(memory_pool_t* pool);
```

获取内存池使用压力比例 [0.0, 1.0]。

**计算：**
```
pressure = active_bytes / max(limit, total_pool_size)
```

---

### mp_reset_stats

```c
void mp_reset_stats(memory_pool_t* pool);
```

重置累积 QPS、操作计数与 Peak 峰值。

---

### mp_dump_info

```c
void mp_dump_info(memory_pool_t* pool);
```

打印详细汇总与健康状态到 stdout。

---

### mp_dump_tree_info

```c
void mp_dump_tree_info(memory_pool_t* pool);
```

打印内存池树状层级结构。

**输出示例：**
```
|- [Arena: RootArena] Active Bytes: 128 B, Active Allocations: 1
  |- [Arena: Child1] Active Bytes: 256 B, Active Allocations: 1
```

---

### mp_dump_histogram

```c
void mp_dump_histogram(memory_pool_t* pool);
```

打印 ASCII 分配尺寸分布直方图。

---

### mp_dump_json_stats

```c
size_t mp_dump_json_stats(memory_pool_t* pool, char* buf, size_t max_len);
```

导出 JSON 格式统计数据。

---

### mp_export_html_report

```c
bool mp_export_html_report(memory_pool_t* pool, const char* filepath);
```

导出交互式可视化 HTML 剖析与泄漏大屏报告。

---

### mp_export_prometheus_metrics

```c
size_t mp_export_prometheus_metrics(memory_pool_t* pool, char* out_buf, size_t max_len);
```

导出 Prometheus / OpenTelemetry 标准格式指标。

**输出示例：**
```
# HELP cmem_active_bytes Active memory bytes
# TYPE cmem_active_bytes gauge
cmem_active_bytes{arena="RootArena"} 3072
```

---

### mp_export_binary_snapshot

```c
bool mp_export_binary_snapshot(memory_pool_t* pool, const char* filepath);
```

导出二进制崩溃后内存快照。

---

### mp_parse_binary_snapshot

```c
bool mp_parse_binary_snapshot(const char* filepath, char* out_report, size_t max_len);
```

解析二进制快照为可读文本报告。

---

### mp_diff_snapshots

```c
bool mp_diff_snapshots(const char* snapshot_a_path, const char* snapshot_b_path, char* out_report, size_t max_len);
```

对比两次内存快照并生成增量泄漏差异报告。

---

## 7. 诊断 CLI 工具

### cmem-inspect

实时进程内诊断 CLI，链接 `libcmem`。

```bash
cmem-inspect <subcommand> [options]
```

子命令：
- `leaks` — 泄漏分析
- `audit` — 堆审计
- `stats` — 池统计
- `tree` — Arena 树
- `histogram` — 分配尺寸直方图
- `snapshot` — 导出二进制快照
- `diff` — 对比两份二进制快照
- `html` — 导出 HTML 报告

通用选项：
- `--json` — JSON 格式输出
- `--output <path>` — 输出到文件
- `--quiet` — 抑制非必要输出

### cmem-analyze

独立离线分析器，用于 `.cmem_dump` 二进制快照。

```bash
cmem-analyze <subcommand> [options] <input>
```

子命令：
- `report` — 完整分析报告
- `top` — 按大小/数量排序的 Top 分配
- `summary` — 精简汇总统计
- `validate` — 校验快照完整性
- `diff` — 对比两份快照

通用选项：
- `--json` — JSON 格式输出
- `--html` — 生成 HTML 报告
- `--output <path>` — 输出到文件
- `--quiet` — 抑制非必要输出
- `--top <n>` — 限制 Top N 结果

构建方式：`make tools`

---

## 7. 泄漏检测与堆审计

### mp_audit_heap

```c
bool mp_audit_heap(memory_pool_t* pool);
```

遍历堆内存，主动审计 Redzone Canary 越界。

**返回值：**
- `true`：堆健康
- `false`：检测到损坏

---

### mp_analyze_leaks

```c
size_t mp_analyze_leaks(memory_pool_t* pool, char* report_buf, size_t max_len);
```

生成包含代码位置（file:line）与调用栈的泄漏报告。

**返回值：**
- 写入 report_buf 的字节数

---

### mp_export_leak_report

```c
bool mp_export_leak_report(memory_pool_t* pool, const char* filepath);
```

导出泄漏分析报告到文本文件。

---

### mp_check_leaks

```c
bool mp_check_leaks(memory_pool_t* pool);
```

检查是否存在未释放的内存分配。

**返回值：**
- `true`：无泄漏
- `false`：存在泄漏

---

## 8. 高级特性 API

### 运行时配置热加载

```c
mp_flags_t mp_reparse_env_flags(memory_pool_t* pool);
uint64_t mp_get_env_generation(memory_pool_t* pool);
```

运行时重新解析 `CMEM_CONF` 环境变量。

---

### 自动压缩触发

```c
void mp_set_auto_compact(memory_pool_t* pool, bool enable, double pressure_threshold, double fragmentation_threshold);
bool mp_auto_compact_check(memory_pool_t* pool);
```

配置并触发基于压力/碎片率的自动压缩。

---

### Arena 配额

```c
void mp_set_arena_quota(memory_pool_t* pool, size_t quota_bytes, mp_watermark_callback_t cb, void* user_data);
bool mp_check_arena_quota(memory_pool_t* pool);
```

设置单 Arena 内存配额与超限回调。

---

### 延迟统计

```c
void mp_record_latency(memory_pool_t* pool, uint64_t latency_ns);
uint64_t mp_get_latency_p99(memory_pool_t* pool);
uint64_t mp_get_latency_avg(memory_pool_t* pool);
void mp_reset_latency_stats(memory_pool_t* pool);
```

记录和查询分配延迟统计。

---

### 可配置 Slab Class

```c
bool mp_set_slab_classes(memory_pool_t* pool, const size_t* sizes, size_t count);
size_t mp_get_slab_classes(memory_pool_t* pool, size_t* out_sizes, size_t max_count);
size_t mp_get_slab_class_count(memory_pool_t* pool);
size_t mp_preferred_size_for_pool(memory_pool_t* pool, size_t size);
```

自定义 Slab 尺寸类表。

---

### 事件日志

```c
mp_event_log_t* mp_event_log_create(size_t capacity);
bool mp_event_log_record(mp_event_log_t* log, mp_event_type_t event_type, void* ptr, size_t size);
bool mp_event_log_consume(mp_event_log_t* log, mp_event_log_entry_t* entry);
size_t mp_event_log_pending(mp_event_log_t* log);
void mp_event_log_clear(mp_event_log_t* log);
void mp_event_log_destroy(mp_event_log_t* log);
```

结构化事件日志 Ring Buffer。

---

### pprof 导出

```c
size_t mp_export_pprof(memory_pool_t* pool, char* out_buf, size_t max_len);
```

导出 pprof 兼容文本格式。

---

### Per-CPU Freelist

```c
void mp_set_percpu_freelist(memory_pool_t* pool, bool enable);
bool mp_get_percpu_freelist(memory_pool_t* pool);
int mp_get_percpu_cpu_count(memory_pool_t* pool);
```

Per-CPU 无锁 freelist 配置。

---

### 优雅降级

```c
void mp_set_fallback_on_oom(memory_pool_t* pool, bool enable);
void mp_set_gc_callback(memory_pool_t* pool, mp_watermark_callback_t cb, void* user_data);
void mp_set_eviction_callback(memory_pool_t* pool, mp_watermark_callback_t cb, void* user_data);
```

OOM 回退、GC 回调、逐出回调。

---

### 内存错误恢复

```c
void mp_mark_pool_dirty(memory_pool_t* pool);
void mp_clear_pool_dirty(memory_pool_t* pool);
bool mp_is_pool_dirty(memory_pool_t* pool);
void mp_set_error_recovery_callback(memory_pool_t* pool, mp_watermark_callback_t cb, void* user_data);
bool mp_isolate_bad_block(memory_pool_t* pool, void* ptr);
```

脏池标记、错误恢复回调、坏块隔离。

---

### 线程级配额与熔断器

```c
void mp_set_thread_quota(memory_pool_t* pool, size_t quota_bytes);
void mp_set_circuit_breaker(memory_pool_t* pool, bool enable);
bool mp_is_circuit_breaker_tripped(memory_pool_t* pool);
size_t mp_get_thread_allocated_bytes(memory_pool_t* pool);
void mp_reset_thread_quota(memory_pool_t* pool);
```

---

### ABI 版本与 cgroup 感知

```c
uint32_t mp_abi_version(void);
void mp_set_cgroup_aware(memory_pool_t* pool, bool enable);
size_t mp_get_cgroup_mem_limit(memory_pool_t* pool);
```

---

### Hot/Cold 页分离

```c
bool mp_mark_page_hot(memory_pool_t* pool, void* page_raw_mem);
bool mp_mark_page_cold(memory_pool_t* pool, void* page_raw_mem);
size_t mp_get_hot_page_count(memory_pool_t* pool);
size_t mp_get_cold_page_count(memory_pool_t* pool);
size_t mp_separate_hot_cold_pages(memory_pool_t* pool);
```

---

### 加密内存

```c
int mp_lock_memory(memory_pool_t* pool, void* addr, size_t length);
int mp_unlock_memory(memory_pool_t* pool, void* addr, size_t length);
int mp_protect_from_dump(memory_pool_t* pool, void* addr, size_t length);
void mp_secure_zero(memory_pool_t* pool, void* ptr, size_t length);
void mp_set_encrypted_memory(memory_pool_t* pool, bool enable);
```

---

### ASan 集成

```c
bool mp_asan_is_enabled(void);
void mp_asan_report_error(memory_pool_t* pool, void* ptr, size_t size, bool is_write);
bool mp_asan_check_memory(memory_pool_t* pool, void* ptr, size_t size);
void mp_set_asan_integration(memory_pool_t* pool, bool enable);
```

---

### 在线扩容

```c
bool mp_expand_pool(memory_pool_t* pool, size_t additional_bytes);
bool mp_can_expand(memory_pool_t* pool);
size_t mp_get_expandable_size(memory_pool_t* pool);
```

---

### Frame Arena

```c
cmem_frame_arena_t* mp_frame_arena_create(size_t capacity);
void* mp_frame_alloc(cmem_frame_arena_t* farena, size_t size);
void mp_frame_end(cmem_frame_arena_t* farena);
void mp_frame_arena_destroy(cmem_frame_arena_t* farena);
```

双缓冲 Ping-Pong 帧竞技场，O(1) 重置。

---

### Typed Object Pool

```c
mp_typed_pool_t* mp_typed_pool_create(size_t elem_size, size_t capacity);
void* mp_typed_alloc(mp_typed_pool_t* tpool);
void mp_typed_free(mp_typed_pool_t* tpool, void* ptr);
void mp_typed_pool_destroy(mp_typed_pool_t* tpool);
```

零开销固定大小对象池。

---

### Ring Buffer

```c
cmem_ring_buffer_t* mp_ring_create(size_t slot_size, size_t capacity);
void* mp_ring_alloc(cmem_ring_buffer_t* ring);
bool mp_ring_free(cmem_ring_buffer_t* ring, void* ptr);
void mp_ring_destroy(cmem_ring_buffer_t* ring);
```

DPDK 风格无锁环形缓冲区。

---

### 水位回调

```c
void mp_set_watermark_callback(memory_pool_t* pool, double high_ratio, double low_ratio, mp_watermark_callback_t cb, void* user_data);
```

配置高/低水位阈值告警回调。

---

### 内存限制

```c
void mp_set_memory_limit(memory_pool_t* pool, size_t max_bytes);
```

设置内存池硬性最大预算限制。

---

### 事件回调

```c
void mp_set_event_callback(memory_pool_t* pool, mp_event_callback_t callback, void* user_data);
```

注册事件回调函数用于实时 Profiling 与调试。

---

### 紧急 OOM 储备

```c
bool mp_enable_emergency_reserve(memory_pool_t* pool, size_t reserve_bytes);
```

启用紧急 OOM 兜底内存储备垫。

---

### NUMA 绑定

```c
bool mp_set_numa_node(memory_pool_t* pool, int numa_node);
```

绑定内存池底层分配到指定 Linux NUMA CPU 节点。

---

### 环境变量解析

```c
mp_flags_t mp_parse_env_flags(mp_flags_t default_flags);
```

解析 `CMEM_CONF` 环境变量。

**支持的环境变量：**
```bash
export CMEM_CONF="canary=1,poison=on,aligned=1,guard=1,tls=1,track=1,hugepages=1"
```

---

## 9. 配置标志位

```c
typedef enum {
    MP_FLAG_DEFAULT            = 0,                    // 默认配置
    MP_FLAG_THREAD_SAFE        = (1 << 0),             // 启用线程安全 (mutex)
    MP_FLAG_DEBUG_CANARY       = (1 << 1),             // 红区金丝雀检测越界
    MP_FLAG_ZERO_ON_ALLOC      = (1 << 2),             // 分配时自动清零
    MP_FLAG_THREAD_LOCAL_CACHE = (1 << 3),             // 线程本地缓存 (Lock-Free 小对象)
    MP_FLAG_STATIC_BUFFER      = (1 << 4),             // 静态缓冲区模式 (无 OS malloc)
    MP_FLAG_TRACK_LOCATIONS    = (1 << 5),             // 记录文件/行/函数/回溯
    MP_FLAG_POISON_ON_FREE     = (1 << 6),             // 释放时填充 0xDD (UAF 检测)
    MP_FLAG_CACHE_ALIGNED      = (1 << 7),             // 强制 64B Cache Line 对齐
    MP_FLAG_GUARD_PAGES        = (1 << 8),             // 页级 Guard Pages (PROT_NONE)
    MP_FLAG_SHARED_MEMORY      = (1 << 9),             // POSIX 共享内存 IPC 模式
    MP_FLAG_HUGE_PAGES         = (1 << 10),            // Linux HugePages (2MB/1GB)
    MP_FLAG_PERCPU_FREELIST    = (1 << 11),            // Per-CPU 无锁 freelist
    MP_FLAG_HOT_COLD_SEPARATION = (1 << 12),           // Hot/Cold 页分离 (TLB 优化)
    MP_FLAG_ENCRYPTED_MEMORY   = (1 << 13),            // 加密内存 (mlock + MADV_DONTDUMP)
    MP_FLAG_ASAN_INTEGRATION   = (1 << 14)             // AddressSanitizer 集成层
} mp_flags_t;
```

---

## 10. C++ API

### cmem::MemoryPool

```cpp
namespace cmem {

class MemoryPool {
public:
    explicit MemoryPool(size_t initial_capacity = 0, mp_flags_t flags = MP_FLAG_DEFAULT);
    MemoryPool(const std::string& shm_name, size_t capacity, mp_flags_t flags);
    MemoryPool(MemoryPool& parent, size_t capacity, mp_flags_t flags, const std::string& name);
    ~MemoryPool();

    void* alloc(size_t size);
    void* alloc_loc(size_t size, const char* file, int line, const char* func);
    void* calloc(size_t num, size_t size);
    void* realloc(void* ptr, size_t new_size);
    void* aligned_alloc(size_t alignment, size_t size);
    void free(void* ptr);

    size_t alloc_batch(size_t size, void** out_ptrs, size_t count);
    void free_batch(void** ptrs, size_t count);

    void reset();
    size_t compact();
    void set_memory_limit(size_t max_bytes);

    bool audit_heap();
    std::string analyze_leaks();
    bool export_leak_report(const std::string& path);
    bool export_html_report(const std::string& path);
    bool export_binary_snapshot(const std::string& path);
    static std::string parse_binary_snapshot(const std::string& path);

    mp_stats_t get_stats() const;
    void dump_info() const;
    void dump_tree_info() const;
    void dump_histogram() const;

    bool check_leaks() const;
    bool get_allocation_info(void *ptr, mp_allocation_info_t *info) const;
    size_t enumerate_regions(mp_region_info_t *regions, size_t max_regions) const;
    memory_pool_t* get() const;
    memory_pool_t* release();
};

}
```

### cmem::allocator<T>

```cpp
template<typename T>
class allocator {
public:
    using value_type = T;
    allocator(memory_pool_t* pool);
    template<typename U> allocator(const allocator<U>& other);

    T* allocate(std::size_t n);
    void deallocate(T* p, std::size_t n);
    bool operator==(const allocator& other) const;
    bool operator!=(const allocator& other) const;
};
```

### cmem::pmr_resource

```cpp
class pmr_resource : public std::pmr::memory_resource {
public:
    explicit pmr_resource(memory_pool_t* pool);
    memory_pool_t* pool() const;

protected:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;
};
```

---

## 11. 事件类型

```c
typedef enum {
    MP_EVENT_ALLOC = 1,              // 内存块分配
    MP_EVENT_FREE,                   // 内存块释放
    MP_EVENT_REALLOC,                // 内存块重分配
    MP_EVENT_CANARY_CORRUPTION,      // 通过 canary 检测到的缓冲区越界
    MP_EVENT_DOUBLE_FREE,            // 检测到双 Free 或无效释放
    MP_EVENT_RESET,                  // 内存池重置
    MP_EVENT_COMPACT,                // 内存池压缩
    MP_EVENT_OOM,                    // 达到内存不足条件
    MP_EVENT_DIRTY                   // 因内存损坏而标记池为脏
} mp_event_type_t;
```

---

## 附录：快速参考卡

### C 语言

```c
#include "cmem.h"

// 创建
memory_pool_t* pool = mp_create(1024*1024, MP_FLAG_THREAD_SAFE);

// 分配/释放
void* p = mp_alloc(pool, 128);
mp_free(pool, p);

// 内省
size_t usable = mp_usable_size(pool, p);
bool valid = mp_ptr_valid(pool, p);

// 销毁
mp_destroy(pool);
```

### C++17

```cpp
#include "cmem.hpp"
#include "cmem_pmr.hpp"

// RAII
cmem::MemoryPool pool(1024*1024, MP_FLAG_THREAD_SAFE);

// STL 容器
cmem::allocator<int> alloc(pool.get());
std::vector<int, cmem::allocator<int>> vec(alloc);

// PMR
cmem::pmr_resource res(pool.get());
std::pmr::vector<std::pmr::string> vec(&res);
```
