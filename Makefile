CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -O3 -std=c11 -I./include -pthread
CXXFLAGS = -Wall -Wextra -O3 -std=c++11 -I./include -pthread
CFLAGS_DEBUG = -Wall -Wextra -g -O0 -std=c11 -I./include -pthread -fsanitize=address,undefined
CXXFLAGS_DEBUG = -Wall -Wextra -g -O0 -std=c++11 -I./include -pthread -fsanitize=address,undefined

SRC = src/memory_pool.c
TEST_SRC = tests/test_main.c
CPP_TEST_SRC = tests/test_cpp.cpp
BENCH_SRC = benchmarks/bench_main.c

BUILD_DIR = build

all: test test_cpp bench

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

test: $(SRC) $(TEST_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS_DEBUG) $(SRC) $(TEST_SRC) -o $(BUILD_DIR)/unit_tests
	./$(BUILD_DIR)/unit_tests

test_cpp: $(SRC) $(CPP_TEST_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS_DEBUG) $(SRC) $(CPP_TEST_SRC) -o $(BUILD_DIR)/cpp_tests
	./$(BUILD_DIR)/cpp_tests

bench: $(SRC) $(BENCH_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) $(BENCH_SRC) -o $(BUILD_DIR)/benchmark
	./$(BUILD_DIR)/benchmark

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all test test_cpp bench clean
