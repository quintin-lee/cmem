#!/bin/bash
# 统一构建系统CI自动化脚本

# 本脚本用于持续集成自动化，执行以下操作：
# 1. 新文件检测与注册
# 2. 代码格式检查
# 3. 静态分析
# 4. 单元测试执行
# 5. 基准测试运行
# 6. 错误收集与报告

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # 无颜色

# 配置
export CONFIG=cmake
CMAKE_BUILD_DIR="${CMAKE_BUILD_DIR:-build_cmake}"
TEST_TIMEOUT=300
REPORT_DIR="ci_reports"

# 创建报告目录
mkdir -p "$REPORT_DIR"

# 日志文件
LOG_FILE="$REPORT_DIR/ci_build.log"
HTML_REPORT="$REPORT_DIR/ci_report.html"
JSON_REPORT="$REPORT_DIR/ci_results.json"

echo "=== cmem CI自动化构建 ===" | tee "$LOG_FILE"
echo "开始时间: $(date)" | tee -a "$LOG_FILE"

# 函数：打印彩色消息
print_info() {
    echo -e "${BLUE}[INFO]${NC} $(date '+%H:%M:%S') $1" | tee -a "$LOG_FILE"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $(date '+%H:%M:%S') $1" | tee -a "$LOG_FILE"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $(date '+%H:%M:%S') $1" | tee -a "$LOG_FILE"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $(date '+%H:%M:%S') $1" | tee -a "$LOG_FILE"
}

# 初始化结果JSON
init_results_json() {
    cat > "$JSON_REPORT" << EOF
{
    "timestamp": "$(date -Iseconds)",
    "config": "$CONFIG",
    "results": {}
}
EOF
}

# 更新结果JSON
update_results_json() {
    local key="$1"
    local status="$2"
    local message="$3"
    local details="$4"
    
    python3 -c "
import json, sys
with open('$JSON_REPORT', 'r') as f:
    data = json.load(f)

data['results']['$key'] = {
    'status': '$status',
    'message': '$message',
    'details': '$details',
    'timestamp': '$(date -Iseconds)'
}

with open('$JSON_REPORT', 'w') as f:
    json.dump(data, f, indent=2)
"
}

# 步骤1：新文件注册
register_new_files() {
    print_info "步骤1：新文件检测与注册"
    
    if [[ -f "scripts/register_new_files.sh" ]]; then
        if bash "scripts/register_new_files.sh"; then
            print_success "新文件注册完成"
            update_results_json "file_registration" "SUCCESS" "检测到新文件并更新Makefile" "完成新文件注册"
        else
            print_warning "新文件注册出现警告"
            update_results_json "file_registration" "WARNING" "新文件注册完成但有警告" "执行新文件注册脚本"
        fi
    else
        print_warning "新文件注册脚本不存在"
        update_results_json "file_registration" "SKIPPED" "新文件注册脚本不存在" "未找到register_new_files.sh"
    fi
}

# 步骤2：代码格式检查
check_format() {
    print_info "步骤2：代码格式检查 (clang-format)"
    
    if command -v clang-format >/dev/null 2>&1; then
        if make format-check 2>&1 | tee "$REPORT_DIR/format_check.log"; then
            print_success "代码格式检查通过"
            update_results_json "format_check" "SUCCESS" "代码格式检查通过" "格式检查结果保存到$REPORT_DIR/format_check.log"
        else
            print_error "代码格式检查失败"
            update_results_json "format_check" "FAILURE" "代码格式检查失败" "请检查$REPORT_DIR/format_check.log"
            return 1
        fi
    else
        print_warning "clang-format未找到，跳过格式检查"
        update_results_json "format_check" "SKIPPED" "clang-format未找到" "安装clang-format以进行格式检查"
    fi
}

# 步骤3：Mermaid图检查
check_mermaid() {
    print_info "步骤3：Mermaid图语法检查"
    
    if [[ -f "scripts/check_mermaid.py" ]]; then
        if python3 "scripts/check_mermaid.py" 2>&1 | tee "$REPORT_DIR/mermaid_check.log"; then
            print_success "Mermaid图检查通过"
            update_results_json "mermaid_check" "SUCCESS" "Mermaid图检查通过" "检查结果保存到$REPORT_DIR/mermaid_check.log"
        else
            print_error "Mermaid图检查失败"
            update_results_json "mermaid_check" "FAILURE" "Mermaid图检查失败" "请检查$REPORT_DIR/mermaid_check.log"
            return 1
        fi
    else
        print_warning "Mermaid检查脚本不存在，跳过检查"
        update_results_json "mermaid_check" "SKIPPED" "Mermaid检查脚本不存在" "未找到check_mermaid.py"
    fi
}

