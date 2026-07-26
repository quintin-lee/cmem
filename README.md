# cmem - Universal High-Performance Tiered Memory Manager

**cmem** 是一个基于 **C11 / C++17** 设计实现的通用、高性能分层内存池管理工具（Universal High-Performance Tiered Memory Manager）。

它融合了工业级内存分配器（如 **Slab** 与 **TLSF**）的核心优点，具备全方位的内存诊断、内省查询、级联树状 Arena 组织、跨平台 OS 页回收以及 C++17 PMR 容器适配能力。

---

## 🌟 核心架构设计 (Architecture)

**cmem** 采用现代高性能 Allocator 的 **三层混合分层架构**，在保证 $O(1)$ 分配时间复杂度的同时极大减少内存碎片化，并显著提升 L1/L2 Cache 局部性：

```text
                         +-------------------------------+
                         |   User Request (mp_alloc)    |
                         +-------------------------------+
                                         |
                  +----------------------+----------------------+
                  | (<= 512 Bytes)       | (512B ~ 4MB)         | (> 4MB)
                  v                      v                      v
         +------------------+   +-------------------+   +--------------------+
         |   Slab Pool      |   |   TLSF Allocator  |   | Direct OS Fallback |
         | (Fixed Blocks)   |   | (Two-Level Fit)   |   |  (System Malloc)   |
         +------------------+   +-------------------+   +--------------------+
         | O(1) Fast Alloc  |   | O(1) Bitmap Search|   | Huge Allocations   |
         | Zero Extra Frag  |   | Immediate Merge   |   | Dynamic Page Track |
         +------------------+   +-------------------+   +--------------------+
```

---

## 🔥 核心特性一览 (Feature Highlights)

### 1. ⚡ 高性能分层 Allocator (Tiered Allocator)
- **Slab Allocator (<= 512B)**：面向小对象的固定尺寸 Class 分配器（8B ~ 512B），单次分配/释放为 $O(1)$，无外部碎片。
- **TLSF Allocator (512B ~ 4MB)**：双层可控适合分配器，利用一级/二级 Bitmap 进行 $O(1)$ 常数时间查找，并支持**原地 Expand (`tlsf_try_inplace_expand`)**，避免无谓的 `memcpy` 拷贝开销。
- **Direct OS Fallback (> 4MB)**：对于超大对象，自动回退至系统底层内存映射（支持 Guard Pages 防踩防护与 HugePages 2MB/1GB 加速）。

### 2. 🔍 内存内省与容量查询 API (Introspection APIs)
- **`mp_usable_size`**：查询分配内存块的实际可用字节容量。
- **`mp_alloc_size`**：查询分配内存块的用户申请字节尺寸。
- **`mp_ptr_valid`**：快速校验指针是否为当前内存池中处于活动状态的有效分配块。
- **`mp_preferred_size`**：计算最佳匹配的 Size Class 对齐尺寸。

### 3. ♻️ 内存紧凑与 OS 页回收 (Trim & RSS Purging)
- **`mp_trim`**：深层物理页紧凑与回收，支持保留缓冲 padding，主动清理池内完全空闲的 Slab 页面。
- **`mp_madvise`**：跨平台 `madvise` 包装函数，Linux 下调用 `madvise(MADV_DONTNEED)`，Windows 下自动适配为 `VirtualAlloc(MEM_RESET)`，主动归还物理 RSS 给操作系统。
- **`mp_compact` / `mp_purge_lazy`**：内存紧凑与物理页延迟清理。
- **`mp_resident`**：获取当前内存池系统 RSS 物理驻留字节数。
- **`mp_freeable`**：获取当前池内可被 `mp_trim` 回收的空闲页字节数。

### 4. 🔒 高并发锁机制 (RWLock & Fine-Grained Locks)
- **读写锁 (`pthread_rwlock_t`)**：所有内省查询与指标统计 API（如 `mp_usable_size`、`mp_pressure`、`mp_get_stats`）均使用**读锁**，多线程并发 profiling 时零锁争用。
- **细粒度 Slab Class 锁**：每个 Slab 尺寸 Class（8B/16B/32B/64B/128B/256B/512B）拥有独立的 `pthread_mutex_t` 锁。不同尺寸的小对象分配/释放并发执行，锁争用降至最小。
- **线程本地缓存 (`MP_FLAG_THREAD_LOCAL_CACHE`)**：小对象分配走 Lock-Free 快速路径，极大提升多线程吞吐。

### 5. 🚀 C++17 PMR 与 STL 容器适配器 (C++17 PMR Support)
- 头文件包含 `#include "cmem_pmr.hpp"`。
- 提供继承自 `std::pmr::memory_resource` 的 `cmem::pmr_resource` 适配器，完美兼容 C++17 原生多态容器（如 `std::pmr::vector`、`std::pmr::string`、`std::pmr::map` 等）。
- 亦提供标准 C++11 RAII 包装类 `cmem::MemoryPool` 与 C++ STL 分配器 `cmem::allocator<T>`。

### 6. 🛡️ 安全强化与溢出防护 (Safety & Security)
- **`mp_reallocarray`**：溢出安全重新分配数组，显式校验 `nmemb * size > SIZE_MAX` 乘法溢出。
- **Redzone Canary 金丝雀 (`MP_FLAG_DEBUG_CANARY`)**：末端填充 `0xAB`，实时检测写越界踩内存。
- **UAF 释放毒化 (`MP_FLAG_POISON_ON_FREE`)**：释放内存填充 `0xDD` 毒化字节，捕获 Use-After-Free。
- **页级 Guard Pages (`MP_FLAG_GUARD_PAGES`)**：利用 `PROT_NONE` 在页首页尾阻断野指针越界。
- **Cache Line 对齐 (`MP_FLAG_CACHE_ALIGNED`)**：强制 64B Cache Line 对齐，消除伪共享。

### 7. 🛠️ 字符串与内存拷贝便利函数 (Convenience Helpers)
- **`mp_strdup` / `mp_strdup_loc`**：自动在内存池中复制以 `\0` 结尾的字符串。
- **`mp_memdup` / `mp_memdup_loc`**：自动在内存池中深拷贝 $N$ 字节内存块。
- **`mp_asprintf` / `mp_asprintf_loc`**：格式化字符串并在内存池中自动分配所需空间。

### 8. 🌳 树状 Memory Arena 导航与元数据 (Arena Trees & Metadata)
- 支持创建父子关系的嵌套内存池 (`mp_create_child`)，销毁/重置父池时递归作用于所有子 Context。
- **元数据与导航 API**：`mp_set_name`、`mp_get_name`、`mp_get_parent`、`mp_get_child_count`。

