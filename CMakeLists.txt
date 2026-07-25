cmake_minimum_required(VERSION 3.10)
project(cmem C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pthread")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pthread")

include_directories(include)

add_library(cmem STATIC src/cmem.c)
target_link_libraries(cmem PRIVATE pthread rt)

add_executable(unit_tests tests/test_main.c)
target_link_libraries(unit_tests PRIVATE cmem pthread rt)

add_executable(cpp_tests tests/test_cpp.cpp)
target_link_libraries(cpp_tests PRIVATE cmem pthread rt)

add_executable(benchmark benchmarks/bench_main.c)
target_link_libraries(benchmark PRIVATE cmem pthread rt)

add_executable(example_basic examples/example_basic.c)
target_link_libraries(example_basic PRIVATE cmem pthread rt)

add_executable(example_embedded examples/example_embedded.c)
target_link_libraries(example_embedded PRIVATE cmem pthread rt)

add_executable(example_leak_analysis examples/example_leak_analysis.c)
target_link_libraries(example_leak_analysis PRIVATE cmem pthread rt)

add_executable(example_arena_tree examples/example_arena_tree.c)
target_link_libraries(example_arena_tree PRIVATE cmem pthread rt)

enable_testing()
add_test(NAME CUnitTests COMMAND unit_tests)
add_test(NAME CppUnitTests COMMAND cpp_tests)
