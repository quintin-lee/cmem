cmake_minimum_required(VERSION 3.10)
project(memory_pool C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pthread")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pthread")

include_directories(include)

add_library(memory_pool STATIC src/memory_pool.c)

add_executable(unit_tests tests/test_main.c)
target_link_libraries(unit_tests PRIVATE memory_pool pthread)

add_executable(cpp_tests tests/test_cpp.cpp)
target_link_libraries(cpp_tests PRIVATE memory_pool pthread)

add_executable(benchmark benchmarks/bench_main.c)
target_link_libraries(benchmark PRIVATE memory_pool pthread)

add_executable(example_basic examples/example_basic.c)
target_link_libraries(example_basic PRIVATE memory_pool pthread)

add_executable(example_embedded examples/example_embedded.c)
target_link_libraries(example_embedded PRIVATE memory_pool pthread)
