# cmem - 通用高性能分层内存管理器

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![CI Build & Test](https://github.com/quintin-lee/cmem/actions/workflows/ci.yml/badge.svg)](https://github.com/quintin-lee/cmem/actions/workflows/ci.yml)
[![C11](https://img.shields.io/badge/C-C11-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B17)
[![clang-format](https://img.shields.io/badge/Code%20Style-clang--format-green.svg)](https://clang.llvm.org/docs/ClangFormat.html)
[![ASan+UBSan](https://img.shields.io/badge/Sanitizer-ASan%20%2B%20UBSan-red.svg)](https://clang.llvm.org/docs/AddressSanitizer.html)
[![Valgrind](https://img.shields.io/badge/Valgrind-Tested-orange.svg)](https://valgrind.org/)

**cmem** 是一个基于 **C11 / C++17** 设计的通用高性能分层内存管理工具。它综合了 **Slab** 和 **TLSF** 等工业级分配器的核心优势，提供全面的内存诊断、内省查询、级联树形 Arena 组织、跨平台 OS 页回收到 C++17 PMR 容器适配。

---

## 🌟 核心架构 (Architecture)

**cmem** 采用现代 **分层混合架构**，在保证 $O(1)$ 分配时间复杂度的同时，大幅降低内存碎片率并显著提升 L1/L2 缓存局部性：

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

## 🔥 功能亮点

### 1. ⚡ 高性能分层分配器
- **Slab 分配器 (<= 512B)**：适用于小对象 (8B ~ 512B) 的固定大小类分配器，具备 $O(1)$ 分配/释放和零外部碎片。
- **TLSF 分配器 (512B ~ 4MB)**：二级分离适配，具备 $O(1)$ 位图搜索和**原地扩展**以避免不必要的 `memcpy`。
- **直接 OS 回退 (> 4MB)**：自动回退到系统内存映射，支持 Guard Pages 和 HugePages 加速。

### 2. 🔍 内存内省 API
- **`mp_usable_size`**：查询已分配块的实际可用字节容量。
- **`mp_alloc_size`**：查询用户请求的字节大小。
- **`mp_ptr_valid`**：快速验证指针是否为活跃分配。
- **`mp_preferred_size`**：计算最佳匹配大小类对齐。

### 3. ♻️ 内存压缩与 OS 页回收
- **`mp_trim`**：深度物理页压缩和回收，可选填充。
- **`mp_madvise`**：跨平台内存建议包装器（Linux 上为 `madvise`，Windows 上为 `VirtualAlloc(MEM_RESET)`）。
- **`mp_compact` / `mp_purge_lazy`**：压缩内存并惰性清除物理页。
- **`mp_resident`**：获取当前 RSS 常驻内存大小。
- **`mp_freeable`**：获取可回收的空闲页字节数。

### 4. 🔒 高并发锁机制 (RWLock & 细粒度锁)
- **读写锁**：所有内省 API 使用读锁以实现零锁争用分析。
- **细粒度 Slab 类锁**：每个 Slab 大小类都有自己的 `pthread_mutex_t`。
- **线程本地缓存**：小对象分配的无锁快速路径。

### 5. 🚀 C++17 PMR & STL 容器适配器
- 包含 `#include "cmem_pmr.hpp"` 以获得 `cmem::pmr_resource` 适配器。
- 提供 `cmem::MemoryPool` RAII 包装器和 `cmem::allocator<T>` STL 分配器。

### 6. 🛡️ 安全与安全加固
- **`mp_reallocarray`**：溢出安全重分配，带 `nmemb * size` 验证。
- **红区金丝雀**：`0xAB` 尾字节用于实时溢出检测。
- **UAF 中毒**：释放时使用 `0xDD` 中毒模式。
- **Guard Pages**：页边界处的 `PROT_NONE`。
- **缓存行对齐**：64B 对齐以消除伪共享。

### 7. 🛠️ 便捷辅助函数
- **`mp_strdup`**：将空终止字符串深度复制到池中。
- **`mp_memdup`**：深度复制 N 字节二进制区域。
- **`mp_asprintf`**：格式化字符串分配。

### 8. 🌳 树形内存 Arena 导航
- 嵌套父子池（`mp_create_child`）支持递归销毁/重置。
- 元数据 API：`mp_set_name`、`mp_get_name`、`mp_get_parent`、`mp_get_child_count`。

### 9. 📊 诊断与监控
- **交互式 HTML 报告**：带内存可视化的单文件 HTML 仪表板。
- **Prometheus 导出器**：标准 exposition 格式指标。
- **二进制快照**：带有增量差异泄漏检测的死后崩溃转储。
- **实时 QPS 与带宽**：`mp_stats_t` 中的吞吐量计量。
- **大小直方图**：ASCII 分配大小分布。

### 10. 🎮 游戏/图形帧 Arena
- **双 Ping-Pong 帧 Arena**：$O(1)$ 帧级批量重置，零锁争用。

### 11. 🎯 零开销类型化对象池
- 用于高频分配的零头部开销固定大小对象池。

### 12. ⚡ 无锁环形缓冲区分配器
- 适用于单生产者/单消费者场景的 DPDK 风格原子环形缓冲区。

### 13. 🔗 POSIX 共享内存 IPC
- 通过 `/dev/shm` 实现零拷贝进程间通信。

### 14. 📦 Linux HugePages 支持
- 2MB/1GB HugeTLB 页面以减少 TLB 未命中。

### 15. 🚨 紧急 OOM 回退储备
- 在 OOM 期间为关键操作保留缓冲。

### 16. 🧭 Linux NUMA 节点亲和性
- 将池后备内存绑定到特定 NUMA 节点。

### 17. ⚙️ 运行时配置热重载
- **`mp_reparse_env_flags(pool)`**：无需重建池即可热重载 `CMEM_CONF`。
- **`mp_get_env_generation(pool)`**：通过代际计数器检测运行时配置更改。

### 18. 🗜️ 自动压缩触发
- **`mp_set_auto_compact(pool, enable, pressure_threshold, fragmentation_threshold)`**：基于池压力或碎片配置自动压缩。
- **`mp_auto_compact_check(pool)`**：在需要时内部检查并触发 `mp_compact()`。

### 19. 📊 分配延迟 P99 统计
- **`mp_record_latency(pool, latency_ns)`**：记录分配延迟样本（纳秒直方图）。
- **`mp_get_latency_p99(pool)`** / **`mp_get_latency_avg(pool)`**：查询 P99 和平均分配延迟。
- **`mp_reset_latency_stats(pool)`**：重置延迟直方图。

### 20. 🎛️ 可配置 Slab 类表
- **`mp_set_slab_classes(pool, sizes, count)`**：替换默认 Slab 大小类表。
- **`mp_get_slab_classes(pool, out_sizes, max_count)`**：查询当前 Slab 类配置。
- **`mp_preferred_size_for_pool(pool, size)`**：基于池的自定义 Slab 表计算最佳对齐。

### 21. 📝 结构化事件日志 & pprof 导出
- **`mp_event_log_create(capacity)`** / **`mp_event_log_record(...)`** / **`mp_event_log_consume(...)`**：基于无锁环形缓冲区的结构化事件日志，用于事后重放。
- **`mp_export_pprof(pool, buf, max_len)`**：导出 pprof 兼容的文本格式以生成火焰图。

### 22. 🚀 每 CPU 无锁空闲列表
- **`MP_FLAG_PERCPU_FREELIST`**：启用每 CPU 无锁空闲列表，用于小对象分配以减少锁争用。
- **`mp_set_percpu_freelist(pool, enable)`** / **`mp_get_percpu_freelist(pool)`** / **`mp_get_percpu_cpu_count(pool)`**：配置和查询每 CPU 空闲列表。

### 23. 🛡️ 内存错误恢复
- **`mp_mark_pool_dirty(pool)`** / **`mp_clear_pool_dirty(pool)`** / **`mp_is_pool_dirty(pool)`**：标记/清除/查询脏池状态。金丝雀损坏或重复释放后新分配将被拒绝。
- **`mp_set_error_recovery_callback(pool, cb, udata)`**：注册内存错误恢复回调。
- **`mp_isolate_bad_block(pool, ptr)`**：通过从活动跟踪中移除来隔离错误块。

### 24. 🎯 线程级配额与熔断器
- **`mp_set_thread_quota(pool, quota_bytes)`**：设置每线程内存配额，防止单个线程耗尽池。
- **`mp_set_circuit_breaker(pool, enable)`** / **`mp_is_circuit_breaker_tripped(pool)`**：启用/查询线程级熔断器。
- **`mp_get_thread_allocated_bytes(pool)`** / **`mp_reset_thread_quota(pool)`**：查询/重置当前线程已分配字节数。

### 25. 🧊 热/冷页分离
- **`MP_FLAG_HOT_COLD_SEPARATION`**：启用热/冷页物理分离以提高 TLB 命中率。
- **`mp_mark_page_hot(pool, page_raw_mem)`** / **`mp_mark_page_cold(pool, page_raw_mem)`**：标记页温度属性。
- **`mp_get_hot_page_count(pool)`** / **`mp_get_cold_page_count(pool)`**：查询热/冷页数量。
- **`mp_separate_hot_cold_pages(pool)`**：执行热/冷页物理分离。

### 26. 🔒 加密内存支持
- **`MP_FLAG_ENCRYPTED_MEMORY`**：启用加密内存模式，带 `mlock` + `madvise(MADV_DONTDUMP)`。
- **`mp_lock_memory(pool, addr, length)`** / **`mp_unlock_memory(pool, addr, length)`**：锁定/解锁内存页以防止交换。
- **`mp_protect_from_dump(pool, addr, length)`**：将内存从核心转储中排除。
- **`mp_secure_zero(pool, ptr, length)`**：易失性安全清零以防止数据残留。
- **`mp_set_encrypted_memory(pool, enable)`**：一键加密内存模式。

### 27. 🛡️ AddressSanitizer 集成
- **`MP_FLAG_ASAN_INTEGRATION`**：启用 ASan 兼容模式。
- **`mp_asan_is_enabled()`**：检测 ASan 是否处于活动状态。
- **`mp_asan_report_error(pool, ptr, size, is_write)`**：向 ASan 报告自定义内存错误。
- **`mp_asan_check_memory(pool, ptr, size)`**：检查内存区域是否存在 ASan 错误。
- **`mp_set_asan_integration(pool, enable)`**：启用/禁用 ASan 集成。

### 28. 🚀 在线池扩展
- **`mp_expand_pool(pool, additional_bytes)`**：通过链接新的 TLSF 池，在不中断服务的情况下添加容量。
- **`mp_can_expand(pool)`**：检查池是否支持扩展。
- **`mp_get_expandable_size(pool)`**：查询剩余可扩展容量。

---

## 📦 头文件

| 语言 | 头文件 | 命名空间 / 标准 |
| :--- | :--- | :--- |
| **C** | `#include "cmem.h"` | C11 标准 C API |
| **C++ 包装器** | `#include "cmem.hpp"` | `cmem::` 命名空间 |
| **C++17 PMR** | `#include "cmem_pmr.hpp"` | `cmem::pmr_resource` (`std::pmr`) |
| **全局覆盖** | `#include "cmem_override.h"` | 重定向 `malloc`/`free` |

---

## 💻 快速开始

### C 示例 (内省与回收)

```c
#include "cmem.h"
#include <stdio.h>
#include <assert.h>

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEBUG_CANARY | MP_FLAG_ZERO_ON_ALLOC);
    mp_set_name(pool, "MyAppRootPool");

    void* ptr = mp_alloc(pool, 120);
    printf("Is pointer valid: %s\n", mp_ptr_valid(pool, ptr) ? "true" : "false");
    printf("Requested size: %zu bytes, Usable size: %zu bytes\n",
           mp_alloc_size(pool, ptr), mp_usable_size(pool, ptr));

    char* str = mp_strdup(pool, "Hello cmem memory manager!");
    char* formatted = mp_asprintf(pool, "Arena [%s] active allocations: %zu", mp_get_name(pool), (size_t)2);
    printf("%s\n", formatted);

    mp_free(pool, ptr);
    size_t reclaimed = mp_trim(pool, 0);
    printf("Reclaimed %zu bytes back to OS\n", reclaimed);

    mp_destroy(pool);
    return 0;
}
```

### C++17 PMR 多态容器

```cpp
#include "cmem.hpp"
#include "cmem_pmr.hpp"
#include <vector>
#include <string>
#include <iostream>

int main() {
    cmem::MemoryPool pool(1024 * 1024, MP_FLAG_THREAD_SAFE);
    cmem::pmr_resource res(pool.get());

    std::pmr::vector<std::pmr::string> vec(&res);
    vec.push_back(std::pmr::string("Polymorphic", &res));
    vec.push_back(std::pmr::string("Memory Resource", &res));
    vec.push_back(std::pmr::string("Integration", &res));

    for (const auto& str : vec) {
        std::cout << str << " ";
    }
    std::cout << std::endl;

    return 0;
}
```

### C++ STL 兼容分配器

```cpp
#include "cmem.hpp"
#include <vector>
#include <unordered_map>
#include <string>
#include <cassert>

int main() {
    cmem::MemoryPool pool(1024 * 1024, MP_FLAG_THREAD_SAFE);

    cmem::allocator<int> vec_alloc(pool);
    std::vector<int, cmem::allocator<int>> vec(vec_alloc);
    for (int i = 0; i < 100; i++) vec.push_back(i * 10);
    assert(vec[50] == 500);

    using MapAlloc = cmem::allocator<std::pair<const int, std::string>>;
    std::unordered_map<int, std::string, std::hash<int>, std::equal_to<int>, MapAlloc> map(10, std::hash<int>(), std::equal_to<int>(), MapAlloc(pool));
    map[1] = "cmem";
    map[2] = "High-Performance";
    assert(map[1] == "cmem");

    pool.check_leaks();
    return 0;
}
```

### 高级：帧 Arena (游戏/图形流水线)

```c
#include "cmem.h"
#include <stdio.h>
#include <string.h>

int main() {
    cmem_frame_arena_t* farena = mp_frame_arena_create(512 * 1024);

    void* frame1_ptr = mp_frame_alloc(farena, 1024);
    strcpy((char*)frame1_ptr, "RenderMeshFrame1");
    mp_frame_end(farena);

    void* frame2_ptr = mp_frame_alloc(farena, 2048);
    strcpy((char*)frame2_ptr, "RenderMeshFrame2");
    mp_frame_end(farena);

    mp_frame_arena_destroy(farena);
    printf("Frame Arena Ping-Pong completed!\n");
    return 0;
}
```

### 高级：类型化对象池 (零开销)

```c
#include "cmem.h"
#include <stdio.h>
#include <assert.h>

typedef struct { int id; char name[32]; double value; } game_entity_t;

int main() {
    mp_typed_pool_t* tpool = mp_typed_pool_create(sizeof(game_entity_t), 128);

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

### 高级：无锁环形缓冲区

```c
#include "cmem.h"
#include <stdio.h>
#include <string.h>

int main() {
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

### 高级：共享内存 IPC

```c
#include "cmem.h"
#include <stdio.h>
#include <string.h>

int main() {
    memory_pool_t* pool = mp_create_shared("/my_ipc_pool", 512 * 1024, MP_FLAG_THREAD_SAFE);
    void* p = mp_alloc(pool, 1024);
    strcpy((char*)p, "Zero-Copy IPC Message from Process A");

    // Process B can open the same shared memory pool
    // memory_pool_t* pool_b = mp_create_shared("/my_ipc_pool", 512 * 1024, MP_FLAG_THREAD_SAFE);
    // printf("Process B reads: %s\n", (char*)p);

    mp_free(pool, p);
    mp_destroy_shared(pool, "/my_ipc_pool");
    return 0;
}
```

### 高级：泄漏分析与 HTML 报告

```c
#include "cmem.h"
#include <stdio.h>

void leaky_function(memory_pool_t* pool) {
    mp_alloc_loc(pool, 256, __FILE__, __LINE__, __func__); // intentional leak
    void* p = mp_alloc_loc(pool, 128, __FILE__, __LINE__, __func__);
    mp_free(pool, p);
}

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_TRACK_LOCATIONS | MP_FLAG_DEBUG_CANARY);
    leaky_function(pool);

    bool healthy = mp_audit_heap(pool);
    printf("Heap Audit: %s\n", healthy ? "HEALTHY" : "CORRUPTED");

    char report[8192];
    mp_analyze_leaks(pool, report, sizeof(report));
    printf("%s\n", report);

    mp_export_html_report(pool, "memory_profile.html");
    mp_export_binary_snapshot(pool, "snapshot_before.cmem_dump");

    mp_destroy(pool);
    return 0;
}
```

### 高级：Prometheus 指标导出

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

    mp_destroy(pool);
    return 0;
}
```

### 高级：环境变量自动调优

```bash
# Enable security features without code changes
export CMEM_CONF="canary=1,poison=on,aligned=1,guard=1"
./your_application
```

```c
#include "cmem.h"

int main() {
    mp_flags_t flags = mp_parse_env_flags(MP_FLAG_DEFAULT);
    memory_pool_t* pool = mp_create(0, flags);
    // pool now has Canary + Poison + Cache Aligned + Guard Pages
    return 0;
}
```

### 高级：运行时配置热重载

```c
#include "cmem.h"

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);

    // Later: hot-reload config from environment
    mp_flags_t new_flags = mp_reparse_env_flags(pool);
    printf("Config generation: %llu\n", (unsigned long long)mp_get_env_generation(pool));

    mp_destroy(pool);
    return 0;
}
```

### 高级：优雅降级 (OOM 回退)

```c
#include "cmem.h"

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);
    mp_set_memory_limit(pool, 1024); // very small limit

    // Enable fallback to system malloc on OOM
    mp_set_fallback_on_oom(pool, true);

    // Register GC callback to free non-critical cache before OOM rejection
    mp_set_gc_callback(pool, [](memory_pool_t* p, bool is_high, size_t current, size_t limit, void* ud) {
        printf("GC callback: freeing non-critical cache\n");
        free_non_critical_cache();
    }, NULL);

    void* p = mp_alloc(pool, 2048); // triggers fallback
    if (p) {
        printf("Fallback allocation succeeded\n");
        mp_free(pool, p);
    }

    mp_destroy(pool);
    return 0;
}
```

### 高级：内存错误恢复

```c
#include "cmem.h"

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEBUG_CANARY);

    // Register error recovery callback
    mp_set_error_recovery_callback(pool, [](memory_pool_t* p, bool is_high, size_t current, size_t limit, void* ud) {
        printf("Memory error recovery triggered\n");
        // Isolate bad blocks, mark pool dirty, etc.
    }, NULL);

    // If canary corruption or double free is detected, pool is marked dirty
    // New allocations will be rejected until recovery

    mp_destroy(pool);
    return 0;
}
```

### 高级：线程配额与熔断器

```c
#include "cmem.h"

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_THREAD_SAFE);

    // Set per-thread quota: 8MB per thread
    mp_set_thread_quota(pool, 8 * 1024 * 1024);

    // Enable circuit breaker
    mp_set_circuit_breaker(pool, true);

    // Allocate from multiple threads...
    // When a thread exceeds its quota, circuit breaker trips and rejects further allocations

    mp_destroy(pool);
    return 0;
}
```

### 高级：热/冷页分离

```c
#include "cmem.h"

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_HOT_COLD_SEPARATION);

    // Mark frequently accessed pages as hot
    // Mark rarely accessed pages as cold
    // Separate hot and cold pages into distinct memory regions

    printf("Hot pages: %zu\n", mp_get_hot_page_count(pool));
    printf("Cold pages: %zu\n", mp_get_cold_page_count(pool));

    mp_separate_hot_cold_pages(pool);

    mp_destroy(pool);
    return 0;
}
```

### 高级：加密内存

```c
#include "cmem.h"

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_ENCRYPTED_MEMORY);

    // Lock sensitive data in RAM
    mp_lock_memory(pool, ptr, len);

    // Protect from core dumps
    mp_protect_from_dump(pool, ptr, len);

    // Secure zero when done
    mp_secure_zero(pool, ptr, len);

    mp_unlock_memory(pool, ptr, len);

    mp_destroy(pool);
    return 0;
}
```

---

## 🚀 构建与测试

```bash
# 构建静态库并运行 C 单元测试 (带 ASan/UBSan)
make test

