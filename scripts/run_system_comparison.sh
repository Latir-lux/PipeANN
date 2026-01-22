#!/bin/bash
#
# 运行DC-PDI、IP-DiskANN和FreshDiskANN的对比实验
#
# 使用方法:
#   ./scripts/run_system_comparison.sh [dataset] [base_dir] [output_dir]
#
# 参数:
#   dataset: sift/deep/gist (默认: sift)
#   base_dir: 数据集和索引的基础目录 (默认: /mnt/xiaoxuanx/dataset)
#   output_dir: 输出目录 (默认: /mnt/xiaoxuanx/dataset/exp/thesis_results/system_comparison)

set -e

# ============= 配置参数 =============
DATASET=${1:-"sift"}
BASE_DIR=${2:-"/mnt/xiaoxuanx/dataset"}
OUTPUT_DIR=${3:-"/mnt/xiaoxuanx/dataset/exp/thesis_results/system_comparison"}
NUM_THREADS=32
RECALL_AT=10

# 创建输出目录
mkdir -p ${OUTPUT_DIR}

# ============= 数据集配置 =============
case $DATASET in
  sift)
    DATA_TYPE="uint8"
    DATA_DIM=128
    DATA_FILE="${BASE_DIR}/bigann/100M.bbin"
    QUERY_FILE="${BASE_DIR}/bigann/bigann_query.bbin"
    GT_FILE="${BASE_DIR}/bigann/100M_gt.bin"
    INSERT_FILE="${BASE_DIR}/bigann/bigann_learn.bbin"  # 用于插入测试
    INDEX_BASE="${BASE_DIR}/indices/bigann/100m"
    ;;
  deep)
    DATA_TYPE="float"
    DATA_DIM=96
    DATA_FILE="${BASE_DIR}/deep1b/100M.fbin"
    QUERY_FILE="${BASE_DIR}/deep1b/deep_query.fbin"
    GT_FILE="${BASE_DIR}/deep1b/100M_gt.bin"
    INSERT_FILE="${BASE_DIR}/deep1b/deep_learn.fbin"
    INDEX_BASE="${BASE_DIR}/indices/deep/100m"
    ;;
  gist)
    DATA_TYPE="uint8"
    DATA_DIM=960
    DATA_FILE="${BASE_DIR}/gist/gist.bin"
    QUERY_FILE="${BASE_DIR}/gist/gist_query.bin"
    GT_FILE="${BASE_DIR}/gist/gist_gt.bin"
    INSERT_FILE="${BASE_DIR}/gist/gist_learn.bin"
    INDEX_BASE="${BASE_DIR}/gist/gist"
    ;;
  *)
    echo "Unknown dataset: $DATASET"
    echo "Supported: sift, deep, gist"
    exit 1
    ;;
esac

echo "============================================"
echo "Dataset: $DATASET"
echo "Data file: $DATA_FILE"
echo "Query file: $QUERY_FILE"
echo "GT file: $GT_FILE"
echo "Index base: $INDEX_BASE"
echo "Output directory: $OUTPUT_DIR"
echo "============================================"

# L值列表（用于搜索延迟测试）
L_VALUES="100 150 200 250 300 350 400"

# ============= 函数定义 =============

# 准备索引的函数
prepare_index() {
  local system_name=$1
  local search_mode=$2
  
  echo "[$(date)] Preparing index for $system_name..." >&2
  
  # 如果索引不存在，先构建
  if [ ! -f "${INDEX_BASE}_disk.index" ]; then
    echo "Building disk index..." >&2
    ./build/tests/build_disk_index ${DATA_TYPE} ${DATA_FILE} ${INDEX_BASE} \
      96 128 32 256 ${NUM_THREADS} l2 pq >&2
  fi
  
  # 为不同系统复制索引（避免相互影响）
  local system_index="${INDEX_BASE}_${system_name}"
  if [ ! -f "${system_index}_disk.index" ]; then
    echo "Copying index for $system_name..." >&2
    # 主索引文件
    cp ${INDEX_BASE}_disk.index ${system_index}_disk.index
    # PQ量化文件（注意：文件名格式是 {prefix}_pq_*.bin，不是 {prefix}_disk.index_pq_*.bin）
    cp ${INDEX_BASE}_pq_compressed.bin ${system_index}_pq_compressed.bin 2>/dev/null || true
    cp ${INDEX_BASE}_pq_pivots.bin ${system_index}_pq_pivots.bin 2>/dev/null || true
    # 其他辅助文件
    cp ${INDEX_BASE}_sample_data.bin ${system_index}_sample_data.bin 2>/dev/null || true
    cp ${INDEX_BASE}_partition.bin.aligned ${system_index}_partition.bin.aligned 2>/dev/null || true
    # 标签文件（如果存在）
    cp ${INDEX_BASE}_disk.index.tags ${system_index}_disk.index.tags 2>/dev/null || true
  fi
  
  echo "[$(date)] Index for $system_name ready: ${system_index}" >&2
  echo "${system_index}"
}