### 9. 📊 交互式诊断大屏 & 监控集成 (Diagnostics & Metrics)
- **交互式 Visual HTML 报告 (`mp_export_html_report`)**：一键导出单文件 HTML，展示内存大图、Slab 分布与代码泄漏卡片。
- **Prometheus / OpenTelemetry 标准导出 (`mp_export_prometheus_metrics`)**：生成 Prometheus exposition 格式监控文本。
- **崩溃快照 Dump 与 Diff 泄漏检测 (`mp_export_binary_snapshot` / `mp_parse_binary_snapshot` / `mp_diff_snapshots`)**。
- **实时 QPS 与带宽吞吐计量** (`alloc_qps`, `bandwidth_mbps` in `mp_stats_t`)。
- **分配尺寸直方图分布诊断** (`mp_dump_histogram`)。

### 10. 🎮 游戏/图形管线专用 Frame Arena (Frame Arena)
- **双缓冲 Ping-Pong Frame Arena** (`mp_frame_arena_create` / `mp_frame_alloc` / `mp_frame_end`)：O(1) 帧级批量重置，零锁争用，专为渲染/物理帧设计。

### 11. 🎯 0-Overhead Typed Object Pool (Typed Object Pool)
- **类型化对象池** (`mp_typed_pool_create` / `mp_typed_alloc` / `mp_typed_free`)：零 Header 开销，适合高频固定大小对象（如游戏实体、粒子、节点）。

### 12. ⚡ Lock-Free Ring Buffer Allocator (Ring Buffer)
- **DPDK 风格无锁环形缓冲区分配器** (`mp_ring_create` / `mp_ring_alloc` / `mp_ring_free`)：单生产者单消费者场景下极致吞吐。

### 13. 🔗 POSIX Shared Memory IPC (Shared Memory)
- **零拷贝进程间通信** (`mp_create_shared` / `mp_destroy_shared`)：基于 `/dev/shm` POSIX 共享内存段。

### 14. 📦 Linux HugePages 支持 (HugePages)
- **`MP_FLAG_HUGE_PAGES`**：启用 2MB/1GB HugeTLB 页面，显著降低 TLB Miss，适合大内存工作负载。

### 15. 🚨 紧急 OOM 兜底储备 (Emergency Reserve)
- **`mp_enable_emergency_reserve`**：为关键路径（如日志、监控上报）预留一块应急内存垫，在主池 OOM 时仍可分配。

### 16. 🧭 Linux NUMA 节点亲和绑定 (NUMA Affinity)
- **`mp_set_numa_node`**：将内存池底层分配绑定到指定 NUMA CPU 节点，消除跨节点内存访问延迟。

### 17. ⚙️ 环境变量运行时自动调优 (CMEM_CONF)
- **`mp_parse_env_flags` / `CMEM_CONF`**：通过环境变量零代码修改开启 Canary、Poison、Cache Aligned 等特性。示例：`CMEM_CONF="canary=1,poison=on,aligned=1"`

### 18. 📈 高/低水位阈值告警回调 (Watermark Callbacks)
- **`mp_set_watermark_callback`**：配置高/低水位比例回调，自动触发扩容、降级或告警。

### 19. 🌐 全局 malloc/free 符号拦截 (Global Override)
- **`cmem_override.h`**：包含即自动重定向标准 `malloc`/`free`/`realloc`/`calloc` 到 cmem，零侵入迁移现有代码库。

### 20. 📦 批量分配/释放与统计重置 (Batch & Stats)
- **`mp_alloc_batch` / `mp_free_batch`**：高吞吐单次批量分配/释放 $N$ 个内存块。
- **`mp_reset_stats`**：重置累积 QPS、操作计数与 Peak 峰值统计数据。

### 21. ⚙️ 运行时配置热加载 (Runtime Config Hot-Reload)
- **`mp_reparse_env_flags(pool)`**：运行时重新解析 `CMEM_CONF` 环境变量并动态应用安全特性，无需重建池。
- **`mp_get_env_generation(pool)`**：获取当前环境配置生成计数器，检测运行时配置是否已更新。

### 22. 🗜️ 自动内存压缩触发 (Auto-Compaction Trigger)
- **`mp_set_auto_compact(pool, enable, pressure_threshold, fragmentation_threshold)`**：配置基于内存压力或碎片率的自动压缩策略。
- **`mp_auto_compact_check(pool)`**：内部自动检查并在必要时触发 `mp_compact()`，避免手动调用遗漏。

### 23. 📊 分配延迟 P99 统计 (Latency Histogram)
- **`mp_record_latency(pool, latency_ns)`**：记录单次分配延迟样本（纳秒级直方图）。
- **`mp_get_latency_p99(pool)`** / **`mp_get_latency_avg(pool)`**：查询 P99 与平均分配延迟，评估尾延迟。
- **`mp_reset_latency_stats(pool)`**：重置延迟统计直方图。

### 24. 🎛️ 可配置 Slab Class 表 (Configurable Slab Classes)
- **`mp_set_slab_classes(pool, sizes, count)`**：替换默认 Slab 尺寸类表，适配特定工作负载。
- **`mp_get_slab_classes(pool, out_sizes, max_count)`** / **`mp_get_slab_class_count(pool)`**：查询当前 Slab 类配置。
- **`mp_preferred_size_for_pool(pool, size)`**：基于池自定义 Slab 表计算最佳对齐尺寸。

### 25. 📝 结构化事件日志 & pprof 导出 (Event Log & pprof)
- **`mp_event_log_create(capacity)`** / **`mp_event_log_record(...)`** / **`mp_event_log_consume(...)`**：基于无锁 Ring Buffer 的结构化事件日志，支持事后 replay。
- **`mp_event_log_pending(log)`** / **`mp_event_log_clear(log)`** / **`mp_event_log_destroy(log)`**：事件日志生命周期管理。
- **`mp_export_pprof(pool, buf, max_len)`**：导出 pprof 兼容文本格式，支持火焰图生成。

### 26. 🚀 Per-CPU Lock-Free Freelist (Low-Contention Fast Path)
- **`MP_FLAG_PERCPU_FREELIST`**：为小对象分配启用 per-CPU 无锁 freelist，显著降低高并发场景下的锁竞争。
- **`mp_set_percpu_freelist(pool, enable)`** / **`mp_get_percpu_freelist(pool)`** / **`mp_get_percpu_cpu_count(pool)`**：Per-CPU freelist 配置与查询。