# 步骤4：构建
build_project() {
    print_info "步骤4：构建项目 (CMake ${CMAKE_BUILD_TYPE:-Release})"
    
    if [[ -d "$CMAKE_BUILD_DIR" ]]; then
        print_info "检测到现有CMake构建目录，清理..."
        rm -rf "$CMAKE_BUILD_DIR"
    fi
    
    # 创建构建目录
    mkdir -p "$CMAKE_BUILD_DIR"
    
    # 运行CMake
    print_info "运行CMake配置..."
    if cmake -B "$CMAKE_BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" 2>&1 | tee "$REPORT_DIR/cmake_configure.log"; then
        print_success "CMake配置完成"
        update_results_json "cmake_configure" "SUCCESS" "CMake配置完成" "配置日志保存到$REPORT_DIR/cmake_configure.log"
    else
        print_error "CMake配置失败"
        update_results_json "cmake_configure" "FAILURE" "CMake配置失败" "请检查$REPORT_DIR/cmake_configure.log"
        return 1
    fi
    
    # 构建项目
    print_info "构建项目..."
    if cmake --build "$CMAKE_BUILD_DIR" -- -j$(nproc) 2>&1 | tee "$REPORT_DIR/cmake_build.log"; then
        print_success "项目构建完成"
        update_results_json "project_build" "SUCCESS" "项目构建完成" "构建日志保存到$REPORT_DIR/cmake_build.log"
    else
        print_error "项目构建失败"
        update_results_json "project_build" "FAILURE" "项目构建失败" "请检查$REPORT_DIR/cmake_build.log"
        return 1
    fi
}

# 步骤5：运行单元测试
run_unit_tests() {
    print_info "步骤5：运行单元测试 (超时${TEST_TIMEOUT}秒)"
    
    # 运行C单元测试
    print_info "运行C单元测试 (unit_tests)..."
    if timeout $TEST_TIMEOUT ctest --test-dir "$CMAKE_BUILD_DIR" -R CUnitTests --output-on-failure 2>&1 | tee "$REPORT_DIR/unit_tests.log"; then
        print_success "C单元测试通过"
        update_results_json "c_unit_tests" "SUCCESS" "C单元测试通过" "测试结果保存到$REPORT_DIR/unit_tests.log"
    else
        print_error "C单元测试失败"
        update_results_json "c_unit_tests" "FAILURE" "C单元测试失败" "请检查$REPORT_DIR/unit_tests.log"
        return 1
    fi
    
    # 运行C++单元测试
    print_info "运行C++单元测试 (cpp_tests)..."
    if timeout $TEST_TIMEOUT ctest --test-dir "$CMAKE_BUILD_DIR" -R CppUnitTests --output-on-failure 2>&1 | tee "$REPORT_DIR/cpp_tests.log"; then
        print_success "C++单元测试通过"
        update_results_json "cpp_unit_tests" "SUCCESS" "C++单元测试通过" "测试结果保存到$REPORT_DIR/cpp_tests.log"
    else
        print_error "C++单元测试失败"
        update_results_json "cpp_unit_tests" "FAILURE" "C++单元测试失败" "请检查$REPORT_DIR/cpp_tests.log"
        return 1
    fi
    
    # 运行高级C单元测试
    print_info "运行C高级单元测试 (advanced_tests)..."
    if timeout $TEST_TIMEOUT ctest --test-dir "$CMAKE_BUILD_DIR" -R CAdvancedUnitTests --output-on-failure 2>&1 | tee "$REPORT_DIR/advanced_tests.log"; then
        print_success "C高级单元测试通过"
        update_results_json "c_advanced_unit_tests" "SUCCESS" "C高级单元测试通过" "测试结果保存到$REPORT_DIR/advanced_tests.log"
    else
        print_error "C高级单元测试失败"
        update_results_json "c_advanced_unit_tests" "FAILURE" "C高级单元测试失败" "请检查$REPORT_DIR/advanced_tests.log"
        return 1
    fi
}

# 步骤6：运行基准测试
run_benchmark() {
    print_info "步骤6：运行基准测试"
    
    print_info "运行基准测试 (benchmark)..."
    if timeout $TEST_TIMEOUT "$CMAKE_BUILD_DIR"/benchmark 2>&1 | tee "$REPORT_DIR/benchmark.log"; then
        print_success "基准测试完成"
        update_results_json "benchmark" "SUCCESS" "基准测试完成" "基准结果保存到$REPORT_DIR/benchmark.log"
    else
        print_warning "基准测试失败（可能是超时），但不是关键任务"
        update_results_json "benchmark" "WARNING" "基准测试失败" "基准测试可能需要调整超时设置"
    fi
}

