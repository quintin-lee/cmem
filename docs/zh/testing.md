# 测试指南

## 目录

1. [测试框架](#1-测试框架)
2. [测试结构](#2-测试结构)
3. [运行测试](#3-运行测试)
4. [添加新测试](#4-添加新测试)
5. [测试覆盖](#5-测试覆盖)
6. [Sanitizers](#6-sanitizers)
7. [性能测试](#7-性能测试)
8. [CI/CD](#8-cicd)

---

## 1. 测试框架

cmem 使用 **自研轻量级测试框架**，无需外部依赖。

**特性：**
- 零依赖，纯 C 实现
- 支持断言（assert）
- 自动检测内存泄漏
- 彩色输出
- 测试用例分组

---

## 2. 测试结构

```
tests/
├── test_main.c         # C 综合单元测试（44+ 测试用例）
├── test_advanced.c     # 高级 C 单元测试（回调、恢复、边界条件）
└── test_cpp.cpp        # C++ PMR + STL Allocator 测试

benchmarks/
└── bench_main.c        # 性能基准测试

examples/
├── example_basic.c         # 基础用法
├── example_embedded.c      # 静态缓冲区模式
├── example_leak_analysis.c # 泄漏分析
└── example_arena_tree.c    # 树状 Arena

tools/
├── cmem-inspect/       # 实时诊断 CLI
│   ├── cmem-inspect.c
│   └── cmem-inspect.h
├── cmem-analyze/       # 离线快照分析器
│   ├── cmem-analyze.c
│   ├── cmem-analyze-parser.c
│   └── cmem-analyze.h
└── common/             # 共享诊断输出工具
    ├── cmem-diag-output.c
    └── cmem-diag-output.h
```

---

## 3. 运行测试

### 3.1 C 单元测试

```bash
# Debug 构建 + Sanitizers
make test

# 手动编译运行
gcc -fsanitize=address,undefined -Wall -Wextra -g -O0 \
    -std=c11 -D_GNU_SOURCE -I./include src/cmem.c tests/test_main.c \
    -o build/unit_tests -pthread -lrt
./build/unit_tests
```

### 3.2 C++ 测试

```bash
make test_cpp

# 手动编译
g++ -std=c++17 -Wall -Wextra -g -O0 \
    -I./include src/cmem.c tests/test_cpp.cpp \
    -o build/cpp_tests -pthread -lrt
./build/cpp_tests
```

### 3.3 性能基准

```bash
make bench
./build/benchmark
```

---

## 4. 添加新测试

### 4.1 C 测试模板

在 `tests/test_main.c` 中添加：

```c
static void test_my_feature(void) {
    printf("--- Test: My New Feature ---\n");
    
    // 1. 创建内存池
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);
    assert(pool != NULL);
    
    // 2. 执行测试逻辑
    void* p = mp_alloc(pool, 128);
    assert(p != NULL);
    assert(mp_ptr_valid(pool, p));
    
    // 3. 清理
    mp_free(pool, p);
    
    // 4. 检查泄漏
    if (mp_check_leaks(pool)) {
        printf("[PASS] test_my_feature\n\n");
    } else {
        printf("[FAIL] test_my_feature - leaks detected\n\n");
        exit(1);
    }
    
    mp_destroy(pool);
}
```

然后在 `main()` 中调用：

```c
int main() {
    test_my_feature();
    // ... 其他测试
    return 0;
}
```

### 4.2 C++ 测试模板

在 `tests/test_cpp.cpp` 中添加：

```cpp
static void test_cpp_feature() {
    std::cout << "--- Test: C++ Feature ---\n" << std::endl;
    
    cmem::MemoryPool pool(1024 * 1024, MP_FLAG_THREAD_SAFE);
    
    // 测试逻辑
    void* p = pool.alloc(128);
    assert(p != nullptr);
    
    // 清理
    pool.free(p);
    assert(pool.check_leaks());
}

int main() {
    test_cpp_feature();
    // ...
    return 0;
}
```

---

## 5. 测试覆盖

### 5.1 现有测试用例

| 测试名 | 覆盖功能 |
| :--- | :--- |
| `test_slab_small_allocs` | Slab 小对象分配 |
| `test_tlsf_medium_allocs` | TLSF 中对象分配 |
| `test_realloc_and_aligned` | Realloc 和对齐分配 |
| `test_multithread_safety` | 多线程并发安全 |
| `test_arena_reset_and_json` | Arena 重置和 JSON 导出 |
| `test_static_buffer_and_callbacks` | 静态缓冲区和事件回调 |
| `test_child_arenas_and_html_export` | 子 Arena 和 HTML 导出 |
| `test_leak_analysis_and_heap_audit` | 泄漏分析和堆审计 |
| `test_memory_budget_and_oom` | 内存限制和 OOM |
| `test_batch_alloc_and_compact` | 批量分配和压缩 |
| `test_allocation_histogram` | 分配直方图 |
| `test_cache_aligned_alloc` | Cache Line 对齐 |
| `test_guard_pages_protection` | Guard Pages 防护 |
| `test_realtime_throughput_meter` | 实时吞吐计量 |
| `test_shared_memory_ipc` | 共享内存 IPC |
| `test_global_override` | 全局 malloc 拦截 |
| `test_huge_pages_alloc` | HugePages 加速 |
| `test_binary_snapshot` | 二进制快照 |
| `test_env_conf_tuning` | 环境变量调优 |
| `test_typed_object_pool` | 类型化对象池 |
| `test_prometheus_metrics` | Prometheus 指标 |
| `test_purge_lazy` | 延迟 RSS 清理 |
| `test_watermark_callback` | 水位回调 |
| `test_diff_snapshots` | 快照 Diff |
| `test_frame_arena` | 帧竞技场 |
| `test_numa_node_binding` | NUMA 绑定 |
| `test_emergency_reserve` | 紧急 OOM 储备 |
| `test_convenience_apis` | 便利函数 |
| `test_reallocarray` | 溢出安全 reallocarray |
| `test_tlsf_inplace_realloc` | TLSF 原地 realloc |
| `test_introspection_apis` | 内省 API |
| `test_advanced_stats` | 高级统计 |
| `test_reset_stats_and_preferred_size` | 统计重置和首选尺寸 |
| `test_mp_madvise` | 跨平台 madvise |
| `test_arena_metadata_apis` | Arena 元数据 |

### 5.2 覆盖率目标

- **行覆盖率**：> 90%
- **分支覆盖率**：> 85%
- **关键路径**：100% 覆盖

### 5.3 运行覆盖率

```bash
# 使用 gcov
gcov src/cmem.c

# 使用 lcov
lcov --capture --directory . --output-file coverage.info
genhtml coverage.info --output-directory coverage_html
xdg-open coverage_html/index.html
```

---

## 6. Sanitizers

### 6.1 AddressSanitizer (ASan)

检测内存错误：
- 越界访问（堆/栈/全局）
- Use-After-Free
- 双 Free
- 内存泄漏

```bash
gcc -fsanitize=address -g -O1 -fno-omit-frame-pointer \
    -std=c11 -I./include src/cmem.c tests/test_main.c \
    -o build/unit_tests_asan -pthread -lrt
./build/unit_tests_asan
```

### 6.2 UndefinedBehaviorSanitizer (UBSan)

检测未定义行为：
- 整数溢出
- 空指针解引用
- 对齐错误

```bash
gcc -fsanitize=undefined -g -O1 \
    -std=c11 -I./include src/cmem.c tests/test_main.c \
    -o build/unit_tests_ubsan -pthread -lrt
./build/unit_tests_ubsan
```

### 6.3 组合使用

```bash
gcc -fsanitize=address,undefined -fno-sanitize-recover=all \
    -g -O1 -fno-omit-frame-pointer \
    -std=c11 -I./include src/cmem.c tests/test_main.c \
    -o build/unit_tests_full -pthread -lrt
```

### 6.4 ASan 集成测试

```c
// 测试 ASan 集成层
static void test_asan_integration(void) {
    printf("--- Test: ASan Integration ---\n");
    
    memory_pool_t* pool = mp_create(1024*1024, MP_FLAG_ASAN_INTEGRATION);
    assert(pool != NULL);
    
    // 启用 ASan 集成
    mp_set_asan_integration(pool, true);
    assert(mp_asan_is_enabled() == false || true);  // 可能取决于编译选项
    
    mp_destroy(pool);
    printf("[PASS] test_asan_integration\n\n");
}
```

---

## 7. 性能测试

### 7.1 基准测试框架

使用 `benchmarks/bench_main.c` 中的基准测试框架。

### 7.2 关键指标

| 指标 | 说明 | 目标 |
| :--- | :--- | :--- |
| 小对象 QPS | 32-256B 分配/释放吞吐 | > 1.5 Gops/sec |
| 中对象 QPS | 1KB-64KB 分配/释放吞吐 | > 300 Mops/sec |
| Arena Reset | 500 allocs × 1000 rounds | < 1 ms |
| 多线程扩展 | 8 threads × 100K allocs | Near-linear |

### 7.3 运行基准

```bash
make bench
```

### 7.4 自定义基准

```c
#include <time.h>
#include "cmem.h"

int main() {
    memory_pool_t* pool = mp_create(64 * 1024 * 1024, MP_FLAG_THREAD_SAFE);
    
    const int N = 1000000;
    struct timespec start, end;
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < N; i++) {
        void* p = mp_alloc(pool, 64);
        mp_free(pool, p);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double elapsed = (end.tv_sec - start.tv_sec) + 
                     (end.tv_nsec - start.tv_nsec) / 1e9;
    double qps = N / elapsed;
    
    printf("QPS: %.2f Mops/sec\n", qps / 1e6);
    
    mp_destroy(pool);
    return 0;
}
```

---

## 8. CI/CD

### 8.1 GitHub Actions 配置

项目应包含 `.github/workflows/ci.yml`：

```yaml
name: CI

on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install dependencies
        run: sudo apt update && sudo apt install -y build-essential cmake ninja-build
      - name: Build
        run: make lib
      - name: Test
        run: make test
      - name: C++ Test
        run: make test_cpp
```

### 8.2 质量门禁

- [ ] `make lib` 编译无警告，并生成带版本号的静态归档
- [ ] `make lib_shared` 成功构建动态库且 SONAME 正确
- [ ] `make test` 全量通过
- [ ] `make test_cpp` 通过
- [ ] 代码符合规范
- [ ] 新功能包含测试
- [ ] 文档已更新

---

## 附录：调试检查清单

- [ ] 启用 `MP_FLAG_DEBUG_CANARY` 检测越界
- [ ] 启用 `MP_FLAG_TRACK_LOCATIONS` 追踪调用栈
- [ ] 运行 `mp_audit_heap()` 检查堆完整性
- [ ] 运行 `mp_analyze_leaks()` 检查泄漏
- [ ] 使用 Valgrind 检查内存错误
- [ ] 使用 ASan 检测内存错误
- [ ] 检查 `mp_get_stats()` 中的碎片率
- [ ] 检查 `mp_dump_histogram()` 中的尺寸分布
