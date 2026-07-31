cmake_minimum_required(VERSION 3.10)
project(cmem VERSION 1.0.0 LANGUAGES C CXX
        DESCRIPTION "Universal High-Performance Tiered Memory Manager"
        HOMEPAGE_URL "https://github.com/your-repo/cmem")

# 版本配置
set(CMEM_VERSION_MAJOR 1)
set(CMEM_VERSION_MINOR 0)
set(CMEM_VERSION_PATCH 0)
set(CMEM_VERSION ${CMEM_VERSION_MAJOR}.${CMEM_VERSION_MINOR}.${CMEM_VERSION_PATCH})

# C/C++ 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译选项
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pthread")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pthread")

# 代码格式检查
find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT} --dry-run --Werror
            ${CMAKE_CURRENT_SOURCE_DIR}/include/cmem.h
            ${CMAKE_CURRENT_SOURCE_DIR}/include/cmem.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/cmem_pmr.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/cmem_override.h
            ${CMAKE_CURRENT_SOURCE_DIR}/src/cmem.c
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_main.c
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_advanced.c
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cpp.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/bench_main.c
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/example_basic.c
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/example_embedded.c
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/example_leak_analysis.c
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/example_arena_tree.c
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/stress_test.c
        COMMENT "Checking code formatting with clang-format"
        VERBATIM
    )
else()
    add_custom_target(format-check
        COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found, skipping format check"
    )
endif()

# Clang-Tidy 静态分析
find_program(CLANG_TIDY clang-tidy)
if(CLANG_TIDY AND NOT DEFINED CMAKE_C_CLANG_TIDY)
    set(CMAKE_C_CLANG_TIDY "${CLANG_TIDY}")
endif()

# 构建类型默认 Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 代码覆盖率配置
set(CMAKE_C_FLAGS_COVERAGE "-g -O0 -fprofile-arcs -ftest-coverage")
set(CMAKE_CXX_FLAGS_COVERAGE "${CMAKE_C_FLAGS_COVERAGE}")
set(CMAKE_EXE_LINKER_FLAGS_COVERAGE "${CMAKE_EXE_LINKER_FLAGS} -fprofile-arcs -ftest-coverage")
set(CMAKE_SHARED_LINKER_FLAGS_COVERAGE "${CMAKE_SHARED_LINKER_FLAGS} -fprofile-arcs -ftest-coverage")

if(CMAKE_BUILD_TYPE STREQUAL "Coverage")
    message(STATUS "Building with code coverage instrumentation")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_COVERAGE}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_COVERAGE}")
endif()

# 包含目录
include_directories(include)

# 平台相关库
if(APPLE OR WIN32)
    set(RT_LIBRARY "")
else()
    find_library(RT_LIBRARY rt)
endif()

# 静态库目标
add_library(cmem STATIC src/cmem.c)
target_link_libraries(cmem PRIVATE pthread ${RT_LIBRARY})
add_dependencies(cmem format-check)
set_target_properties(cmem PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    VERSION ${CMEM_VERSION}
    SOVERSION ${CMEM_VERSION_MAJOR}
    PUBLIC_HEADER include/cmem.h
)

# 安装配置
install(TARGETS cmem
    EXPORT cmemTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    PUBLIC_HEADER DESTINATION include
)

install(FILES
    include/cmem.h
    include/cmem.hpp
    include/cmem_pmr.hpp
    include/cmem_override.h
    DESTINATION include
)

include(CMakePackageConfigHelpers)

# 生成 cmemConfig.cmake 用于 find_package 支持
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmemConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cmemConfig.cmake
    INSTALL_DESTINATION lib/cmake/cmem
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/cmemConfigVersion.cmake
    VERSION ${CMEM_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/cmemConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/cmemConfigVersion.cmake
    DESTINATION lib/cmake/cmem
)

# 导出目标
install(EXPORT cmemTargets
    FILE cmemTargets.cmake
    NAMESPACE cmem::
    DESTINATION lib/cmake/cmem
)

# pkg-config 支持
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmem.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/cmem.pc
    @ONLY
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cmem.pc
    DESTINATION lib/pkgconfig
)

# 测试可执行文件
add_executable(unit_tests tests/test_main.c)
target_link_libraries(unit_tests PRIVATE cmem pthread ${RT_LIBRARY})
add_dependencies(unit_tests format-check)

add_executable(advanced_tests tests/test_advanced.c)
target_link_libraries(advanced_tests PRIVATE cmem pthread ${RT_LIBRARY})
add_dependencies(advanced_tests format-check)

add_executable(cpp_tests tests/test_cpp.cpp)
target_link_libraries(cpp_tests PRIVATE cmem pthread ${RT_LIBRARY})
add_dependencies(cpp_tests format-check)

add_executable(benchmark benchmarks/bench_main.c)
target_link_libraries(benchmark PRIVATE cmem pthread ${RT_LIBRARY})
add_dependencies(benchmark format-check)

# 示例程序
add_executable(example_basic examples/example_basic.c)
target_link_libraries(example_basic PRIVATE cmem pthread ${RT_LIBRARY})
add_dependencies(example_basic format-check)

add_executable(example_embedded examples/example_embedded.c)
target_link_libraries(example_embedded PRIVATE cmem pthread ${RT_LIBRARY})
add_dependencies(example_embedded format-check)

add_executable(example_leak_analysis examples/example_leak_analysis.c)
target_link_libraries(example_leak_analysis PRIVATE cmem pthread ${RT_LIBRARY})
add_dependencies(example_leak_analysis format-check)

add_executable(example_arena_tree examples/example_arena_tree.c)
target_link_libraries(example_arena_tree PRIVATE cmem pthread ${RT_LIBRARY})
add_dependencies(example_arena_tree format-check)

add_executable(stress_test tests/stress_test.c)
target_link_libraries(stress_test PRIVATE cmem pthread ${RT_LIBRARY})
add_dependencies(stress_test format-check)

# CTest 集成
enable_testing()
add_test(NAME CUnitTests COMMAND unit_tests)
add_test(NAME CAdvancedUnitTests COMMAND advanced_tests)
add_test(NAME CppUnitTests COMMAND cpp_tests)

# 可选：安装示例程序
# install(PROGRAMS example_basic example_embedded example_leak_analysis example_arena_tree DESTINATION bin)

# 包配置 (CPack)
set(CPACK_PACKAGE_NAME "cmem")
set(CPACK_PACKAGE_VERSION ${CMEM_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Universal High-Performance Tiered Memory Manager")
set(CPACK_PACKAGE_VENDOR "cmem contributors")
set(CPACK_PACKAGE_CONTACT "cmem@example.com")
set(CPACK_GENERATOR "TGZ;DEB;RPM")
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
include(CPack)