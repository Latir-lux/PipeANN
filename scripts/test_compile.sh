#!/bin/bash
# 编译测试脚本 - 验证代码修改是否能正确编译
# 使用方法: ./scripts/test_compile.sh

set -e

echo "=========================================="
echo "PipeANN 编译测试"
echo "=========================================="

cd "$(dirname "$0")/.."

# 确保 build 目录存在
mkdir -p build
cd build

# 配置 CMake
echo "配置 CMake..."
cmake .. -DCMAKE_BUILD_TYPE=Release

# 编译
echo "开始编译..."
make -j$(nproc) search_disk_index

echo ""
echo "=========================================="
echo "编译成功！"
echo "=========================================="
echo ""
echo "可执行文件: build/tests/search_disk_index"
echo ""
echo "使用示例 (启用追踪和碎片化):"
echo "  ./build/tests/search_disk_index float <index_prefix> 1 8 \\"
echo "      <query.bin> <gt.bin> 10 l2 pq 2 0 50 \\"
echo "      --trace --trace-output ./draw --fragmentation 0.5"
