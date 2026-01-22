#!/bin/bash
# 论文第5章实验运行脚本
# 实验1: 搜索延迟分布测试 (5.2.1节)

set -e

# 配置路径 - 根据实际环境修改
DATA_DIR=${DATA_DIR:-"/mnt/nvme/data"}
INDEX_DIR=${INDEX_DIR:-"/mnt/nvme2/indices"}
OUTPUT_DIR=${OUTPUT_DIR:-"./data/thesis_results"}

mkdir -p ${OUTPUT_DIR}

# 通用参数
NUM_THREADS=32
BEAM_WIDTH=16
RECALL_AT=10
MEM_L=10

echo "=========================================="
echo "实验1: 搜索延迟分布测试 (5.2.1节)"
echo "=========================================="

# SIFT1B 数据集 (如果有100M子集则使用)
echo "Testing on SIFT1B..."
if [ -f "${INDEX_DIR}/bigann/100m_disk.index" ]; then
    ./build/tests/thesis_benchmark 1 uint8 \
        ${INDEX_DIR}/bigann/100m \
        ${DATA_DIR}/bigann/bigann_query.bbin \
        ${DATA_DIR}/bigann/100M_gt.bin \
        ${OUTPUT_DIR}/exp1_latency_sift100m.csv \
        ${NUM_THREADS} ${BEAM_WIDTH} ${RECALL_AT} ${MEM_L}
fi

# DEEP 数据集
echo "Testing on DEEP..."
if [ -f "${INDEX_DIR}/deep/100M_disk.index" ]; then
    ./build/tests/thesis_benchmark 1 float \
        ${INDEX_DIR}/deep/100M \
        ${DATA_DIR}/deep/queries.fbin \
        ${DATA_DIR}/deep/100M_gt.bin \
        ${OUTPUT_DIR}/exp1_latency_deep100m.csv \
        ${NUM_THREADS} ${BEAM_WIDTH} ${RECALL_AT} ${MEM_L}
fi

# GIST 数据集
echo "Testing on GIST..."
if [ -f "${INDEX_DIR}/gist/1m_disk.index" ]; then
    ./build/tests/thesis_benchmark 1 uint8 \
        ${INDEX_DIR}/gist/1m \
        ${DATA_DIR}/gist/gist_query.bin \
        ${DATA_DIR}/gist/1M_gt.bin \
        ${OUTPUT_DIR}/exp1_latency_gist1m.csv \
        ${NUM_THREADS} ${BEAM_WIDTH} ${RECALL_AT} ${MEM_L}
fi

echo "实验1完成，结果保存在 ${OUTPUT_DIR}"
