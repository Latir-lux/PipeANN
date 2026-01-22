#!/bin/bash
# 论文第5章实验运行脚本
# 实验6: 流水线宽度敏感性测试 (5.5.1节)

set -e

DATA_DIR=${DATA_DIR:-"/mnt/nvme/data"}
INDEX_DIR=${INDEX_DIR:-"/mnt/nvme2/indices"}
OUTPUT_DIR=${OUTPUT_DIR:-"./data/thesis_results"}

mkdir -p ${OUTPUT_DIR}

NUM_THREADS=32
RECALL_AT=10
MEM_L=10

echo "=========================================="
echo "实验6: 流水线宽度敏感性测试 (5.5.1节)"
echo "=========================================="

# SIFT数据集
echo "Testing pipeline width on SIFT..."
if [ -f "${INDEX_DIR}/bigann/100m_disk.index" ]; then
    ./build/tests/thesis_benchmark 6 uint8 \
        ${INDEX_DIR}/bigann/100m \
        ${DATA_DIR}/bigann/bigann_query.bbin \
        ${DATA_DIR}/bigann/100M_gt.bin \
        ${OUTPUT_DIR}/exp6_pipeline_width_sift.csv \
        ${NUM_THREADS} 16 ${RECALL_AT} ${MEM_L}
fi

# DEEP数据集
echo "Testing pipeline width on DEEP..."
if [ -f "${INDEX_DIR}/deep/100M_disk.index" ]; then
    ./build/tests/thesis_benchmark 6 float \
        ${INDEX_DIR}/deep/100M \
        ${DATA_DIR}/deep/queries.fbin \
        ${DATA_DIR}/deep/100M_gt.bin \
        ${OUTPUT_DIR}/exp6_pipeline_width_deep.csv \
        ${NUM_THREADS} 16 ${RECALL_AT} ${MEM_L}
fi

echo "实验6完成，结果保存在 ${OUTPUT_DIR}"