# 构建并运行 C++17 PMR 和 STL 分配器测试
make test_cpp

# 构建并运行性能基准测试
make bench

# 构建并运行所有示例
make examples

# CMake 构建
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

---

## 📊 性能基准测试

| 场景 | 系统 Malloc | cmem | 加速比 |
| :--- | :--- | :--- | :--- |
| 小对象 (32-256B x 1M ops) | ~450 Mops/sec | ~1,800 Mops/sec | **~4.0x** |
| 中等对象 (1KB-64KB x 100K ops) | ~180 Mops/sec | ~420 Mops/sec | **~2.3x** |
| Arena 重置 (500 allocs x 1000 rounds) | 12.4 ms | 0.8 ms | **~15x** |
| 多线程 (8 threads x 100K allocs) | 高争用 | 近线性扩展 | **~8x** |

---

## 📁 项目结构

```
cmem/
├── include/
│   ├── cmem.h              # C11 公共 API
│   ├── cmem.hpp            # C++11 RAII 包装器 + STL 分配器
│   ├── cmem_pmr.hpp        # C++17 std::pmr::memory_resource 适配器
│   └── cmem_override.h     # 全局 malloc/free 符号拦截
├── src/
│   └── cmem.c              # 核心实现
├── tests/
│   ├── test_main.c         # C 单元测试 (35+ 测试用例)
│   └── test_cpp.cpp        # C++ PMR + STL 分配器测试
├── benchmarks/
│   └── bench_main.c        # 性能基准测试
├── examples/
│   ├── example_basic.c
│   ├── example_embedded.c
│   ├── example_leak_analysis.c
│   └── example_arena_tree.c
├── docs/
│   ├── index.md            # 文档索引
│   ├── en/                 # 英文文档
│   └── zh/                 # 中文文档
├── CMakeLists.txt
├── Makefile
├── LICENSE
└── README.md
```