### 27. 🛡️ 内存错误恢复 (Error Recovery & Dirty Pool)
- **`mp_mark_pool_dirty(pool)`** / **`mp_clear_pool_dirty(pool)`** / **`mp_is_pool_dirty(pool)`**：标记/清除/查询池的脏状态，检测到 canary 越界或 double free 后自动拒绝新分配。
- **`mp_set_error_recovery_callback(pool, cb, udata)`**：注册内存错误恢复回调。
- **`mp_isolate_bad_block(pool, ptr)`**：隔离坏块，将其从活动追踪中移除并标记为已释放。

### 28. 🎯 线程级配额与熔断器 (Thread Quota & Circuit Breaker)
- **`mp_set_thread_quota(pool, quota_bytes)`**：设置单线程内存配额上限，防止单个线程耗尽池资源。
- **`mp_set_circuit_breaker(pool, enable)`** / **`mp_is_circuit_breaker_tripped(pool)`**：启用/查询线程级熔断器。
- **`mp_get_thread_allocated_bytes(pool)`** / **`mp_reset_thread_quota(pool)`**：查询/重置当前线程已分配字节数。

### 29. 🧊 Hot/Cold 页分离 (Hot/Cold Page Separation)
- **`MP_FLAG_HOT_COLD_SEPARATION`**：启用热/冷页物理分离，提升 TLB 命中率。
- **`mp_mark_page_hot(pool, page_raw_mem)`** / **`mp_mark_page_cold(pool, page_raw_mem)`**：标记页的温度属性。
- **`mp_get_hot_page_count(pool)`** / **`mp_get_cold_page_count(pool)`**：查询热/冷页数量。
- **`mp_separate_hot_cold_pages(pool)`**：执行热/冷页分离，将冷页移至独立内存区域。

### 30. 🔒 加密内存支持 (Encrypted Memory)
- **`MP_FLAG_ENCRYPTED_MEMORY`**：启用加密内存模式，mlock 加 madvise(MADV_DONTDUMP) 双重防护。
- **`mp_lock_memory(pool, addr, length)`** / **`mp_unlock_memory(pool, addr, length)`**：mlock/munlock 内存页，防止敏感数据被 swap 到磁盘。
- **`mp_protect_from_dump(pool, addr, length)`**：madvise(MADV_DONTDUMP) 排除内存免于 core dump。
- **`mp_secure_zero(pool, ptr, length)`**：volatile 安全清零，防止编译器优化导致的数据残留。
- **`mp_set_encrypted_memory(pool, enable)`**：一键启用/禁用加密内存模式。

### 31. 🛡️ AddressSanitizer 集成层 (ASan Integration)
- **`MP_FLAG_ASAN_INTEGRATION`**：启用 ASan 兼容模式，与 AddressSanitizer shadow memory 协同工作。
- **`mp_asan_is_enabled()`**：检测当前是否运行在 ASan 环境下。
- **`mp_asan_report_error(pool, ptr, size, is_write)`**：向 ASan 报告自定义内存错误。
- **`mp_asan_check_memory(pool, ptr, size)`**：检查内存区域是否存在 ASan 错误。
- **`mp_set_asan_integration(pool, enable)`**：启用/禁用 ASan 集成。

### 32. 🚀 在线 Pool 扩容 (Online Pool Expansion)
- **`mp_expand_pool(pool, additional_bytes)`**：在不停止服务的情况下为池追加容量，通过新增 TLSF 池链表实现。
- **`mp_can_expand(pool)`**：查询池是否支持扩容。
- **`mp_get_expandable_size(pool)`**：查询池当前还可扩容的字节数。

---

## 📦 头文件包含 (Headers)

| 语言环境 | 包含头文件 | 命名空间 / 语言规范 |
| :--- | :--- | :--- |
| **C 语言** | `#include "cmem.h"` | C11 标准 C 接口 |
| **C++ 基础封装** | `#include "cmem.hpp"` | `cmem::` 命名空间 |
| **C++17 PMR 适配** | `#include "cmem_pmr.hpp"` | `cmem::pmr_resource` (`std::pmr`) |
| **全局符号拦截** | `#include "cmem_override.h"` | 重定向 `malloc`/`free` 等标准符号 |

---

## 💻 快速开始与代码示例 (Quick Start)

### C 语言使用示例 (内省与内存回收)

```c
#include "cmem.h"
#include <stdio.h>
#include <assert.h>

int main() {
    // 1. 创建开启调试金丝雀与零初始化的内存池
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEBUG_CANARY | MP_FLAG_ZERO_ON_ALLOC);
    mp_set_name(pool, "MyAppRootPool");

    // 2. 内存分配与内省查询
    void* ptr = mp_alloc(pool, 120);
    printf("Is pointer valid: %s\n", mp_ptr_valid(pool, ptr) ? "true" : "false");
    printf("Requested size: %zu bytes, Usable size: %zu bytes\n",
           mp_alloc_size(pool, ptr), mp_usable_size(pool, ptr));

    // 3. 使用便利函数 mp_strdup 与 mp_asprintf
    char* str = mp_strdup(pool, "Hello cmem memory manager!");
    char* formatted = mp_asprintf(pool, "Arena [%s] active allocations: %zu", mp_get_name(pool), (size_t)2);
    printf("%s\n", formatted);

    // 4. 主动内存回收 trim
    mp_free(pool, ptr);
    size_t reclaimed = mp_trim(pool, 0);
    printf("Reclaimed %zu bytes back to OS\n", reclaimed);

    // 5. 销毁内存池
    mp_destroy(pool);
    return 0;
}
```

### C++17 PMR 多态容器使用示例

```cpp
#include "cmem.hpp"
#include "cmem_pmr.hpp"
#include <vector>
#include <string>
#include <iostream>

int main() {
    // 1. 使用 RAII 创建 cmem 内存池
    cmem::MemoryPool pool(1024 * 1024, MP_FLAG_THREAD_SAFE);

    // 2. 构造 C++17 pmr_resource 适配器
    cmem::pmr_resource res(pool.get());

    // 3. 使用 C++17 std::pmr 容器，无需手动管理 free
    std::pmr::vector<std::pmr::string> vec(&res);
    vec.push_back(std::pmr::string("Polymorphic", &res));
    vec.push_back(std::pmr::string("Memory Resource", &res));
    vec.push_back(std::pmr::string("Integration", &res));

    for (const auto& str : vec) {
        std::cout << str << " ";
    }
    std::cout << std::endl;

    return 0; // 退出作用域后 pool 自动安全销毁
}
```

