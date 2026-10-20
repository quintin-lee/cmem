# Makefile兼容模式

# 构建目录和配置
BUILD_DIR = build-$(CONFIG)
CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE -I./include
CXXFLAGS = -Wall -Wextra -O3 -std=c++17 -D_GNU_SOURCE -I./include
LDFLAGS = -pthread -lrt

# 根据不同的CONFIG进行处理
ifeq ($(CONFIG),debug)
    CFLAGS = -Wall -Wextra -O0 -g -std=c11 -D_GNU_SOURCE -I./include
    CXXFLAGS = -Wall -Wextra -O0 -g -std=c++17 -D_GNU_SOURCE -I./include
else ifeq ($(CONFIG),asan)
    CFLAGS = -Wall -Wextra -fsanitize=address -g -O0 -std=c11 -D_GNU_SOURCE -I./include
    CXXFLAGS = -Wall -Wextra -fsanitize=address -g -O0 -std=c++17 -D_GNU_SOURCE -I./include
else ifeq ($(CONFIG),tsan)
    CFLAGS = -Wall -Wextra -fsanitize=thread -g -O0 -std=c11 -D_GNU_SOURCE -I./include
    CXXFLAGS = -Wall -Wextra -fsanitize=thread -g -O0 -std=c++17 -D_GNU_SOURCE -I./include
else ifeq ($(CONFIG),ubsan)
    CFLAGS = -Wall -Wextra -fsanitize=undefined -g -O0 -std=c11 -D_GNU_SOURCE -I./include
    CXXFLAGS = -Wall -Wextra -fsanitize=undefined -g -O0 -std=c++17 -D_GNU_SOURCE -I./include
endif