# 步骤7：运行压力测试
run_stress_test() {
    print_info "步骤7：运行压力测试 (快速模式，短时间)"
    
    # 设置短时间压力测试
    export STRESS_DURATION_SEC=30
    
    print_info "运行压力测试 (stress_test)..."
    if timeout $TEST_TIMEOUT "$CMAKE_BUILD_DIR"/stress_test 2>&1 | tee "$REPORT_DIR/stress_test.log"; then
        print_success "压力测试完成"
        update_results_json "stress_test" "SUCCESS" "压力测试完成" "压力测试结果保存到$REPORT_DIR/stress_test.log"
    else
        print_warning "压力测试失败（可能是超时），继续执行"
        update_results_json "stress_test" "WARNING" "压力测试失败" "压力测试可能需要更长时间"
    fi
}

# 步骤8：生成HTML报告
generate_html_report() {
    print_info "步骤8：生成HTML报告"
    
    cat > "$HTML_REPORT" << EOF
<!DOCTYPE html>
<html>
<head>
    <title>cmem CI自动化构建报告</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; }
        .header { background-color: #f0f0f0; padding: 20px; border-radius: 5px; }
        .result { margin: 10px 0; padding: 10px; border-left: 4px solid #ccc; }
        .success { border-left-color: #28a745; background-color: #d4edda; }
        .failure { border-left-color: #dc3545; background-color: #f8d7da; }
        .warning { border-left-color: #ffc107; background-color: #fff3cd; }
        .skipped { border-left-color: #6c757d; background-color: #e2e3f3; }
        .log-link { color: #007bff; text-decoration: none; }
        .log-link:hover { text-decoration: underline; }
        table { border-collapse: collapse; width: 100%; }
        th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }
        th { background-color: #f2f2f2; }
    </style>
</head>
<body>
    <div class="header">
        <h1>cmem CI自动化构建报告</h1>
        <p>构建时间: $(date)</p>
        <p>配置: $CONFIG</p>
    </div>
    
    <h2>构建结果</h2>
    <table>
        <tr>
            <th>阶段</th>
            <th>状态</th>
            <th>消息</th>
            <th>时间</th>
        </tr>
EOF
    
    # 从JSON中读取结果并生成HTML
    python3 << END
import json, datetime, os
with open('$JSON_REPORT', 'r') as f:
    data = json.load(f)

with open('$HTML_REPORT', 'a') as f:
    for stage, result in data['results'].items():
        status = result['status'].lower()
        status_class = status
        status_display = {
            'success': '通过',
            'failure': '失败',
            'warning': '警告',
            'skipped': '跳过'
        }.get(status, status)
        
        f.write(f"""
        <tr>
            <td>{stage}</td>
            <td class=\"result {status_class}\">{status_display}</td>
            <td>{result['message']}</td>
            <td>{result['timestamp']}</td>
        </tr>
        """)

END
    
    cat >> "$HTML_REPORT" << EOF
    </table>
    
    <h2>日志文件</h2>
    <ul>
EOF
    
    # 列出所有日志文件
    for logfile in "$REPORT_DIR"/*.log; do
        if [[ -f "$logfile" ]]; then
            basename=$(basename "$logfile")
            cat >> "$HTML_REPORT" << EOF
        <li><a href="$basename" class="log-link">$basename</a></li>
EOF
        fi
    done
    
    cat >> "$HTML_REPORT" << EOF
    </ul>
    
    <h2>操作</h2>
    <ul>
        <li><a href="../README.md">返回README</a></li>
        <li><a href="Makefile">查看Makefile</a></li>
        <li><a href="CMakeLists.txt">查看CMakeLists.txt</a></li>
    </ul>
</body>
</html>
EOF
    
    print_success "HTML报告生成完成: $HTML_REPORT"
    update_results_json "html_report" "SUCCESS" "HTML报告生成完成" "报告保存到$HTML_REPORT"
}

# 主执行函数
main() {
    init_results_json
    
    # 运行所有步骤
    register_new_files || true
    check_format || true
    check_mermaid || true
    build_project || {
        print_error "构建失败，中止"
        exit 1
    }
    run_unit_tests || {
        print_error "单元测试失败，中止"
        exit 1
    }
    run_benchmark || true
    run_stress_test || true
    
    # 生成最终报告
    generate_html_report
    
    print_success "=== CI自动化构建完成 ==="
    print_success "报告已生成: $HTML_REPORT"
    print_success "详细结果: $JSON_REPORT"
}

# 执行主函数
main "$@"
