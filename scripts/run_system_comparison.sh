#!/bin/bash
#
# 运行DC-PDI、IP-DiskANN和FreshDiskANN的对比实验
#
# 使用方法:
#   ./scripts/run_system_comparison.sh [experiment] [dataset] [base_dir] [output_dir] [exp1_base_ratio] [exp1_update_ratio] [exp1_duration_sec] [exp1_target_recall] [exp2_base_ratio] [exp2_update_rate] [exp2_duration_sec] [exp3_base_ratio] [exp3_update_ratio] [exp3_duration_sec] [exp2_update_ratio] [systems]
#
# 参数:
#   experiment: 1/2/3/all (默认: all)
#   dataset: sift/deep/gist (默认: sift)
#   base_dir: 数据集和索引的基础目录 (默认: /mnt/xiaoxuanx/dataset)
#   output_dir: 输出目录 (默认: /mnt/xiaoxuanx/dataset/exp/thesis_results/system_comparison)
#   exp1_base_ratio: 实验1基础索引占比 (默认: 0.5)
#   exp1_update_ratio: 实验1更新集占比(在剩余更新集中取比例, 默认: 0.5)
#   exp1_duration_sec: 实验1持续时间(秒, 默认: 180)
#   exp1_target_recall: 实验1目标召回率百分比(默认: 90)
#   exp2_base_ratio: 实验2基础索引占比 (默认: 0.5)
#   exp2_update_rate: 实验2更新速率(向量/秒, 0=不限制) (默认: 0)
#   exp2_duration_sec: 实验2持续时间(秒, 0=全量更新) (默认: 0)
#   exp2_update_ratio: 实验2更新集占比(在剩余更新集中取比例, 默认: 1.0)
#   systems: 要测试的系统 (dc-pdi/ip-diskann/fresh-diskann/all, 逗号分隔, 默认: all)
#            例如: "dc-pdi,fresh-diskann" 只测试这两个系统

set -e

# ============= 配置参数 =============
EXPERIMENT=${1:-"all"}
DATASET=${2:-"sift"}
BASE_DIR=${3:-"/mnt/xiaoxuanx/dataset"}
OUTPUT_DIR=${4:-"/mnt/xiaoxuanx/dataset/exp/thesis_results/system_comparison"}
EXP1_BASE_RATIO=${5:-${EXP1_BASE_RATIO:-"0.5"}}
EXP1_UPDATE_RATIO=${6:-${EXP1_UPDATE_RATIO:-"0.5"}}
EXP1_DURATION_SEC=${7:-${EXP1_DURATION_SEC:-"180"}}
EXP1_TARGET_RECALL=${8:-${EXP1_TARGET_RECALL:-"90"}}
EXP2_BASE_RATIO=${9:-"0.5"}
EXP2_UPDATE_RATE=${10:-"0"}
EXP2_DURATION_SEC=${11:-"0"}
EXP3_BASE_RATIO=${12:-"0.5"}
EXP3_UPDATE_RATIO=${13:-"0.5"}
EXP3_DURATION_SEC=${14:-"120"}
EXP2_UPDATE_RATIO=${15:-${EXP2_UPDATE_RATIO:-"1.0"}}
SYSTEMS_TO_TEST=${16:-"all"}
NUM_THREADS=32
RECALL_AT=10

TOTAL_MEM_KB=$(awk '/MemTotal/ {print $2}' /proc/meminfo 2>/dev/null || true)
if [ -n "${TOTAL_MEM_KB}" ]; then
  BUILD_RAM_GB=$(awk -v kb="${TOTAL_MEM_KB}" 'BEGIN { printf "%d", kb / 1024 / 1024 / 2 }')
else
  BUILD_RAM_GB=128
fi
if [ "${BUILD_RAM_GB}" -lt 1 ]; then
  BUILD_RAM_GB=1
fi

