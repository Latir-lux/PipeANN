#!/bin/bash
# DC-PDI实验批量运行脚本
# 用法: ./run_all_dc_pdi_experiments.sh [数据集路径]

set -e  # 遇到错误立即退出

# ==================== 配置区 ====================

PROJECT_ROOT=/home/xiaoxuanx/WorkSpace/PipeANN
RESULTS_DIR=$PROJECT_ROOT/results
DRAW_DIR=$PROJECT_ROOT/draw

# 默认数据集路径（可通过命令行参数覆盖）
if [ $# -ge 1 ]; then
    DATA_ROOT=$1
else
    DATA_ROOT=/mnt/xiaoxuanx/dataset
fi

# 数据集选择（根据实际情况修改）
DATASET_NAME=sift1m       # 选项: sift1m, sift100m, deep100m
DATA_TYPE=float           # 数据类型: float, uint8, int8

# 根据数据集设置路径
case $DATASET_NAME in
    sift1m)
        INDEX_PREFIX=$DATA_ROOT/sift/sift_index
        QUERY_FILE=$DATA_ROOT/sift/sift_query.fbin
        GT_FILE=$DATA_ROOT/sift/sift_groundtruth.ibin
        DATA_TYPE=float
        ;;
    sift100m)
        INDEX_PREFIX=$DATA_ROOT/indices/sift100m
        QUERY_FILE=$DATA_ROOT/bigann/bigann_query.bbin
        GT_FILE=$DATA_ROOT/bigann/100M_gt.bin
        DATA_TYPE=uint8
        ;;
    deep100m)
        INDEX_PREFIX=$DATA_ROOT/indices/deep100m
        QUERY_FILE=$DATA_ROOT/deep/queries.fbin
        GT_FILE=$DATA_ROOT/deep/100M_gt.bin
        DATA_TYPE=float
        ;;
    *)
        echo "未知数据集: $DATASET_NAME"
        echo "支持的数据集: sift1m, sift100m, deep100m"
        exit 1
        ;;
esac

# 实验参数
NUM_THREADS=32
BEAM_WIDTH=16
RECALL_AT=10
MEM_L=10

# ==================== 初始化 ====================

mkdir -p $RESULTS_DIR
cd $PROJECT_ROOT

echo "=========================================="
echo "DC-PDI实验批量运行"
echo "=========================================="
echo "项目根目录: $PROJECT_ROOT"
echo "数据集: $DATASET_NAME ($DATA_TYPE)"
echo "索引前缀: $INDEX_PREFIX"
echo "查询文件: $QUERY_FILE"
echo "真值文件: $GT_FILE"
echo "结果目录: $RESULTS_DIR"
echo "线程数: $NUM_THREADS"
echo "流水线宽度: $BEAM_WIDTH"
echo "=========================================="

# 检查可执行文件
if [ ! -f "$PROJECT_ROOT/build/tests/thesis_benchmark" ]; then
    echo "❌ 错误: thesis_benchmark 未找到"
    echo "请先运行: bash build.sh"
    exit 1
fi

# 检查索引文件
if [ ! -f "${INDEX_PREFIX}_disk.index" ]; then
    echo "❌ 错误: 索引文件不存在: ${INDEX_PREFIX}_disk.index"
    echo "请先构建索引（参见 experiment_guide.md 步骤3）"
    exit 1
fi

# 检查查询和真值文件
if [ ! -f "$QUERY_FILE" ] || [ ! -f "$GT_FILE" ]; then
    echo "❌ 错误: 查询文件或真值文件不存在"
    echo "查询: $QUERY_FILE"
    echo "真值: $GT_FILE"
    exit 1
fi

echo "✓ 所有前置检查通过"
echo ""

# ==================== 实验运行 ====================

start_time=$(date +%s)

# 实验8: 物理离散度评估（核心实验）
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[1/5] 实验8: DC-PDI物理离散度评估"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
exp8_start=$(date +%s)

./build/tests/thesis_benchmark \
  8 \
  $DATA_TYPE \
  $INDEX_PREFIX \
  $QUERY_FILE \
  $GT_FILE \
  $RESULTS_DIR/exp8_dispersion_${DATASET_NAME}.csv \
  $NUM_THREADS \
  $BEAM_WIDTH \
  $RECALL_AT \
  $MEM_L

exp8_end=$(date +%s)
exp8_duration=$((exp8_end - exp8_start))
echo "✓ 实验8完成 (用时: ${exp8_duration}秒)"
echo "  输出: $RESULTS_DIR/exp8_dispersion_${DATASET_NAME}.csv"

if [ -f "$RESULTS_DIR/exp8_dispersion_${DATASET_NAME}.csv.pages.csv" ]; then
    page_count=$(wc -l < "$RESULTS_DIR/exp8_dispersion_${DATASET_NAME}.csv.pages.csv")
    echo "  页面统计: $RESULTS_DIR/exp8_dispersion_${DATASET_NAME}.csv.pages.csv ($page_count 页)"
fi
echo ""

# 实验1: 搜索延迟分布
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[2/5] 实验1: 搜索延迟分布"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
exp1_start=$(date +%s)

