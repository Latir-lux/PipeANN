#!/bin/bash
#
# run_ablation_study.sh - DC-PDI系统消融实验脚本
#
# 本脚本运行毕业论文exp-chapter4and3-3.docx中定义的四个消融实验：
#
# 实验1: 拓扑感知分配 vs. 随机分配（验证动态聚类有效性）
# 实验2: 重组织机制的性能恢复能力（验证后台重组织效果）
# 实验3: 异步流水线 vs. 同步阻塞搜索（验证流水线加速效果）
# 实验4: 并发控制协议的扩展性测试（验证锁机制效率）
#
# 使用方法:
#   ./scripts/run_ablation_study.sh [experiment] [dataset] [base_dir] [output_dir] [options...]
#
# 参数说明:
#   experiment:  1/2/3/4/all (默认: all)
#                1 = 动态聚类消融实验
#                2 = 重组织机制消融实验
#                3 = 流水线消融实验
#                4 = 并发扩展性实验
#                all = 运行所有实验
#
#   dataset:     sift/deep/gist (默认: sift)
#                sift = SIFT1B数据集
#                deep = DEEP1B数据集
#                gist = GIST数据集
#
#   base_dir:    数据集和索引的基础目录
#                (默认: /mnt/xiaoxuanx/dataset)
#
#   output_dir:  输出目录
#                (默认: /mnt/xiaoxuanx/dataset/exp/thesis_results/ablation_study)
#
# 可选参数:
#   --num-threads <n>      搜索线程数 (默认: 32)
#   --insert-count <n>     实验1的插入向量数量 (默认: 100000)
#   --duration <sec>       实验2/4的持续时间(秒) (默认: 120)
#   --insert-rate <n>      实验2的插入速率(ops/s) (默认: 1000)
#   --base-ratio <ratio>   基础索引占数据集比例 (默认: 0.5)
#   --L-values <list>      逗号分隔的L值列表 (默认: 50,100,150,200,250,300)
#   --thread-list <list>   实验4的线程数列表 (默认: 1,4,8,16,32,64)
#
# 示例:
#   # 运行所有实验，使用SIFT数据集
#   ./scripts/run_ablation_study.sh all sift
#
#   # 只运行实验3（流水线消融），使用DEEP数据集
#   ./scripts/run_ablation_study.sh 3 deep
#
#   # 自定义参数运行实验1
#   ./scripts/run_ablation_study.sh 1 sift /data /output --insert-count 500000
#

set -e

# ============= 配置参数 =============
# 注意：不要在这里初始化位置参数，因为它们会在后面的参数解析部分被正确设置
# 这里只定义用于展示的默认值注释

# EXPERIMENT: 1/2/3/4/all (默认: all)
# DATASET: sift/deep/gist (默认: sift)
# BASE_DIR: 数据集和索引的基础目录 (默认: /mnt/xiaoxuanx/dataset)
# OUTPUT_DIR: 输出目录 (默认: /mnt/xiaoxuanx/dataset/exp/thesis_results/ablation_study)

# 默认参数
NUM_THREADS=32
INSERT_COUNT=100000
DURATION_SEC=120
INSERT_RATE=1000
BASE_RATIO=0.5
# L值范围：10-100，步长5（覆盖低recall 10% 到高recall 95% 的完整范围）
L_VALUES="10,15,20,25,30,35,40,45,50,55,60,65,70,75,80,85,90,95,100"
THREAD_LIST="1,4,8,16,32,64"
RECALL_AT=10

# 解析额外参数
# 先保存前4个位置参数，然后跳过它们，再解析可选参数
saved_arg1="$1"
saved_arg2="$2"
saved_arg3="$3"
saved_arg4="$4"

# 只有在有第5个参数时才开始解析选项
if [ $# -gt 4 ]; then
  shift 4
  while [[ $# -gt 0 ]]; do
    case $1 in
      --num-threads)
        NUM_THREADS="$2"
        shift 2
        ;;
      --insert-count)
        INSERT_COUNT="$2"
        shift 2
        ;;
      --duration)
        DURATION_SEC="$2"
        shift 2
        ;;
      --insert-rate)
        INSERT_RATE="$2"
        shift 2
        ;;
      --base-ratio)
        BASE_RATIO="$2"
        shift 2
        ;;
      --L-values)
        L_VALUES="$2"
        shift 2
        ;;
      --thread-list)
        THREAD_LIST="$2"
        shift 2
        ;;
      --recall-at)
        RECALL_AT="$2"
        shift 2
        ;;
      *)
        echo "Unknown option: $1"
        exit 1
        ;;
    esac
  done