# 运行搜索延迟实验
run_search_latency_exp() {
  local system_type=$1
  local system_name=$2
  
  echo ""
  echo "========================================"
  echo "实验1: 搜索延迟分布测试 - ${system_name}"
  echo "========================================"
  
  local system_index=$(prepare_index ${system_name} ${system_type})
  local output_file="${OUTPUT_DIR}/exp1_search_latency_${system_name}.csv"
  
  echo "[$(date)] Running search latency test for ${system_name}..."
  ./build/tests/compare_systems ${DATA_TYPE} ${system_index} ${QUERY_FILE} ${GT_FILE} \
    ${INSERT_FILE} ${system_type} 1 ${OUTPUT_DIR} ${NUM_THREADS} ${RECALL_AT} ${L_VALUES}
  
  echo "[$(date)] Search latency test completed: ${output_file}"
}

# 运行更新吞吐量实验
run_update_throughput_exp() {
  local system_type=$1
  local system_name=$2
  local num_inserts=${3:-10000}
  
  echo ""
  echo "========================================"
  echo "实验2: 更新吞吐量测试 - ${system_name}"
  echo "========================================"
  
  local system_index=$(prepare_index ${system_name} ${system_type})
  local output_file="${OUTPUT_DIR}/exp2_update_throughput_${system_name}.csv"
  
  echo "[$(date)] Running update throughput test for ${system_name} (${num_inserts} inserts)..."
  ./build/tests/compare_systems ${DATA_TYPE} ${system_index} ${QUERY_FILE} ${GT_FILE} \
    ${INSERT_FILE} ${system_type} 2 ${OUTPUT_DIR} ${NUM_THREADS} ${RECALL_AT}
  
  echo "[$(date)] Update throughput test completed: ${output_file}"
}

# 运行读写并发实验
run_concurrent_exp() {
  local system_type=$1
  local system_name=$2
  local duration_sec=${3:-60}
  
  echo ""
  echo "========================================"
  echo "实验3: 读写并发性能测试 - ${system_name}"
  echo "========================================"
  
  local system_index=$(prepare_index ${system_name} ${system_type})
  local output_file="${OUTPUT_DIR}/exp3_concurrent_${system_name}.csv"
  
  echo "[$(date)] Running concurrent test for ${system_name} (${duration_sec}s)..."
  ./build/tests/compare_systems ${DATA_TYPE} ${system_index} ${QUERY_FILE} ${GT_FILE} \
    ${INSERT_FILE} ${system_type} 3 ${OUTPUT_DIR} ${NUM_THREADS} ${RECALL_AT}
  
  echo "[$(date)] Concurrent test completed: ${output_file}"
}

# ============= 主执行流程 =============

echo ""
echo "######################################"
echo "#   系统对比实验开始                 #"
echo "######################################"
echo ""

# 检查必要文件
if [ ! -f "${DATA_FILE}" ]; then
  echo "Error: Data file not found: ${DATA_FILE}"
  exit 1
fi

if [ ! -f "${QUERY_FILE}" ]; then
  echo "Error: Query file not found: ${QUERY_FILE}"
  exit 1
fi

if [ ! -f "${GT_FILE}" ]; then
  echo "Error: Ground truth file not found: ${GT_FILE}"
  exit 1
fi

# 检查可执行文件
if [ ! -f "./build/tests/compare_systems" ]; then
  echo "Error: compare_systems executable not found. Please compile first:"
  echo "  cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make compare_systems"
  exit 1
fi

# ============= 运行所有系统的所有实验 =============

# 实验1: 搜索延迟分布
echo ""
echo ">>> 开始实验1: 搜索延迟分布测试 <<<"
run_search_latency_exp 0 "dc-pdi"
run_search_latency_exp 1 "ip-diskann"
run_search_latency_exp 2 "fresh-diskann"

# 实验2: 更新吞吐量
echo ""
echo ">>> 开始实验2: 更新吞吐量测试 <<<"
run_update_throughput_exp 0 "dc-pdi" 50000
run_update_throughput_exp 1 "ip-diskann" 50000
run_update_throughput_exp 2 "fresh-diskann" 50000

# 实验3: 读写并发性能
echo ""
echo ">>> 开始实验3: 读写并发性能测试 <<<"
run_concurrent_exp 0 "dc-pdi" 120
run_concurrent_exp 1 "ip-diskann" 120
run_concurrent_exp 2 "fresh-diskann" 120

# ============= 生成对比图表 =============
echo ""
echo ">>> 生成对比图表 <<<"
if [ -f "./draw/plot_system_comparison.py" ]; then
  python3 ./draw/plot_system_comparison.py ${OUTPUT_DIR}
  echo "[$(date)] Plots generated in: ${OUTPUT_DIR}/figures/"
else
  echo "Warning: plot_system_comparison.py not found, skipping visualization"
fi

# ============= 完成 =============
echo ""
echo "######################################"
echo "#   所有实验完成!                    #"
echo "######################################"
echo ""
echo "结果保存在: ${OUTPUT_DIR}"
echo ""
echo "生成的文件:"
ls -lh ${OUTPUT_DIR}/*.csv

echo ""
echo "可以使用以下命令查看结果:"
echo "  cat ${OUTPUT_DIR}/exp1_search_latency_*.csv"
echo "  cat ${OUTPUT_DIR}/exp2_update_throughput_*.csv"
echo "  cat ${OUTPUT_DIR}/exp3_concurrent_*.csv"
