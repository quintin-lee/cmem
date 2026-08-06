cmake_minimum_required(VERSION 3.10)

# Read version from unified VERSION file
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" _version_str)
string(STRIP "${_version_str}" _version_str)
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$" _version_match "${_version_str}")
if(NOT _version_match)
    message(FATAL_ERROR "Invalid VERSION file format: '${_version_str}'. Expected MAJOR.MINOR.PATCH")
endif()
set(CMEM_VERSION_MAJOR ${CMAKE_MATCH_1})
set(CMEM_VERSION_MINOR ${CMAKE_MATCH_2})
set(CMEM_VERSION_PATCH ${CMAKE_MATCH_3})
set(CMEM_VERSION "${CMEM_VERSION_MAJOR}.${CMEM_VERSION_MINOR}.${CMEM_VERSION_PATCH}")

project(cmem VERSION ${CMEM_VERSION} LANGUAGES C CXX
        DESCRIPTION "Universal High-Performance Tiered Memory Manager"
        HOMEPAGE_URL "https://github.com/your-repo/cmem")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# C/C++ 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT MSVC)
    set(CMAKE_C_EXTENSIONS ON)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译选项
if(MSVC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /W3 /std:c11")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3 /std:c11")
    add_compile_definitions(_STDC_LIMIT_MACROS _STDC_FORMAT_MACROS)
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pthread -D_GNU_SOURCE")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pthread -D_GNU_SOURCE")
endif()

# 代码格式检查
find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT} --dry-run --Werror
            ${CMAKE_CURRENT_SOURCE_DIR}/include/cmem.h
            ${CMAKE_CURRENT_SOURCE_DIR}/include/cmem_diag.h
            ${CMAKE_CURRENT_SOURCE_DIR}/include/cmem_ring.h
            ${CMAKE_CURRENT_SOURCE_DIR}/include/cmem_tlsf.h
            ${CMAKE_CURRENT_SOURCE_DIR}/include/cmem_snapshot.h
            ${CMAKE_CURRENT_SOURCE_DIR}/include/cmem_metrics.h
            ${CMAKE_CURRENT_SOURCE_DIR}/include/cmem_arena.h
            ${CMAKE_CURRENT_SOURCE_DIR}/include/cmem_frame.h
            ${CMAKE_CURRENT_SOURCE_DIR}/include/cmem_typed_pool.h
            ${CMAKE_CURRENT_SOURCE_DIR}/include/cmem.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/cmem_pmr.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/cmem_override.h
            ${CMAKE_CURRENT_SOURCE_DIR}/src/cmem.c
            ${CMAKE_CURRENT_SOURCE_DIR}/src/cmem_slab.c
            ${CMAKE_CURRENT_SOURCE_DIR}/src/cmem_tlsf.c
            ${CMAKE_CURRENT_SOURCE_DIR}/src/cmem_sys.c
            ${CMAKE_CURRENT_SOURCE_DIR}/src/cmem_diag.c
            ${CMAKE_CURRENT_SOURCE_DIR}/src/cmem_event.c
            ${CMAKE_CURRENT_SOURCE_DIR}/src/cmem_compress.c
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

# Mermaid 图语法检查
find_program(PYTHON_EXECUTABLE python3)
if(PYTHON_EXECUTABLE)
    add_custom_target(check-mermaid
        COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_mermaid.py ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Validating Mermaid diagram syntax"
        VERBATIM
    )