fi

# 恢复位置参数
EXPERIMENT="${saved_arg1:-all}"
DATASET="${saved_arg2:-sift}"
BASE_DIR="${saved_arg3:-/mnt/xiaoxuanx/dataset}"
OUTPUT_DIR="${saved_arg4:-/mnt/xiaoxuanx/dataset/exp/thesis_results/ablation_study}"

# 为不同数据集隔离输出目录
RESULTS_DIR="${OUTPUT_DIR}/${DATASET}"
mkdir -p ${RESULTS_DIR}

# ============= 数据集配置 =============
case $DATASET in
  sift)
    DATA_TYPE="uint8"
    DATA_DIM=128
    DATA_FILE="${BASE_DIR}/bigann/bigann.bin"
    QUERY_FILE="${BASE_DIR}/bigann/bigann_query.bin"
    GT_FILE="${BASE_DIR}/bigann/100M_gt.bin"
    INSERT_FILE="${BASE_DIR}/bigann/bigann_learn.bin"
    INDEX_BASE="${BASE_DIR}/bigann/indices/sift-100m"
    ;;
  deep)
    DATA_TYPE="float"
    DATA_DIM=96
    DATA_FILE="${BASE_DIR}/deep1b/deep1b_base.bin"
    QUERY_FILE="${BASE_DIR}/deep1b/deep1b_query.bin"
    GT_FILE="${BASE_DIR}/deep1b/deep1b_gt.bin"
    INSERT_FILE="${BASE_DIR}/deep1b/deep1b_learn.bin"
    INDEX_BASE="${BASE_DIR}/deep1b/indices/deep-1b"
    ;;
  gist)
    DATA_TYPE="uint8"
    DATA_DIM=960
    DATA_FILE="${BASE_DIR}/gist/gist.bin"
    QUERY_FILE="${BASE_DIR}/gist/gist_query.bin"
    GT_FILE="${BASE_DIR}/gist/gist_gt.bin"
    INSERT_FILE="${BASE_DIR}/gist/gist_learn.bin"
    INDEX_BASE="${BASE_DIR}/gist/indices/gist"
    ;;
  *)
    echo "Unknown dataset: $DATASET"
    echo "Supported: sift, deep, gist"
    exit 1
    ;;
esac

echo "============================================"
echo "DC-PDI 消融实验"
echo "============================================"
echo "Dataset: $DATASET"
echo "Data type: $DATA_TYPE"
echo "Data file: $DATA_FILE"
echo "Query file: $QUERY_FILE"
echo "GT file: $GT_FILE"
echo "Insert file: $INSERT_FILE"
echo "Index base: $INDEX_BASE"
echo "Output directory: $RESULTS_DIR"
echo "Experiment: $EXPERIMENT"
echo ""
echo "Parameters:"
echo "  Threads: $NUM_THREADS"
echo "  Insert count: $INSERT_COUNT"
echo "  Duration: ${DURATION_SEC}s"
echo "  Insert rate: ${INSERT_RATE} ops/s"
echo "  Base ratio: $BASE_RATIO"
echo "  L values: $L_VALUES"
echo "  Thread list: $THREAD_LIST"
echo "============================================"

# ============= 文件检查 =============
check_file() {
  if [ ! -f "$1" ]; then
    echo "Error: File not found: $1"
    exit 1
  fi
}

# 检查可执行文件
if [ ! -f "./build/tests/ablation_study" ]; then
  echo "Error: ablation_study executable not found."
  echo "Please compile first:"
  echo "  cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make ablation_study"
  exit 1
fi

# 检查数据文件（如果存在才检查）
if [ -f "${QUERY_FILE}" ]; then
  echo "Query file found: ${QUERY_FILE}"
else
  echo "Warning: Query file not found: ${QUERY_FILE}"
fi

if [ -f "${GT_FILE}" ]; then
  echo "Ground truth file found: ${GT_FILE}"
else
  echo "Warning: Ground truth file not found: ${GT_FILE}"
fi

if [ -f "${INSERT_FILE}" ]; then
  echo "Insert file found: ${INSERT_FILE}"
else
  echo "Warning: Insert file not found: ${INSERT_FILE}"
  INSERT_FILE=${DATA_FILE}
