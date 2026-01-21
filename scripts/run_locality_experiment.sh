#!/bin/bash
# PipeANN 章节3.2.1 实验执行脚本
# 基于 PipeANN 的动态图索引逻辑空间局部性验证与特征提取实验

set -e

# ==================== 配置区域 ====================
# 请根据实际环境修改以下路径

# 数据集路径 (需要修改为实际路径)
# SIFT 数据集（本地构建）
INDEX_PREFIX="/mnt/xiaoxuanx/dataset/sift/sift_index"                    # 索引文件前缀
QUERY_BIN="/mnt/xiaoxuanx/dataset/sift/sift_query.fbin"                   # 查询向量文件
TRUTHSET_BIN="/mnt/xiaoxuanx/dataset/sift/sift_groundtruth.ibin"         # Ground truth 文件

# 如果要使用 BIGANN 数据集，取消注释下面的行：
# INDEX_PREFIX="/mnt/nvme2/indices/bigann/100m"           # 索引文件前缀
# QUERY_BIN="/mnt/nvme/data/bigann/bigann_query.bbin"     # 查询向量文件
# TRUTHSET_BIN="/mnt/nvme/data/bigann/100M_gt.bin"        # Ground truth 文件

# 构建输出目录
OUTPUT_DIR="$(dirname "$0")/../draw"
mkdir -p "${OUTPUT_DIR}"

# 可执行文件路径
SEARCH_BIN="$(dirname "$0")/../build/tests/search_disk_index"

# 实验参数
DATA_TYPE="float"           # float / int8 / uint8
NUM_THREADS=1               # 单线程以获取准确的延迟数据
PIPELINE_WIDTH=8            # Pipeline width
RECALL_AT=10                # Recall@K
DISTANCE="l2"               # l2 / cosine
NBR_TYPE="pq"               # pq / rabitq
SEARCH_MODE=2               # 0=beam, 1=page, 2=pipe
MEM_L=0                     # 内存索引 L 值，0 表示不使用
L_VALUES="50"               # 搜索 L 值

# ==================== 实验函数 ====================

run_baseline() {
    echo "=========================================="
    echo "运行基线实验 (无碎片化 - Static)"
    echo "=========================================="
    
    ${SEARCH_BIN} ${DATA_TYPE} ${INDEX_PREFIX} ${NUM_THREADS} ${PIPELINE_WIDTH} \
        ${QUERY_BIN} ${TRUTHSET_BIN} ${RECALL_AT} ${DISTANCE} ${NBR_TYPE} \
        ${SEARCH_MODE} ${MEM_L} ${L_VALUES} \
        --trace --trace-output ${OUTPUT_DIR}
    
    echo "基线实验完成"
}

run_fragmentation_experiment() {
    local FRAG_RATIO=$1
    local FRAG_SEED=${2:-42}
    
    echo "=========================================="
    echo "运行碎片化实验 (比例: ${FRAG_RATIO})"
    echo "=========================================="
    
    ${SEARCH_BIN} ${DATA_TYPE} ${INDEX_PREFIX} ${NUM_THREADS} ${PIPELINE_WIDTH} \
        ${QUERY_BIN} ${TRUTHSET_BIN} ${RECALL_AT} ${DISTANCE} ${NBR_TYPE} \
        ${SEARCH_MODE} ${MEM_L} ${L_VALUES} \
        --trace --trace-output ${OUTPUT_DIR} \
        --fragmentation ${FRAG_RATIO} --frag-seed ${FRAG_SEED}
    
    echo "碎片化实验 (${FRAG_RATIO}) 完成"
}

run_pipeline_sensitivity() {
    echo "=========================================="
    echo "运行 Pipeline Width 敏感性分析"
    echo "=========================================="
    
    for WIDTH in 4 8 16 32; do
        echo "Testing Pipeline Width = ${WIDTH}"
        
        # Static
        ${SEARCH_BIN} ${DATA_TYPE} ${INDEX_PREFIX} ${NUM_THREADS} ${WIDTH} \
            ${QUERY_BIN} ${TRUTHSET_BIN} ${RECALL_AT} ${DISTANCE} ${NBR_TYPE} \
            ${SEARCH_MODE} ${MEM_L} ${L_VALUES} \
            --trace --trace-output ${OUTPUT_DIR}
        
        # 重命名输出文件
        mv ${OUTPUT_DIR}/trace_static.jsonl ${OUTPUT_DIR}/trace_w${WIDTH}_static.jsonl 2>/dev/null || true
        
        # Dynamic (50% fragmentation)
        ${SEARCH_BIN} ${DATA_TYPE} ${INDEX_PREFIX} ${NUM_THREADS} ${WIDTH} \
            ${QUERY_BIN} ${TRUTHSET_BIN} ${RECALL_AT} ${DISTANCE} ${NBR_TYPE} \
            ${SEARCH_MODE} ${MEM_L} ${L_VALUES} \
            --trace --trace-output ${OUTPUT_DIR} \
            --fragmentation 0.5
        
        mv ${OUTPUT_DIR}/trace_frag50.jsonl ${OUTPUT_DIR}/trace_w${WIDTH}_frag50.jsonl 2>/dev/null || true
    done
    
    echo "Pipeline 敏感性分析完成"
}

analyze_results() {
    echo "=========================================="
    echo "分析实验结果并生成图表"
    echo "=========================================="
    
    cd ${OUTPUT_DIR}
    python3 analyze_traces.py --data-dir . --output-dir .
    
    echo "分析完成，图表已保存到 ${OUTPUT_DIR}"
}

# ==================== 主执行流程 ====================

main() {
    echo "============================================="
    echo "PipeANN 逻辑空间局部性验证实验"
    echo "============================================="
    echo ""
    echo "配置信息："
    echo "  索引前缀: ${INDEX_PREFIX}"
    echo "  查询文件: ${QUERY_BIN}"
    echo "  输出目录: ${OUTPUT_DIR}"
    echo ""
    
    # 检查可执行文件
    if [ ! -f "${SEARCH_BIN}" ]; then
        echo "错误: 找不到可执行文件 ${SEARCH_BIN}"
        echo "请先编译项目: cd build && cmake .. && make -j"
        exit 1
    fi
    
    # 检查数据文件
    if [ ! -f "${QUERY_BIN}" ]; then
        echo "警告: 找不到查询文件 ${QUERY_BIN}"
        echo "请修改脚本中的数据集路径"
    fi
    
    case "${1:-all}" in
        baseline)
            run_baseline
            ;;
        frag)
            run_fragmentation_experiment 0.5
            ;;
        frag30)
            run_fragmentation_experiment 0.3
            ;;
        sensitivity)
            run_pipeline_sensitivity
            ;;
        analyze)
            analyze_results
            ;;
        all)
            echo "运行完整实验流程..."
            run_baseline
            run_fragmentation_experiment 0.3
            run_fragmentation_experiment 0.5
            analyze_results
            ;;
        *)
            echo "用法: $0 [baseline|frag|frag30|sensitivity|analyze|all]"
            echo ""
            echo "命令说明："
            echo "  baseline    - 运行基线实验 (无碎片化)"
            echo "  frag        - 运行 50% 碎片化实验"
            echo "  frag30      - 运行 30% 碎片化实验"
            echo "  sensitivity - 运行 Pipeline Width 敏感性分析"
            echo "  analyze     - 分析结果并生成图表"
            echo "  all         - 运行完整实验流程"
            exit 1
            ;;
    esac
}

main "$@"
