# FAST_PATH × 边界标志组合审计报告

规范：`docs/superpowers/specs/2026-10-22-fastpath-combo-audit-design.md`（提交 `a151752`）。
修复提交：`c8f36a1` + `6233b43`（`fix(core): 🐛 balance stats and audit fields across fastpath/plain api pairing`）。
日期：2026-10-22。

## 方法
逐组合代码走查（grep/read 取证）；探针仅在 `/tmp/opencode/` 编译链接 `src/*.c`
（不入库），在 Release 构建（`CMEM_SAFETY_LEVEL=2`，默认池 = canary + `MP_FLAG_FAST_PATH`）
与 Debug 构建（`CMEM_SAFETY_LEVEL=3`，默认池无 FAST_PATH）中运行以隔离标志相关行为。
第 8 组使用独立 `-fsanitize=thread` 构建。

## 结论

| # | 组合 | 结论 | 证据 |
|---|------|------|------|
| 1 | FAST_PATH × 普通 `mp_alloc`/`mp_free`/`mp_realloc` | **通过** | realloc 拷贝路径：普通 `mp_alloc`（置 COUNTED 位）+ `mp_free`（按旧头部 COUNTED 位门控递减）对称净零；原地缩容与 TLSF 原地路径只改 `requested_size`/canary/事件字段。 |
| 2 | `mp_alloc_fast` 与 `mp_free` 混用（及反向） | **缺陷 → 已修复** | free-list 链接覆盖头部前 8 字节（`mp_slab_slot_t` 即头部前 8 字节）后出现陈旧字段：`magic=0`、`requested_size`/`usable_size` 陈旧、`mp_get_allocation_info` 报 false；`mp_alloc` 后接 `mp_free_fast` 导致统计计数漂移（残留 +24 块）。修复：新增 `MP_BLOCK_FLAG_COUNTED` 头部位，所有计数分配点置位；`mp_alloc_fast` 重新完整落头部；`mp_free_fast` 免统计缓存推入按位门控；全部释放侧递减按位门控；`mp_free`/`mp_free_batch` magic 检查无条件化。回归测试：`test_fastpath_mixed_api` + `test_fastpath_audit_fields`。探针：400 次分配/400 次释放后 `active=0`，`mp_check_leaks` 干净。 |
| 3 | FAST_PATH × `MP_FLAG_SHARED_MEMORY` | **通过** | 默认 level-2 共享池绝不携带 `MP_FLAG_PERCPU_FREELIST`（默认仅按位或叠加）。TLS 缓存为 `thread_local` → 进程本地，对 IPC 正确；slab 页 free list 与池 rwlock 经 pshared 属性共享。既有潜在隐患已记录：显式在共享池启用 `MP_FLAG_PERCPU_FREELIST` 会把进程私有 `calloc` 表指针存入共享结构（其他进程读到悬垂指针）。标志文档与 README 已加告警。 |
| 4 | FAST_PATH × `MP_FLAG_STATIC_BUFFER` | **通过** | `mp_create_from_buffer` 先解析编译期默认与环境变量再置 static 位，默认策略按预期生效；slab 页在 buffer 之外经 `mmap(MAP_PRIVATE\|MAP_ANONYMOUS)` 创建，结构审计辅助函数在所有配置中读取的都是真实映射。 |
| 5 | FAST_PATH × 多 arena 子池 | **通过** | 子池经 `mp_create_child_verbatim` 原样继承父池标志（仅掩掉 `MP_FLAG_MULTI_ARENA`）；主池 `mp_alloc` 经 `mp_get_thread_bound_arena` 路由到 `mp_alloc(arena, ...)`，arena 分配计入子池。探针：主池计数精确，`mp_check_leaks`/`mp_purge_lazy` 干净，各 arena `mp_get_stats` 一致。 |
| 6 | FAST_PATH × 诊断 | **通过** | 默认池上 `mp_check_leaks` 能检出待释放的 slab 与 TLSF 分配（泄漏列表渲染 `Tier: SLAB`/`TLSF` 及尺寸）；`mp_enumerate_regions` 返回 slab + TLSF 区域；`mp_get_stats` 总量平衡。探针：201 个未释放分配被报告，释放后计数精确。 |
| 7 | `CMEM_CONF` 运行时重解析 × FAST_PATH | **通过（不变式）** | `mp_reparse_env_flags` 仅把识别到的 `name=1\|on` 条目按位或到当前标志；无法摘除 `MP_FLAG_FAST_PATH`（或任何构造期标志）。FAST_PATH 成员资格设计上不可变。已在 api-reference §3.4 备注中记录。 |
| 8 | 结构审计遍历 × 并发释放 | **通过** | `mp_fastpath_slot_live` 在 `sc->lock` 下遍历 `page->free_list`；释放侧对 `page->free_list` 的改动持有同一锁。8 线程 TSan 探针（`mp_alloc`/`mp_alloc_fast`/`mp_free`/`mp_free_fast` 混用并并发调用 `mp_ptr_valid`）干净通过，计数精确。 |
| 9 | OS/emergency 层级 `raw_base` 不变式 | **通过** | 每个分配层级都无条件打 `magic = MP_MAGIC_HEAD`，并设置 `raw_base`（slab 槽指向自身，OS 块指向块基址）与 `subpool`。非 slab 层级的 `mp_fastpath_slot_live` 要求 `raw_base != NULL`；已释放的 OS/TLSF 块在被复用前可能仍报活——已记录为结构审计的已知边界（仅 slab 层级精确）。 |

## 遗留权衡（已记录，非缺陷）
- FAST_PATH 池上已释放的 OS/TLSF 块在审计 API 中可能仍报活直到被复用（无 active list 可走）；
  slab 层级判活精确。
- `MP_FLAG_PERCPU_FREELIST` + `MP_FLAG_SHARED_MEMORY` 组合不安全（进程私有表位于共享结构内）；
  已在 `docs/en|zh/api-reference.md` 与 `docs/en|zh/development.md` 中告警。
- `MP_BLOCK_FLAG_COUNTED` 给每次计数释放增加一个分支判断；基准显示无可测吞吐回归
  （普通 `mp_alloc` ≈ 28 Mops/s，`mp_alloc_fast` 快速通道 ≈ 141 Mops/s，与修复前基线噪声内一致）。

## 探针清理
所有探针（`probe_mixdiag`、`probe_purge`、`probe_multiarena`、`probe_diag`、`probe_tsan` 及
可执行文件）均位于 `/tmp/opencode/`，已删除。

## 验收
- CTest：`build_safety_release`、`build_safety_debug`、`build_safety_none`、
  `build_safety_asan` 均 3/3 通过。
- `make format-check CONFIG=cmake`：exit 0；`git diff --check`：干净。
- 所有改动的源文件均已运行 `clang-format`。