fi

# ============= 准备基础索引 =============
prepare_base_index() {
  local base_ratio=$1
  local base_pct
  base_pct=$(python3 -c "print(int(round(${base_ratio} * 100)))")

  # 构造base data文件路径（与prepare_exp1_gt保持一致）
  local base_data="${DATA_FILE%.*}_exp1_base${base_pct}.bin"

  # 构造run_system_comparison.sh生成的exp1索引路径（用于复用）
  local exp1_index_base="${INDEX_BASE}_exp1_base${base_pct}"

  # 优先检查并复用run_system_comparison.sh生成的exp1索引
  if [ -f "${exp1_index_base}_disk.index" ]; then
    # 验证相关文件都存在
    if [ -f "${base_data}" ] && [ -f "${exp1_index_base}_pq_compressed.bin" ] && [ -f "${exp1_index_base}_pq_pivots.bin" ]; then
      echo "Reusing exp1 index from run_system_comparison.sh: ${exp1_index_base}" >&2
      echo "${exp1_index_base}"
      return
    else
      echo "Warning: exp1 index exists but data files incomplete, will rebuild..." >&2
    fi
  fi

  local base_index="${INDEX_BASE}_ablation_base${base_pct}"

  # 如果索引存在，但base data文件不存在，说明索引是基于错误的数据集构建的
  # 需要重新构建索引以确保与GT文件匹配
  if [ -f "${base_index}_disk.index" ] && [ ! -f "${base_data}" ]; then
    echo "Warning: Index exists but base data file missing: ${base_data}" >&2
    echo "Removing stale index to rebuild with correct data..." >&2
    rm -f "${base_index}_disk.index"
    rm -f "${base_index}_pq_compressed.bin"
    rm -f "${base_index}_pq_pivots.bin"
    rm -f "${base_index}_disk.index.tags" 2>/dev/null || true
  fi

  if [ -f "${base_index}_disk.index" ]; then
    echo "Base index already exists: ${base_index}" >&2
    echo "${base_index}"
    return
  fi

  if [ -f "${INDEX_BASE}_disk.index" ]; then
    echo "Using existing full index as base: ${INDEX_BASE}" >&2
    echo "${INDEX_BASE}"
    return
  fi

  # 需要构建基础索引
  echo "Building base index (${base_pct}% of data)..." >&2

  # 分割数据（使用与run_system_comparison.sh一致的命名，便于复用GT文件）
  local base_data="${DATA_FILE%.*}_exp1_base${base_pct}.bin"
  if [ ! -f "${base_data}" ]; then
    python3 - >&2 <<PY
import sys
import struct

data_file = "${DATA_FILE}"
base_file = "${base_data}"
base_ratio = float("${base_ratio}")
data_type = "${DATA_TYPE}"

dtype_size = {"uint8": 1, "int8": 1, "float": 4}.get(data_type)
if dtype_size is None:
    print(f"Unsupported data type: {data_type}", file=sys.stderr)
    sys.exit(1)

with open(data_file, "rb") as f:
    header = f.read(8)
    if len(header) != 8:
        print(f"Invalid data file header: {data_file}", file=sys.stderr)
        sys.exit(1)
    npts, dim = struct.unpack("<ii", header)

base_pts = int(npts * base_ratio)
if base_pts <= 0:
    print(f"Invalid base_pts: {base_pts}", file=sys.stderr)
    sys.exit(1)

print(f"Splitting: {base_pts} / {npts} vectors", file=sys.stderr)

with open(data_file, "rb") as src, open(base_file, "wb") as dst:
    dst.write(struct.pack("<ii", base_pts, dim))
    src.seek(8)
    remaining_bytes = base_pts * dim * dtype_size
    buf_size = 1024 * 1024
    while remaining_bytes > 0:
        to_read = min(buf_size, remaining_bytes)
        chunk = src.read(to_read)
        if not chunk:
            print(f"Unexpected EOF while reading {data_file}", file=sys.stderr)
            sys.exit(1)
        dst.write(chunk)
        remaining_bytes -= len(chunk)
PY
  fi
  
  # 构建索引（输出重定向到stderr，避免污染返回值）
  mkdir -p $(dirname "${base_index}")
  ./build/tests/build_disk_index ${DATA_TYPE} ${base_data} ${base_index} \
    96 128 32 256 ${NUM_THREADS} l2 pq >&2
  
  echo "${base_index}"
}

