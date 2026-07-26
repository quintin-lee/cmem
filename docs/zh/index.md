# cmem 文档中心

欢迎查阅 cmem 项目文档。本文档中心涵盖架构设计、API 参考、开发指南、测试、性能调优和安全特性。

## 📚 文档索引

### 核心文档

| 文档 | 说明 | 适用读者 |
| :--- | :--- | :--- |
| [architecture.md](architecture.md) | 架构设计文档 | 架构师、高级开发者 |
| [api-reference.md](api-reference.md) | API 参考文档 | 所有开发者 |
| [development.md](development.md) | 开发与贡献指南 | 贡献者、开发者 |
| [testing.md](testing.md) | 测试指南 | QA、开发者 |
| [performance.md](performance.md) | 性能调优指南 | 性能工程师、开发者 |
| [security.md](security.md) | 安全特性文档 | 安全工程师、开发者 |

### 快速开始

1. **新手入门**：先阅读 [README.md](../../README.md) 了解项目概览
2. **集成开发**：查阅 [api-reference.md](api-reference.md) 获取 API 详情
3. **性能优化**：参考 [performance.md](performance.md) 进行调优
4. **安全加固**：查看 [security.md](security.md) 配置安全特性
5. **贡献代码**：阅读 [development.md](development.md) 了解开发流程

---

## 📖 文档说明

### architecture.md

包含以下内容：
- 系统概述与设计目标
- 三层混合架构详解（Slab / TLSF / OS Fallback）
- 核心数据结构与内存布局
- 并发控制模型与锁策略
- 扩展机制（在线扩容、自定义 Backend、事件回调）
- 诊断与可观测性设计
- 安全特性架构
- C++ 接口设计
- 性能优化策略
- 未来演进方向

### api-reference.md

包含以下内容：
- 内存池生命周期 API（创建/销毁/重置）
- 内存分配与释放 API（alloc/calloc/realloc/free）
- 内省查询与元数据 API（usable_size/ptr_valid 等）
- 便利辅助函数（strdup/memdup/asprintf）
- 内存紧凑与回收 API（trim/compact/purge）
- 统计、诊断与监控 API（stats/histogram/html/prometheus）
- 泄漏检测与堆审计 API（audit/analyze/check）
- 高级特性 API（热加载、自动压缩、延迟统计、Slab 配置等）
- 配置标志位（mp_flags_t）
- C++ API（MemoryPool/allocator/pmr_resource）
- 事件类型枚举

### development.md

包含以下内容：
- 开发环境搭建（Linux/macOS/Windows）
- 代码规范（C11/C++17）
- 构建系统（Makefile/CMake）
- 测试指南（添加测试、运行测试、覆盖率）
- 提交规范（Gitmoji 约定）
- 分支策略
- 文档编写规范
- 性能调优建议
- 调试技巧（GDB/ASan/Valgrind）
- 常见问题 FAQ

### testing.md

包含以下内容：
- 测试框架说明
- 测试结构与目录
- 运行测试（C/C++/Benchmark）
- 添加新测试（模板代码）
- 测试覆盖清单（35+ 测试用例）
- Sanitizers 使用（ASan/UBSan）
- 性能基准测试
- CI/CD 配置
- 调试检查清单

### performance.md

包含以下内容：
- 性能模型（三层分配器吞吐对比）
- 池大小调优（初始容量/在线扩容/内存限制）
- Flag 选择策略（开销对比表）
- Slab Class 调优（自定义尺寸表/监控分布）
- 并发优化（TLS Cache vs Per-CPU Freelist/批量操作/Frame Arena）
- 内存回收策略（自动压缩/手动回收/延迟回收）
- NUMA 优化（节点绑定/多节点场景）
- HugePages 优化（系统配置/适用场景）
- 监控指标（Prometheus/JSON）
- 性能陷阱与排查流程

### security.md

包含以下内容：
- 安全威胁模型（威胁分类/攻击面）
- 调试防护（Canary/UAF 毒化/Guard Pages/双 Free）
- 加密内存（mlock/MADV_DONTDUMP/secure zero）
- ASan 集成（检测能力/集成模式/自定义错误报告）
- 安全配置示例（最高安全/平衡/最小安全）
- 安全最佳实践（开发/生产/事件回调）
- 合规性（PCI DSS/GDPR/HIPAA/SOC 2）

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

## 🔗 外部资源

- [GitHub Repository](https://github.com/your-repo/cmem)
- [Issue Tracker](https://github.com/your-repo/cmem/issues)
- [Discussions](https://github.com/your-repo/cmem/discussions)
- [Doxygen Documentation](https://your-repo.github.io/cmem/)

---

## 📝 文档贡献

发现文档问题或希望补充内容？

1. Fork 本仓库
2. 修改 `docs/` 目录下的对应文件
3. 提交 Pull Request

**文档规范：**
- 使用 Markdown 格式
- 代码块指定语言高亮
- 表格对齐使用 `:---` 语法
- 链接使用相对路径
