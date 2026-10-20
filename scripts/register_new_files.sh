#!/bin/bash
# 自动注册新源文件到构建系统

# 检测新添加的文件（不在现有Makefile/CMakeLists.txt中）

cd "$(dirname "$0")"

# 获取所有源文件
ALL_SRC_FILES="$(find src -name "*.c")"
ALL_CPP_FILES="$(find tests -name "*.cpp")"

# 从现有Makefile中提取已注册的C源文件
MAKEFILE_SRC_FILES="$(grep -E '^[A-Z_]*_SRC =' Makefile | grep -v '^#' | sed 's/.*= //' | tr -d ' \\\n' | tr ',' '\n' | grep -v '^$')"

# 从CMakeLists.txt中提取已注册的C源文件
CMAKE_SRC_FILES="$(grep -E '^[[:space:]]*src/[^[:space:]]*\.c' CMakeLists.txt | sed 's/[[:space:]]*//' | grep -v '^#')"

# 从CMakeLists.txt中提取所有目标源文件
CMAKE_ALL_SRCS="$(grep -E '^set\(CMEM_CORE_SRCS' CMakeLists.txt -A 10 | sed -n '/^set(CMEM_CORE_SRCS/,/^)/p' | grep -E '^[[:space:]]*src/[^[:space:]]*\.c' | sed 's/[[:space:]]*//' | grep -v '^#')"

# 合并提取到的文件
EXTRACTED_FILES="$MAKEFILE_SRC_FILES$CMAKE_SRC_FILES$CMAKE_ALL_SRCS"

echo "=== 新文件注册分析 ==="
echo "发现 ${ALL_SRC_FILES//\n/ } 个源文件"
echo "提取到 ${EXTRACTED_FILES//\n/ } 个文件"

# 识别需要注册的新文件
NEW_FILES=""
for file in $ALL_SRC_FILES; do
    if [[ ! " $EXTRACTED_FILES " == *" $file "* ]]; then
        echo "✓ 需要注册新源文件: $file"
        NEW_FILES="$NEW_FILES$file"
    fi
done

# 检查C++测试文件
if [[ -n "$ALL_CPP_FILES" ]]; then
    echo "\n=== C++测试文件 ==="
    for cppfile in $ALL_CPP_FILES; do
        if [[ ! " $EXTRACTED_FILES " == *" $cppfile "* ]]; then
            echo "✓ 需要注册新C++测试文件: $cppfile"
            NEW_FILES="$NEW_FILES$cppfile"
        fi
done
fi

# 更新Makefile
if [[ -n "$NEW_FILES" ]]; then
    echo -e "\n=== 更新Makefile ==="
    
    # 创建备份
    cp Makefile Makefile.backup
    
    # 更新SRC变量 (从 src/*.c 中提取)
    sed -i 's|^SRC = $(wildcard src/.*.c)|SRC = $(wildcard src/*.c)|' Makefile
    
    # 更新TEST_SRC、ADV_TEST_SRC等变量
    for var in "TEST_SRC = tests/test_main.c" "ADV_TEST_SRC = tests/test_advanced.c" "STRESS_SRC = tests/stress_test.c" "CPP_TEST_SRC = tests/test_cpp.cpp" "BENCH_SRC = benchmarks/bench_main.c"; do
        # 检查是否有对应的测试文件
        var_name=$(echo $var | cut -d'=' -f1 | sed 's/ *= *//')
        test_file=$(echo $var | cut -d'=' -f2 | sed 's/ *= *//')
        base_file=$(basename $test_file)
        if [[ -n "$NEW_FILES" && " $NEW_FILES " == *" $test_file "* ]]; then
            echo "✓ 将更新 $var_name"
            sed -i "s|$var.*|$var $test_file|" Makefile
        fi
    done
    
    # 更新format-check目标
    echo "✓ 将更新format-check目标"
    sed -i '/^format-check:/,/^\t@/ { /^\t@/i\\tinclude/*.h include/*.hpp \\
\t\tsrc/*.c \\
\t\ttests/*.c tests/*.cpp \\
\t\texamples/*.c \\
\t\tbenchmarks/*.c \\
\t\ttests/stress_test.c' Makefile
    
    # 恢复备份
    rm -f Makefile.backup
    
    echo "✓ Makefile已更新"
fi

# 更新CMakeLists.txt
echo -e "\n=== 更新CMakeLists.txt ==="

# 检查是否需要更新CMEM_CORE_SRCS
cmake_core_file="src/cmem.c src/cmem_slab.c src/cmem_tlsf.c src/cmem_sys.c src/cmem_event.c src/cmem_compress.c"

# 检查是否有缺失的核心文件
missing_core_files=""
for core_file in $cmake_core_file; do
    if [[ ! " $CMAKE_ALL_SRCS " == *" $core_file "* ]]; then
        missing_core_files="$missing_core_files$core_file\n"
    fi
done

if [[ -n "$missing_core_files" ]]; then
    echo "核心文件缺失："
    echo -e "$missing_core_files"
    echo "请手动检查CMakeLists.txt的CMEM_CORE_SRCS"
fi

# 创建需要手动干预的通知
echo "\n=== 注意 ==="
echo "CMakeLists.txt需要手动更新，以添加新源文件到CMEM_CORE_SRCS和测试目标。"
echo "通常需要更新三处位置："
echo "1. format-check文件列表 (~line 52)"
echo "2. add_library(cmem_core STATIC ...) (~line 155)"
echo "3. add_library(cmem_shared SHARED ...) (~line 164)"

# 显示需要更新的区域
echo -e "\n=== 需要检查的CMakeLists.txt区域 ==="
sed -n '145,180p' CMakeLists.txt

# 创建自动化辅助工具
sed -i '
/^# 生成发布包$/,/^$/ {
    /^\t@/ {
        i\
# 新文件注册辅助工具\
register-new-files:
		@echo "正在注册新文件..."
		@bash scripts/register_new_files.sh
		@cmake -S . -B build_cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
		@cmake --build build_cmake
		@echo "新文件注册完成！"
	}
}
' Makefile

echo "\n✓ 自动化脚本已添加"
echo "\n=== 新文件注册完成 ==="
echo "要注册新文件，请运行: make register-new-files"
echo "\n=== 重要提示 ==="
echo "CMakeLists.txt仍然需要手动维护CMEM_CORE_SRCS。"
echo "建议将make register-new-files集成到CI管道中。"
