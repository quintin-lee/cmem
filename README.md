# Universal High-Performance C Memory Pool Manager

一个基于 **C11** 设计实现的通用、高性能分层内存池管理工具（High-Performance Tiered Memory Manager）。

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

## ✨ 核心特性 (Features)

1. **线程安全 (Thread Safety)**：可选支持细粒度 Mutex 互斥锁 (`MP_FLAG_THREAD_SAFE`)。
2. **内存对齐 (Aligned Allocation)**：支持 SIMD/AVX 要求的任意 2 的幂次对齐分配 (`mp_aligned_alloc`)。
3. **安全检测与防越界 (Canary Redzone)**：支持开启 Canary 溢出标记 (`MP_FLAG_DEBUG_CANARY`)，在释放时实时校验缓冲区溢出。
4. **内存泄漏检测 (Leak Detection)**：内置双向链表活动追踪，提供 `mp_check_leaks()` 准确指出泄露指针与大小。
5. **实时诊断与统计 (Diagnostics)**：提供 `mp_dump_info()` 打印内存分配快照、历史峰值与分布图。

---

## 🛠️ 公共 API 说明 (Public API)

头文件：[include/memory_pool.h](file:///home/quintin/Data/source/c_cpp/memory_pool/include/memory_pool.h)

| 函数 API | 说明 |
| :--- | :--- |
| `mp_create(initial_cap, flags)` | 创建并初始化内存池实例 |
| `mp_destroy(pool)` | 销毁内存池并释放归还所有系统资源 |
| `mp_alloc(pool, size)` | 从内存池分配 `size` 字节内存 |
| `mp_calloc(pool, num, size)` | 分配内存并自动清零 |
| `mp_realloc(pool, ptr, new_size)`| 重新调整内存块大小 |
| `mp_aligned_alloc(pool, align, size)` | 分配指定边界对齐的内存 |
| `mp_free(pool, ptr)` | 将内存块释放归还至内存池 |
| `mp_get_stats(pool, &stats)` | 获取当前内存池统计数据 |
| `mp_dump_info(pool)` | 打印内存池状态快照至 stdout |
| `mp_check_leaks(pool)` | 检查是否存在未释放的内存泄漏 |

---

## 🚀 编译与运行 (Build & Test)

### 使用 Makefile

```bash
# 1. 编译并运行单元测试 (含 ASan AddressSanitizer 校验)
make test

# 2. 编译并运行性能 Benchmark 压测
make bench

# 3. 清理构建产物
make clean
```

### 使用 CMake

```bash
mkdir build && cd build
cmake ..
make
./unit_tests
./benchmark
```

---

## 📊 性能测试结果 (Benchmarks)

在 Linux x86_64 环境下（使用 GCC `-O3` 优化）：

- **小对象分配 (32 - 256 Bytes x 1,000,000 次操作)**：
  - 系统 `malloc`/`free`：~0.404 秒 (4.95 Mops/sec)
  - Memory Pool：~0.368 秒 (5.42 Mops/sec) —— **提升 ~1.10x**
- **中型动态分配 (1KB - 64KB x 100,000 次操作)**：
  - 系统 `malloc`/`free`：~0.430 秒
  - Memory Pool (TLSF)：~0.375 秒 —— **提升 ~1.15x**
- **内存泄漏检测**：单元测试在开启 AddressSanitizer / UndefinedBehaviorSanitizer 下通过 **100% 零泄漏与零异常**。