# ============= 准备实验1专用Groundtruth =============
# 为分割后的base数据集生成对应的GT文件，确保recall计算正确
# 注意：GT文件命名与run_system_comparison.sh保持一致，实现跨脚本复用
prepare_exp1_gt() {
  local base_ratio=$1

  local base_pct
  base_pct=$(python3 -c "print(int(round(${base_ratio} * 100)))")

  local gt_ext="${GT_FILE##*.}"
  local gt_prefix="${GT_FILE%.*}"
  local exp1_gt_k=100
  local exp1_gt_file="${gt_prefix}_exp1_base${base_pct}_k${exp1_gt_k}.${gt_ext}"

  if [ -f "${exp1_gt_file}" ]; then
    echo "Using existing exp1 GT: ${exp1_gt_file}" >&2
    echo "${exp1_gt_file}"
    return
  fi

  # 构造base数据文件路径（与prepare_base_index中的逻辑一致，使用exp1前缀）
  local base_data="${DATA_FILE%.*}_exp1_base${base_pct}.bin"
  if [ ! -f "${base_data}" ]; then
    echo "Error: Base data file not found: ${base_data}" >&2
    echo "Hint: Make sure prepare_base_index was called first to generate the base data file" >&2
    exit 1
  fi

  echo "Generating exp1 GT for base ${base_pct}% (${exp1_gt_k} nearest neighbors)..." >&2
  if [ ! -x "./build/tests/utils/compute_groundtruth" ]; then
    echo "Error: ./build/tests/utils/compute_groundtruth not found or not executable" >&2
    exit 1
  fi

  ./build/tests/utils/compute_groundtruth "${DATA_TYPE}" "${base_data}" "${QUERY_FILE}" "${exp1_gt_k}" "${exp1_gt_file}" >&2
  if [ $? -ne 0 ]; then
    echo "Error: Failed to generate exp1 GT" >&2
    exit 1
  fi

  echo "${exp1_gt_file}"
}

# ============= 函数定义 =============

should_run() {
  local exp=$1
  if [ "${EXPERIMENT}" = "all" ]; then
    return 0
  fi
  if [ "${EXPERIMENT}" = "${exp}" ]; then
    return 0
  fi
  return 1
}

# 运行实验1: 动态聚类消融
run_exp1_clustering() {
  echo ""
  echo "######################################"
  echo "# 实验1: 拓扑感知分配 vs. 随机分配  #"
  echo "######################################"
  echo ""

  # 直接复用run_system_comparison.sh生成的所有文件，不进行任何文件生成
  local base_pct
  base_pct=$(python3 -c "print(int(round(${BASE_RATIO} * 100)))")

  # 使用run_system_comparison.sh的exp1索引路径
  local exp_index="${INDEX_BASE}_exp1_base${base_pct}"

  # 使用run_system_comparison.sh的GT文件路径
  local gt_ext="${GT_FILE##*.}"
  local gt_prefix="${GT_FILE%.*}"
  local exp1_gt="${gt_prefix}_exp1_base${base_pct}_k100.${gt_ext}"

  # 验证所有必需文件存在
  if [ ! -f "${exp_index}_disk.index" ]; then
    echo "Error: exp1 index not found: ${exp_index}_disk.index" >&2
    echo "Please run run_system_comparison.sh first to generate it" >&2
    exit 1
  fi

  if [ ! -f "${exp_index}_pq_compressed.bin" ]; then
    echo "Error: exp1 PQ compressed file not found: ${exp_index}_pq_compressed.bin" >&2
    exit 1
  fi

  if [ ! -f "${exp_index}_pq_pivots.bin" ]; then
    echo "Error: exp1 PQ pivots file not found: ${exp_index}_pq_pivots.bin" >&2
    exit 1
  fi

  if [ ! -f "${exp1_gt}" ]; then
    echo "Error: exp1 GT file not found: ${exp1_gt}" >&2
    echo "Please run run_system_comparison.sh first to generate it" >&2
    exit 1
  fi

  echo "Reusing files from run_system_comparison.sh:" >&2
  echo "  Index: ${exp_index}_disk.index" >&2
  echo "  GT: ${exp1_gt}" >&2
  echo "  PQ: ${exp_index}_pq_compressed.bin" >&2

  ./build/tests/ablation_study "${DATA_TYPE}" "${exp_index}" "${QUERY_FILE}" "${exp1_gt}" \
    "${INSERT_FILE}" 1 "${RESULTS_DIR}" \
    --num-threads ${NUM_THREADS} \
    --insert-count ${INSERT_COUNT} \
    --L-values "${L_VALUES}" \
    --recall-at ${RECALL_AT}

  echo ""
  echo "实验1完成! 结果: ${RESULTS_DIR}/exp_ablation_clustering.csv"
}

