# cmem - Universal High-Performance Tiered Memory Manager

**cmem** 是一个基于 **C11 / C++17** 设计实现的通用、高性能分层内存池管理工具（Universal High-Performance Tiered Memory Manager）。

它融合了工业级内存分配器（如 **Slab** 与 **TLSF**）的核心优点，具备全方位的内存诊断、内省查询、级联树状 Arena 组织、跨平台 OS 页回收以及 C++17 PMR 容器适配能力。

---

## 🌟 核心架构设计 (Architecture)

**cmem** 采用现代高性能 Allocator 的**三层混合分层架构**，在保证 $O(1)$ 分配时间复杂度的同时极大减少内存碎片化，并显著提升 L1/L2 Cache 局部性：

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

### 3. ♻️ 内存紧凑与 OS 页回收 (Trim & RSS Purging)
- **`mp_trim`**：深层物理页紧凑与回收，支持保留缓冲 padding，主动清理池内完全空闲的 Slab 页面。
- **`mp_madvise`**：跨平台 `madvise` 包装函数，Linux 下调用 `madvise(MADV_DONTNEED)`，Windows 下自动适配为 `VirtualAlloc(MEM_RESET)`，主动归还物理 RSS 给操作系统。
- **`mp_compact` / `mp_purge_lazy`**：内存紧凑与物理页延迟清理。

### 4. 🔒 高并发锁机制 (RWLock & Fine-Grained Locks)
- **读写锁 (`pthread_rwlock_t`)**：所有内省查询与指标统计 API（如 `mp_usable_size`、`mp_pressure`、`mp_get_stats`）均使用**读锁**，多线程并发 profiling 时零锁争用。
- **细粒度 Slab Class 锁**：每个 Slab 尺寸 Class（8B/16B/32B/64B/128B/256B/512B）拥有独立的 `pthread_mutex_t` 锁。不同尺寸的小对象分配/释放并发执行，锁争用降低至最小。

### 5. 🚀 C++17 PMR 与 STL 容器适配器 (C++17 PMR Support)
- 头文件包含 `#include "cmem_pmr.hpp"`。
- 提供继承自 `std::pmr::memory_resource` 的 `cmem::pmr_resource` 适配器，完美兼容 C++17 原生多态容器（如 `std::pmr::vector`、`std::pmr::string`、`std::pmr::map` 等）。
- 亦提供标准 C++11 RAII 包装类 `cmem::MemoryPool` 与 C++ STL 分配器 `cmem::allocator<T>`。

### 6. 🛡️ 安全强化与溢出防护 (Safety & Security)
- **`mp_reallocarray`**：溢出安全重新分配数组，显式校验 `nmemb * size > SIZE_MAX` 乘法溢出。
- **Redzone Canary 金丝雀 (`MP_FLAG_DEBUG_CANARY`)**：末端填充 `0xAB`，实时检测写越界踩内存。
- **UAF 释放毒化 (`MP_FLAG_POISON_ON_FREE`)**：释放内存填充 `0xDD` 毒化字节。
- **页级 Guard Pages (`MP_FLAG_GUARD_PAGES`)**：利用 `PROT_NONE` 在页首页尾阻断野指针越界。

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
- **崩溃快照 Dump 与 Diff 泄漏检测 (`mp_export_binary_snapshot` / `mp_diff_snapshots`)**。

---

## 📦 头文件包含 (Headers)

| 语言环境 | 包含头文件 | 命名空间 / 语言规范 |
| :--- | :--- | :--- |
| **C 语言** | `#include "cmem.h"` | C11 标准 C 接口 |
| **C++ 基础封装** | `#include "cmem.hpp"` | `cmem::` 命名空间 |
| **C++17 PMR 适配** | `#include "cmem_pmr.hpp"` | `cmem::pmr_resource` (`std::pmr`) |

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
| `mp_set_name(pool, name)` | 设置内存池的易读名称 |
| `mp_get_name(pool)` | 获取内存池的易读名称 |
| `mp_get_parent(pool)` | 获取父内存池指针（若为子 Arena） |
| `mp_get_child_count(pool)` | 获取当前池直接关联的子 Arena 数量 |
| `mp_preferred_size(size)` | 计算最佳匹配的 Size Class 对齐尺寸 |

### 4. 便利辅助函数 (Convenience APIs)

| API 接口 | 功能说明 |
| :--- | :--- |
| `mp_strdup(pool, str)` | 在内存池中深拷贝复制以 `\0` 结尾的字符串 |
| `mp_memdup(pool, src, n)` | 在内存池中深拷贝复制 $N$ 字节二进制数据 |
| `mp_asprintf(pool, fmt, ...)` | 格式化字符串并在内存池中分配存储空间 |

### 5. 内存紧凑与回收 (Trim & Purging)

| API 接口 | 功能说明 |
| :--- | :--- |
| `mp_trim(pool, pad)` | 深层内存紧凑与页归还，回收未使用的物理页并保留 pad 字节 |
| `mp_madvise(pool, addr, len, advice)` | 跨平台内存 Advice 包装（Linux `madvise` / Win `VirtualAlloc`） |
| `mp_compact(pool)` | 紧凑内存池并释放空闲 Slab 页面 |
| `mp_purge_lazy(pool)` | 延迟物理内存页 RSS 清理 |

### 6. 统计、诊断与监控

| API 接口 | 功能说明 |
| :--- | :--- |
| `mp_get_stats(pool, stats)` | 获取包含 QPS、带宽、分配阶分布的统计快照 |
| `mp_pressure(pool)` | 获取内存池使用压力比例 $[0.0, 1.0]$ |
| `mp_freeable(pool)` | 获取当前池内可被 `mp_trim` 回收的空闲页字节数 |
| `mp_resident(pool)` | 获取当前内存池系统 RSS 物理驻留字节数 |
| `mp_reset_stats(pool)` | 重置累积 QPS、操作计数与 Peak 峰值统计数据 |
| `mp_export_html_report(pool, path)` | 导出交互式可视化 HTML 剖析与泄漏大屏报告 |
| `mp_export_prometheus_metrics(pool, buf, len)` | 导出 Prometheus / OpenTelemetry 标准格式指标 |
| `mp_audit_heap(pool)` | 遍历堆内存，主动审计 Redzone Canary 越界踩内存 |
| `mp_analyze_leaks(pool, buf, len)` | 生成包含代码位置 (`file:line`) 与调用栈的泄漏报告 |
| `mp_diff_snapshots(snap_a, snap_b, buf, len)` | 对比两次内存快照并生成增量泄漏差异报告 |

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

# 6. 清理构建产物
make clean
```

---

## 📄 开源许可证 (License)

本项目基于 [MIT License](LICENSE) 开源发布。