### C++ STL 兼容分配器示例

```cpp
#include "cmem.hpp"
#include <vector>
#include <unordered_map>
#include <string>
#include <cassert>

int main() {
    cmem::MemoryPool pool(1024 * 1024, MP_FLAG_THREAD_SAFE);

    // 使用 cmem::allocator<T> 适配标准容器
    cmem::allocator<int> vec_alloc(pool);
    std::vector<int, cmem::allocator<int>> vec(vec_alloc);
    for (int i = 0; i < 100; i++) vec.push_back(i * 10);
    assert(vec[50] == 500);

    using MapAlloc = cmem::allocator<std::pair<const int, std::string>>;
    std::unordered_map<int, std::string, std::hash<int>, std::equal_to<int>, MapAlloc> map(10, std::hash<int>(), std::equal_to<int>(), MapAlloc(pool));
    map[1] = "cmem";
    map[2] = "High-Performance";
    assert(map[1] == "cmem");

    // 容器析构自动归还内存
    pool.check_leaks(); // 无泄漏
    return 0;
}
```

### 高级特性示例：Frame Arena (游戏/渲染管线)

```c
#include "cmem.h"
#include <stdio.h>
#include <string.h>

int main() {
    // 创建双缓冲 Ping-Pong 帧竞技场 (512KB per frame)
    cmem_frame_arena_t* farena = mp_frame_arena_create(512 * 1024);

    // Frame 1
    void* frame1_ptr = mp_frame_alloc(farena, 1024);
    strcpy((char*)frame1_ptr, "RenderMeshFrame1");
    mp_frame_end(farena);  // O(1) Swap to buffer 2

    // Frame 2
    void* frame2_ptr = mp_frame_alloc(farena, 2048);
    strcpy((char*)frame2_ptr, "RenderMeshFrame2");
    mp_frame_end(farena);  // O(1) Swap back to buffer 1

    mp_frame_arena_destroy(farena);
    printf("Frame Arena Ping-Pong completed!\n");
    return 0;
}
```

### 高级特性示例：Typed Object Pool (零开销对象池)

```c
#include "cmem.h"
#include <stdio.h>
#include <assert.h>

typedef struct { int id; char name[32]; double value; } game_entity_t;

int main() {
    // 创建类型化对象池，容量 128 个 game_entity_t
    mp_typed_pool_t* tpool = mp_typed_pool_create(sizeof(game_entity_t), 128);

    // 分配对象 - 零 Header 开销，直接返回对象指针
    game_entity_t* e1 = (game_entity_t*)mp_typed_alloc(tpool);
    game_entity_t* e2 = (game_entity_t*)mp_typed_alloc(tpool);
    assert(e1 != e2);

    e1->id = 42;
    strcpy(e1->name, "Player");
    e1->value = 100.0;

    printf("Entity: id=%d, name=%s, value=%.1f\n", e1->id, e1->name, e1->value);

    mp_typed_free(tpool, e1);
    mp_typed_free(tpool, e2);
    mp_typed_pool_destroy(tpool);
    return 0;
}
```

### 高级特性示例：Lock-Free Ring Buffer

```c
#include "cmem.h"
#include <stdio.h>
#include <string.h>

int main() {
    // 创建无锁环形缓冲区，槽大小 128B，容量 64 槽
    cmem_ring_buffer_t* ring = mp_ring_create(128, 64);

    void* slot1 = mp_ring_alloc(ring);
    void* slot2 = mp_ring_alloc(ring);
    strcpy((char*)slot1, "Lock-Free Payload 1");
    strcpy((char*)slot2, "Lock-Free Payload 2");

    printf("Slot 1: %s\n", (char*)slot1);
    printf("Slot 2: %s\n", (char*)slot2);

    mp_ring_free(ring, slot1);
    mp_ring_free(ring, slot2);
    mp_ring_destroy(ring);
    return 0;
}
```

### 高级特性示例：Shared Memory IPC

```c
#include "cmem.h"
#include <stdio.h>
#include <string.h>

int main() {
    // 进程 A: 创建共享内存池
    memory_pool_t* pool = mp_create_shared("/my_ipc_pool", 512 * 1024, MP_FLAG_THREAD_SAFE);
    void* p = mp_alloc(pool, 1024);
    strcpy((char*)p, "Zero-Copy IPC Message from Process A");
    
    // 进程 B: 打开同名共享内存池
    // memory_pool_t* pool_b = mp_create_shared("/my_ipc_pool", 512 * 1024, MP_FLAG_THREAD_SAFE);
    // printf("Process B reads: %s\n", (char*)p);
    
    mp_free(pool, p);
    mp_destroy_shared(pool, "/my_ipc_pool");
    return 0;
}
```

### 高级特性示例：泄漏分析与 HTML 报告

```c
#include "cmem.h"
#include <stdio.h>

void leaky_function(memory_pool_t* pool) {
    mp_alloc_loc(pool, 256, __FILE__, __LINE__, __func__); // 故意泄漏
    void* p = mp_alloc_loc(pool, 128, __FILE__, __LINE__, __func__);
    mp_free(pool, p); // 正常释放
}

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_TRACK_LOCATIONS | MP_FLAG_DEBUG_CANARY);
    
    leaky_function(pool);
    
    // 1. 堆完整性审计
    bool healthy = mp_audit_heap(pool);
    printf("Heap Audit: %s\n", healthy ? "HEALTHY" : "CORRUPTED");
    
    // 2. 详细泄漏分析报告 (含 file:line)
    char report[8192];
    mp_analyze_leaks(pool, report, sizeof(report));
    printf("%s\n", report);
    
    // 3. 导出交互式 HTML 大屏
    mp_export_html_report(pool, "memory_profile.html");
    
    // 4. 导出二进制快照用于 Diff 对比
    mp_export_binary_snapshot(pool, "snapshot_before.cmem_dump");
    
    mp_destroy(pool);
    return 0;
}
```

### 高级特性示例：Prometheus 监控导出

```c
#include "cmem.h"
#include <stdio.h>

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);
    
    mp_alloc(pool, 1024);
    mp_alloc(pool, 2048);
    
    char prom_buf[4096];
    size_t len = mp_export_prometheus_metrics(pool, prom_buf, sizeof(prom_buf));
    
    printf("Prometheus Metrics (%zu bytes):\n%s\n", len, prom_buf);
    // 输出示例:
    // cmem_active_bytes{arena="RootArena"} 3072
    // cmem_total_alloc_ops{arena="RootArena"} 2
    // cmem_pressure_ratio{arena="RootArena"} 0.002
    
    mp_destroy(pool);
    return 0;
}
```

