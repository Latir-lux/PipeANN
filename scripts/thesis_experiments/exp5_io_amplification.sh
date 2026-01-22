#!/bin/bash
# 论文第5章实验运行脚本
# 实验5: I/O放大率测试 (5.4.1节)

set -e

DATA_DIR=${DATA_DIR:-"/mnt/nvme/data"}
INDEX_DIR=${INDEX_DIR:-"/mnt/nvme2/indices"}
OUTPUT_DIR=${OUTPUT_DIR:-"./data/thesis_results"}

mkdir -p ${OUTPUT_DIR}

NUM_THREADS=32
BEAM_WIDTH=16
RECALL_AT=10
MEM_L=10

echo "=========================================="
echo "实验5: I/O放大率测试 (5.4.1节)"
echo "=========================================="

# DC-PDI (本文方法)
echo "Testing DC-PDI on SIFT..."
if [ -f "${INDEX_DIR}/bigann/100m_disk.index" ]; then
    ./build/tests/thesis_benchmark 5 uint8 \
        ${INDEX_DIR}/bigann/100m \
        ${DATA_DIR}/bigann/bigann_query.bbin \
        ${DATA_DIR}/bigann/100M_gt.bin \
        ${OUTPUT_DIR}/exp5_io_amp_dcpdi_sift.csv \
        ${NUM_THREADS} ${BEAM_WIDTH} ${RECALL_AT} ${MEM_L}
fi

# IP-DiskANN (无聚类优化的对比)
# 注意: 需要准备一个经过大量随机插入后的索引
echo "Testing IP-DiskANN on SIFT (if available)..."
if [ -f "${INDEX_DIR}/bigann/100m_random_insert_disk.index" ]; then
    ./build/tests/thesis_benchmark 5 uint8 \
        ${INDEX_DIR}/bigann/100m_random_insert \
        ${DATA_DIR}/bigann/bigann_query.bbin \
        ${DATA_DIR}/bigann/100M_gt.bin \
        ${OUTPUT_DIR}/exp5_io_amp_ipdiskann_sift.csv \
        ${NUM_THREADS} ${BEAM_WIDTH} ${RECALL_AT} 0  # mem_L=0 for DiskANN mode
fi

echo "实验5完成，结果保存在 ${OUTPUT_DIR}"