else()
    add_custom_target(check-mermaid
        COMMAND ${CMAKE_COMMAND} -E echo "python3 not found, skipping Mermaid check"
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

option(CMEM_BUILD_SHARED "Build shared library" ON)
option(CMEM_ENABLE_DIAGNOSTICS "Enable diagnostics and telemetry subsystem" ON)

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

# Custom build types for sanitizers
set(CMAKE_C_FLAGS_ASAN "-fsanitize=address -g -O0" CACHE STRING "Flags for ASan build" FORCE)
set(CMAKE_CXX_FLAGS_ASAN "-fsanitize=address -g -O0" CACHE STRING "Flags for ASan build" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_ASAN "-fsanitize=address" CACHE STRING "Linker flags for ASan build" FORCE)
set(CMAKE_C_FLAGS_TSAN "-fsanitize=thread -g -O0" CACHE STRING "Flags for TSan build" FORCE)
set(CMAKE_CXX_FLAGS_TSAN "-fsanitize=thread -g -O0" CACHE STRING "Flags for TSan build" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_TSAN "-fsanitize=thread" CACHE STRING "Linker flags for TSan build" FORCE)
set(CMAKE_C_FLAGS_UBSAN "-fsanitize=undefined -g -O0" CACHE STRING "Flags for UBSan build" FORCE)
set(CMAKE_CXX_FLAGS_UBSAN "-fsanitize=undefined -g -O0" CACHE STRING "Flags for UBSan build" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_UBSAN "-fsanitize=undefined" CACHE STRING "Linker flags for UBSan build" FORCE)

# 包含目录
include_directories(include)

# 平台相关库
if(APPLE OR WIN32)
    set(RT_LIBRARY "")
else()
    find_library(RT_LIBRARY rt)
endif()

set(CMEM_CORE_SRCS
    src/cmem.c
    src/cmem_slab.c
    src/cmem_tlsf.c
    src/cmem_sys.c
    src/cmem_event.c
    src/cmem_compress.c
)

# 1. 核心分配器静态库与动态库目标 (libcmem_core)
add_library(cmem_core STATIC ${CMEM_CORE_SRCS})
target_link_libraries(cmem_core PRIVATE pthread ${RT_LIBRARY})
add_dependencies(cmem_core format-check check-mermaid)
set_target_properties(cmem_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    VERSION ${CMEM_VERSION}
    SOVERSION ${CMEM_VERSION_MAJOR}
)

if(CMEM_BUILD_SHARED)
    add_library(cmem_core_shared SHARED ${CMEM_CORE_SRCS})
    target_link_libraries(cmem_core_shared PRIVATE pthread ${RT_LIBRARY})
    # PRIVATE: the define must only affect cmem_core_shared's own compilation
    # (compiling out mp_check_leaks() references in cmem_event.c). PUBLIC would
    # propagate it to cmem_diag_shared, which links this target but needs the
    # diagnostic types (mp_leak_severity_t, mp_leak_pattern_t) from cmem_diag.h.
    target_compile_definitions(cmem_core_shared PRIVATE CMEM_DISABLE_DIAGNOSTICS)
    set_target_properties(cmem_core_shared PROPERTIES
        OUTPUT_NAME cmem_core
        VERSION ${CMEM_VERSION}
        SOVERSION ${CMEM_VERSION_MAJOR}
    )
endif()

# 2. 完整合并库与诊断库目标 (libcmem & libcmem_diag)
if(CMEM_ENABLE_DIAGNOSTICS)
    add_library(cmem STATIC ${CMEM_CORE_SRCS} src/cmem_diag.c)
    target_link_libraries(cmem PRIVATE pthread ${RT_LIBRARY})
    add_dependencies(cmem format-check check-mermaid)
    set_target_properties(cmem PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        VERSION ${CMEM_VERSION}
        SOVERSION ${CMEM_VERSION_MAJOR}
        PUBLIC_HEADER include/cmem.h
    )

    add_library(cmem_diag STATIC src/cmem_diag.c)
    target_link_libraries(cmem_diag PRIVATE cmem_core pthread ${RT_LIBRARY})
    set_target_properties(cmem_diag PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        VERSION ${CMEM_VERSION}
        SOVERSION ${CMEM_VERSION_MAJOR}
    )

    if(CMEM_BUILD_SHARED)
        add_library(cmem_shared SHARED ${CMEM_CORE_SRCS} src/cmem_diag.c)
        target_link_libraries(cmem_shared PRIVATE pthread ${RT_LIBRARY})
        set_target_properties(cmem_shared PROPERTIES
            VERSION ${CMEM_VERSION}
            SOVERSION ${CMEM_VERSION_MAJOR}
            PUBLIC_HEADER include/cmem.h
            OUTPUT_NAME cmem
        )
        if(NOT MSVC)
            set_target_properties(cmem_shared PROPERTIES PREFIX "lib" SUFFIX ".so")
            target_compile_options(cmem_shared PRIVATE -ftls-model=initial-exec)
        endif()

        add_library(cmem_diag_shared SHARED src/cmem_diag.c)
        target_link_libraries(cmem_diag_shared PRIVATE cmem_core_shared pthread ${RT_LIBRARY})
        set_target_properties(cmem_diag_shared PROPERTIES
            OUTPUT_NAME cmem_diag
            VERSION ${CMEM_VERSION}
            SOVERSION ${CMEM_VERSION_MAJOR}
        )
        if(NOT MSVC)
            set_target_properties(cmem_diag_shared PROPERTIES PREFIX "lib" SUFFIX ".so")
            target_compile_options(cmem_diag_shared PRIVATE -ftls-model=initial-exec)
        endif()

        install(TARGETS cmem_core_shared cmem_shared cmem_diag_shared
            EXPORT cmemTargets
            LIBRARY DESTINATION lib
            RUNTIME DESTINATION bin
            PUBLIC_HEADER DESTINATION include
        )
        install(TARGETS cmem_core cmem cmem_diag
            EXPORT cmemTargets
            ARCHIVE DESTINATION lib
            LIBRARY DESTINATION lib
            RUNTIME DESTINATION bin
            PUBLIC_HEADER DESTINATION include
        )
    else()
        install(TARGETS cmem_core cmem cmem_diag
            EXPORT cmemTargets
            ARCHIVE DESTINATION lib
            LIBRARY DESTINATION lib
            RUNTIME DESTINATION bin
            PUBLIC_HEADER DESTINATION include
        )
    endif()
else()
    target_compile_definitions(cmem_core PUBLIC CMEM_DISABLE_DIAGNOSTICS)
    add_library(cmem STATIC ${CMEM_CORE_SRCS})
    target_link_libraries(cmem PRIVATE pthread ${RT_LIBRARY})
    target_compile_definitions(cmem PUBLIC CMEM_DISABLE_DIAGNOSTICS)
    set_target_properties(cmem PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        VERSION ${CMEM_VERSION}
        SOVERSION ${CMEM_VERSION_MAJOR}
    )

    if(CMEM_BUILD_SHARED)
        target_compile_definitions(cmem_core_shared PUBLIC CMEM_DISABLE_DIAGNOSTICS)
        add_library(cmem_shared SHARED ${CMEM_CORE_SRCS})
        target_link_libraries(cmem_shared PRIVATE pthread ${RT_LIBRARY})
        target_compile_definitions(cmem_shared PUBLIC CMEM_DISABLE_DIAGNOSTICS)
        set_target_properties(cmem_shared PROPERTIES
            OUTPUT_NAME cmem
            VERSION ${CMEM_VERSION}
            SOVERSION ${CMEM_VERSION_MAJOR}
        )
        install(TARGETS cmem_core_shared cmem_shared
            EXPORT cmemTargets
            LIBRARY DESTINATION lib
            RUNTIME DESTINATION bin
            PUBLIC_HEADER DESTINATION include
        )
    endif()
    install(TARGETS cmem_core cmem
        EXPORT cmemTargets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
        PUBLIC_HEADER DESTINATION include
    )
endif()

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/libcmem-${CMEM_VERSION}.a
    ${CMAKE_CURRENT_BINARY_DIR}/libcmem_core-${CMEM_VERSION}.a
    ${CMAKE_CURRENT_BINARY_DIR}/libcmem_diag-${CMEM_VERSION}.a
    DESTINATION lib
    OPTIONAL
)

