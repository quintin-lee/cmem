# cmem - Universal High-Performance Tiered Memory Manager
# Makefile with support for build, test, benchmark, install, uninstall, package

CC = gcc
CXX = g++
LDFLAGS = -pthread -lrt

# 版本信息（从 VERSION 文件读取）
VERSION := $(shell cat VERSION | tr -d '[:space:]')
LIBNAME = libcmem.a
SONAME = libcmem.so.$(VERSION)
CORE_SONAME = libcmem_core.so.$(VERSION)
DIAG_SONAME = libcmem_diag.so.$(VERSION)

# 构建配置（通过 CONFIG 变量选择，默认 release）
#   release -O3 性能测试
#   debug   -O0 -g 日常调试
#   asan    -fsanitize=address 内存错误
#   tsan    -fsanitize=thread 并发错误
#   ubsan   -fsanitize=undefined 未定义行为
CONFIG ?= release

ifeq ($(CONFIG),release)
  CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE -I./include
  CXXFLAGS = -Wall -Wextra -O3 -std=c++17 -D_GNU_SOURCE -I./include
  BUILD_DIR = build
else ifeq ($(CONFIG),debug)
  CFLAGS = -Wall -Wextra -O0 -g -std=c11 -D_GNU_SOURCE -I./include
  CXXFLAGS = -Wall -Wextra -O0 -g -std=c++17 -D_GNU_SOURCE -I./include
  BUILD_DIR = build-debug
else ifeq ($(CONFIG),asan)
  CFLAGS = -Wall -Wextra -fsanitize=address -g -O0 -std=c11 -D_GNU_SOURCE -I./include
  CXXFLAGS = -Wall -Wextra -fsanitize=address -g -O0 -std=c++17 -D_GNU_SOURCE -I./include
  BUILD_DIR = build-asan
else ifeq ($(CONFIG),tsan)
  CFLAGS = -Wall -Wextra -fsanitize=thread -g -O0 -std=c11 -D_GNU_SOURCE -I./include
  CXXFLAGS = -Wall -Wextra -fsanitize=thread -g -O0 -std=c++17 -D_GNU_SOURCE -I./include
  BUILD_DIR = build-tsan
else ifeq ($(CONFIG),ubsan)
  CFLAGS = -Wall -Wextra -fsanitize=undefined -g -O0 -std=c11 -D_GNU_SOURCE -I./include
  CXXFLAGS = -Wall -Wextra -fsanitize=undefined -g -O0 -std=c++17 -D_GNU_SOURCE -I./include
  BUILD_DIR = build-ubsan
else
  $(error Unknown CONFIG=$(CONFIG). Valid values: release, debug, asan, tsan, ubsan)
endif

