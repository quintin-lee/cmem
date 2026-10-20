# cmem - Unified Build System (CMake-first with Makefile compatibility)
# 支持构建、测试、基准、安装、分发、Docker等

CC = gcc
CXX = g++

# 读取版本信息（从 VERSION 文件读取）
VERSION := $(shell cat VERSION | tr -d '[:space:]')
LIBNAME = libcmem.a
SONAME = libcmem.so.$(VERSION)
CORE_SONAME = libcmem_core.so.$(VERSION)
DIAG_SONAME = libcmem_diag.so.$(VERSION)

# 构建配置（通过 CONFIG 变量选择，默认 cmake）
#   cmake  - 使用CMake构建 (首选)
#   make   - 使用Makefile构建 (兼容性)
#   debug  - 使用Makefile构建 (debug配置)
#   asan   - 使用Makefile构建 (ASan配置)
#   tsan   - 使用Makefile构建 (TSan配置)
#   ubsan  - 使用Makefile构建 (UBSan配置)
CONFIG ?= cmake

# CMake 配置
CMAKE_BUILD_DIR = build_cmake
CMAKE_GENERATOR = Ninja
CMAKE_BUILD_TYPE ?= Release

# 根据不同的CONFIG进行处理
ifeq ($(CONFIG),cmake)

    # 包含子Makefile
    include Makefile.cmake

else

    # Makefile兼容模式
    include Makefile.make

endif