# 为不同数据集隔离输出目录，避免覆盖
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
    INSERT_FILE="${BASE_DIR}/bigann/bigann_learn.bin"  # 用于插入测试
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
echo "Dataset: $DATASET"
echo "Data file: $DATA_FILE"
echo "Query file: $QUERY_FILE"
echo "GT file: $GT_FILE"
echo "Index base: $INDEX_BASE"
echo "Output directory: $RESULTS_DIR"
echo "Experiment: $EXPERIMENT"
echo "Exp1 base ratio: $EXP1_BASE_RATIO"
echo "Exp1 update ratio: $EXP1_UPDATE_RATIO"
echo "Exp1 duration sec: $EXP1_DURATION_SEC"
echo "Exp1 target recall: $EXP1_TARGET_RECALL"
echo "Exp2 base ratio: $EXP2_BASE_RATIO"
echo "Exp2 update rate: $EXP2_UPDATE_RATE"
echo "Exp2 duration sec: $EXP2_DURATION_SEC"
echo "Exp2 update ratio: $EXP2_UPDATE_RATIO"
echo "Build RAM budget (GB): ${BUILD_RAM_GB}"
echo "Exp3 base ratio: $EXP3_BASE_RATIO"
echo "Exp3 update ratio: $EXP3_UPDATE_RATIO"
echo "Exp3 duration sec: $EXP3_DURATION_SEC"
echo "Systems to test: $SYSTEMS_TO_TEST"
echo "============================================"

if [ ! -f "${INSERT_FILE}" ]; then
  echo "Warning: Insert file not found, fallback to data file: ${INSERT_FILE}" >&2
  INSERT_FILE=${DATA_FILE}
fi

# L值列表（用于搜索延迟测试）
L_VALUES="50 55 60 65 70 75 80 85 90 95 100 105 110 115 120 125 130 135 140 145 150 155 160 165 170 175 180 185 190 195 200 205 210 215 220 225 230 235 240 245 250 255 260 265 270 275 280 285 290 295 300 305 310 315 320 325 330 335 340 345 350 355 360 365 370 375 380 385 390 395 400 405 410 415 420 425 430 435 440 445 450 455 460 465 470 475 480 485 490 495 500"

# ============= 函数定义 =============

# 判断是否应该测试某个系统
should_test_system() {
  local system=$1
  if [ "${SYSTEMS_TO_TEST}" = "all" ]; then
    return 0
  fi
  # 将逗号分隔的系统列表转换为数组并检查
  IFS=',' read -ra SYSTEMS_ARRAY <<< "${SYSTEMS_TO_TEST}"
  for s in "${SYSTEMS_ARRAY[@]}"; do
    if [ "${s}" = "${system}" ]; then
      return 0
    fi
  done
  return 1
}

# 软链接/复制辅助函数（优先硬链接，失败则复制）
link_or_copy() {
  local src=$1
  local dst=$2

  if [ -f "${src}" ] && [ ! -f "${dst}" ]; then
    ln "${src}" "${dst}" 2>/dev/null || cp "${src}" "${dst}"
  fi
}

# 确保索引输出目录存在
ensure_index_dir() {
  local index_prefix=$1
  local index_dir
  index_dir=$(dirname "${index_prefix}")
  mkdir -p "${index_dir}"
}

# 清理系统专用索引文件
cleanup_system_index() {
  local system_index=$1

  rm -f "${system_index}_disk.index" "${system_index}_disk.index.tags"
  rm -f "${system_index}_pq_compressed.bin" "${system_index}_pq_pivots.bin"
  rm -f "${system_index}_sample_data.bin" "${system_index}_partition.bin.aligned"
  rm -f "${system_index}_mem.index" "${system_index}_mem.index.data" "${system_index}_mem.index.tags"
  rm -rf "${system_index}_merge" "${system_index}_merge"*
}

