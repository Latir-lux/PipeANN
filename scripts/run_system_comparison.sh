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
EXP3_BASE_RATIO=${4:-"0.5"}
EXP3_UPDATE_RATIO=${5:-"0.5"}
EXP3_DURATION_SEC=${6:-"120"}
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
  local index_base=${3:-"${INDEX_BASE}"}
  
  echo "[$(date)] Preparing index for $system_name..." >&2
  
  # 如果索引不存在，先构建
  if [ ! -f "${index_base}_disk.index" ]; then
    echo "Building disk index..." >&2
    ./build/tests/build_disk_index ${DATA_TYPE} ${DATA_FILE} ${index_base} \
      96 128 32 256 ${NUM_THREADS} l2 pq >&2
  fi
  
  # 为不同系统复制索引（避免相互影响）
  local system_index="${index_base}_${system_name}"
  if [ ! -f "${system_index}_disk.index" ]; then
    echo "Copying index for $system_name..." >&2
    # 主索引文件
    cp ${index_base}_disk.index ${system_index}_disk.index
    # PQ量化文件（注意：文件名格式是 {prefix}_pq_*.bin，不是 {prefix}_disk.index_pq_*.bin）
    cp ${index_base}_pq_compressed.bin ${system_index}_pq_compressed.bin 2>/dev/null || true
    cp ${index_base}_pq_pivots.bin ${system_index}_pq_pivots.bin 2>/dev/null || true
    # 其他辅助文件
    cp ${index_base}_sample_data.bin ${system_index}_sample_data.bin 2>/dev/null || true
    cp ${index_base}_partition.bin.aligned ${system_index}_partition.bin.aligned 2>/dev/null || true
    # 标签文件（如果存在）
    cp ${index_base}_disk.index.tags ${system_index}_disk.index.tags 2>/dev/null || true
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

# 准备实验3的数据分片（按比例拆分）
prepare_exp3_data() {
  local data_file=$1
  local data_type=$2
  local base_ratio=$3
  local update_ratio=$4

  local data_ext="${data_file##*.}"
  local data_prefix="${data_file%.*}"

  local base_pct
  base_pct=$(python3 - <<PY
import math
print(int(round(${base_ratio} * 100)))
PY
)
  local update_pct
  update_pct=$(python3 - <<PY
import math
print(int(round(${update_ratio} * 100)))
PY
)

  local base_file="${data_prefix}_base${base_pct}.${data_ext}"
  local update_file="${data_prefix}_update${update_pct}.${data_ext}"

  if [ -f "${base_file}" ] && [ -f "${update_file}" ]; then
    echo "Using existing exp3 data splits: ${base_file}, ${update_file}" >&2
    EXP3_BASE_FILE=${base_file}
    EXP3_UPDATE_FILE=${update_file}
    return
  fi

  python3 - <<PY
import os
import struct

data_file = "${data_file}"
base_file = "${base_file}"
update_file = "${update_file}"
base_ratio = float("${base_ratio}")
update_ratio = float("${update_ratio}")
data_type = "${data_type}"

dtype_size = {"uint8": 1, "int8": 1, "float": 4}.get(data_type)
if dtype_size is None:
    raise SystemExit(f"Unsupported data type: {data_type}")

with open(data_file, "rb") as f:
    header = f.read(8)
    if len(header) != 8:
        raise SystemExit(f"Invalid data file header: {data_file}")
    npts, dim = struct.unpack("<ii", header)

base_pts = int(npts * base_ratio)
update_pts = int(npts * update_ratio)
remaining = max(0, npts - base_pts)
if update_pts > remaining:
    update_pts = remaining

if base_pts <= 0 or update_pts <= 0:
    raise SystemExit(f"Invalid split sizes: base={base_pts}, update={update_pts}, total={npts}")

def write_split(out_path, start_pt, count):
    with open(data_file, "rb") as src, open(out_path, "wb") as dst:
        dst.write(struct.pack("<ii", count, dim))
        src.seek(8 + start_pt * dim * dtype_size)
        remaining_bytes = count * dim * dtype_size
        buf_size = 1024 * 1024
        while remaining_bytes > 0:
            to_read = min(buf_size, remaining_bytes)
            chunk = src.read(to_read)
            if not chunk:
                raise SystemExit(f"Unexpected EOF while reading {data_file}")
            dst.write(chunk)
            remaining_bytes -= len(chunk)

if not os.path.exists(base_file):
    write_split(base_file, 0, base_pts)

if not os.path.exists(update_file):
    write_split(update_file, base_pts, update_pts)

print(base_file)
print(update_file)
PY

  EXP3_BASE_FILE=${base_file}
  EXP3_UPDATE_FILE=${update_file}
}

# 运行读写并发实验
run_concurrent_exp() {
  local system_type=$1
  local system_name=$2
  local duration_sec=${3:-60}
  local index_base=${4:-"${INDEX_BASE}"}
  local insert_file=${5:-"${INSERT_FILE}"}
  
  echo ""
  echo "========================================"
  echo "实验3: 读写并发性能测试 - ${system_name}"
  echo "========================================"
  
  local system_index=$(prepare_index ${system_name} ${system_type} ${index_base})
  local output_file="${OUTPUT_DIR}/exp3_concurrent_${system_name}.csv"
  
  echo "[$(date)] Running concurrent test for ${system_name} (${duration_sec}s)..."
  ./build/tests/compare_systems ${DATA_TYPE} ${system_index} ${QUERY_FILE} ${GT_FILE} \
    ${insert_file} ${system_type} 3 ${OUTPUT_DIR} ${NUM_THREADS} ${RECALL_AT} ${L_VALUES} \
    --exp3-duration-sec ${duration_sec} --exp3-update-ratio ${EXP3_UPDATE_RATIO}
  
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
prepare_exp3_data ${DATA_FILE} ${DATA_TYPE} ${EXP3_BASE_RATIO} ${EXP3_UPDATE_RATIO}

EXP3_BASE_TAG=$(printf "%s" "${EXP3_BASE_RATIO}" | tr '.' 'p')
EXP3_INDEX_BASE="${INDEX_BASE}_exp3_base${EXP3_BASE_TAG}"

if [ ! -f "${EXP3_INDEX_BASE}_disk.index" ]; then
  echo "Building exp3 base index..." >&2
  ./build/tests/build_disk_index ${DATA_TYPE} ${EXP3_BASE_FILE} ${EXP3_INDEX_BASE} \
    96 128 32 256 ${NUM_THREADS} l2 pq >&2
fi

run_concurrent_exp 0 "dc-pdi" ${EXP3_DURATION_SEC} ${EXP3_INDEX_BASE} ${EXP3_UPDATE_FILE}
run_concurrent_exp 1 "ip-diskann" ${EXP3_DURATION_SEC} ${EXP3_INDEX_BASE} ${EXP3_UPDATE_FILE}
run_concurrent_exp 2 "fresh-diskann" ${EXP3_DURATION_SEC} ${EXP3_INDEX_BASE} ${EXP3_UPDATE_FILE}

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
