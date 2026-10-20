# CMake-first Makefile部分

# 默认目标
.PHONY: all cmake-build cmake-test cmake-clean install format-check format check-mermaid docs help

all: cmake-build cmake-test

# CMake目标
cmake-build: format-check check-mermaid
	@cmake --build $(CMAKE_BUILD_DIR)

cmake-test: cmake-build
	@ctest --test-dir $(CMAKE_BUILD_DIR) --output-on-failure

cmake-clean:
	@rm -rf $(CMAKE_BUILD_DIR) build build-debug build-asan build-tsan build-ubsan
	@rm -f leak_report.txt test_report.html memory_profile.html snap_a.cmem_dump snap_b.cmem_dump test_snapshot.cmem_dump
	@rm -f test_report.html snapshot_diff.txt

# 安装目标
install: cmake-build
	@cmake --install $(CMAKE_BUILD_DIR) --prefix $(PREFIX)

# 生成发布包
package: cmake-clean
	@cd .. && tar --exclude='*/build' --exclude='*/.git' --exclude='*/cmake-build-*' \
		--exclude='*/.vscode' --exclude='*/.idea' --exclude='*/.cache' \
		--exclude='*/$(CMAKE_BUILD_DIR)' -czf cmem-$(VERSION).tar.gz cmem
	@echo "Package created: ../cmem-$(VERSION).tar.gz"

# 格式检查
format-check:
	@which clang-format > /dev/null || (echo "clang-format not found"; exit 1)
	@clang-format --dry-run --Werror \
		include/*.h include/*.hpp \
		src/*.c \
		tests/*.c tests/*.cpp \
		examples/*.c \
		benchmarks/*.c \
		tests/stress_test.c

format:
	@which clang-format > /dev/null || (echo "clang-format not found"; exit 1)
	@clang-format -i \
		include/*.h include/*.hpp \
		src/*.c \
		tests/*.c tests/*.cpp \
		examples/*.c \
		benchmarks/*.c \
		tests/stress_test.c

# 检查 Mermaid 图语法
check-mermaid:
	@which python3 > /dev/null || (echo "python3 not found"; exit 1)
	@python3 scripts/check_mermaid.py

# 生成文档
# docs:
# 	@which doxygen > /dev/null || (echo "doxygen not found"; exit 1)
# 	@test -f Doxyfile || (echo "Doxyfile not found, skipping docs generation"; exit 0)
# 	doxygen Doxyfile

# 帮助
help:
	@echo "cmem Unified Build System (CMake-first)"
	@echo ""
	@echo "CMake-first模式配置 (CONFIG=cmake):"
	@echo "  cmake-build    - 配置并构建CMake项目"
	@echo "  cmake-test     - 构建并运行CMake测试"
	@echo "  cmake-clean    - 清理CMake构建"
	@echo "  install        - 安装库和头文件到 $(PREFIX)"
	@echo "  package        - 创建源码发布包"
	@echo "  format-check   - 检查代码格式"
	@echo "  format         - 修复代码格式"
	@echo "  check-mermaid  - 验证Mermaid图语法"
	@echo "  help           - 显示帮助信息"
	@echo ""
	@echo "Make兼容模式 (CONFIG=make/debug/asan/tsan/ubsan):"
	@echo "  lib            - 构建静态库"
	@echo "  test           - 构建并运行C单元测试"
	@echo "  test_advanced  - 构建并运行C高级单元测试"
	@echo "  test_all       - 运行所有C测试"
	@echo "  test_cpp       - 构建并运行C++17 PMR/STL测试"
	@echo "  bench          - 构建并运行性能基准测试"
	@echo "  examples       - 构建并运行所有示例程序"
	@echo "  tools          - 构建诊断工具"
	@echo "  clean          - 删除构建产物"
	@echo "  help           - 显示帮助信息"
	@echo ""
	@echo "变量:"
	@echo "  CONFIG         - 构建系统 (cmake/make/debug/asan/tsan/ubsan; 默认: cmake)"
	@echo "  PREFIX         - 安装前缀 (默认: /usr/local)"
	@echo "  CMAKE_BUILD_TYPE - CMake构建类型 (默认: Release)"