# 获取所有源文件
ALL_SRC = $(wildcard src/*.c)
ALL_TEST_SRC = $(wildcard tests/*.c tests/*.cpp)
ALL_EXAMPLE_SRC = $(wildcard examples/*.c)
ALL_BENCH_SRC = $(wildcard benchmarks/*.c)
ALL_TOOLS_SRC = $(wildcard tools/*/cmem-*.c)
ALL_COMMON_TOOLS_SRC = $(wildcard tools/common/*.c)

# 自动推导目标
CORE_SRCS = $(filter src/cmem.c src/cmem_slab.c src/cmem_tlsf.c src/cmem_sys.c src/cmem_event.c src/cmem_compress.c, $(ALL_SRC))
DIAG_SRCS = src/cmem_diag.c
ALL_SRCS = $(ALL_SRC)

# 构建目录规则
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# 默认目标
.PHONY: all test test_advanced test_all test_cpp bench examples tools clean help

all: format-check check-mermaid lib test test_advanced test_cpp bench examples tools

# 库目标
lib: format-check lib_core lib_diag
	$(AR) rcs $(BUILD_DIR)/$(LIBNAME) $(wildcard $(BUILD_DIR)/*.o) $(BUILD_DIR)/cmem_diag.o
	cp $(BUILD_DIR)/$(LIBNAME) $(BUILD_DIR)/$(LIBNAME:.a=-$(VERSION).a)
	@echo "Built static library: $(BUILD_DIR)/$(LIBNAME)"

lib_core: format-check $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(CORE_SRCS))
	$(AR) rcs $(BUILD_DIR)/libcmem_core.a $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(CORE_SRCS))
	cp $(BUILD_DIR)/libcmem_core.a $(BUILD_DIR)/libcmem_core-$(VERSION).a
	@echo "Built static library: $(BUILD_DIR)/libcmem_core.a"

lib_diag: format-check $(BUILD_DIR)/cmem_diag.o
	$(AR) rcs $(BUILD_DIR)/libcmem_diag.a $(BUILD_DIR)/cmem_diag.o
	cp $(BUILD_DIR)/libcmem_diag.a $(BUILD_DIR)/libcmem_diag-$(VERSION).a
	@echo "Built static library: $(BUILD_DIR)/libcmem_diag.a"

# 构建程序
define build_program
	$(CC) $(CFLAGS) $(1) -o $(BUILD_DIR)/$(shell basename $(1) .c) $(LDFLAGS)
endef

# 测试目标
test: format-check $(ALL_SRCS) tests/test_main.c | $(BUILD_DIR)
	$(call build_program,$(ALL_SRCS) tests/test_main.c)
	@echo "Running C unit tests..."
	./$(BUILD_DIR)/unit_tests

test_advanced: format-check $(ALL_SRCS) tests/test_advanced.c | $(BUILD_DIR)
	$(call build_program,$(ALL_SRCS) tests/test_advanced.c)
	@echo "Running C advanced unit tests..."
	./$(BUILD_DIR)/advanced_tests

test_cpp: format-check $(ALL_SRCS) tests/test_cpp.cpp | $(BUILD_DIR)
	@set -e; for src in $(CORE_SRCS); do \
		$(CC) $(CFLAGS) -c $$src -o $(BUILD_DIR)/$$(basename $$src .c).o; \
	done; \
	$(CXX) $(CXXFLAGS) -c tests/test_cpp.cpp -o $(BUILD_DIR)/test_cpp.o; \
	$(CXX) $(CXXFLAGS) $(addprefix $(BUILD_DIR)/,$(notdir $(patsubst %.c,%.o,$(CORE_SRCS)))) $(BUILD_DIR)/test_cpp.o -o $(BUILD_DIR)/cpp_tests $(LDFLAGS)
	@echo "Running C++ tests..."
	./$(BUILD_DIR)/cpp_tests

test_all: test test_advanced

bench: format-check $(ALL_SRCS) benchmarks/bench_main.c | $(BUILD_DIR)
	$(call build_program,$(ALL_SRCS) benchmarks/bench_main.c)
	@echo "Running benchmarks..."
	./$(BUILD_DIR)/benchmark

examples: format-check $(ALL_SRCS) | $(BUILD_DIR)
	$(call build_program,$(ALL_SRCS) examples/example_basic.c)
	$(call build_program,$(ALL_SRCS) examples/example_embedded.c)
	$(call build_program,$(ALL_SRCS) examples/example_leak_analysis.c)
	$(call build_program,$(ALL_SRCS) examples/example_arena_tree.c)
	@echo "Running examples..."
	./$(BUILD_DIR)/example_basic
	./$(BUILD_DIR)/example_embedded
	./$(BUILD_DIR)/example_leak_analysis
	./$(BUILD_DIR)/example_arena_tree

tools: cmem-inspect cmem-analyze

cmem-inspect: lib
	$(CC) $(CFLAGS) -I./include -I./tools/common tools/cmem-inspect/cmem-inspect.c $(ALL_COMMON_TOOLS_SRC) -o $(BUILD_DIR)/cmem-inspect -L$(BUILD_DIR) -lcmem -lpthread $(LDFLAGS)

cmem-analyze: tools/common/cmem-diag-output.c
	$(CC) $(CFLAGS) -I./include -I./tools/common tools/cmem-analyze/cmem-analyze.c tools/cmem-analyze/cmem-analyze-parser.c tools/common/cmem-diag-output.c -o $(BUILD_DIR)/cmem-analyze -lpthread $(LDFLAGS)

# 压力测试
stress_test: format-check $(ALL_SRCS) tests/stress_test.c | $(BUILD_DIR)
	$(call build_program,$(ALL_SRCS) tests/stress_test.c)
	@echo "Running stress test..."
	./$(BUILD_DIR)/stress_test

# 辅助目标
help:
	@echo "cmem Unified Build System (CMake-first with Makefile compatibility)"
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
	@echo "  stress_test    - 运行压力测试"
	@echo "  clean          - 删除构建产物"
	@echo "  help           - 显示帮助信息"
	@echo ""
	@echo "变量:"
	@echo "  CONFIG         - 构建系统 (cmake/make/debug/asan/tsan/ubsan; 默认: cmake)"
	@echo "  PREFIX         - 安装前缀 (默认: /usr/local)"
	@echo "  BUILD_DIR      - Makefile构建的目录 (默认: build-$(CONFIG))"
	@echo "  CFLAGS         - C编译器标志"
	@echo "  CXXFLAGS       - C++编译器标志"