### 高级特性示例：环境变量自动调优

```bash
# 无需修改代码，通过环境变量开启安全特性
export CMEM_CONF="canary=1,poison=on,aligned=1,guard=1"

./your_application
```

```c
// 代码中也可显式解析
#include "cmem.h"

int main() {
    mp_flags_t flags = mp_parse_env_flags(MP_FLAG_DEFAULT);
    memory_pool_t* pool = mp_create(0, flags);
    // pool 自动拥有 Canary + Poison + Cache Aligned + Guard Pages
    return 0;
}
```

### 全局 malloc/free 拦截 (零侵入迁移)

```c
// 在现有代码库的统一公共头文件中包含 (或在 main.c 最先包含)
#include "cmem_override.h"

// 之后所有的 malloc/free/realloc/calloc 自动重定向到 cmem
// 无需修改任何业务逻辑代码
int main() {
    char* p = malloc(100);  // 实际调用 cmem_malloc -> mp_alloc
    strcpy(p, "Redirected transparently!");
    free(p);                // 实际调用 cmem_free -> mp_free
    return 0;
}
```

---

## 🛠️ API 完整索引表 (API Reference)

### 1. 内存池生命周期与创建

| API 接口 | 功能说明 |
| :--- | :--- |
| `mp_create(capacity, flags)` | 创建并初始化标准内存池实例 |
| `mp_create_child(parent, capacity, flags, name)` | 创建嵌套在父内存池下的子 Arena |
| `mp_create_from_buffer(buf, size, flags)` | 在预分配静态缓冲区中创建内存池（0 OS `malloc` 依赖） |
| `mp_create_shared(shm_name, capacity, flags)` | 创建 POSIX 共享内存池 (`/dev/shm` 零拷贝 IPC) |
| `mp_create_custom(capacity, flags, sys_allocator)` | 注入自定义系统内存分配器句柄 |
| `mp_destroy(pool)` | 销毁内存池并递归销毁旗下所有子 Context |
| `mp_destroy_shared(pool, shm_name)` | 销毁共享内存池并解除 POSIX 共享内存段链接 |
| `mp_reset(pool)` | $O(1)$ 批量重置内存池及其旗下所有子 Context |

### 2. 内存分配与释放

| API 接口 | 功能说明 |
| :--- | :--- |
| `mp_alloc(pool, size)` | 从内存池中分配指定字节的内存块 |
| `mp_calloc(pool, num, size)` | 分配内存块并自动填充零数据 |
| `mp_realloc(pool, ptr, new_size)` | 重新调整内存块尺寸（TLSF 自动触发原地 Expand） |
| `mp_reallocarray(pool, ptr, nmemb, size)` | 溢出安全的数组重新分配（校验 `nmemb * size` 溢出） |
| `mp_aligned_alloc(pool, alignment, size)` | 分配指定对齐边界（如 Cache Line 64B）的内存块 |
| `mp_free(pool, ptr)` | 释放内存块归还内存池 |
| `mp_alloc_batch(pool, size, out_ptrs, count)` | 高吞吐单次批量分配 $N$ 个内存块 |
| `mp_free_batch(pool, ptrs, count)` | 单次批量释放 $N$ 个内存块 |

### 3. 内省查询与元数据

| API 接口 | 功能说明 |
| :--- | :--- |
| `mp_usable_size(pool, ptr)` | 查询已分配内存块的真实可用字节容量 |
| `mp_alloc_size(pool, ptr)` | 查询已分配内存块的用户申请字节尺寸 |
| `mp_ptr_valid(pool, ptr)` | 校验指针是否为当前池中活动状态的有效指针 |
| `mp_preferred_size(size)` | 计算最佳匹配的 Size Class 对齐尺寸 |
| `mp_set_name(pool, name)` | 设置内存池的易读名称 |
| `mp_get_name(pool)` | 获取内存池的易读名称 |
| `mp_get_parent(pool)` | 获取父内存池指针（若为子 Arena） |
| `mp_get_child_count(pool)` | 获取当前池直接关联的子 Arena 数量 |

### 4. 便利辅助函数 (Convenience APIs)

| API 接口 | 功能说明 |
| :--- | :--- |
| `mp_strdup(pool, str)` | 在内存池中深拷贝复制以 `\0` 结尾的字符串 |
| `mp_memdup(pool, src, n)` | 在内存池中深拷贝复制 $N$ 字节二进制数据 |
| `mp_asprintf(pool, fmt, ...)` | 格式化字符串并在内存池中分配存储空间 |
| `mp_strdup_loc / mp_memdup_loc / mp_asprintf_loc` | 带源码位置追踪的变体 |

### 5. 内存紧凑与回收 (Trim & Purging)

| API 接口 | 功能说明 |
| :--- | :--- |
| `mp_trim(pool, pad)` | 深层内存紧凑与页归还，回收未使用的物理页并保留 pad 字节 |
| `mp_madvise(pool, addr, len, advice)` | 跨平台内存 Advice 包装（Linux `madvise` / Win `VirtualAlloc`） |
| `mp_compact(pool)` | 紧凑内存池并释放空闲 Slab 页面 |
| `mp_purge_lazy(pool)` | 延迟物理内存页 RSS 清理 (Linux `MADV_DONTNEED` / `MADV_FREE`) |
| `mp_resident(pool)` | 获取当前内存池系统 RSS 物理驻留字节数 |
| `mp_freeable(pool)` | 获取当前池内可被 `mp_trim` 回收的空闲页字节数 |

### 6. 统计、诊断与监控

| API 接口 | 功能说明 |
| :--- | :--- |
| `mp_get_stats(pool, stats)` | 获取包含 QPS、带宽、分配阶分布的统计快照 |
| `mp_pressure(pool)` | 获取内存池使用压力比例 $[0.0, 1.0]$ |
| `mp_reset_stats(pool)` | 重置累积 QPS、操作计数与 Peak 峰值统计数据 |
| `mp_export_html_report(pool, path)` | 导出交互式可视化 HTML 剖析与泄漏大屏报告 |
| `mp_export_prometheus_metrics(pool, buf, len)` | 导出 Prometheus / OpenTelemetry 标准格式指标 |
| `mp_export_binary_snapshot(pool, filepath)` | 导出二进制崩溃后内存快照 |
| `mp_parse_binary_snapshot(filepath, buf, len)` | 解析二进制快照为可读文本报告 |
| `mp_diff_snapshots(snap_a, snap_b, buf, len)` | 对比两次内存快照并生成增量泄漏差异报告 |
| `mp_dump_histogram(pool)` | 打印 ASCII 分配尺寸分布直方图 |
| `mp_dump_info(pool)` | 打印详细汇总与健康状态到 stdout |
| `mp_dump_tree_info(pool)` | 打印内存池树状层级结构到 stdout |
| `mp_dump_json_stats(pool, buf, max_len)` | 导出 JSON 格式统计数据供遥测监控采集 |

