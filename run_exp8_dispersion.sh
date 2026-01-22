#!/bin/bash
# 单独运行实验8（物理离散度评估）的简化脚本

set -e

# ==================== 用户配置 ====================
# 请根据实际情况修改以下路径

# 数据集路径
INDEX_PREFIX=/mnt/xiaoxuanx/dataset/sift                        # 索引前缀（不含_disk.index后缀）
QUERY_FILE=/mnt/xiaoxuanx/dataset/sift/sift_query.fbin          # 查询文件
GT_FILE=/mnt/xiaoxuanx/dataset/sift/sift_groundtruth.bin        # 真值文件
DATA_TYPE=float                                                 # 数据类型: float/uint8/int8

# 实验参数
NUM_THREADS=32        # 搜索线程数
BEAM_WIDTH=16         # 流水线宽度
RECALL_AT=10          # 计算Recall@K
MEM_L=10              # 内存索引候选列表长度（设为0表示不使用内存索引）

# 输出路径
OUTPUT_DIR=./results
mkdir -p $OUTPUT_DIR
OUTPUT_FILE=$OUTPUT_DIR/exp8_dispersion.csv

# ==================== 运行实验 ====================

PROJECT_ROOT=/home/xiaoxuanx/WorkSpace/PipeANN
cd $PROJECT_ROOT

echo "=========================================="
echo "实验8: DC-PDI物理离散度评估"
echo "=========================================="
echo "索引: $INDEX_PREFIX"
echo "查询: $QUERY_FILE"
echo "真值: $GT_FILE"
echo "数据类型: $DATA_TYPE"
echo "线程数: $NUM_THREADS"
echo "流水线宽度: $BEAM_WIDTH"
echo "输出: $OUTPUT_FILE"
echo "=========================================="
echo ""

# 检查文件存在性
echo "检查前置条件..."
if [ ! -f "$PROJECT_ROOT/build/tests/thesis_benchmark" ]; then
    echo "❌ 错误: thesis_benchmark 未编译"
    echo "请运行: bash build.sh"
    exit 1
fi

if [ ! -f "${INDEX_PREFIX}_disk.index" ]; then
    echo "❌ 错误: 索引文件不存在: ${INDEX_PREFIX}_disk.index"
    exit 1
fi

if [ ! -f "$QUERY_FILE" ]; then
    echo "❌ 错误: 查询文件不存在: $QUERY_FILE"
    exit 1
fi

if [ ! -f "$GT_FILE" ]; then
    echo "❌ 错误: 真值文件不存在: $GT_FILE"
    exit 1
fi

echo "✓ 所有文件检查通过"
echo ""

# 运行实验
echo "开始运行实验..."
start_time=$(date +%s)

./build/tests/thesis_benchmark \
  8 \
  $DATA_TYPE \
  $INDEX_PREFIX \
  $QUERY_FILE \
  $GT_FILE \
  $OUTPUT_FILE \
  $NUM_THREADS \
  $BEAM_WIDTH \
  $RECALL_AT \
  $MEM_L

end_time=$(date +%s)
duration=$((end_time - start_time))

echo ""
echo "=========================================="
echo "✓ 实验完成！用时: ${duration}秒"
echo "=========================================="

# 显示结果
if [ -f "$OUTPUT_FILE" ]; then
    echo ""
    echo "【主要统计结果】"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    cat $OUTPUT_FILE
    
    if [ -f "${OUTPUT_FILE}.pages.csv" ]; then
        page_count=$(wc -l < "${OUTPUT_FILE}.pages.csv")
        echo ""
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo "页面级统计: ${OUTPUT_FILE}.pages.csv"
        echo "总页面数: $((page_count - 1))"
        echo "前5行预览:"
        head -6 "${OUTPUT_FILE}.pages.csv"
    fi
else
    echo "⚠️  警告: 输出文件未生成"
fi

echo ""
echo "=========================================="
echo "📁 结果保存在: $OUTPUT_FILE"
echo "📖 详细说明: work2do/experiment_guide.md"
echo "=========================================="