install(FILES
    include/cmem.h
    include/cmem_diag.h
    include/cmem_ring.h
    include/cmem_tlsf.h
    include/cmem_snapshot.h
    include/cmem_metrics.h
    include/cmem_arena.h
    include/cmem_frame.h
    include/cmem_typed_pool.h
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

if(CMEM_ENABLE_DIAGNOSTICS)
    # 测试可执行文件
    add_executable(unit_tests tests/test_main.c)
    target_link_libraries(unit_tests PRIVATE cmem pthread ${RT_LIBRARY})
    add_dependencies(unit_tests format-check check-mermaid)

    add_executable(advanced_tests tests/test_advanced.c)
    target_link_libraries(advanced_tests PRIVATE cmem pthread ${RT_LIBRARY})
    add_dependencies(advanced_tests format-check check-mermaid)

    add_executable(cpp_tests tests/test_cpp.cpp)
    target_link_libraries(cpp_tests PRIVATE cmem pthread ${RT_LIBRARY})
    add_dependencies(cpp_tests format-check check-mermaid)

    # CTest 集成
    enable_testing()
    add_test(NAME CUnitTests COMMAND unit_tests)
    add_test(NAME CAdvancedUnitTests COMMAND advanced_tests)
    add_test(NAME CppUnitTests COMMAND cpp_tests)
    if(CMAKE_BUILD_TYPE STREQUAL "ASan")
        find_library(ASAN_LIB asan)
        if(ASAN_LIB)
            set_tests_properties(CUnitTests CAdvancedUnitTests CppUnitTests PROPERTIES
                ENVIRONMENT "LD_PRELOAD=${ASAN_LIB}")
        endif()
    endif()
endif()

add_executable(benchmark benchmarks/bench_main.c)
target_link_libraries(benchmark PRIVATE cmem pthread ${RT_LIBRARY})
add_dependencies(benchmark format-check check-mermaid)

# 示例程序
add_executable(example_basic examples/example_basic.c)
target_link_libraries(example_basic PRIVATE cmem pthread ${RT_LIBRARY})
add_dependencies(example_basic format-check check-mermaid)

add_executable(example_embedded examples/example_embedded.c)
target_link_libraries(example_embedded PRIVATE cmem pthread ${RT_LIBRARY})
add_dependencies(example_embedded format-check check-mermaid)

if(CMEM_ENABLE_DIAGNOSTICS)
    add_executable(example_leak_analysis examples/example_leak_analysis.c)
    target_link_libraries(example_leak_analysis PRIVATE cmem pthread ${RT_LIBRARY})
    add_dependencies(example_leak_analysis format-check check-mermaid)

    add_executable(example_arena_tree examples/example_arena_tree.c)
    target_link_libraries(example_arena_tree PRIVATE cmem pthread ${RT_LIBRARY})
    add_dependencies(example_arena_tree format-check check-mermaid)

    add_executable(cmem-inspect tools/cmem-inspect/cmem-inspect.c tools/common/cmem-diag-output.c)
    target_link_libraries(cmem-inspect PRIVATE cmem pthread ${RT_LIBRARY})
    target_include_directories(cmem-inspect PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/common)
    add_dependencies(cmem-inspect format-check check-mermaid)
endif()

add_executable(cmem-analyze tools/cmem-analyze/cmem-analyze.c tools/cmem-analyze/cmem-analyze-parser.c tools/common/cmem-diag-output.c)
target_link_libraries(cmem-analyze PRIVATE pthread ${RT_LIBRARY})
target_include_directories(cmem-analyze PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/common)
add_dependencies(cmem-analyze format-check check-mermaid)

add_executable(stress_test tests/stress_test.c)
target_link_libraries(stress_test PRIVATE cmem pthread ${RT_LIBRARY})
add_dependencies(stress_test format-check check-mermaid)

# Fuzz target (libFuzzer)
add_executable(fuzz_alloc tests/fuzz_alloc.c)
target_link_libraries(fuzz_alloc PRIVATE cmem pthread ${RT_LIBRARY})
target_compile_definitions(fuzz_alloc PRIVATE STANDALONE_FUZZ)
set_target_properties(fuzz_alloc PROPERTIES
    C_CLANG_TIDY ""
    CXX_CLANG_TIDY ""
)
add_custom_target(fuzz
    COMMAND mkdir -p fuzz_corpus && echo "AABBCCDD" > fuzz_corpus/seed1 && echo "0123456789" > fuzz_corpus/seed2
    COMMAND fuzz_alloc -max_total_time=60 -rss_limit_mb=1024 fuzz_corpus fuzz_artifacts
    DEPENDS fuzz_alloc
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)



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