### 7. 泄漏检测与堆审计

| API 接口 | 功能说明 |
| :--- | :--- |
| `mp_audit_heap(pool)` | 遍历堆内存，主动审计 Redzone Canary 越界踩内存 |
| `mp_analyze_leaks(pool, buf, len)` | 生成包含代码位置 (`file:line`) 与调用栈的泄漏报告 |
| `mp_export_leak_report(pool, filepath)` | 导出泄漏分析报告到文本文件 |
| `mp_check_leaks(pool)` | 检查是否存在未释放的内存分配 |

### 8. 高级特性 API

| API 接口 | 功能说明 |
| :--- | :--- |
| `mp_enable_emergency_reserve(pool, bytes)` | 启用紧急 OOM 兜底内存储备垫 |
| `mp_set_numa_node(pool, numa_node)` | 绑定内存池底层分配到指定 Linux NUMA CPU 节点 |
| `mp_parse_env_flags(default_flags)` | 解析 `CMEM_CONF` 环境变量并返回合并配置标志 |
| `mp_reparse_env_flags(pool)` | 运行时重新解析 `CMEM_CONF` 并动态应用安全特性 |
| `mp_get_env_generation(pool)` | 获取当前环境配置生成计数器 |
| `mp_set_auto_compact(pool, enable, pressure, fragmentation)` | 配置自动压缩触发阈值 |
| `mp_auto_compact_check(pool)` | 检查并触发自动压缩 |
| `mp_set_arena_quota(pool, quota_bytes, cb, udata)` | 设置单 Arena 内存配额与超限回调 |
| `mp_check_arena_quota(pool)` | 检查 Arena 是否在配额范围内 |
| `mp_record_latency(pool, latency_ns)` | 记录分配延迟样本（纳秒直方图） |
| `mp_get_latency_p99(pool)` / `mp_get_latency_avg(pool)` | 查询 P99 / 平均分配延迟 |
| `mp_reset_latency_stats(pool)` | 重置延迟统计直方图 |
| `mp_set_slab_classes(pool, sizes, count)` | 自定义 Slab 尺寸类表 |
| `mp_get_slab_classes(pool, out_sizes, max_count)` | 查询当前 Slab 类配置 |
| `mp_get_slab_class_count(pool)` | 查询 Slab 类数量 |
| `mp_preferred_size_for_pool(pool, size)` | 基于池自定义 Slab 表计算最佳尺寸 |
| `mp_event_log_create(capacity)` | 创建结构化事件日志 Ring Buffer |
| `mp_event_log_record(log, event_type, ptr, size)` | 记录事件到日志 |
| `mp_event_log_consume(log, entry)` | 消费日志条目 |
| `mp_event_log_pending(log)` | 查询待处理事件数 |
| `mp_event_log_clear(log)` | 清空日志 |
| `mp_event_log_destroy(log)` | 销毁事件日志 |
| `mp_export_pprof(pool, buf, max_len)` | 导出 pprof 兼容文本 |
| `mp_set_percpu_freelist(pool, enable)` | 启用/禁用 Per-CPU 无锁 freelist |
| `mp_get_percpu_freelist(pool)` | 查询 Per-CPU freelist 状态 |
| `mp_get_percpu_cpu_count(pool)` | 查询 Per-CPU 检测到的 CPU 数 |
| `mp_set_fallback_on_oom(pool, enable)` | 启用 OOM 时回退到系统 malloc |
| `mp_set_gc_callback(pool, cb, udata)` | 注册 GC 回调（OOM 前释放非关键缓存） |
| `mp_set_eviction_callback(pool, cb, udata)` | 注册逐出回调（压力下驱逐低优先级对象） |
| `mp_mark_pool_dirty(pool)` / `mp_clear_pool_dirty(pool)` / `mp_is_pool_dirty(pool)` | 标记/清除/查询脏池状态 |
| `mp_set_error_recovery_callback(pool, cb, udata)` | 注册内存错误恢复回调 |
| `mp_isolate_bad_block(pool, ptr)` | 隔离坏块 |
| `mp_set_thread_quota(pool, quota_bytes)` | 设置单线程内存配额 |
| `mp_set_circuit_breaker(pool, enable)` | 启用/禁用线程级熔断器 |
| `mp_is_circuit_breaker_tripped(pool)` | 查询熔断器是否触发 |
| `mp_get_thread_allocated_bytes(pool)` | 查询当前线程已分配字节数 |
| `mp_reset_thread_quota(pool)` | 重置当前线程配额计数器 |
| `mp_abi_version()` | 获取 ABI 版本号 |
| `mp_set_cgroup_aware(pool, enable)` | 启用容器 cgroup 内存限制感知 |
| `mp_get_cgroup_mem_limit(pool)` | 获取检测到的 cgroup 内存限制 |
| `mp_mark_page_hot(pool, page_raw_mem)` / `mp_mark_page_cold(pool, page_raw_mem)` | 标记 Slab 页温度 |
| `mp_get_hot_page_count(pool)` / `mp_get_cold_page_count(pool)` | 查询热/冷页数量 |
| `mp_separate_hot_cold_pages(pool)` | 执行热/冷页物理分离 |
| `mp_lock_memory(pool, addr, length)` / `mp_unlock_memory(pool, addr, length)` | mlock/munlock 内存页 |
| `mp_protect_from_dump(pool, addr, length)` | madvise(MADV_DONTDUMP) 排除 core dump |
| `mp_secure_zero(pool, ptr, length)` | volatile 安全清零 |
| `mp_set_encrypted_memory(pool, enable)` | 启用/禁用加密内存模式 |
| `mp_asan_is_enabled()` | 检测 ASan 是否激活 |
| `mp_asan_report_error(pool, ptr, size, is_write)` | 向 ASan 报告内存错误 |
| `mp_asan_check_memory(pool, ptr, size)` | 检查 ASan 错误 |
| `mp_set_asan_integration(pool, enable)` | 启用/禁用 ASan 集成 |
| `mp_expand_pool(pool, additional_bytes)` | 在线扩容池容量 |
| `mp_can_expand(pool)` | 查询池是否可扩容 |
| `mp_get_expandable_size(pool)` | 查询池可扩容字节数 |
| `mp_frame_arena_create(capacity)` | 创建游戏/图形双缓冲 Ping-Pong 帧竞技场 |
| `mp_frame_alloc(farena, size)` | 为当前帧分配临时内存 |
| `mp_frame_end(farena)` | 结束当前帧并交换 Ping-Pong 缓冲区 |
| `mp_frame_arena_destroy(farena)` | 销毁帧竞技场实例 |
| `mp_typed_pool_create(elem_size, capacity)` | 创建 0-Overhead 定型对象池分配器 |
| `mp_typed_alloc(tpool)` | 从类型化对象池分配对象指针 |
| `mp_typed_free(tpool, ptr)` | 将对象指针归还类型化对象池 |
| `mp_typed_pool_destroy(tpool)` | 销毁类型化对象池实例 |
| `mp_ring_create(slot_size, capacity)` | 创建 DPDK 风格无锁环形缓冲区分配器 |
| `mp_ring_alloc(ring)` | 从无锁环形缓冲区分配槽位指针 |
| `mp_ring_free(ring, ptr)` | 将槽位指针归还无锁环形缓冲区 |
| `mp_ring_destroy(ring)` | 销毁无锁环形缓冲区分配器实例 |
| `mp_set_watermark_callback(pool, high, low, cb, udata)` | 配置高/低水位阈值告警回调 |
| `mp_set_memory_limit(pool, max_bytes)` | 设置内存池硬性最大预算限制 |
| `mp_set_event_callback(pool, cb, udata)` | 注册事件回调函数用于实时 Profiling 与调试 |