# 准备索引的函数
prepare_index() {
  local system_name=$1
  local search_mode=$2
  local index_base=${3:-"${INDEX_BASE}"}
  local read_only=${4:-"false"}
  local data_file=${5:-"${DATA_FILE}"}
  
  echo "[$(date)] Preparing index for $system_name..." >&2
  
  # 如果索引不存在，先构建
  if [ ! -f "${index_base}_disk.index" ]; then
    echo "Building disk index..." >&2
    ensure_index_dir "${index_base}"
    ./build/tests/build_disk_index ${DATA_TYPE} ${data_file} ${index_base} \
      96 128 32 ${BUILD_RAM_GB} ${NUM_THREADS} l2 pq >&2
    if [ ! -f "${index_base}_disk.index" ]; then
      echo "Error: Index build failed, missing ${index_base}_disk.index" >&2
      exit 1
    fi
  fi
  
  if [ "${read_only}" = "true" ]; then
    echo "[$(date)] Reusing shared index for ${system_name}: ${index_base}" >&2
    echo "${index_base}"
    return
  fi

  # 为不同系统准备专用索引（仅复制会被修改的文件）
  local system_index="${index_base}_${system_name}"
  if [ ! -f "${system_index}_disk.index" ]; then
    echo "Preparing writable index for $system_name..." >&2
    # 主索引文件需要独立副本
    cp ${index_base}_disk.index ${system_index}_disk.index
    # 标签文件可能被更新，保持独立副本
    cp ${index_base}_disk.index.tags ${system_index}_disk.index.tags 2>/dev/null || true
    # 共享只读文件（优先硬链接，失败则复制）
    link_or_copy ${index_base}_pq_compressed.bin ${system_index}_pq_compressed.bin
    link_or_copy ${index_base}_pq_pivots.bin ${system_index}_pq_pivots.bin
    link_or_copy ${index_base}_sample_data.bin ${system_index}_sample_data.bin
    link_or_copy ${index_base}_partition.bin.aligned ${system_index}_partition.bin.aligned
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
  
  prepare_exp1_data ${DATA_FILE} ${DATA_TYPE} ${EXP1_BASE_RATIO} ${EXP1_UPDATE_RATIO}
  local exp1_base_tag
  exp1_base_tag=$(printf "%s" "${EXP1_BASE_RATIO}" | tr '.' 'p')
  local exp1_index_base="${INDEX_BASE}_exp1_base${exp1_base_tag}"
  local system_index=$(prepare_index ${system_name} ${system_type} "${exp1_index_base}" false "${EXP1_BASE_FILE}")
  local output_file="${RESULTS_DIR}/exp1_search_latency_${system_name}.csv"

  echo "[$(date)] Running search latency test for ${system_name}..."
  rm -f "${output_file}"
  ./build/tests/compare_systems ${DATA_TYPE} ${system_index} ${QUERY_FILE} ${GT_FILE} \
    ${EXP1_UPDATE_FILE} ${system_type} 1 ${RESULTS_DIR} ${NUM_THREADS} ${RECALL_AT} ${L_VALUES} \
    --exp1-duration-sec ${EXP1_DURATION_SEC} --exp1-update-ratio 1.0 --exp1-target-recall ${EXP1_TARGET_RECALL}
  
  echo "[$(date)] Search latency test completed: ${output_file}"
  cleanup_system_index ${system_index}
}

# 运行更新吞吐量实验
run_update_throughput_exp() {
  local system_type=$1
  local system_name=$2
  local insert_file=$3
  local index_base=$4
  
  echo ""
  echo "========================================"
  echo "实验2: 更新吞吐量测试 - ${system_name}"
  echo "========================================"
  
  local system_index=$(prepare_index ${system_name} ${system_type} ${index_base} false)
  local output_file="${RESULTS_DIR}/exp2_update_throughput_${system_name}.csv"
  
  echo "[$(date)] Running update throughput test for ${system_name}..."
  ./build/tests/compare_systems ${DATA_TYPE} ${system_index} ${QUERY_FILE} ${GT_FILE} \
    ${insert_file} ${system_type} 2 ${RESULTS_DIR} ${NUM_THREADS} ${RECALL_AT}
  
  echo "[$(date)] Update throughput test completed: ${output_file}"
  cleanup_system_index ${system_index}
}

