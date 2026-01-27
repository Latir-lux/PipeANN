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

# ============= 准备实验1的数据分片（按比例拆分）复用run_system_comparison.sh生成的文件 =============
# 默认强制重建exp1 GT，避免复用错误范围
FORCE_REGEN_EXP1_GT=${FORCE_REGEN_EXP1_GT:-"1"}
EXP1_GT_K=${EXP1_GT_K:-"100"}
prepare_exp1_data() {
  local data_file=$1
  local data_type=$2
  local base_ratio=$3
  local update_ratio=${4:-"0.5"}

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

  local base_file="${data_prefix}_exp1_base${base_pct}.${data_ext}"
  local update_file="${data_prefix}_exp1_update${update_pct}.${data_ext}"

  # Exp1 GT文件路径（基于base ratio的数据集）
  local gt_ext="${GT_FILE##*.}"
  local gt_prefix="${GT_FILE%.*}"
  local exp1_gt_k=${EXP1_GT_K}
  if [ "${exp1_gt_k}" -lt 100 ]; then
    exp1_gt_k=100
  fi
  local exp1_gt_file="${gt_prefix}_exp1_base${base_pct}_k${exp1_gt_k}.${gt_ext}"

  if [ -f "${base_file}" ] && [ -f "${update_file}" ]; then
    echo "Using existing exp1 data splits: ${base_file}, ${update_file}" >&2
    EXP1_BASE_FILE=${base_file}
    EXP1_UPDATE_FILE=${update_file}
  else
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
remaining = max(0, npts - base_pts)
update_pts = int(remaining * update_ratio)
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
    EXP1_BASE_FILE=${base_file}
    EXP1_UPDATE_FILE=${update_file}
  fi

  # 获取基础/更新数据集点数，用于生成GT
  local base_pts_count
  base_pts_count=$(python3 - <<PY
import struct
with open("${EXP1_BASE_FILE}", "rb") as f:
    npts, dim = struct.unpack("<ii", f.read(8))
    print(npts)
PY
)
  if [ "${base_pts_count}" -lt "${exp1_gt_k}" ]; then
    echo "Error: base points (${base_pts_count}) less than required GT K (${exp1_gt_k})" >&2
    exit 1
  fi

  if [ "${FORCE_REGEN_EXP1_GT}" = "1" ] && [ -f "${exp1_gt_file}" ]; then
    echo "Removing existing exp1 GT to force regeneration: ${exp1_gt_file}" >&2
    rm -f "${exp1_gt_file}"
  fi

  if [ -f "${exp1_gt_file}" ]; then
    echo "Using existing exp1 GT: ${exp1_gt_file}" >&2
  else
    echo "Generating exp1 GT for base ${base_pct}% (${base_pts_count} points), K=${exp1_gt_k}..." >&2
    if [ ! -x "./build/tests/utils/compute_groundtruth" ]; then
      echo "Error: ./build/tests/utils/compute_groundtruth not found or not executable" >&2
      exit 1
    fi
    ./build/tests/utils/compute_groundtruth "${data_type}" "${EXP1_BASE_FILE}" "${QUERY_FILE}" "${exp1_gt_k}" "${exp1_gt_file}" >&2
    if [ $? -ne 0 ]; then
      echo "Error: Failed to generate exp1 GT" >&2
      exit 1
    fi
  fi

  EXP1_GT_FILE=${exp1_gt_file}
  echo "Exp1 GT file: ${EXP1_GT_FILE}" >&2
}

# ============= 验证GT文件 =============
# 使用gt_update工具验证GT文件是否包含足够的近邻
validate_exp1_gt() {
  local gt_file=$1
  local base_pts=$2
  local total_pts=$3
  local target_topk=$4

  echo "Validating GT file: ${gt_file}" >&2
  echo "  Base points: ${base_pts}" >&2
  echo "  Total points: ${total_pts}" >&2
  echo "  Target top-K: ${target_topk}" >&2

  if [ ! -x "./build/tests/gt_update" ]; then
    echo "Error: ./build/tests/gt_update executable not found or not executable" >&2
    echo "Please compile first: cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make gt_update" >&2
    exit 1
  fi

  if [ ! -f "${gt_file}" ]; then
    echo "Error: GT file not found: ${gt_file}" >&2
    exit 1
  fi

  # 创建验证结果目录
  local validate_dir="${RESULTS_DIR}/gt_validate"
  mkdir -p "${validate_dir}"

  echo "Running GT validation..." >&2
  # gt_update <gt_file> <index_npts> <total_npts> <batch_npts> <target_topk> <target_dir> <insert_only>
  # 使用batch_npts=1000做验证
  ./build/tests/gt_update "${gt_file}" "${base_pts}" "${total_pts}" 1000 "${target_topk}" "${validate_dir}" 1 >&2
  
  if [ $? -ne 0 ]; then
    echo "Warning: GT validation encountered issues" >&2
  else
    echo "GT validation completed successfully" >&2
  fi
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

  # 准备实验1的数据分片（从run_system_comparison.sh复用）
  prepare_exp1_data ${DATA_FILE} ${DATA_TYPE} ${BASE_RATIO} 0.5
  local base_pct
  base_pct=$(python3 -c "print(int(round(${BASE_RATIO} * 100)))")

  # 使用run_system_comparison.sh的exp1索引路径
  local exp_index="${INDEX_BASE}_exp1_base${base_pct}"

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

  if [ ! -f "${EXP1_GT_FILE}" ]; then
    echo "Error: exp1 GT file not found: ${EXP1_GT_FILE}" >&2
    echo "Please run run_system_comparison.sh first to generate it" >&2
    exit 1
  fi

  echo "Reusing files from run_system_comparison.sh:" >&2
  echo "  Index: ${exp_index}_disk.index" >&2
  echo "  GT: ${EXP1_GT_FILE}" >&2
  echo "  PQ: ${exp_index}_pq_compressed.bin" >&2
  echo "  Base data: ${EXP1_BASE_FILE}" >&2
  echo "  Update data: ${EXP1_UPDATE_FILE}" >&2

  # 验证GT文件
  local base_pts_count
  base_pts_count=$(python3 - <<PY
import struct
with open("${EXP1_BASE_FILE}", "rb") as f:
    npts, dim = struct.unpack("<ii", f.read(8))
    print(npts)
PY
)
  local total_pts_count
  total_pts_count=$(python3 - <<PY
import struct
with open("${DATA_FILE}", "rb") as f:
    npts, dim = struct.unpack("<ii", f.read(8))
    print(npts)
PY
)
  validate_exp1_gt "${EXP1_GT_FILE}" "${base_pts_count}" "${total_pts_count}" 100

  ./build/tests/ablation_study "${DATA_TYPE}" "${exp_index}" "${QUERY_FILE}" "${EXP1_GT_FILE}" \
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
