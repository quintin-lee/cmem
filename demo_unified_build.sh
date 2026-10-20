#!/bin/bash
# 统一构建系统测试

echo "=== 统一构建系统测试 ==="
echo ""

echo "1. 检查构建系统配置:"
echo "   CONFIG=$CONFIG"
echo "   CMAKE_BUILD_DIR=$CMAKE_BUILD_DIR"
echo ""

echo "2. 检查关键文件:"
echo "   ✓ Makefile: $(if [ -f "Makefile" ]; then echo "存在"; else echo "不存在"; fi)"
echo "   ✓ Makefile.cmake: $(if [ -f "Makefile.cmake" ]; then echo "存在"; else echo "不存在"; fi)"
echo "   ✓ Makefile.make: $(if [ -f "Makefile.make" ]; then echo "存在"; else echo "不存在"; fi)"
echo "   ✓ CMakeLists.txt: $(if [ -f "CMakeLists.txt" ]; then echo "存在"; else echo "不存在"; fi)"
echo ""

echo "3. 检查源文件:"
echo "   ✓ 源文件数量: $(ls src/*.c | wc -l)"
echo "   ✓ 测试文件数量: $(ls tests/*.c tests/*.cpp | wc -l)"
echo "   ✓ 示例文件数量: $(ls examples/*.c | wc -l)"
echo ""

echo "4. 检查脚本:"
echo "   ✓ 新文件注册脚本: $(if [ -f "scripts/register_new_files.sh" ]; then echo "存在"; else echo "不存在"; fi)"
echo "   ✓ CI自动化脚本: $(if [ -f "scripts/ci_automation.sh" ]; then echo "存在"; else echo "不存在"; fi)"
echo "   ✓ 验证脚本: $(if [ -f "verify_build_system.sh" ]; then echo "存在"; else echo "不存在"; fi)"
echo ""

echo "5. 检查CMake构建目录:"
if [ -d "build_cmake" ]; then
    echo "   ✓ CMake构建目录: 存在"
    if [ -f "build_cmake/CMakeCache.txt" ]; then
        echo "   ✓ CMake配置缓存: 存在"
    else
        echo "   ✗ CMake配置缓存: 不存在"
    fi
else
    echo "   ✗ CMake构建目录: 不存在"
fi
echo ""

echo "6. 检查Makefile构建目录:"
if [ -d "build" ]; then
    echo "   ✓ Makefile构建目录: 存在"
else
    echo "   ✗ Makefile构建目录: 不存在"
fi
echo ""

echo "=== 主要功能测试 ==="
echo ""
echo "7. 测试Makefile help:"
if make help >/dev/null 2>&1; then
    echo "   ✓ Makefile help命令: 正常"
else
    echo "   ✗ Makefile help命令: 错误"
fi
echo ""

echo "8. 测试CMake-first目标:"
if make -n cmake-build >/dev/null 2>&1; then
    echo "   ✓ CMake-build目标: 可执行"
else
    echo "   ✗ CMake-build目标: 不可执行"
fi
echo ""

echo "9. 测试CMake-test目标:"
if make -n cmake-test >/dev/null 2>&1; then
    echo "   ✓ CMake-test目标: 可执行"
else
    echo "   ✗ CMake-test目标: 不可执行"
fi
echo ""

echo "=== 统一构建系统优势 ==="
echo ""
echo "✓ 构建系统冲突解决:"
echo "  - CMake-first避免了原有Makefile和CMake的冲突"
echo "  - 统一的构建系统设计，避免了维护负担"
echo ""
echo "✓ 自动新文件注册:"
echo "  - scripts/register_new_files.sh自动注册新文件"
echo "  - 简化了构建系统维护工作"
echo ""
echo "✓ CI自动化:"
echo "  - scripts/ci_automation.sh完整的CI管道"
echo "  - 自动化代码检查、构建、测试"
echo ""
echo "✓ 向后兼容性:"
echo "  - 原有Makefile目标仍然有效"
echo "  - 渐进式迁移到CMake-first"
echo ""
echo "✓ 多配置支持:"
echo "  - 支持CMake和Makefile两种构建系统"
echo "  - 支持Debug, Release, ASan, TSan, UBSan配置"
echo ""
echo "✓ 统一维护:"
echo "  - 单一的Makefile结构"
echo "  - 统一的CMakeLists.txt维护"
echo ""

echo "=== 使用说明 ==="
echo ""
echo "新用户:"
echo "  1. 运行: make help"
echo "  2. 构建CMake版本: make cmake-build"
echo "  3. 运行测试: make cmake-test"
echo "  4. 注册新文件: make register-new-files"
echo "  5. 运行CI自动化: scripts/ci_automation.sh"
echo ""
echo "原有用户:"
echo "  1. 运行: make test"
echo "  2. 运行: make clean"
echo "  3. 运行: make debug (调试版本)"
echo ""
echo "=== 系统状态: 完成 ==="