SRC = $(wildcard src/*.c)
OBJS = $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRC))
TEST_SRC = tests/test_main.c
ADV_TEST_SRC = tests/test_advanced.c
STRESS_SRC = tests/stress_test.c
CPP_TEST_SRC = tests/test_cpp.cpp
BENCH_SRC = benchmarks/bench_main.c

PREFIX = /usr/local
LIBDIR = $(PREFIX)/lib
INCLUDEDIR = $(PREFIX)/include

# 默认目标
.PHONY: all lib test test_advanced test_all test_cpp bench examples clean install uninstall package distclean help format-check stress_test coverage bench-regression static-analysis docker-build fuzz-build fuzz-run fuzz-ci fuzz-clean check-mermaid tools debug asan tsan ubsan release

all: format-check check-mermaid lib test test_advanced test_cpp bench examples tools

debug:
	$(MAKE) CONFIG=debug all

asan:
	$(MAKE) CONFIG=asan all

tsan:
	$(MAKE) CONFIG=tsan all

ubsan:
	$(MAKE) CONFIG=ubsan all

release:
	$(MAKE) CONFIG=release all

help:
	@echo "cmem Makefile Targets:"
	@echo "  all          - Build library, run all tests, benchmarks, and examples (CONFIG=$(CONFIG))"
	@echo "  debug        - Build and test with Debug configuration (-O0 -g)"
	@echo "  asan         - Build and test with AddressSanitizer (-fsanitize=address)"
	@echo "  tsan         - Build and test with ThreadSanitizer (-fsanitize=thread)"
	@echo "  ubsan        - Build and test with UndefinedBehaviorSanitizer (-fsanitize=undefined)"
	@echo "  release      - Build and test with Release configuration (-O3)"
	@echo "  lib          - Build static library $(BUILD_DIR)/$(LIBNAME)"
	@echo "  lib_shared   - Build shared library $(BUILD_DIR)/$(SONAME)"
	@echo "  test         - Build and run C unit tests"
	@echo "  test_advanced- Build and run C advanced unit tests"
	@echo "  test_all     - Build and run all C tests"
	@echo "  test_cpp     - Build and run C++17 PMR/STL tests"
	@echo "  bench        - Build and run performance benchmarks"
	@echo "  bench-regression - Run performance regression baseline"
	@echo "  stress_test  - Build and run long-run high-concurrency stress test"
	@echo "  coverage     - Generate code coverage report (requires lcov)"
	@echo "  static-analysis - Run cppcheck static analysis"
	@echo "  examples     - Build and run all example programs"
	@echo "  tools        - Build diagnostic tools (cmem-inspect, cmem-analyze)"
	@echo "  install      - Install library and headers to $(PREFIX)"
	@echo "  uninstall    - Remove installed files from $(PREFIX)"
	@echo "  package      - Create source tarball for distribution"
	@echo "  docker-build - Build and test inside Docker container (Ubuntu 22.04)"
	@echo "  check-mermaid- Validate Mermaid diagram syntax in documentation"
	@echo "  clean        - Remove build artifacts"
	@echo "  distclean    - Remove build artifacts and generated files"
	@echo ""
	@echo "Variables:"
	@echo "  CONFIG       - Build configuration (release, debug, asan, tsan, ubsan; default: release)"
	@echo "  PREFIX       - Installation prefix (default: /usr/local)"
	@echo "  BUILD_DIR    - Build directory (default: build-$(CONFIG))"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

CORE_SRC = src/cmem.c src/cmem_slab.c src/cmem_tlsf.c src/cmem_sys.c src/cmem_event.c src/cmem_compress.c
CORE_OBJS = $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(CORE_SRC))

# 静态库 (核心分配器)
lib_core: format-check $(CORE_OBJS)
	ar rcs $(BUILD_DIR)/libcmem_core.a $(CORE_OBJS)
	cp $(BUILD_DIR)/libcmem_core.a $(BUILD_DIR)/libcmem_core-$(VERSION).a
	@echo "Built static library: $(BUILD_DIR)/libcmem_core.a"

# 静态库 (诊断扩展库)
lib_diag: format-check $(BUILD_DIR)/cmem_diag.o
	ar rcs $(BUILD_DIR)/libcmem_diag.a $(BUILD_DIR)/cmem_diag.o
	cp $(BUILD_DIR)/libcmem_diag.a $(BUILD_DIR)/libcmem_diag-$(VERSION).a
	@echo "Built static library: $(BUILD_DIR)/libcmem_diag.a"

# 静态库 (全功能合并库)
lib: format-check $(OBJS) lib_core lib_diag
	ar rcs $(BUILD_DIR)/$(LIBNAME) $(OBJS)
	cp $(BUILD_DIR)/$(LIBNAME) $(BUILD_DIR)/$(LIBNAME:.a=-$(VERSION).a)
	@echo "Built static library: $(BUILD_DIR)/$(LIBNAME)"

# 共享库
lib_shared: format-check | $(BUILD_DIR)
	$(CC) $(CFLAGS) -fPIC -shared -ftls-model=initial-exec $(SRC) -o $(BUILD_DIR)/$(SONAME) -Wl,-soname,libcmem.so.1 $(LDFLAGS)
	ln -sf $(SONAME) $(BUILD_DIR)/libcmem.so
	ln -sf $(SONAME) $(BUILD_DIR)/libcmem.so.1
	$(CC) $(CFLAGS) -fPIC -shared -ftls-model=initial-exec $(CORE_SRC) -o $(BUILD_DIR)/$(CORE_SONAME) -Wl,-soname,libcmem_core.so.1 $(LDFLAGS)
	ln -sf $(CORE_SONAME) $(BUILD_DIR)/libcmem_core.so
	ln -sf $(CORE_SONAME) $(BUILD_DIR)/libcmem_core.so.1
	$(CC) $(CFLAGS) -fPIC -shared -ftls-model=initial-exec src/cmem_diag.c -o $(BUILD_DIR)/$(DIAG_SONAME) -Wl,-soname,libcmem_diag.so.1 $(LDFLAGS) -L$(BUILD_DIR) -lcmem_core
	ln -sf $(DIAG_SONAME) $(BUILD_DIR)/libcmem_diag.so
	ln -sf $(DIAG_SONAME) $(BUILD_DIR)/libcmem_diag.so.1
	@echo "Built shared libraries: $(BUILD_DIR)/$(SONAME), $(BUILD_DIR)/$(CORE_SONAME), $(BUILD_DIR)/$(DIAG_SONAME)"

ASAN_SO := $(shell find /usr/lib /usr/lib64 /usr/lib32 -name "libasan.so*" 2>/dev/null | head -n 1)
RUN_ASAN = $(if $(filter asan,$(CONFIG)),$(if $(ASAN_SO),LD_PRELOAD=$(ASAN_SO),),)

# C 单元测试
test: format-check $(SRC) $(TEST_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) $(TEST_SRC) -o $(BUILD_DIR)/unit_tests $(LDFLAGS)
	@echo "Running C unit tests..."
	$(RUN_ASAN) LSAN_OPTIONS=detect_leaks=0 ./$(BUILD_DIR)/unit_tests

# C 高级单元测试
test_advanced: format-check $(SRC) $(ADV_TEST_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) $(ADV_TEST_SRC) -o $(BUILD_DIR)/advanced_tests $(LDFLAGS)
	@echo "Running C advanced unit tests..."
	$(RUN_ASAN) LSAN_OPTIONS=detect_leaks=0 ./$(BUILD_DIR)/advanced_tests

# 长时间高并发压测
stress_test: format-check $(SRC) $(STRESS_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(STRESS_DEFINES) $(SRC) $(STRESS_SRC) -o $(BUILD_DIR)/stress_test $(LDFLAGS)
	@echo "Running long-run stress test..."
	./$(BUILD_DIR)/stress_test

# 运行所有 C 测试
test_all: test test_advanced

# C++ 测试
test_cpp: format-check $(SRC) $(CPP_TEST_SRC) | $(BUILD_DIR)
	@set -e; for src in $(SRC); do \
		$(CC) $(CFLAGS) -c $$src -o $(BUILD_DIR)/$$(basename $$src .c).o; \
	done; \
	$(CXX) $(CXXFLAGS) -c $(CPP_TEST_SRC) -o $(BUILD_DIR)/test_cpp.o; \
	$(CXX) $(CXXFLAGS) $(addprefix $(BUILD_DIR)/,$(notdir $(patsubst %.c,%.o,$(SRC)))) $(BUILD_DIR)/test_cpp.o -o $(BUILD_DIR)/cpp_tests $(LDFLAGS)
	@echo "Running C++ tests..."
	$(RUN_ASAN) LSAN_OPTIONS=detect_leaks=0 ./$(BUILD_DIR)/cpp_tests

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

# 构建诊断工具
tools: cmem-inspect cmem-analyze

cmem-inspect: lib
	$(CC) $(CFLAGS) -I./include -I./tools/common tools/cmem-inspect/cmem-inspect.c tools/common/cmem-diag-output.c -o $(BUILD_DIR)/cmem-inspect -L$(BUILD_DIR) -lcmem -lpthread $(LDFLAGS)

cmem-analyze: tools/common/cmem-diag-output.c
	$(CC) $(CFLAGS) -I./include -I./tools/common tools/cmem-analyze/cmem-analyze.c tools/cmem-analyze/cmem-analyze-parser.c tools/common/cmem-diag-output.c -o $(BUILD_DIR)/cmem-analyze -lpthread $(LDFLAGS)

# Fuzzing targets (requires clang; falls back to ASan-only on gcc)
FUZZ_SRCS = tests/fuzz_alloc.c src/*.c
FUZZ_CC ?= clang
FUZZ_CFLAGS = -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -O1 -g -D_GNU_SOURCE -I./include
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
	install -m 644 $(BUILD_DIR)/$(LIBNAME:.a=-$(VERSION).a) $(DESTDIR)$(LIBDIR)/$(LIBNAME:.a=-$(VERSION).a)
	install -m 644 $(BUILD_DIR)/libcmem_core.a $(DESTDIR)$(LIBDIR)/libcmem_core.a
	install -m 644 $(BUILD_DIR)/libcmem_core-$(VERSION).a $(DESTDIR)$(LIBDIR)/libcmem_core-$(VERSION).a
	install -m 644 $(BUILD_DIR)/libcmem_diag.a $(DESTDIR)$(LIBDIR)/libcmem_diag.a
	install -m 644 $(BUILD_DIR)/libcmem_diag-$(VERSION).a $(DESTDIR)$(LIBDIR)/libcmem_diag-$(VERSION).a
	install -m 644 include/cmem.h $(DESTDIR)$(INCLUDEDIR)/cmem.h
	install -m 644 include/cmem_diag.h $(DESTDIR)$(INCLUDEDIR)/cmem_diag.h
	install -m 644 include/cmem_ring.h $(DESTDIR)$(INCLUDEDIR)/cmem_ring.h
	install -m 644 include/cmem_tlsf.h $(DESTDIR)$(INCLUDEDIR)/cmem_tlsf.h
	install -m 644 include/cmem_snapshot.h $(DESTDIR)$(INCLUDEDIR)/cmem_snapshot.h
	install -m 644 include/cmem_metrics.h $(DESTDIR)$(INCLUDEDIR)/cmem_metrics.h
	install -m 644 include/cmem_arena.h $(DESTDIR)$(INCLUDEDIR)/cmem_arena.h
	install -m 644 include/cmem_frame.h $(DESTDIR)$(INCLUDEDIR)/cmem_frame.h
	install -m 644 include/cmem_typed_pool.h $(DESTDIR)$(INCLUDEDIR)/cmem_typed_pool.h
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
	install -m 755 $(BUILD_DIR)/$(CORE_SONAME) $(DESTDIR)$(LIBDIR)/$(CORE_SONAME)
	ln -sf $(CORE_SONAME) $(DESTDIR)$(LIBDIR)/libcmem_core.so
	ln -sf $(CORE_SONAME) $(DESTDIR)$(LIBDIR)/libcmem_core.so.1
	install -m 755 $(BUILD_DIR)/$(DIAG_SONAME) $(DESTDIR)$(LIBDIR)/$(DIAG_SONAME)
	ln -sf $(DIAG_SONAME) $(DESTDIR)$(LIBDIR)/libcmem_diag.so
	ln -sf $(DIAG_SONAME) $(DESTDIR)$(LIBDIR)/libcmem_diag.so.1
	install -m 644 include/cmem.h $(DESTDIR)$(INCLUDEDIR)/cmem.h
	install -m 644 include/cmem_diag.h $(DESTDIR)$(INCLUDEDIR)/cmem_diag.h
	install -m 644 include/cmem_ring.h $(DESTDIR)$(INCLUDEDIR)/cmem_ring.h
	install -m 644 include/cmem_tlsf.h $(DESTDIR)$(INCLUDEDIR)/cmem_tlsf.h
	install -m 644 include/cmem_snapshot.h $(DESTDIR)$(INCLUDEDIR)/cmem_snapshot.h
	install -m 644 include/cmem_metrics.h $(DESTDIR)$(INCLUDEDIR)/cmem_metrics.h
	install -m 644 include/cmem_arena.h $(DESTDIR)$(INCLUDEDIR)/cmem_arena.h
	install -m 644 include/cmem_frame.h $(DESTDIR)$(INCLUDEDIR)/cmem_frame.h
	install -m 644 include/cmem_typed_pool.h $(DESTDIR)$(INCLUDEDIR)/cmem_typed_pool.h
	install -m 644 include/cmem.hpp $(DESTDIR)$(INCLUDEDIR)/cmem.hpp
	install -m 644 include/cmem_pmr.hpp $(DESTDIR)$(INCLUDEDIR)/cmem_pmr.hpp
	install -m 644 include/cmem_override.h $(DESTDIR)$(INCLUDEDIR)/cmem_override.h
	@echo "Installed shared library cmem to $(DESTDIR)$(PREFIX)"

# 卸载
uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(LIBNAME)
	rm -f $(DESTDIR)$(LIBDIR)/$(LIBNAME:.a=-$(VERSION).a)
	rm -f $(DESTDIR)$(LIBDIR)/libcmem_core.a
	rm -f $(DESTDIR)$(LIBDIR)/libcmem_core-$(VERSION).a
	rm -f $(DESTDIR)$(LIBDIR)/libcmem_diag.a
	rm -f $(DESTDIR)$(LIBDIR)/libcmem_diag-$(VERSION).a
	rm -f $(DESTDIR)$(LIBDIR)/libcmem.so
	rm -f $(DESTDIR)$(LIBDIR)/libcmem.so.1
	rm -f $(DESTDIR)$(LIBDIR)/$(SONAME)
	rm -f $(DESTDIR)$(LIBDIR)/libcmem_core.so
	rm -f $(DESTDIR)$(LIBDIR)/libcmem_core.so.1
	rm -f $(DESTDIR)$(LIBDIR)/$(CORE_SONAME)
	rm -f $(DESTDIR)$(LIBDIR)/libcmem_diag.so
	rm -f $(DESTDIR)$(LIBDIR)/libcmem_diag.so.1
	rm -f $(DESTDIR)$(LIBDIR)/$(DIAG_SONAME)
	rm -f $(DESTDIR)$(INCLUDEDIR)/cmem.h
	rm -f $(DESTDIR)$(INCLUDEDIR)/cmem_diag.h
	rm -f $(DESTDIR)$(INCLUDEDIR)/cmem_ring.h
	rm -f $(DESTDIR)$(INCLUDEDIR)/cmem_tlsf.h
	rm -f $(DESTDIR)$(INCLUDEDIR)/cmem_snapshot.h
	rm -f $(DESTDIR)$(INCLUDEDIR)/cmem_metrics.h
	rm -f $(DESTDIR)$(INCLUDEDIR)/cmem_arena.h
	rm -f $(DESTDIR)$(INCLUDEDIR)/cmem_frame.h
	rm -f $(DESTDIR)$(INCLUDEDIR)/cmem_typed_pool.h
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
	rm -rf $(BUILD_DIR) build-debug build-asan build-tsan build-ubsan
	rm -f leak_report.txt test_report.html memory_profile.html
	rm -f snap_a.cmem_dump snap_b.cmem_dump test_snapshot.cmem_dump
	rm -f test_report.html snapshot_diff.txt

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