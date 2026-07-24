# Universal High-Performance C Memory Pool Manager

一个基于 **C11/C++11** 设计实现的通用、高性能分层内存池管理工具（High-Performance Tiered Memory Manager）。

---

## 🌟 核心架构设计 (Architecture)

本内存管理工具采用现代高性能 Allocator（如 Slab + TLSF）的**三层混合分层架构**，在保证 $O(1)$ 时间复杂度的同时极大地减少内存碎片化，提升缓存局部性（L1/L2 Cache Locality）。

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

## 🔍 内存泄漏与异常诊断引擎 (Diagnostics Engine)

专门针对 C/C++ 常见内存问题（内存泄漏、缓冲区溢出、野指针/Use-After-Free、重复释放）设计了完善的诊断引擎：

### 1. 泄漏源头追溯与调用栈记录 (Location & Callstack Tracking)
- **代码源头追踪**：开启 `MP_FLAG_TRACK_LOCATIONS` 标志后，自动记录内存申请的文件名 (`__FILE__`)、行号 (`__LINE__`) 及函数名 (`__func__`)。
- **调用栈捕获**：基于 `backtrace()` 自动捕获内存分配时的函数调用栈 Symbol 帧。

### 2. 详细泄露分析报告生成与导出 (`mp_analyze_leaks` & `mp_export_leak_report`)
- 自动生成格式化的结构化报告，精确指示泄露块的内存地址、字节大小、所属分配层级（Slab/TLSF/OS）、源码代码行及调用栈。
- 支持直接导出为文本报告文件 (`leak_report.txt`)。

### 3. 堆完整性主动审计 (`mp_audit_heap`)
- 遍历所有活动内存块，主动检查 Header 幻数及 Redzone 金丝雀 (`MP_CANARY_BYTE`) 校验码，实时检测缓冲区上溢/踩内存问题。

### 4. 释放后内存毒化 (UAF Protection via Poisoning)
- 开启 `MP_FLAG_POISON_ON_FREE` 标志后，释放内存时自动将 Payload 填充为 `0xDD` 毒化模式，防止释放后读写（Use-After-Free）破坏逻辑。

---

## ✨ 核心高级特性 (Features)

1. **静态内存缓冲区模式 (Static Buffer Arena)**：通过 [mp_create_from_buffer](file:///home/quintin/Data/source/c_cpp/memory_pool/include/memory_pool.h#L97) 直接在预先分配的静态字节数组中初始化内存池，零 OS `malloc` 依赖。
2. **自定义系统分配器注入 (Custom Backing Allocator)**：通过 [mp_create_custom](file:///home/quintin/Data/source/c_cpp/memory_pool/include/memory_pool.h#L88) 注入共享内存（`/dev/shm`）、大页内存（HugePages）或自定义虚表。
3. **实时 Profiler 事件回调 (`mp_set_event_callback`)**：捕获 `ALLOC`、`FREE`、`REALLOC`、`DOUBLE_FREE`、`CANARY_CORRUPTION` 事件。
4. **线程本地缓存 (Thread-Local Cache)**：开启 `MP_FLAG_THREAD_LOCAL_CACHE` 后小对象分配无锁化。
5. **Arena 批量快速重置 (`mp_reset`)**：$O(1)$ 批量清空恢复能力。
6. **C++11 RAII 与 STL Allocator 支持**：提供 [include/memory_pool.hpp](file:///home/quintin/Data/source/c_cpp/memory_pool/include/memory_pool.hpp)。
7. **JSON 监控导出 (`mp_dump_json_stats`)**：导出 JSON 格式监控数据。

---

## 🛠️ 诊断与泄漏分析 API (Diagnostics API)

| 函数 / 类 API | 说明 |
| :--- | :--- |
| `mp_alloc_loc(pool, size, file, line, func)` | 带代码位置追踪的内存分配接口 |
| `mp_audit_heap(pool)` | 主动遍历堆内存，检测 Redzone Canary 越界踩内存 |
| `mp_analyze_leaks(pool, buf, max_len)` | 生成结构化内存泄漏分析报告（含文件行号与调用栈） |
| `mp_export_leak_report(pool, filepath)` | 将内存泄漏分析报告导出至文本文件 |
| `mp_check_leaks(pool)` | 运行时检查是否存在未释放内存泄漏 |

---

## 🚀 编译与运行 (Build & Test)

```bash
# 1. 编译并运行 C 单元测试 (含 ASan / UBSan 检查)
make test

# 2. 编译并运行 C++ STL 容器集成测试
make test_cpp

# 3. 编译并运行性能 Benchmark 压测
make bench

# 4. 编译并运行示例程序 (基础剖析 / 静态 Buffer / 泄漏分析报告导出)
make examples

# 5. 编译运行全部
make all
```

---

## 📊 泄漏分析报告输出示例 (Leak Report Example)

```
=================== DETAILED MEMORY LEAK ANALYSIS REPORT ===================
  Total Managed System Memory: 1085336 bytes (1059.90 KB)
  Active Leaked Allocations  : 1 blocks
  Total Leaked Payload Bytes : 128 bytes (0.12 KB)
============================================================================

[Leak #1] Address: 0x5620096dc0c0 | Payload Size: 128 bytes | Tier: SLAB
  Source Location : examples/example_leak_analysis.c:13 (function 'do_leaky_work')
  Callstack Frames:
    #0 ./build/example_leak_analysis(+0x348b) [0x56200790548b]
    #1 ./build/example_leak_analysis(+0x45d6) [0x5620079065d6]
    #2 ./build/example_leak_analysis(+0x11d9) [0x5620079031d9]
    #3 /usr/lib/libc.so.6(+0x27741) [0x7fa5cf027741]
```