# 运行实验2: 重组织机制消融
run_exp2_reorganization() {
  echo ""
  echo "######################################"
  echo "# 实验2: 重组织机制性能恢复测试     #"
  echo "######################################"
  echo ""
  
  local exp_index=$(prepare_base_index ${BASE_RATIO})
  
  ./build/tests/ablation_study "${DATA_TYPE}" "${exp_index}" "${QUERY_FILE}" "${GT_FILE}" \
    "${INSERT_FILE}" 2 "${RESULTS_DIR}" \
    --num-threads ${NUM_THREADS} \
    --duration ${DURATION_SEC} \
    --insert-rate ${INSERT_RATE}
  
  echo ""
  echo "实验2完成! 结果: ${RESULTS_DIR}/exp_ablation_reorganization.csv"
}

# 运行实验3: 流水线消融
run_exp3_pipeline() {
  echo ""
  echo "######################################"
  echo "# 实验3: 异步流水线 vs. 同步阻塞    #"
  echo "######################################"
  echo ""
  
  # 实验3使用完整索引进行搜索测试
  local exp_index="${INDEX_BASE}"
  if [ ! -f "${exp_index}_disk.index" ]; then
    exp_index=$(prepare_base_index 1.0)
  fi
  
  ./build/tests/ablation_study "${DATA_TYPE}" "${exp_index}" "${QUERY_FILE}" "${GT_FILE}" \
    "${INSERT_FILE}" 3 "${RESULTS_DIR}" \
    --num-threads ${NUM_THREADS} \
    --L-values "${L_VALUES}" \
    --recall-at ${RECALL_AT}
  
  echo ""
  echo "实验3完成! 结果: ${RESULTS_DIR}/exp_ablation_pipeline.csv"
}

# 运行实验4: 并发扩展性测试
run_exp4_scalability() {
  echo ""
  echo "######################################"
  echo "# 实验4: 并发控制扩展性测试         #"
  echo "######################################"
  echo ""
  
  local exp_index=$(prepare_base_index ${BASE_RATIO})
  
  ./build/tests/ablation_study "${DATA_TYPE}" "${exp_index}" "${QUERY_FILE}" "${GT_FILE}" \
    "${INSERT_FILE}" 4 "${RESULTS_DIR}" \
    --duration ${DURATION_SEC} \
    --thread-list "${THREAD_LIST}"
  
  echo ""
  echo "实验4完成! 结果: ${RESULTS_DIR}/exp_ablation_scalability.csv"
}

# ============= 主执行流程 =============

echo ""
echo "######################################"
echo "#   消融实验开始                     #"
echo "######################################"
echo ""

# 运行选定的实验
if should_run "1"; then
  run_exp1_clustering
fi

if should_run "2"; then
  run_exp2_reorganization
fi

if should_run "3"; then
  run_exp3_pipeline
fi

if should_run "4"; then
  run_exp4_scalability
fi

# ============= 完成 =============
echo ""
echo "######################################"
echo "#   消融实验完成!                    #"
echo "######################################"
echo ""
echo "结果保存在: ${RESULTS_DIR}"
echo ""
echo "生成的文件:"
ls -lh ${RESULTS_DIR}/*.csv 2>/dev/null || echo "  (无CSV文件)"
echo ""
echo "图表目录:"
ls -lh ${RESULTS_DIR}/figures/*.pdf 2>/dev/null || echo "  (无图表文件)"
echo ""
echo "查看结果命令:"
echo "  cat ${RESULTS_DIR}/exp_ablation_clustering.csv"
echo "  cat ${RESULTS_DIR}/exp_ablation_reorganization.csv"
echo "  cat ${RESULTS_DIR}/exp_ablation_pipeline.csv"
echo "  cat ${RESULTS_DIR}/exp_ablation_scalability.csv"
echo ""
echo "查看汇总:"
echo "  cat ${RESULTS_DIR}/ablation_summary.csv"