### 9. 配置标志位 (mp_flags_t)

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

### 10. C++ API 概览 (cmem.hpp / cmem_pmr.hpp)

#### `cmem::MemoryPool` (RAII Wrapper)
| 方法 | 说明 |
| :--- | :--- |
| `MemoryPool(capacity, flags)` | 构造标准池 |
| `MemoryPool(shm_name, capacity, flags)` | 构造共享内存池 |
| `MemoryPool(parent, capacity, flags, name)` | 构造子 Arena |
| `alloc(size)` / `alloc_loc(...)` | 分配内存 |
| `calloc(num, size)` | 分配并清零 |
| `realloc(ptr, new_size)` | 重新分配 |
| `aligned_alloc(alignment, size)` | 对齐分配 |
| `free(ptr)` | 释放 |
| `alloc_batch / free_batch` | 批量操作 |
| `reset()` | $O(1)$ 重置 |
| `compact()` | 紧凑内存 |
| `set_memory_limit(bytes)` | 设置内存上限 |
| `audit_heap()` | 堆审计 |
| `analyze_leaks()` | 返回泄漏报告字符串 |
| `export_leak_report(path)` | 导出泄漏报告文件 |
| `export_html_report(path)` | 导出 HTML 报告 |
| `export_binary_snapshot(path)` | 导出二进制快照 |
| `parse_binary_snapshot(path)` | 静态方法解析快照 |
| `get_stats()` | 获取 `mp_stats_t` 结构体 |
| `dump_info() / dump_tree_info() / dump_histogram()` | 打印诊断信息 |
| `check_leaks()` | 检查泄漏 |
| `get_raw_pool() / get()` | 获取底层 `memory_pool_t*` |

#### `cmem::allocator<T>` (STL-Compatible Allocator)
- 完全符合 C++11/14/17 Allocator requirements
- 支持 `std::vector<T, cmem::allocator<T>>`、`std::unordered_map<...>` 等所有标准容器
- 提供 `construct` / `destroy` / `rebind` 完整接口

#### `cmem::pmr_resource` (C++17 PMR)
| 方法 | 说明 |
| :--- | :--- |
| `pmr_resource(pool)` | 构造 PMR 资源适配器 |
| `pool()` | 获取底层 `memory_pool_t*` |
| `do_allocate(bytes, alignment)` | 从 cmem 池分配内存（继承自 `memory_resource`） |
| `do_deallocate(p, bytes, alignment)` | 归还内存到 cmem 池（继承自 `memory_resource`） |
| `do_is_equal(other)` | 比较是否包装同一个 cmem 池（继承自 `memory_resource`） |

- 继承自 `std::pmr::memory_resource`
- 适配 `std::pmr::vector`、`std::pmr::string`、`std::pmr::map` 等多态容器
- 当 alignment > sizeof(void*) 时自动调用 `mp_aligned_alloc`
- 通过 `dynamic_cast` 实现基于底层池指针的相等性比较

**典型用法：**
```cpp
cmem::MemoryPool pool(1024 * 1024, MP_FLAG_THREAD_SAFE);
cmem::pmr_resource res(pool.get());

// 所有 std::pmr 容器自动使用 cmem 作为后端分配器
std::pmr::vector<std::pmr::string> vec(&res);
std::pmr::unordered_map<int, std::pmr::string> map(&res);

vec.push_back(std::pmr::string("Hello", &res));
map[1] = std::pmr::string("cmem PMR", &res);
```

---

## 🚀 编译与测试 (Build & Test)

项目支持标准的 **Makefile** 与 **CMake** 构建系统：

```bash
# 1. 编译静态库 libcmem.a 并运行 C 单元测试 (含 ASan / UBSan 检查)
make test

# 2. 编译并运行 C++17 PMR 与 STL 容器集成测试
make test_cpp

# 3. 编译并运行性能 Benchmark 压测
make bench

# 4. 编译并运行所有 Example 示例代码
make examples

# 5. 使用 CMake 构建与运行 CTest
cmake -B build_cmake -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build_cmake
ctest --test-dir build_cmake --output-on-failure

# 6. Release 优化构建
cmake -B build_release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_release

# 7. 安装到系统 (可选)
cmake --install build_release --prefix /usr/local

# 8. 清理构建产物
make clean
```

### Makefile 目标说明

| 目标 | 说明 |
| :--- | :--- |
| `all` | 编译库 + 所有测试 + Benchmark + 示例 |
| `lib` | 仅编译静态库 `build/libcmem.a` |
| `test` | 编译并运行 C 单元测试 (Debug + Sanitizers) |
| `test_cpp` | 编译并运行 C++ 测试 (PMR + STL Allocator) |
| `bench` | 编译并运行性能基准测试 |
| `examples` | 编译并运行 4 个示例程序 |
| `clean` | 清理构建目录与临时报告文件 |