# 准备实验2的数据分片（按比例拆分，并可限制更新量）
prepare_exp2_data() {
  local data_file=$1
  local data_type=$2
  local base_ratio=$3
  local update_ratio=$4
  local update_rate=$5
  local duration_sec=$6

  local data_ext="${data_file##*.}"
  local data_prefix="${data_file%.*}"

  local base_pct
  base_pct=$(python3 - <<PY
import math
print(int(round(${base_ratio} * 100)))
PY
)

  local base_file="${data_prefix}_exp2_base${base_pct}.${data_ext}"
  local update_file="${data_prefix}_exp2_update${base_pct}.${data_ext}"

  if [ -f "${base_file}" ] && [ -f "${update_file}" ]; then
    EXP2_BASE_FILE=${base_file}
    EXP2_UPDATE_FILE=${update_file}
  else
    python3 - <<PY
import os
import struct

data_file = "${data_file}"
base_file = "${base_file}"
update_file = "${update_file}"
base_ratio = float("${base_ratio}")
update_ratio = 1.0 - base_ratio
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
    EXP2_BASE_FILE=${base_file}
    EXP2_UPDATE_FILE=${update_file}
  fi

  if [ "${update_ratio}" != "1" ]; then
    local ratio_target
    local update_total
    read -r ratio_target update_total <<<$(python3 - <<PY
import struct

ratio = float("${update_ratio}")
with open("${EXP2_UPDATE_FILE}", "rb") as f:
    header = f.read(8)
    if len(header) != 8:
        raise SystemExit("Invalid update file header")
    npts, _ = struct.unpack("<ii", header)
target = int(max(0, ratio * float(npts)))
print(target, npts)
PY
)
    if [ "${ratio_target}" -gt 0 ] && [ "${ratio_target}" -lt "${update_total}" ]; then
      local ratio_tag
      ratio_tag=$(python3 - <<PY
ratio=float("${update_ratio}")
print(int(round(ratio * 100)))
PY
)
      local ratio_file="${data_prefix}_exp2_update${base_pct}_ratio${ratio_tag}.${data_ext}"
      if [ ! -f "${ratio_file}" ]; then
        python3 - <<PY
import struct

src_file = "${EXP2_UPDATE_FILE}"
dst_file = "${ratio_file}"
target = int("${ratio_target}")
data_type = "${data_type}"

dtype_size = {"uint8": 1, "int8": 1, "float": 4}.get(data_type)
if dtype_size is None:
    raise SystemExit(f"Unsupported data type: {data_type}")

with open(src_file, "rb") as src:
    header = src.read(8)
    if len(header) != 8:
        raise SystemExit(f"Invalid data file header: {src_file}")
    npts, dim = struct.unpack("<ii", header)
    target = min(target, npts)
    if target <= 0:
        raise SystemExit("Target inserts must be > 0")
    with open(dst_file, "wb") as dst:
        dst.write(struct.pack("<ii", target, dim))
        remaining_bytes = target * dim * dtype_size
        buf_size = 1024 * 1024
        while remaining_bytes > 0:
            to_read = min(buf_size, remaining_bytes)
            chunk = src.read(to_read)
            if not chunk:
                raise SystemExit(f"Unexpected EOF while reading {src_file}")
            dst.write(chunk)
            remaining_bytes -= len(chunk)
PY
      fi
      EXP2_UPDATE_FILE=${ratio_file}
    fi
  fi

  if [ "${update_rate}" != "0" ] && [ "${duration_sec}" != "0" ]; then
    local target_inserts
    target_inserts=$(python3 - <<PY
rate=float("${update_rate}")
duration=float("${duration_sec}")
target=int(rate*duration)
print(max(0, target))
PY
)
    if [ "${target_inserts}" -gt 0 ]; then
      local capped_file="${data_prefix}_exp2_update${base_pct}_cap${target_inserts}.${data_ext}"
      if [ ! -f "${capped_file}" ]; then
        python3 - <<PY
import struct

src_file = "${EXP2_UPDATE_FILE}"
dst_file = "${capped_file}"
target = int("${target_inserts}")
data_type = "${data_type}"

dtype_size = {"uint8": 1, "int8": 1, "float": 4}.get(data_type)
if dtype_size is None:
    raise SystemExit(f"Unsupported data type: {data_type}")

with open(src_file, "rb") as src:
    header = src.read(8)
    if len(header) != 8:
        raise SystemExit(f"Invalid data file header: {src_file}")
    npts, dim = struct.unpack("<ii", header)
    target = min(target, npts)
    if target <= 0:
        raise SystemExit("Target inserts must be > 0")
    with open(dst_file, "wb") as dst:
        dst.write(struct.pack("<ii", target, dim))
        remaining_bytes = target * dim * dtype_size
        buf_size = 1024 * 1024
        while remaining_bytes > 0:
            to_read = min(buf_size, remaining_bytes)
            chunk = src.read(to_read)
            if not chunk:
                raise SystemExit(f"Unexpected EOF while reading {src_file}")
            dst.write(chunk)
            remaining_bytes -= len(chunk)
PY
      fi
      EXP2_UPDATE_FILE=${capped_file}
    fi
  fi
}


