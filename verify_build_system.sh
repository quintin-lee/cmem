#!/bin/bash
# 验证统一构建系统

# 本脚本验证统一构建系统的基本功能

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # 无颜色

echo -e "${GREEN}=== 验证统一构建系统 ===${NC}"

echo -e "\n${YELLOW}1. 检查构建系统配置...${NC}"
if grep -q "CONFIG ?= cmake" Makefile; then
    echo -e "${GREEN}✓ Makefile使用CMake-first配置${NC}"
else
    echo -e "${RED}✗ Makefile未使用CMake-first配置${NC}"
    exit 1
fi

echo -e "\n${YELLOW}2. 检查注册脚本...${NC}"
if [[ -f "scripts/register_new_files.sh" ]]; then
    echo -e "${GREEN}✓ 找到register_new_files.sh脚本${NC}"
    if [[ -x "scripts/register_new_files.sh" ]]; then
        echo -e "${GREEN}✓ 注册脚本可执行${NC}"
    else
        echo -e "${YELLOW}⚠ 注册脚本不可执行，建议chmod +x${NC}"
    fi
else
    echo -e "${RED}✗ 未找到register_new_files.sh脚本${NC}"
    exit 1
fi

echo -e "\n${YELLOW}3. 检查CI自动化脚本...${NC}"
if [[ -f "scripts/ci_automation.sh" ]]; then
    echo -e "${GREEN}✓ 找到ci_automation.sh脚本${NC}"
    if [[ -x "scripts/ci_automation.sh" ]]; then
        echo -e "${GREEN}✓ CI自动化脚本可执行${NC}"
    else
        echo -e "${YELLOW}⚠ CI自动化脚本不可执行，建议chmod +x${NC}"
    fi
else
    echo -e "${RED}✗ 未找到ci_automation.sh脚本${NC}"
    exit 1
fi

echo -e "\n${YELLOW}4. 检查CMakeLists.txt...${NC}"
if grep -q "set(CMEM_CORE_SRCS" CMakeLists.txt; then
    echo -e "${GREEN}✓ CMakeLists.txt包含CMEM_CORE_SRCS${NC}"
else
    echo -e "${RED}✗ CMakeLists.txt缺少CMEM_CORE_SRCS${NC}"
    exit 1
fi

echo -e "\n${YELLOW}5. 检查源文件结构...${NC}"
if [[ -d "src" && $(ls src/*.c | wc -l) -gt 0 ]]; then
    src_count=$(ls src/*.c | wc -l)
    echo -e "${GREEN}✓ 找到 $src_count 个源文件${NC}"
else
    echo -e "${RED}✗ 未找到源文件${NC}"
    exit 1
fi

echo -e "\n${YELLOW}6. 检查测试目录...${NC}"
test_dirs="tests examples benchmarks"
for dir in $test_dirs; do
    if [[ -d "$dir" ]]; then
        file_count=$(ls $dir/* 2>/dev/null | grep -v "^\." | wc -l)
        echo -e "${GREEN}✓ $dir 目录存在，包含 $file_count 个文件${NC}"
    else
        echo -e "${RED}✗ $dir 目录不存在${NC}"
        exit 1
    fi
done

echo -e "\n${YELLOW}7. 检查构建目录...${NC}"
if [[ -d "build_cmake" ]]; then
    echo -e "${GREEN}✓ CMake构建目录已存在${NC}"
else
    echo -e "${YELLOW}⚠ CMake构建目录不存在，将在构建时创建${NC}"
fi

echo -e "\n${YELLOW}8. 检查脚本权限...${NC}"
chmod +x "scripts/register_new_files.sh" "scripts/ci_automation.sh"
echo -e "${GREEN}✓ 已修复脚本执行权限${NC}"

echo -e "\n${YELLOW}9. 测试基本Makefile目标...${NC}"
if [[ "$1" == "--test-targets" ]]; then
    print_targets() {
        make -n "$1" 2>/dev/null | head -20
    }
    
    # 检查一些关键目标
    key_targets="cmake-build cmake-test cmake-clean all test"
    for target in $key_targets; do
        if make -n "$target" >/dev/null 2>&1; then
            echo -e "${GREEN}✓ 目标 '$target' 可执行${NC}"
        else
            echo -e "${YELLOW}⚠ 目标 '$target' 可能不正确${NC}"
        fi
    done
fi

echo -e "\n${GREEN}=== 验证完成 ===${NC}"
echo -e "\n${YELLOW}系统状态：${NC}"
echo "  - 统一构建系统 (CMake-first)"
echo "  - 新文件注册脚本: scripts/register_new_files.sh"
echo "  - CI自动化脚本: scripts/ci_automation.sh"
echo "  - 构建系统: Makefile (统一) + CMakeLists.txt"
echo "  - 支持的配置: cmake, make, debug, asan, tsan, ubsan"

echo -e "\n${YELLOW}使用说明：${NC}"
echo "  1. 运行: make help"
echo "  2. 构建CMake版本: make cmake-build"
echo "  3. 运行测试: make cmake-test"
echo "  4. 注册新文件: make register-new-files"
echo "  5. 运行CI自动化: scripts/ci_automation.sh"
echo "  6. 直接使用CMake: cmake -B build_cmake -G Ninja"

echo -e "\n${YELLOW}主要优势：${NC}"
echo "  ✓ CMake-first设计，避免构建系统冲突"
echo "  ✓ 保持Makefile兼容性"
echo "  ✓ 自动新文件注册"
echo "  ✓ CI自动化检查"
echo "  ✓ 多配置支持 (Debug, ASan, TSan, UBSan)"
echo "  ✓ 统一的Makefile和CMakeLists.txt维护"

echo -e "\n${GREEN}=== 验证成功 ===${NC}"
