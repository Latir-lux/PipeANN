#!/bin/bash
# 运行所有论文第5章实验

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}/../.."

# 创建结果目录
mkdir -p data/thesis_results

echo "=========================================="
echo "运行论文第5章所有实验"
echo "=========================================="

# 确保编译完成
if [ ! -f "build/tests/thesis_benchmark" ]; then
    echo "请先编译: mkdir -p build && cd build && cmake .. -DUSE_AIO=ON && make -j"
    exit 1
fi

# 运行各实验
bash ${SCRIPT_DIR}/exp1_latency.sh
bash ${SCRIPT_DIR}/exp5_io_amplification.sh
bash ${SCRIPT_DIR}/exp6_pipeline_width.sh

echo "=========================================="
echo "所有实验完成!"
echo "结果保存在 data/thesis_results/"
echo "=========================================="
