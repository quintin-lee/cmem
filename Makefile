# cmem - Universal High-Performance Tiered Memory Manager
# Makefile with support for build, test, benchmark, install, uninstall, package

CC = gcc
CXX = g++
LDFLAGS = -pthread -lrt
CFLAGS = -Wall -Wextra -O3 -std=c11 -D_POSIX_C_SOURCE=200809L -I./include
CXXFLAGS = -Wall -Wextra -O3 -std=c++17 -I./include
CFLAGS_DEBUG = -fsanitize=address,undefined -Wall -Wextra -g -O0 -std=c11 -D_POSIX_C_SOURCE=200809L -I./include
CXXFLAGS_DEBUG = -fsanitize=address,undefined -Wall -Wextra -g -O0 -std=c++17 -I./include

# 版本信息（从 VERSION 文件读取）
VERSION := $(shell cat VERSION | tr -d '[:space:]')
LIBNAME = libcmem.a
SONAME = libcmem.so.$(VERSION)

SRC = $(wildcard src/*.c)
TEST_SRC = tests/test_main.c
ADV_TEST_SRC = tests/test_advanced.c
STRESS_SRC = tests/stress_test.c
CPP_TEST_SRC = tests/test_cpp.cpp
BENCH_SRC = benchmarks/bench_main.c

BUILD_DIR = build
PREFIX = /usr/local
LIBDIR = $(PREFIX)/lib
INCLUDEDIR = $(PREFIX)/include

# 默认目标
.PHONY: all lib test test_advanced test_all test_cpp bench examples clean install uninstall package distclean help format-check stress_test coverage bench-regression static-analysis docker-build fuzz-build fuzz-run fuzz-ci fuzz-clean check-mermaid

all: format-check check-mermaid lib test test_advanced test_cpp bench examples

help:
	@echo "cmem Makefile Targets:"
	@echo "  all          - Build library, run all tests, benchmarks, and examples"
	@echo "  lib          - Build static library $(BUILD_DIR)/$(LIBNAME)"
	@echo "  lib_shared   - Build shared library $(BUILD_DIR)/$(SONAME)"
	@echo "  test         - Build and run C unit tests (with ASan/UBSan)"
	@echo "  test_advanced- Build and run C advanced unit tests (with ASan/UBSan)"
	@echo "  test_all     - Build and run all C tests"
	@echo "  test_cpp     - Build and run C++17 PMR/STL tests (with ASan/UBSan)"
	@echo "  bench        - Build and run performance benchmarks"
	@echo "  bench-regression - Run performance regression baseline"
	@echo "  stress_test  - Build and run long-run high-concurrency stress test"
	@echo "  coverage     - Generate code coverage report (requires lcov)"
	@echo "  static-analysis - Run cppcheck static analysis"
	@echo "  examples     - Build and run all example programs"
	@echo "  install      - Install library and headers to $(PREFIX)"
	@echo "  uninstall    - Remove installed files from $(PREFIX)"
	@echo "  package      - Create source tarball for distribution"
	@echo "  docker-build - Build and test inside Docker container (Ubuntu 22.04)"
	@echo "  check-mermaid- Validate Mermaid diagram syntax in documentation"
	@echo "  clean        - Remove build artifacts"
	@echo "  distclean    - Remove build artifacts and generated files"
	@echo ""
	@echo "Variables:"
	@echo "  PREFIX       - Installation prefix (default: /usr/local)"
	@echo "  BUILD_DIR    - Build directory (default: build)"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# 静态库
lib: format-check $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRC))
	ar rcs $(BUILD_DIR)/$(LIBNAME) $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRC))
	@echo "Built static library: $(BUILD_DIR)/$(LIBNAME)"

# 共享库
lib_shared: format-check $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRC))
	$(CC) $(CFLAGS) -fPIC -shared $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRC)) -o $(BUILD_DIR)/$(SONAME) $(LDFLAGS)
	ln -sf $(SONAME) $(BUILD_DIR)/libcmem.so
	ln -sf $(SONAME) $(BUILD_DIR)/libcmem.so.1
	@echo "Built shared library: $(BUILD_DIR)/$(SONAME)"

# C 单元测试
test: format-check $(SRC) $(TEST_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS_DEBUG) $(SRC) $(TEST_SRC) -o $(BUILD_DIR)/unit_tests $(LDFLAGS)
	@echo "Running C unit tests..."
	./$(BUILD_DIR)/unit_tests

# C 高级单元测试
test_advanced: format-check $(SRC) $(ADV_TEST_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS_DEBUG) $(SRC) $(ADV_TEST_SRC) -o $(BUILD_DIR)/advanced_tests $(LDFLAGS)
	@echo "Running C advanced unit tests..."
	./$(BUILD_DIR)/advanced_tests

# 长时间高并发压测
stress_test: format-check $(SRC) $(STRESS_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) $(STRESS_SRC) -o $(BUILD_DIR)/stress_test $(LDFLAGS)
	@echo "Running long-run stress test..."
	./$(BUILD_DIR)/stress_test

# 运行所有 C 测试
test_all: test test_advanced

# C++ 测试
test_cpp: format-check $(SRC) $(CPP_TEST_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS_DEBUG) $(SRC) $(CPP_TEST_SRC) -o $(BUILD_DIR)/cpp_tests $(LDFLAGS)
	@echo "Running C++ tests..."
	./$(BUILD_DIR)/cpp_tests

# 性能基准测试
bench: format-check $(SRC) $(BENCH_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) $(BENCH_SRC) -o $(BUILD_DIR)/benchmark $(LDFLAGS)
	@echo "Running benchmarks..."
	./$(BUILD_DIR)/benchmark

# 示例程序
examples: format-check $(SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) examples/example_basic.c -o $(BUILD_DIR)/example_basic $(LDFLAGS)
	$(CC) $(CFLAGS) $(SRC) examples/example_embedded.c -o $(BUILD_DIR)/example_embedded $(LDFLAGS)
	$(CC) $(CFLAGS) $(SRC) examples/example_leak_analysis.c -o $(BUILD_DIR)/example_leak_analysis $(LDFLAGS)
	$(CC) $(CFLAGS) $(SRC) examples/example_arena_tree.c -o $(BUILD_DIR)/example_arena_tree $(LDFLAGS)
	@echo "Running examples..."
	./$(BUILD_DIR)/example_basic
	./$(BUILD_DIR)/example_embedded
	./$(BUILD_DIR)/example_leak_analysis
	./$(BUILD_DIR)/example_arena_tree

# Fuzzing targets (requires clang; falls back to ASan-only on gcc)
FUZZ_SRCS = tests/fuzz_alloc.c src/*.c
FUZZ_CC ?= clang
FUZZ_CFLAGS = -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -O1 -g -D_POSIX_C_SOURCE=200809L -I./include
FUZZ_LDFLAGS = -fsanitize=fuzzer,address,undefined -pthread -lrt
FUZZ_ARGS = -max_len=4096 -jobs=4

fuzz-build: format-check | $(BUILD_DIR)
	@$(FUZZ_CC) --version >/dev/null 2>&1 || { echo "Fuzzing requires clang. Install clang or set FUZZ_CC=gcc (ASan-only mode)."; exit 1; }
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_SRCS) -o $(BUILD_DIR)/fuzz_alloc $(FUZZ_LDFLAGS)
	@echo "Built fuzz target: build/fuzz_alloc"

fuzz-run: fuzz-build
	@echo "Running fuzzing (Ctrl+C to stop)..."
	@mkdir -p corpus
	./build/fuzz_alloc corpus $(FUZZ_ARGS)

fuzz-ci: fuzz-build
	@mkdir -p corpus
	@echo "Running CI fuzzing (10 seconds)..."
	./build/fuzz_alloc corpus -max_len=4096 -timeout=10 -runs=1000

fuzz-clean:
	rm -f build/fuzz_alloc

# 安装
install: lib | $(BUILD_DIR)
	install -d $(DESTDIR)$(LIBDIR)
	install -d $(DESTDIR)$(INCLUDEDIR)
	install -m 644 $(BUILD_DIR)/$(LIBNAME) $(DESTDIR)$(LIBDIR)/$(LIBNAME)
	install -m 644 include/cmem.h $(DESTDIR)$(INCLUDEDIR)/cmem.h
	install -m 644 include/cmem.hpp $(DESTDIR)$(INCLUDEDIR)/cmem.hpp
	install -m 644 include/cmem_pmr.hpp $(DESTDIR)$(INCLUDEDIR)/cmem_pmr.hpp
	install -m 644 include/cmem_override.h $(DESTDIR)$(INCLUDEDIR)/cmem_override.h
	@echo "Installed cmem to $(DESTDIR)$(PREFIX)"
	@echo "  Library: $(DESTDIR)$(LIBDIR)/$(LIBNAME)"
	@echo "  Headers: $(DESTDIR)$(INCLUDEDIR)/"

# 安装共享库 (可选)
install-shared: lib_shared | $(BUILD_DIR)
	install -d $(DESTDIR)$(LIBDIR)
	install -d $(DESTDIR)$(INCLUDEDIR)
	install -m 755 $(BUILD_DIR)/$(SONAME) $(DESTDIR)$(LIBDIR)/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(LIBDIR)/libcmem.so
	ln -sf $(SONAME) $(DESTDIR)$(LIBDIR)/libcmem.so.1
	install -m 644 include/cmem.h $(DESTDIR)$(INCLUDEDIR)/cmem.h
	install -m 644 include/cmem.hpp $(DESTDIR)$(INCLUDEDIR)/cmem.hpp
	install -m 644 include/cmem_pmr.hpp $(DESTDIR)$(INCLUDEDIR)/cmem_pmr.hpp
	install -m 644 include/cmem_override.h $(DESTDIR)$(INCLUDEDIR)/cmem_override.h
	@echo "Installed shared library cmem to $(DESTDIR)$(PREFIX)"

# 卸载
uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(LIBNAME)
	rm -f $(DESTDIR)$(LIBDIR)/libcmem.so
	rm -f $(DESTDIR)$(LIBDIR)/libcmem.so.1
	rm -f $(DESTDIR)$(LIBDIR)/$(SONAME)
	rm -f $(DESTDIR)$(INCLUDEDIR)/cmem.h
	rm -f $(DESTDIR)$(INCLUDEDIR)/cmem.hpp
	rm -f $(DESTDIR)$(INCLUDEDIR)/cmem_pmr.hpp
	rm -f $(DESTDIR)$(INCLUDEDIR)/cmem_override.h
	@echo "Uninstalled cmem from $(DESTDIR)$(PREFIX)"

# 创建源码发布包
package: distclean
	@echo "Creating source package..."
	cd .. && tar --exclude='*/build' --exclude='*/.git' --exclude='*/cmake-build-*' \
		--exclude='*/.vscode' --exclude='*/.idea' --exclude='*/.cache' \
		-czf cmem-$(VERSION).tar.gz cmem
	@echo "Package created: ../cmem-$(VERSION).tar.gz"

# Docker 可重现构建
docker-build:
	@echo "Building cmem in Docker (Ubuntu 22.04)..."
	docker build --rm -t cmem-build .
	@echo "Docker build complete. Run with: docker run -it cmem-build /bin/bash"

# 清理构建产物
clean:
	rm -rf $(BUILD_DIR)
	rm -f leak_report.txt test_report.html memory_profile.html
	rm -f snap_a.cmem_dump snap_b.cmem_dump test_snapshot.cmem_dump
	rm -f test_report.html

# 彻底清理
distclean: clean
	rm -f ../cmem-$(VERSION).tar.gz

# 开发模式：快速编译测试 (无 sanitizer)
test-fast: $(SRC) $(TEST_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) $(TEST_SRC) -o $(BUILD_DIR)/unit_tests_fast
	./$(BUILD_DIR)/unit_tests_fast

# 使用 CMake 构建 (备选)
cmake-build:
	cmake -B build_cmake -G Ninja -DCMAKE_BUILD_TYPE=Debug
	cmake --build build_cmake

cmake-test:
	ctest --test-dir build_cmake --output-on-failure

cmake-install:
	cmake --install build_cmake --prefix $(PREFIX)

# 检查代码风格 (需要 clang-format)
format-check:
	@which clang-format > /dev/null || (echo "clang-format not found"; exit 1)
	clang-format --dry-run --Werror \
		include/*.h include/*.hpp \
		src/*.c \
		tests/*.c tests/*.cpp \
		examples/*.c \
		benchmarks/*.c

format:
	@which clang-format > /dev/null || (echo "clang-format not found"; exit 1)
	clang-format -i \
		include/*.h include/*.hpp \
		src/*.c \
		tests/*.c tests/*.cpp \
		examples/*.c \
		benchmarks/*.c \
		tests/stress_test.c

# 静态分析 (需要 cppcheck)
static-analysis:
	@which cppcheck > /dev/null || (echo "cppcheck not found"; exit 1)
	cppcheck --enable=all --std=c11 --suppress=missingIncludeSystem -I include src/

# 代码覆盖率
coverage:
	@which lcov > /dev/null || (echo "lcov not found; install lcov"; exit 1)
	@which genhtml > /dev/null || (echo "genhtml not found; install lcov"; exit 1)
	rm -f coverage.info coverage.info.cleaned
	lcov --capture --initial --directory . --output-file coverage.info.cleaned --ignore-errors mismatch
	lcov --capture --directory . --output-file coverage.info --ignore-errors mismatch
	lcov --add-tracefile coverage.info.cleaned --add-tracefile coverage.info --output-file coverage.info.merged
	lcov --remove coverage.info.merged '/usr/*' '*/tests/*' --output-file coverage.info
	genhtml coverage.info --output-directory coverage_report --title "cmem Coverage Report"
	@echo "Coverage report generated at coverage_report/index.html"

# 性能回归测试
bench-regression:
	@echo "Running performance regression baseline..."
	@./build/bench_main > bench_baseline.txt 2>&1
	@echo "Baseline saved to bench_baseline.txt"

# 生成文档 (需要 doxygen)
docs:
	@which doxygen > /dev/null || (echo "doxygen not found"; exit 1)
	@test -f Doxyfile || (echo "Doxyfile not found, skipping docs generation"; exit 0)
	doxygen Doxyfile

# 检查 Mermaid 图语法 (需要 python3)
check-mermaid:
	@which python3 > /dev/null || (echo "python3 not found"; exit 1)
	@python3 scripts/check_mermaid.py