---

## 🔒 ABI 稳定性与版本管理

### ABI 版本

当前 ABI 版本为 `1`。可通过 `mp_abi_version()` 在运行时查询。

### 稳定性承诺

| 变更类型 | 是否破坏 ABI | 语义版本 |
| :--- | :--- | :--- |
| Bug 修复（无 API 变更） | 否 | Patch |
| 新增 API（向后兼容） | 否 | Minor |
| 新增 API（向后不兼容） | 是 | Major |
| 内部重构 | 否 | Patch |
| 结构体布局变更 | 是 | Major |
| Flag 枚举值变更 | 是 | Major |

### 兼容性规则

- 新字段仅追加到 public struct 的末尾。
- `mp_flags_t` 枚举值遵循 append-only 原则；禁止复用或重新编号现有值。
- 旧版客户端可通过 `mp_abi_version()` 检测新版本并优雅降级。

---

## 🖥️ 平台支持

| 平台 | 状态 | 备注 |
| :--- | :--- | :--- |
| **Linux (glibc)** | ✅ 完整 | NUMA、HugePages、共享内存、madvise、mlock |
| **Linux (musl)** | ⚠️ 部分 | 基础分配器可用；NUMA/HugePages 可能需移植 |
| **macOS** | ⚠️ 部分 | 无 NUMA/HugePages/共享内存；madvise 受限 |
| **Windows (MSVC)** | ⚠️ 部分 | 需将 mmap/madvise 移植到 VirtualAlloc |
| **FreeBSD** | ⚠️ 基础 | 可能只需少量 #ifdef 调整即可运行 |
| **Android** | ⚠️ 基础 | Bionic libc；生产前需测试 |

### 编译器支持

| 编译器 | 最低版本 | 推荐版本 |
| :--- | :--- | :--- |
| GCC | 7.0 | 13.0+ |
| Clang | 6.0 | 17.0+ |
| MSVC | 2017 | 2022+ |

---

## 📄 许可证

MIT License。详见 [LICENSE](LICENSE) 文件。

---

**cmem** — 让内存管理更简单、更安全、更快速。 🚀