# 准备实验1的数据分片（按比例拆分）
prepare_exp1_data() {
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

  local base_file="${data_prefix}_exp1_base${base_pct}.${data_ext}"
  local update_file="${data_prefix}_exp1_update${update_pct}.${data_ext}"

  if [ -f "${base_file}" ] && [ -f "${update_file}" ]; then
    echo "Using existing exp1 data splits: ${base_file}, ${update_file}" >&2
    EXP1_BASE_FILE=${base_file}
    EXP1_UPDATE_FILE=${update_file}
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
  
  local system_index=$(prepare_index ${system_name} ${system_type} ${index_base} false)
  local output_file="${RESULTS_DIR}/exp3_concurrent_${system_name}.csv"
  
  echo "[$(date)] Running concurrent test for ${system_name} (${duration_sec}s)..."
  ./build/tests/compare_systems ${DATA_TYPE} ${system_index} ${QUERY_FILE} ${GT_FILE} \
    ${insert_file} ${system_type} 3 ${RESULTS_DIR} ${NUM_THREADS} ${RECALL_AT} ${L_VALUES} \
    --exp3-duration-sec ${duration_sec} --exp3-update-ratio 1.0
  
  echo "[$(date)] Concurrent test completed: ${output_file}"
  cleanup_system_index ${system_index}
}

# ============= 主执行流程 =============

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
if should_run "1"; then
  echo ""
  echo ">>> 开始实验1: 搜索延迟分布测试 <<<"
  if should_test_system "dc-pdi"; then
    run_search_latency_exp 0 "dc-pdi"
  fi
  if should_test_system "ip-diskann"; then
    run_search_latency_exp 1 "ip-diskann"
  fi
  if should_test_system "fresh-diskann"; then
    run_search_latency_exp 2 "fresh-diskann"
  fi
fi

