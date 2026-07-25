# cmem - Universal High-Performance C Memory Manager

**cmem** 是一个基于 **C11 / C++11** 设计实现的通用、高性能分层内存池管理工具（Universal High-Performance Tiered Memory Manager）。

---

## 🌟 核心架构设计 (Architecture)

**cmem** 采用现代高性能 Allocator（如 Slab + TLSF）的**三层混合分层架构**，在保证 $O(1)$ 时间复杂度的同时极大地减少内存碎片化，提升缓存局部性（L1/L2 Cache Locality）。

```
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

## 📦 头文件包含 (Headers)

- **C 项目**：包含 `#include "cmem.h"`
- **C++ 项目**：包含 `#include "cmem.hpp"` (命名空间 `cmem::`)

---

## ⚡ 高吞吐批量分配与内存紧凑归还 (Batch Alloc & Compaction)

- **批量分配与释放 (`mp_alloc_batch` / `mp_free_batch`)**：单次函数调用即可批量获取/归还 $N$ 个内存块，大幅降低锁竞争与函数调用开销。
- **内存紧凑与 OS 页归还 (`mp_compact`)**：遍历内存池，主动释放未使用的空闲 Slab 页面归还给操作系统内核，降低长期运行进程的 Resident Set Size (RSS)。

---

## 🌳 树状分层 Memory Arena 架构 (Hierarchical Arena Trees)

针对复杂系统（如编译器 AST、游戏引擎场景树、Web 服务器 HTTP Request 作用域），支持**父子嵌套内存池（Parent-Child Arenas）**：

- **父子关联 (`mp_create_child`)**：可创建挂载于父 Memory Pool 下的命名子 Arena（如 `SceneArena`、`ASTArena`）。
- **级联销毁/重置**：销毁父 Pool 时自动递归销毁旗下所有子 Arena；重置父 Pool 时自动递归重置所有子 Context。
- **树状结构Dump (`mp_dump_tree_info`)**：层级化打印内存池树状形态及各自的内存占用。

---

## 🎨 交互式 Visual HTML 诊断大屏 (HTML Visual Dashboard)

除了控制台 Dump 与 JSON 数据导出外，提供 **交互式单文件 HTML 可视化诊断大屏 ([mp_export_html_report](file:///home/quintin/Data/source/c_cpp/memory_pool/include/cmem.h#L169))**：

- **数据卡片**：总系统内存、活动 Payload、活动分配块数、碎片率%。
- **色彩分布条**：Slab / TLSF / Direct OS 动态分配比例条。
- **泄露与活动内存表格**：高亮显示内存地址、Tier 标签、源码代码行 (`file:line`) 与函数名。

---

## 🔍 内存泄漏与异常诊断引擎 (Diagnostics Engine)

- **代码源头追踪**：开启 `MP_FLAG_TRACK_LOCATIONS` 标志后，自动记录内存申请的文件名 (`__FILE__`)、行号 (`__LINE__`) 及函数名 (`__func__`)。
- **调用栈捕获**：基于 `backtrace()` 自动捕获内存分配时的函数调用栈 Symbol 帧。
- **详细内存泄漏分析报告导出 ([mp_analyze_leaks](file:///home/quintin/Data/source/c_cpp/memory_pool/include/cmem.h#L155) & [mp_export_leak_report](file:///home/quintin/Data/source/c_cpp/memory_pool/include/cmem.h#L164))**。
- **堆完整性主动审计 (`mp_audit_heap`)**：校验 Redzone 金丝雀 (`MP_CANARY_BYTE`)。
- **释放后内存毒化 (UAF Protection via Poisoning)**：`MP_FLAG_POISON_ON_FREE` 填充 `0xDD` 数据。

---

## 🛠️ 核心 API 一览 (API Index)

| 函数 / 类 API | 说明 |
| :--- | :--- |
| `mp_create(initial_cap, flags)` | 创建并初始化 **cmem** 内存池实例 |
| `mp_create_child(parent, cap, flags, name)` | 创建挂载于父内存池下的子 Arena |
| `mp_create_from_buffer(buffer, size, flags)` | 在静态内存缓冲区中创建内存池（零 OS `malloc`） |
| `mp_alloc_batch(pool, size, out_ptrs, count)` | 高吞吐单次批量分配 $N$ 个内存块 |
| `mp_free_batch(pool, ptrs, count)` | 单次批量释放 $N$ 个内存块 |
| `mp_compact(pool)` | 紧凑内存池并释放未使用 Slab 空闲页归还系统 OS |
| `mp_destroy(pool)` | 销毁内存池并递归销毁所有子 Context |
| `mp_reset(pool)` | $O(1)$ 批量重置内存池及所有子 Context |
| `mp_export_html_report(pool, filepath)` | 导出交互式可视化 HTML 剖析与泄漏大屏报告 |
| `mp_dump_tree_info(pool)` | 打印层级化子 Arena 树状分布及内存占用 |
| `mp_audit_heap(pool)` | 主动遍历堆内存，检测 Redzone Canary 越界踩内存 |
| `mp_analyze_leaks(pool, buf, max_len)` | 生成结构化内存泄漏分析报告（含文件行号与调用栈） |
| `cmem::MemoryPool` | C++ RAII 内存池包装类 |
| `cmem::allocator<T>` | 兼容 STL 容器（`std::vector` 等）的 C++ 分配器 |

---

## 🚀 编译与运行 (Build & Test)

```bash
# 1. 编译静态库 libcmem.a 并运行 C 单元测试 (含 ASan / UBSan 检查)
make test

# 2. 编译并运行 C++ STL 容器集成测试
make test_cpp

# 3. 编译并运行性能 Benchmark 压测
make bench

# 4. 编译并运行示例程序 (基础剖析 / 静态 Buffer / 泄漏分析报告 / 树状 Arena)
make examples

# 5. 编译与清理
make all
make clean
```
