#!/bin/bash
#
# 论文第3-5章实验统一执行脚本
# 采集所有图表所需的真实数据
#

set -e

# 配置参数
BUILD_DIR="${BUILD_DIR:-./build}"
DATA_DIR="${DATA_DIR:-/path/to/data}"
INDEX_PREFIX="${INDEX_PREFIX:-$DATA_DIR/sift1m}"
QUERY_FILE="${QUERY_FILE:-$DATA_DIR/query.bin}"
GT_FILE="${GT_FILE:-$DATA_DIR/gt100.bin}"
OUTPUT_DIR="${OUTPUT_DIR:-./draw/experiment_results}"
DATA_TYPE="${DATA_TYPE:-float}"
NUM_THREADS="${NUM_THREADS:-32}"
BEAM_WIDTH="${BEAM_WIDTH:-16}"

# 创建输出目录
mkdir -p "$OUTPUT_DIR"

BENCHMARK="$BUILD_DIR/tests/thesis_benchmark"

# 检查可执行文件是否存在
if [ ! -f "$BENCHMARK" ]; then
    echo "Error: thesis_benchmark not found at $BENCHMARK"
    echo "Please build the project first: cd build && cmake .. && make -j"
    exit 1
fi

echo "======================================"
echo "论文实验数据采集"
echo "======================================"
echo "Index prefix: $INDEX_PREFIX"
echo "Query file: $QUERY_FILE"
echo "Ground truth: $GT_FILE"
echo "Output directory: $OUTPUT_DIR"
echo "Data type: $DATA_TYPE"
echo "Threads: $NUM_THREADS"
echo ""

# 函数：运行实验
run_experiment() {
    local exp_type=$1
    local output_file=$2
    local description=$3
    
    echo "--------------------------------------"
    echo "实验 $exp_type: $description"
    echo "输出文件: $output_file"
    echo "--------------------------------------"
    
    "$BENCHMARK" "$exp_type" "$DATA_TYPE" "$INDEX_PREFIX" "$QUERY_FILE" "$GT_FILE" \
        "$OUTPUT_DIR/$output_file" "$NUM_THREADS" "$BEAM_WIDTH"
    
    if [ $? -eq 0 ]; then
        echo "✓ 实验 $exp_type 完成"
    else
        echo "✗ 实验 $exp_type 失败"
    fi
    echo ""
}

# 第3章实验: DC-PDI核心设计
echo ""
echo "========== 第3章实验 =========="
echo ""

# 实验9: 拓扑强度统计 (图3-1)
run_experiment 9 "exp9_topology_strength.csv" "拓扑强度统计 (3.1节)"

# 实验10: 离散度演变 (图3-2)
run_experiment 10 "exp10_dispersion_evolution.csv" "离散度演变测试 (3.3节)"

# 实验11: 跨页边比例 (图3-3)
run_experiment 11 "exp11_cross_page_ratio.csv" "跨页边比例测试 (3.4节)"

# 实验8: 物理离散度评估
run_experiment 8 "exp8_dispersion.csv" "物理离散度评估 (3.2节)"

# 第5章实验: 系统性能评估
echo ""
echo "========== 第5章实验 =========="
echo ""

# 实验1: 搜索延迟分布 (图5-1)
run_experiment 1 "exp1_latency_${DATA_TYPE}.csv" "搜索延迟分布 (5.2.1节)"

# 实验2: 动态更新延迟稳定性 (图5-2)
# 注意: 这个实验需要较长时间 (默认10分钟)
echo "实验2将运行约10分钟..."
run_experiment 2 "exp2_dynamic_latency.csv" "动态更新延迟稳定性 (5.2.2节)"

# 实验6: 流水线宽度敏感性 (图5-3)
run_experiment 6 "exp6_pipeline_width.csv" "流水线宽度敏感性 (5.5.1节)"

# 实验5: I/O放大率 (图5-4)
run_experiment 5 "exp5_io_amplification.csv" "I/O放大率测试 (5.4.1节)"

# 实验7: 资源开销评估 (图5-6)
run_experiment 7 "exp7_resource.csv" "资源开销评估 (5.5.3节)"

echo ""
echo "======================================"
echo "所有实验完成"
echo "======================================"
echo ""
echo "数据文件保存在: $OUTPUT_DIR"
echo ""
echo "现在可以运行绘图脚本生成图表:"
echo "  python3 draw/plot_thesis_figures.py --data-dir $OUTPUT_DIR --output-dir draw/figures"
echo ""

# 列出生成的文件
echo "生成的数据文件:"
ls -la "$OUTPUT_DIR"/*.csv 2>/dev/null || echo "(无CSV文件)"
