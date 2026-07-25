CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -O3 -std=c11 -I./include -pthread -lrt
CXXFLAGS = -Wall -Wextra -O3 -std=c++11 -I./include -pthread -lrt
CFLAGS_DEBUG = -Wall -Wextra -g -O0 -std=c11 -I./include -pthread -lrt -fsanitize=address,undefined
CXXFLAGS_DEBUG = -Wall -Wextra -g -O0 -std=c++11 -I./include -pthread -lrt -fsanitize=address,undefined

SRC = src/cmem.c
TEST_SRC = tests/test_main.c
CPP_TEST_SRC = tests/test_cpp.cpp
BENCH_SRC = benchmarks/bench_main.c

BUILD_DIR = build

all: lib test test_cpp bench examples

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

lib: $(SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $(SRC) -o $(BUILD_DIR)/cmem.o
	ar rcs $(BUILD_DIR)/libcmem.a $(BUILD_DIR)/cmem.o

test: $(SRC) $(TEST_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS_DEBUG) $(SRC) $(TEST_SRC) -o $(BUILD_DIR)/unit_tests
	./$(BUILD_DIR)/unit_tests

test_cpp: $(SRC) $(CPP_TEST_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS_DEBUG) $(SRC) $(CPP_TEST_SRC) -o $(BUILD_DIR)/cpp_tests
	./$(BUILD_DIR)/cpp_tests

bench: $(SRC) $(BENCH_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) $(BENCH_SRC) -o $(BUILD_DIR)/benchmark
	./$(BUILD_DIR)/benchmark

examples: $(SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) examples/example_basic.c -o $(BUILD_DIR)/example_basic
	$(CC) $(CFLAGS) $(SRC) examples/example_embedded.c -o $(BUILD_DIR)/example_embedded
	$(CC) $(CFLAGS) $(SRC) examples/example_leak_analysis.c -o $(BUILD_DIR)/example_leak_analysis
	$(CC) $(CFLAGS) $(SRC) examples/example_arena_tree.c -o $(BUILD_DIR)/example_arena_tree
	./$(BUILD_DIR)/example_basic
	./$(BUILD_DIR)/example_embedded
	./$(BUILD_DIR)/example_leak_analysis
	./$(BUILD_DIR)/example_arena_tree

clean:
	rm -rf $(BUILD_DIR) leak_report.txt test_report.html memory_profile.html

.PHONY: all lib test test_cpp bench examples clean
