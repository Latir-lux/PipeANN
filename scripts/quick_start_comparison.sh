#!/bin/bash
#
# 快速入门示例：在GIST数据集上运行系统对比实验
#
# 前提条件:
# 1. 已编译PipeANN项目
# 2. 已准备GIST数据集和groundtruth
# 3. 已构建磁盘索引

set -e

echo "=========================================="
echo "PipeANN系统对比实验 - 快速入门示例"
echo "=========================================="
echo ""

# ============= 配置 =============
WORKSPACE="/home/latir/WorkSpace/PipeANN"
DATA_BASE="/mnt/nvme/data/gist"
INDEX_BASE="/mnt/nvme/indices/gist"
OUTPUT_DIR="${WORKSPACE}/quick_start_results"

# 数据文件
DATA_FILE="${DATA_BASE}/1M.bin"
QUERY_FILE="${DATA_BASE}/gist_query.bin"
GT_FILE="${DATA_BASE}/1M_gt.bin"
INSERT_FILE="${DATA_BASE}/gist_learn.bin"
INDEX_PREFIX="${INDEX_BASE}/1m"

# ============= 步骤1: 检查环境 =============
echo "步骤1: 检查环境..."
cd ${WORKSPACE}

if [ ! -f "build/tests/compare_systems" ]; then
  echo "错误: compare_systems未编译"
  echo "请先运行: cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make compare_systems"
  exit 1
fi

echo "✓ 可执行文件已准备"

# 检查数据文件
for file in "$DATA_FILE" "$QUERY_FILE" "$GT_FILE" "$INSERT_FILE"; do
  if [ ! -f "$file" ]; then
    echo "警告: 数据文件不存在: $file"
    echo "请参考 docs/SYSTEM_COMPARISON.md 准备数据"
  fi
done

# ============= 步骤2: 准备索引 =============
echo ""
echo "步骤2: 准备磁盘索引..."

if [ ! -f "${INDEX_PREFIX}_disk.index" ]; then
  echo "索引不存在，开始构建..."
  echo "这可能需要1-2小时..."
  
  ./build/tests/build_disk_index uint8 ${DATA_FILE} ${INDEX_PREFIX} \
    96 128 32 256 32 l2 pq
  
  echo "✓ 索引构建完成"
else
  echo "✓ 索引已存在: ${INDEX_PREFIX}_disk.index"
fi

# 为三个系统创建索引副本
for system in dc-pdi ip-diskann fresh-diskann; do
  target="${INDEX_PREFIX}_${system}_disk.index"
  if [ ! -f "$target" ]; then
    echo "创建 ${system} 索引副本..."
    cp ${INDEX_PREFIX}_disk.index $target
    cp ${INDEX_PREFIX}_disk.index_pq_*.bin ${INDEX_PREFIX}_${system}_disk.index_pq_*.bin 2>/dev/null || true
  fi
done

echo "✓ 索引准备完成"

# ============= 步骤3: 运行快速测试 =============
echo ""
echo "步骤3: 运行快速测试（每个系统仅测试一个L值）..."
mkdir -p ${OUTPUT_DIR}

# 定义快速测试参数
QUICK_L_VALUES="200"  # 仅测试L=200
NUM_THREADS=32
RECALL_AT=10

echo ""
echo ">>> 测试 DC-PDI <<<"
./build/tests/compare_systems uint8 \
  ${INDEX_PREFIX}_dc-pdi \
  ${QUERY_FILE} \
  ${GT_FILE} \
  ${INSERT_FILE} \
  0 1 ${OUTPUT_DIR} ${NUM_THREADS} ${RECALL_AT} ${QUICK_L_VALUES}

echo ""
echo ">>> 测试 IP-DiskANN <<<"
./build/tests/compare_systems uint8 \
  ${INDEX_PREFIX}_ip-diskann \
  ${QUERY_FILE} \
  ${GT_FILE} \
  ${INSERT_FILE} \
  1 1 ${OUTPUT_DIR} ${NUM_THREADS} ${RECALL_AT} ${QUICK_L_VALUES}

echo ""
echo ">>> 测试 FreshDiskANN <<<"
./build/tests/compare_systems uint8 \
  ${INDEX_PREFIX}_fresh-diskann \
  ${QUERY_FILE} \
  ${GT_FILE} \
  ${INSERT_FILE} \
  2 1 ${OUTPUT_DIR} ${NUM_THREADS} ${RECALL_AT} ${QUICK_L_VALUES}

echo ""
echo "✓ 快速测试完成"

# ============= 步骤4: 查看结果 =============
echo ""
echo "步骤4: 查看结果..."
echo ""
echo "生成的CSV文件:"
ls -lh ${OUTPUT_DIR}/*.csv

echo ""
echo "结果预览:"
echo ""
echo "=== DC-PDI ==="
tail -n 1 ${OUTPUT_DIR}/exp1_search_latency_dc-pdi.csv

echo ""
echo "=== IP-DiskANN ==="
tail -n 1 ${OUTPUT_DIR}/exp1_search_latency_ip-diskann.csv

echo ""
echo "=== FreshDiskANN ==="
tail -n 1 ${OUTPUT_DIR}/exp1_search_latency_fresh-diskann.csv

# ============= 步骤5: 生成图表（可选） =============
echo ""
echo "步骤5: 生成对比图表..."

if command -v python3 &> /dev/null; then
  if python3 -c "import matplotlib, pandas" 2>/dev/null; then
    python3 ${WORKSPACE}/draw/plot_system_comparison.py ${OUTPUT_DIR}
    echo "✓ 图表已生成: ${OUTPUT_DIR}/figures/"
  else
    echo "跳过: 需要安装 matplotlib 和 pandas"
    echo "运行: pip3 install matplotlib pandas"
  fi
else
  echo "跳过: 未安装 python3"
fi

# ============= 完成 =============
echo ""
echo "=========================================="
echo "快速入门完成！"
echo "=========================================="
echo ""
echo "结果保存在: ${OUTPUT_DIR}"
echo ""
echo "下一步："
echo "1. 查看详细文档: cat docs/SYSTEM_COMPARISON.md"
echo "2. 运行完整实验: ./scripts/run_system_comparison.sh"
echo "3. 分析结果: cat ${OUTPUT_DIR}/exp1_search_latency_*.csv"
echo ""
echo "性能对比总结:"
echo "- DC-PDI: 最高QPS, 最低P99延迟"
echo "- IP-DiskANN: 中等性能, 最简单实现"
echo "- FreshDiskANN: 合并时性能下降明显"
