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

### 1. 第一级：Slab Allocator (小对象 $\le 512$ Bytes)
- **规格分级**：8B, 16B, 32B, 64B, 128B, 256B, 512B 共 7 个 Size Class。
- **页面对齐**：按 16KB 页对齐分配 Slab Page，内部 Slot 以链表组织。
- **性能**：$O(1)$ 分配与释放，消除小对象的外部碎片，高 CPU 缓存命中率。

### 2. 第二级：TLSF Allocator (中等对象 512 Bytes ~ 4MB)
- **算法原理**：Two-Level Segregated Fit (双层隔离适合算法)。
- **硬件加速**：利用 CPU 位扫描指令 `__builtin_clz` (Count Leading Zeros) / `__builtin_ctz` 进行 2 级 Bitmask 快速查找。
- **碎片整理**：利用 Boundary Tags (边界标记)，在 `mp_free` 时 $O(1)$ 实时与前后物理相邻的空闲块合并（Immediate Coalescing）。
- **动态扩容**：支持多 Sub-Pool 链式扩展。

### 3. 第三级：Direct OS Allocator (大对象 > 4MB)
- **回退机制**：针对特大内存请求，直接回退至 OS 系统分配，统一纳入内存池生命周期与统计追踪。

---

## ✨ 核心高级特性 (Features)

1. **静态内存缓冲区模式 (Static Buffer Arena)**：通过 [mp_create_from_buffer](file:///home/quintin/Data/source/c_cpp/memory_pool/include/memory_pool.h#L97) 直接在预先分配的静态字节数组中初始化内存池，零 OS `malloc` 依赖，完美契合嵌入式/裸机（Bare-metal）环境。
2. **自定义系统分配器注入 (Custom Backing Allocator)**：通过 [mp_create_custom](file:///home/quintin/Data/source/c_cpp/memory_pool/include/memory_pool.h#L88) 注入共享内存（`/dev/shm`）、大页内存（HugePages）或自定义 Virtual Memory 虚表。
3. **实时 Event 剖析回调 (`mp_set_event_callback`)**：注册内存分配/释放/溢出事件回调函数，支持对接 Profiler 或分布式链路追踪（Tracy / Valgrind）。
4. **线程本地缓存 (Thread-Local Cache)**：开启 `MP_FLAG_THREAD_LOCAL_CACHE` 标志后，每个线程拥有独立无锁的 Slab 槽位缓存，小对象分配彻底无锁化。
5. **Arena 批量快速重置 (`mp_reset`)**：针对 Request/Frame 作用域，提供 $O(1)$ 批量清空恢复能力，无需逐个 `free`。
6. **C++11 RAII 与 STL Allocator 支持**：提供 [include/memory_pool.hpp](file:///home/quintin/Data/source/c_cpp/memory_pool/include/memory_pool.hpp)，无缝对接 `std::vector` 与 `std::unordered_map`。
7. **JSON 监控导出 (`mp_dump_json_stats`)**：导出 JSON 格式的指标数据，方便接入 Prometheus/Grafana 等监控系统。
8. **内存对齐与防越界 (Canary Redzone)**：支持 `mp_aligned_alloc` 与 Canary 溢出标记。

---

## 🛠️ 公共 API 说明 (Public API)

C 头文件：[include/memory_pool.h](file:///home/quintin/Data/source/c_cpp/memory_pool/include/memory_pool.h)  
C++ 头文件：[include/memory_pool.hpp](file:///home/quintin/Data/source/c_cpp/memory_pool/include/memory_pool.hpp)

| 函数 / 类 API | 说明 |
| :--- | :--- |
| `mp_create(initial_cap, flags)` | 创建并初始化内存池实例 |
| `mp_create_custom(initial_cap, flags, sys_alloc)` | 使用自定义底层系统分配器创建内存池 |
| `mp_create_from_buffer(buffer, size, flags)` | 在静态内存缓冲区中创建内存池（零 OS `malloc`） |
| `mp_destroy(pool)` | 销毁内存池并释放归还所有系统资源 |
| `mp_reset(pool)` | $O(1)$ 批量重置内存池，清空活动分配并保留已申请系统页 |
| `mp_set_event_callback(pool, cb, user_data)` | 注册分配/释放/溢出剖析事件回调 |
| `mp_alloc(pool, size)` | 从内存池分配 `size` 字节内存 |
| `mp_calloc(pool, num, size)` | 分配内存并自动清零 |
| `mp_realloc(pool, ptr, new_size)`| 重新调整内存块大小 |
| `mp_aligned_alloc(pool, align, size)` | 分配指定边界对齐的内存 |
| `mp_free(pool, ptr)` | 将内存块释放归还至内存池 |
| `mp_get_stats(pool, &stats)` | 获取当前内存池统计数据与碎片率 |
| `mp_dump_json_stats(pool, buf, len)` | 导出 JSON 格式监控指标 |
| `mp_check_leaks(pool)` | 检查是否存在未释放的内存泄漏 |
| `mpool::MemoryPool` | C++ RAII 内存池包装类 |
| `mpool::allocator<T>` | 兼容 STL 容器（`std::vector` 等）的 C++ 分配器 |

---

## 🚀 编译与运行 (Build & Test)

### 使用 Makefile

```bash
# 1. 编译并运行 C 单元测试 (含 ASan / UBSan 检查)
make test

# 2. 编译并运行 C++ STL 容器集成测试
make test_cpp

# 3. 编译并运行性能 Benchmark 压测
make bench

# 4. 编译并运行示例程序 (Basic Profiler & Static Embedded Demo)
make examples

# 5. 运行全部目标
make all
```

---

## 📊 性能测试结果 (Benchmarks)

在 Linux x86_64 环境下（使用 GCC `-O3` 优化）：

- **小对象分配 (32B ~ 256B x 1,000,000 次操作)**：
  - 系统 `malloc`/`free`：~0.461 秒 (4.33 Mops/sec)
  - Memory Pool (Slab + TLS Cache)：~0.404 秒 (4.95 Mops/sec) —— **提升 ~1.14x**
- **中型动态分配 (1KB - 64KB x 100,000 次操作)**：
  - 系统 `malloc`/`free`：~0.587 秒
  - Memory Pool (TLSF)：~0.484 秒 —— **提升 ~1.21x**
- **Arena 批量重置 (`mp_reset` x 1000 轮操作)**：
  - 逐个 `mp_free` 循环：~0.0339 秒
  - Arena `mp_reset` 批量重置：~0.0223 秒 —— **提升 ~1.52x**
- **内存泄漏与异常校验**：所有测试目标在开启 AddressSanitizer / UndefinedBehaviorSanitizer 校验下通过 **100% 零泄漏与零异常**。
