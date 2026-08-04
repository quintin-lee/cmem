# 开发与贡献指南

## 目录

1. [开发环境搭建](#1-开发环境搭建)
2. [代码规范](#2-代码规范)
3. [构建系统](#3-构建系统)
4. [测试指南](#4-测试指南)
5. [提交规范](#5-提交规范)
6. [分支策略](#6-分支策略)
7. [文档编写规范](#7-文档编写规范)
8. [性能调优建议](#8-性能调优建议)
9. [调试技巧](#9-调试技巧)
10. [常见问题](#10-常见问题)

---

## 1. 开发环境搭建

### 1.1 必需工具

| 工具 | 最低版本 | 推荐版本 | 用途 |
| :--- | :--- | :--- | :--- |
| GCC | 7.0 | 13.0+ | C/C++ 编译 |
| Clang | 6.0 | 17.0+ | 替代编译器 |
| Make | 3.81 | 4.3+ | 构建 |
| CMake | 3.10 | 3.28+ | 可选构建 |
| Ninja | 1.10 | 1.11+ | 可选 CMake 生成器 |
| Git | 2.20 | 2.45+ | 版本控制 |
| Python | 3.8 | 3.12+ | 脚本/工具 |
| Doxygen | 1.8 | 1.9+ | API 文档生成 |

### 1.2 Ubuntu/Debian 安装

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build git python3 doxygen
```

### 1.3 Fedora/RHEL 安装

```bash
sudo dnf install gcc gcc-c++ cmake ninja-build git python3 doxygen
```

### 1.4 macOS 安装

```bash
brew install gcc cmake ninja git python3 doxygen
```

---

## 2. 代码规范

### 2.1 C 代码规范

- **标准**：C11（`-std=c11`）
- **编译器警告**：`-Wall -Wextra -Wpedantic -Werror`
- **缩进**：4 空格
- **命名**：
  - 函数：`snake_case`
  - 结构体：`snake_case` + `_t` 后缀
  - 宏：`UPPER_SNAKE_CASE`
  - 局部变量：`snake_case`
- **注释**：Doxygen 风格（`@brief`, `@param`, `@return`）

### 2.2 C++ 代码规范

- **标准**：C++17（`-std=c++17`）
- **命名空间**：`cmem`
- **类名**：`PascalCase`
- **方法名**：`snake_case`
- **模板参数**：`PascalCase`（如 `typename T`）

### 2.3 注释规范

所有公共 API 必须包含 Doxygen 注释：

```c
/**
 * @brief 简短描述（一行）
 *
 * 详细描述（可选，多行）
 *
 * @param pool 内存池指针
 * @param size 请求字节数
 * @return 成功返回指针，失败返回 NULL
 */
void* mp_alloc(memory_pool_t* pool, size_t size);
```

### 2.4 头文件保护

```c
#ifndef CMEM_H
#define CMEM_H

// ... 内容 ...

#endif // CMEM_H
```

---

## 3. 构建系统

### 3.1 Makefile 目标

```bash
make lib          # 编译静态库 build/libcmem.a
make test         # 编译并运行 C 单元测试（Debug + Sanitizers）
make test_cpp     # 编译并运行 C++ 测试
make bench        # 编译并运行性能基准测试
make examples     # 编译并运行示例程序
make tools        # 构建诊断工具（cmem-inspect、cmem-analyze）
make clean        # 清理构建产物
make all          # 编译库 + 测试 + Benchmark + 示例 + 工具
```

### 3.2 CMake 构建

```bash
# Debug 构建
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure

# Release 构建
cmake -B build_release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_release

# 安装
cmake --install build_release --prefix /usr/local
```

**CMake 工具链说明：**
- 自动检测 MSVC：使用 `/W3 /std:c11` 并定义 `_STDC_LIMIT_MACROS` / `_STDC_FORMAT_MACROS`。
- GCC/Clang 使用 `-Wall -Wextra -pthread -D_GNU_SOURCE`。
- 当 `PATH` 中存在 `clang-tidy` 时，CMake 会将其作为编译期检查接入；仓库根目录的 `.clang-tidy` 文件控制启用的检查项。

### 3.3 编译器标志

**Release（生产环境）：**
```bash
-O3 -march=native -flto -DNDEBUG
```

**Debug（开发/测试）：**
```bash
-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer
```

**特性宏：** 两套构建系统均定义 `_GNU_SOURCE`（在严格的 `-std=c11` 下需要它来启用 `sched.h`、`backtrace`、`madvise`）；不再需要逐个文件定义 `_POSIX_C_SOURCE` / `_GNU_SOURCE`。

---

## 4. 测试指南

### 4.1 测试结构

```
tests/
├── test_main.c         # C 综合单元测试（44+ 测试用例）
├── test_advanced.c     # 高级 C 单元测试（此前未覆盖的 API、回调、错误恢复）
├── test_cpp.cpp        # C++ PMR + STL Allocator 测试
├── stress_test.c       # 长期压力/泄漏测试（时长由 STRESS_DURATION_SEC 控制）
└── fuzz_alloc.c        # 独立模糊测试入口（STANDALONE_FUZZ 模式）
```

### 4.2 添加新测试

在 `tests/test_main.c` 中添加测试函数：

```c
static void test_my_new_feature(void) {
    printf("--- Test: My New Feature ---\n");
    
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);
    assert(pool != NULL);
    
    // 测试逻辑
    void* p = mp_alloc(pool, 128);
    assert(p != NULL);
    
    // 清理
    mp_free(pool, p);
    mp_destroy(pool);
    
    printf("[PASS] test_my_new_feature\n\n");
}
```

然后在 `main()` 函数中调用：

```c
int main() {
    test_my_new_feature();
    // ... 其他测试
    return 0;
}
```

### 4.3 运行测试

```bash
# C 单元测试
make test

# C++ 测试
make test_cpp

# 带 AddressSanitizer
make test

# 带 Valgrind
valgrind --leak-check=full ./build/unit_tests
```

### 4.4 测试覆盖检查

```bash
# 使用 gcov
gcov src/cmem.c

# 使用 lcov
lcov --capture --directory . --output-file coverage.info
genhtml coverage.info --output-directory coverage_html
```

---

## 5. 提交规范

### 5.1 Gitmoji 约定

采用 Gitmoji 作为提交消息前缀：

| Emoji | 类型 | 说明 |
| :--- | :--- | :--- |
| 🎨 `:art:` | `style` | 代码格式调整 |
| ⚡️ `:zap:` | `perf` | 性能优化 |
| 🔥 `:fire:` | `remove` | 删除代码或文件 |
| 🐛 `:bug:` | `fix` | Bug 修复 |
| 🚑️ `:ambulance:` | `hotfix` | 紧急修复 |
| ✨ `:sparkles:` | `feat` | 新功能 |
| 📝 `:memo:` | `docs` | 文档更新 |
| 🚀 `:rocket:` | `deploy` | 部署相关 |
| 💄 `:lipstick:` | `ui` | UI/界面更新 |
| ✅ `:white_check_mark:` | `test` | 测试相关 |
| 🔒 `:lock:` | `security` | 安全修复 |
| 🛡️ `:shield:` | `security` | 安全特性 |
| ♻️ `:recycle:` | `refactor` | 代码重构 |
| ➕ `:heavy_plus_sign:` | `add` | 添加依赖 |
| ➖ `:heavy_minus_sign:` | `remove` | 删除依赖 |
| 📦 `:package:` | `build` | 构建相关 |
| 🏗️ `:building_construction:` | `arch` | 架构变更 |
| 🎯 `:dart:` | `target` | 目标/配额相关 |
| 🧊 `:snowflake:` | `tlb` | TLB/冷热相关 |
| 🚀 `:rocket:` | `expansion` | 扩容/扩展 |
| 🚨 `:rotating_light:` | `recovery` | 错误恢复 |
| 📊 `:bar_chart:` | `observability` | 可观测性 |

### 5.2 提交格式

```
<type>(<scope>): <emoji> <subject>

<body>

<footer>
```

**示例：**
```
feat(core): ⚡️ add Per-CPU lock-free freelist

- Implement per-CPU lock-free freelist for low-contention fast path
- Add mp_set_percpu_freelist() API
- Initialize freelist in mp_create() and destroy in mp_destroy()

Closes #123
```

### 5.3 Scope 约定

| Scope | 说明 |
| :--- | :--- |
| `core` | 核心实现（src/cmem.c） |
| `api` | 头文件（include/*.h） |
| `test` | 测试文件 |
| `bench` | 基准测试 |
| `docs` | 文档 |
| `build` | 构建系统 |
| `ci` | CI/CD |

### 5.4 版本管理与 Tag

版本号的唯一真实来源是仓库根目录下的 `VERSION` 文件。

使用 `scripts/tag.sh` 来递增版本号并创建 git tag：

```bash
# 递增 patch 版本并创建 tag
./scripts/tag.sh --bump patch

# 递增 minor/major 版本
./scripts/tag.sh --bump minor
./scripts/tag.sh --bump major

# 设置显式版本
./scripts/tag.sh 1.2.3

# 预演模式
./scripts/tag.sh --dry-run --bump patch
```

该脚本会更新 `VERSION` 文件，使用 `chore(version): 🧹 bump version to x.y.z` 提交，并创建 `v<x.y.z>` git tag。

`CMakeLists.txt` 和 `Makefile` 都会在构建时从 `VERSION` 文件读取版本号。

或通过 Makefile：

```bash
make tag --bump=patch
make tag 1.2.3
```

---

## 6. 分支策略

### 6.1 分支命名

```
main          # 主分支，生产就绪
develop       # 开发分支
feature/*     # 功能分支
fix/*         # 修复分支
release/*     # 发布分支
hotfix/*      # 紧急修复分支
```

### 6.2 工作流

```bash
# 1. 从 develop 创建功能分支
git checkout -b feature/my-new-feature develop

# 2. 开发并提交
git commit -m "feat(core): ⚡️ add new feature"

# 3. 推送到远程
git push origin feature/my-new-feature

# 4. 创建 Pull Request
# 5. Code Review 通过后合并到 develop
```

---

## 7. 文档编写规范

### 7.1 文档结构

项目文档位于 `docs/` 目录：

```
docs/
├── architecture.md      # 架构设计文档
├── api-reference.md     # API 参考文档
├── development.md       # 开发指南（本文档）
├── testing.md           # 测试指南
├── performance.md       # 性能调优指南
├── security.md          # 安全特性文档
└── troubleshooting.md   # 故障排查指南
```

### 7.2 Markdown 规范

- 使用 **GitHub Flavored Markdown**
- 标题层级不超过 3 层
- 代码块必须指定语言
- 表格对齐使用 `:---` 语法
- 链接使用相对路径

### 7.3 更新文档 checklist

添加新功能时，务必更新：

- [ ] `README.md`：特性列表和 API 索引
- [ ] `docs/api-reference.md`：详细 API 说明
- [ ] `docs/architecture.md`：架构变更（如有）
- [ ] `include/cmem.h`：文件头注释
- [ ] `CHANGELOG.md`：变更日志

---

## 8. 性能调优建议

### 8.1 池大小配置

```c
// 根据工作负载选择合适的初始容量
size_t initial_capacity = 0;  // 自动计算
// 或手动指定
size_t initial_capacity = 64 * 1024 * 1024;  // 64MB
```

### 8.2 Flag 选择

| 场景 | 推荐 Flag 组合 |
| :--- | :--- |
| 生产环境（高性能） | `MP_FLAG_THREAD_SAFE \| MP_FLAG_THREAD_LOCAL_CACHE` |
| 开发/调试 | `MP_FLAG_THREAD_SAFE \| MP_FLAG_DEBUG_CANARY \| MP_FLAG_TRACK_LOCATIONS` |
| 安全敏感 | `MP_FLAG_THREAD_SAFE \| MP_FLAG_ENCRYPTED_MEMORY \| MP_FLAG_GUARD_PAGES` |
| 高并发 | `MP_FLAG_THREAD_SAFE \| MP_FLAG_PERCPU_FREELIST` |
| 大内存工作负载 | `MP_FLAG_HUGE_PAGES \| MP_FLAG_HOT_COLD_SEPARATION` |

### 8.3 自动压缩配置

```c
mp_set_auto_compact(pool, true, 0.8, 0.3);
// 压力 > 80% 或 碎片率 > 30% 时触发压缩
```

### 8.4 Per-CPU Freelist

```c
mp_set_percpu_freelist(pool, true);
// 适用于：
// - CPU 核心数 <= 64
// - 小对象分配为主
// - 高并发场景
```

---

## 9. 调试技巧

### 9.1 启用调试特性

```c
// 通过代码
mp_flags_t flags = MP_FLAG_DEBUG_CANARY | MP_FLAG_TRACK_LOCATIONS | MP_FLAG_POISON_ON_FREE;

// 通过环境变量
setenv("CMEM_CONF", "canary=1,track=1,poison=1", 1);
```

### 9.2 堆审计

```c
bool healthy = mp_audit_heap(pool);
if (!healthy) {
    fprintf(stderr, "Heap corruption detected!\n");
}
```

### 9.3 泄漏分析

```c
char report[8192];
mp_analyze_leaks(pool, report, sizeof(report));
printf("%s\n", report);
```

### 9.4 事件追踪

```c
mp_set_event_callback(pool, [](memory_pool_t* p, mp_event_type_t ev, void* ptr, size_t size, void* ud) {
    printf("Event: %d, ptr=%p, size=%zu\n", ev, ptr, size);
}, NULL);
```

### 9.5 诊断 CLI 工具

仓库在 `tools/` 下提供两个诊断 CLI：

- `tools/cmem-inspect` — 链接 `libcmem`，用于实时进程内诊断：
  - 子命令：`leaks`、`audit`、`stats`、`tree`、`histogram`、`snapshot`、`diff`、`html`
  - 支持 `--json`、`--output <path>`、`--quiet`
- `tools/cmem-analyze` — 独立离线分析器，解析 `.cmem_dump` 二进制快照：
  - 子命令：`report`、`top`、`summary`、`validate`、`diff`
  - 支持 `--json`、`--html`、`--output <path>`、`--quiet`、`--top <n>`

构建方式：

```bash
make tools
```

### 9.6 GDB 调试

```bash
# 启动 GDB
gdb ./build/unit_tests

# 设置断点
break mp_alloc

# 运行
run

# 查看内存池状态
p *pool
```

---

## 10. 常见问题

### Q1: 如何选择合适的 Slab Class 数量？

**A:** 默认 7 个 Class（8B~512B）适用于大多数场景。如果工作负载集中在特定尺寸范围，可以使用 `mp_set_slab_classes` 自定义。

### Q2: Per-CPU Freelist 何时启用？

**A:** 当满足以下条件时建议启用：
- CPU 核心数 >= 4
- 小对象分配占比高（> 60%）
- 多线程并发度较高

### Q3: 如何诊断内存碎片？

**A:**
```c
mp_stats_t stats;
mp_get_stats(pool, &stats);
printf("Fragmentation: %.2f%%\n", stats.fragmentation_ratio * 100);

// 查看可回收内存
printf("Freeable: %zu bytes\n", mp_freeable(pool));
```

### Q4: 在线扩容有什么限制？

**A:**
- 仅 TLSF 层支持在线扩容
- Slab 层和 OS Fallback 层不参与扩容
- 扩容后新容量仅在新的 TLSF Pool 链表中有效

### Q5: 加密内存的性能影响？

**A:**
- `mlock` 有系统限制（通常每个进程最多 64KB~1GB，取决于系统配置）
- `MADV_DONTDUMP` 几乎无性能影响
- 建议仅在安全敏感场景启用

### Q6: 如何集成到现有项目？

**A:**
```c
// 方案 1：直接替换 malloc
#include "cmem_override.h"

// 方案 2：按模块替换
memory_pool_t* pool = mp_create(1024*1024, MP_FLAG_DEFAULT);
void* p = mp_alloc(pool, size);
```

### Q7: 支持哪些平台？

**A:**
- Linux（完整支持）
- macOS（部分支持，无 NUMA/HugePages）
- Windows（部分支持；仅 MSVC，mmap/madvise 由 VirtualAlloc 提供，不支持 POSIX 共享内存）
- FreeBSD/Android（基础支持）

### Q8: 如何贡献代码？

**A:**
1. Fork 仓库
2. 创建特性分支
3. 编写代码 + 测试
4. 确保 `make test` 通过
5. 提交 Pull Request

详见 [贡献指南](https://github.com/quintin-lee/cmem/blob/main/CONTRIBUTING.md)。