./build/tests/thesis_benchmark \
  1 \
  $DATA_TYPE \
  $INDEX_PREFIX \
  $QUERY_FILE \
  $GT_FILE \
  $RESULTS_DIR/exp1_latency_${DATASET_NAME}.csv \
  $NUM_THREADS \
  $BEAM_WIDTH \
  $RECALL_AT \
  $MEM_L

exp1_end=$(date +%s)
exp1_duration=$((exp1_end - exp1_start))
echo "✓ 实验1完成 (用时: ${exp1_duration}秒)"
echo "  输出: $RESULTS_DIR/exp1_latency_${DATASET_NAME}.csv"
echo ""

# 实验5: I/O放大率测试
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[3/5] 实验5: I/O放大率测试"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
exp5_start=$(date +%s)

./build/tests/thesis_benchmark \
  5 \
  $DATA_TYPE \
  $INDEX_PREFIX \
  $QUERY_FILE \
  $GT_FILE \
  $RESULTS_DIR/exp5_io_amp_${DATASET_NAME}.csv \
  $NUM_THREADS \
  $BEAM_WIDTH \
  $RECALL_AT \
  $MEM_L

exp5_end=$(date +%s)
exp5_duration=$((exp5_end - exp5_start))
echo "✓ 实验5完成 (用时: ${exp5_duration}秒)"
echo "  输出: $RESULTS_DIR/exp5_io_amp_${DATASET_NAME}.csv"
echo ""

# 实验6: 流水线宽度敏感性
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[4/5] 实验6: 流水线宽度敏感性"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
exp6_start=$(date +%s)

./build/tests/thesis_benchmark \
  6 \
  $DATA_TYPE \
  $INDEX_PREFIX \
  $QUERY_FILE \
  $GT_FILE \
  $RESULTS_DIR/exp6_pipeline_${DATASET_NAME}.csv \
  $NUM_THREADS \
  $BEAM_WIDTH \
  $RECALL_AT \
  $MEM_L

exp6_end=$(date +%s)
exp6_duration=$((exp6_end - exp6_start))
echo "✓ 实验6完成 (用时: ${exp6_duration}秒)"
echo "  输出: $RESULTS_DIR/exp6_pipeline_${DATASET_NAME}.csv"
echo ""

# 实验7: 资源开销评估
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[5/5] 实验7: 资源开销评估"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
exp7_start=$(date +%s)

./build/tests/thesis_benchmark \
  7 \
  $DATA_TYPE \
  $INDEX_PREFIX \
  $QUERY_FILE \
  $GT_FILE \
  $RESULTS_DIR/exp7_resource_${DATASET_NAME}.csv \
  $NUM_THREADS \
  $BEAM_WIDTH \
  $RECALL_AT \
  $MEM_L

exp7_end=$(date +%s)
exp7_duration=$((exp7_end - exp7_start))
echo "✓ 实验7完成 (用时: ${exp7_duration}秒)"
echo "  输出: $RESULTS_DIR/exp7_resource_${DATASET_NAME}.csv"
echo ""

# ==================== 汇总 ====================

end_time=$(date +%s)
total_duration=$((end_time - start_time))

echo "=========================================="
echo "🎉 所有实验完成！"
echo "=========================================="
echo "总用时: ${total_duration}秒 ($((total_duration / 60))分钟)"
echo ""
echo "各实验用时:"
echo "  实验8 (物理离散度): ${exp8_duration}秒"
echo "  实验1 (搜索延迟): ${exp1_duration}秒"
echo "  实验5 (I/O放大率): ${exp5_duration}秒"
echo "  实验6 (流水线宽度): ${exp6_duration}秒"
echo "  实验7 (资源评估): ${exp7_duration}秒"
echo ""
echo "结果文件:"
ls -lh $RESULTS_DIR/*${DATASET_NAME}.csv 2>/dev/null || echo "  (无结果文件)"
echo ""

# ==================== 快速分析 ====================

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "📊 快速结果预览"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [ -f "$RESULTS_DIR/exp8_dispersion_${DATASET_NAME}.csv" ]; then
    echo ""
    echo "【实验8: 物理离散度】"
    echo "━━━━━━━━━━━━━━━━━━━━━"
    grep "avg_physical_dispersion" $RESULTS_DIR/exp8_dispersion_${DATASET_NAME}.csv || true
    grep "avg_page_local_edge_ratio" $RESULTS_DIR/exp8_dispersion_${DATASET_NAME}.csv || true
    grep "global_fragmented_pages" $RESULTS_DIR/exp8_dispersion_${DATASET_NAME}.csv || true
fi

if [ -f "$RESULTS_DIR/exp1_latency_${DATASET_NAME}.csv" ]; then
    echo ""
    echo "【实验1: 延迟分布（R@10=95%附近）】"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    head -1 $RESULTS_DIR/exp1_latency_${DATASET_NAME}.csv
    grep -E "0\.(94|95|96)" $RESULTS_DIR/exp1_latency_${DATASET_NAME}.csv | head -3 || true
fi

echo ""
echo "=========================================="
echo "📁 完整结果位于: $RESULTS_DIR"
echo "📈 运行绘图脚本: python3 draw/plot_thesis_figures.py"
echo "📖 详细说明: work2do/experiment_guide.md"
echo "=========================================="