# 实验2: 更新吞吐量
if should_run "2"; then
  echo ""
  echo ">>> 开始实验2: 更新吞吐量测试 <<<"
  prepare_exp2_data ${DATA_FILE} ${DATA_TYPE} ${EXP2_BASE_RATIO} ${EXP2_UPDATE_RATIO} ${EXP2_UPDATE_RATE} ${EXP2_DURATION_SEC}

  EXP2_BASE_TAG=$(printf "%s" "${EXP2_BASE_RATIO}" | tr '.' 'p')
  EXP2_INDEX_BASE="${INDEX_BASE}_exp2_base${EXP2_BASE_TAG}"

  if [ ! -f "${EXP2_INDEX_BASE}_disk.index" ]; then
    echo "Building exp2 base index..." >&2
    ensure_index_dir "${EXP2_INDEX_BASE}"
    ./build/tests/build_disk_index ${DATA_TYPE} ${EXP2_BASE_FILE} ${EXP2_INDEX_BASE} \
      96 128 32 ${BUILD_RAM_GB} ${NUM_THREADS} l2 pq >&2
    if [ ! -f "${EXP2_INDEX_BASE}_disk.index" ]; then
      echo "Error: Exp2 index build failed, missing ${EXP2_INDEX_BASE}_disk.index" >&2
      exit 1
    fi
  fi

  if should_test_system "dc-pdi"; then
    run_update_throughput_exp 0 "dc-pdi" ${EXP2_UPDATE_FILE} ${EXP2_INDEX_BASE}
  fi
  if should_test_system "ip-diskann"; then
    run_update_throughput_exp 1 "ip-diskann" ${EXP2_UPDATE_FILE} ${EXP2_INDEX_BASE}
  fi
  if should_test_system "fresh-diskann"; then
    run_update_throughput_exp 2 "fresh-diskann" ${EXP2_UPDATE_FILE} ${EXP2_INDEX_BASE}
  fi
fi

# 实验3: 读写并发性能
if should_run "3"; then
  echo ""
  echo ">>> 开始实验3: 读写并发性能测试 <<<"
  prepare_exp3_data ${DATA_FILE} ${DATA_TYPE} ${EXP3_BASE_RATIO} ${EXP3_UPDATE_RATIO}

  EXP3_BASE_TAG=$(printf "%s" "${EXP3_BASE_RATIO}" | tr '.' 'p')
  EXP3_INDEX_BASE="${INDEX_BASE}_exp3_base${EXP3_BASE_TAG}"

  if [ ! -f "${EXP3_INDEX_BASE}_disk.index" ]; then
    echo "Building exp3 base index..." >&2
    ensure_index_dir "${EXP3_INDEX_BASE}"
    ./build/tests/build_disk_index ${DATA_TYPE} ${EXP3_BASE_FILE} ${EXP3_INDEX_BASE} \
      96 128 32 ${BUILD_RAM_GB} ${NUM_THREADS} l2 pq >&2
    if [ ! -f "${EXP3_INDEX_BASE}_disk.index" ]; then
      echo "Error: Exp3 index build failed, missing ${EXP3_INDEX_BASE}_disk.index" >&2
      exit 1
    fi
  fi

  if should_test_system "dc-pdi"; then
    run_concurrent_exp 0 "dc-pdi" ${EXP3_DURATION_SEC} ${EXP3_INDEX_BASE} ${EXP3_UPDATE_FILE}
  fi
  if should_test_system "ip-diskann"; then
    run_concurrent_exp 1 "ip-diskann" ${EXP3_DURATION_SEC} ${EXP3_INDEX_BASE} ${EXP3_UPDATE_FILE}
  fi
  if should_test_system "fresh-diskann"; then
    run_concurrent_exp 2 "fresh-diskann" ${EXP3_DURATION_SEC} ${EXP3_INDEX_BASE} ${EXP3_UPDATE_FILE}
  fi
fi

# ============= 完成 =============
echo ""
echo "######################################"
echo "#   所有实验完成!                    #"
echo "######################################"
echo ""
echo "结果保存在: ${RESULTS_DIR}"
echo ""
echo "生成的文件:"
ls -lh ${RESULTS_DIR}/*.csv

echo ""
echo "可以使用以下命令查看结果:"
echo "  cat ${RESULTS_DIR}/exp1_search_latency_*.csv"
echo "  cat ${RESULTS_DIR}/exp2_update_throughput_*.csv"
echo "  cat ${RESULTS_DIR}/exp3_concurrent_*.csv"