---

## 📊 性能基准 (Performance Benchmarks)

在典型工作负载下对比系统 `malloc`/`free` (Linux x86_64, GCC 13, Release 构建)：

| 基准场景 | System Malloc | cmem | 加速比 |
| :--- | :--- | :--- | :---: |
| **小对象分配 (32-256B × 1M ops)** | ~450 Mops/sec | ~1,800 Mops/sec | **~4.0x** |
| **中对象分配 (1KB-64KB × 100K ops)** | ~180 Mops/sec | ~420 Mops/sec | **~2.3x** |
| **Arena Reset (500 allocs × 1000 rounds)** | 12.4 ms (逐个 free) | 0.8 ms (`mp_reset`) | **~15x** |
| **多线程竞争 (8 threads × 100K small allocs)** | High contention | Near-linear scaling | **~8x** |

> Benchmark 运行：`make bench` 或 `./build/benchmark`

---

## 📁 项目结构 (Project Structure)

```
cmem/
├── include/
│   ├── cmem.h              # C11 公共 API 头文件
│   ├── cmem.hpp            # C++11 RAII 封装 + STL Allocator
│   ├── cmem_pmr.hpp        # C++17 std::pmr::memory_resource 适配
│   └── cmem_override.h     # 全局 malloc/free 符号拦截
├── src/
│   └── cmem.c              # 核心实现 (单文件 ~8000+ 行)
├── tests/
│   ├── test_main.c         # C 综合单元测试 (35+ 测试用例)
│   └── test_cpp.cpp        # C++ PMR + STL Allocator 测试
├── benchmarks/
│   └── bench_main.c        # 性能基准测试
├── examples/
│   ├── example_basic.c         # 基础用法 + Event Profiler
│   ├── example_embedded.c      # 静态缓冲区零依赖模式
│   ├── example_leak_analysis.c # 泄漏分析 + 堆审计 + HTML
│   └── example_arena_tree.c    # 树状 Arena + HTML Dashboard
├── CMakeLists.txt          # CMake 构建配置
├── Makefile                # Makefile 构建配置
├── LICENSE                 # MIT 许可证
└── README.md               # 本文档
```

---

## 📋 依赖要求 (Requirements)

| 平台 | 要求 |
| :--- | :--- |
| **C 编译器** | GCC ≥ 7 / Clang ≥ 6 / MSVC ≥ 19.20 (C11 标准) |
| **C++ 编译器** | GCC ≥ 7 / Clang ≥ 6 / MSVC ≥ 19.20 (C++17 标准，PMR 需 C++17) |
| **构建系统** | GNU Make ≥ 3.81 **或** CMake ≥ 3.10 |
| **系统库** | `pthread`, `rt` (Linux), `librt` (实时扩展) |
| **可选特性** | Linux: `libnuma` (NUMA 绑定), `libhugetlbfs` (HugePages)<br>Windows: 无额外依赖 |

### 编译器标志推荐

```bash
# Release (生产环境)
-O3 -march=native -flto -DNDEBUG

# Debug + Sanitizers (开发/测试)
-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer
```

---

## 🧪 测试覆盖 (Test Coverage)

`make test` 运行 35+ 综合单元测试覆盖：

- 基础分配/释放/重分配 (Slab/TLSF/OS Fallback 三层)
- 线程安全与并发压力测试
- 内省 API (`usable_size`, `alloc_size`, `ptr_valid`)
- TLSF 原地重分配优化
- 溢出安全 `reallocarray`
- 便利函数 (`strdup`, `memdup`, `asprintf`)
- `mp_trim` / `mp_compact` / `mp_purge_lazy` 内存回收
- Arena 元数据与层级导航
- 高级统计 (`pressure`, `resident`, `freeable`)
- 统计重置与 `mp_preferred_size`
- 跨平台 `mp_madvise`
- 紧急 OOM 储备垫
- NUMA 节点绑定
- Frame Arena Ping-Pong
- 二进制快照 Diff 泄漏检测
- 水位回调告警
- Prometheus 指标导出
- Typed Object Pool 零开销分配
- 环境变量 `CMEM_CONF` 自动调优
- Lock-Free Ring Buffer
- 二进制崩溃快照 Dump/Parse
- HugePages 加速
- 全局 `malloc`/`free` 符号拦截
- 实时 QPS/带宽吞吐计量
- POSIX 共享内存零拷贝 IPC
- 子 Arena 与 HTML 导出
- Arena Reset 与 JSON 统计导出
- 静态缓冲区与事件回调
- 多线程安全验证
- 运行时配置热加载 (`mp_reparse_env_flags`)
- 自动压缩触发 (`mp_set_auto_compact`)
- 分配延迟 P99 统计 (`mp_record_latency`)
- 可配置 Slab Class 表 (`mp_set_slab_classes`)
- 结构化事件日志 & pprof 导出
- Per-CPU Lock-Free Freelist
- 内存错误恢复 (Dirty Pool / Bad-Block Isolation)
- 线程级配额与熔断器 (Thread Quota / Circuit Breaker)
- ABI 版本 & cgroup 感知
- Hot/Cold 页分离
- 加密内存支持 (mlock / MADV_DONTDUMP / secure zero)
- AddressSanitizer 集成层
- 在线 Pool 扩容

---

## 📄 开源许可证 (License)

本项目基于 **MIT License** 开源发布。详见 [LICENSE](LICENSE)。

```
MIT License

Copyright (c) 2024 cmem contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 🤝 贡献指南 (Contributing)

欢迎提交 Issue 与 Pull Request！

1. Fork 本仓库
2. 创建特性分支: `git checkout -b feature/amazing-feature`
3. 提交变更: `git commit -m 'feat: add amazing feature'`
4. 推送分支: `git push origin feature/amazing-feature`
5. 发起 Pull Request

### 代码规范
- C 代码遵循 C11 标准，使用 `-Wall -Wextra -Wpedantic`
- C++ 代码遵循 C++17 标准
- 所有新增 API 必须包含 Doxygen 风格注释
- 新增功能需在 `tests/test_main.c` 或 `tests/test_cpp.cpp` 添加对应测试用例
- 通过 `make test` 与 `make test_cpp` 全量测试验证

---

## 📞 联系与支持 (Contact)

- **Issues**: [GitHub Issues](https://github.com/your-repo/cmem/issues)
- **Discussions**: [GitHub Discussions](https://github.com/your-repo/cmem/discussions)

---

**cmem** — 让内存管理更简单、更安全、更高效。 🚀