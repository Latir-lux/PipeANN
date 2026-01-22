# DC-PDI实验执行指南

本文档提供DC-PDI系统实验的详细执行步骤，包括编译、数据准备、索引构建和实验运行。

---

## 📋 实验列表

已实现的实验代码：

| 实验编号 | 实验名称 | 对应论文章节 | 输出文件 |
|---------|---------|-------------|---------|
| 实验8 | 物理离散度评估 | 第3.2节 | CSV格式统计数据 |
| 实验1 | 搜索延迟分布 | 第5.2.1节 | 延迟分布数据 |
| 实验5 | I/O放大率测试 | 第5.4.1节 | I/O统计数据 |
| 实验6 | 流水线宽度敏感性 | 第5.5.1节 | 性能曲线数据 |
| 实验7 | 资源开销评估 | 第5.5.3节 | 资源使用统计 |

---

## 🔧 步骤1: 编译系统

### 1.1 配置编译选项

编辑 `CMakeLists.txt` 确认以下选项：

```cmake
option(USE_AIO "Use libaio for I/O" ON)                      # 使用libaio
option(ENABLE_BLOCK_AWARE_PRUNE "Enable block-aware pruning" ON)  # 块感知剪枝
option(ENABLE_DISPERSION_MONITOR "Enable dispersion monitor" ON)  # 离散度监控
option(COLLECT_IO_STATS "Collect I/O statistics" ON)         # 收集I/O统计
```

### 1.2 执行编译

```bash
cd /home/latir/WorkSpace/PipeANN

# 清理旧的构建（如果需要）
rm -rf build

# 创建构建目录
mkdir -p build && cd build

# 配置CMake（启用所有DC-PDI选项）
cmake .. \
  -DUSE_AIO=ON \
  -DENABLE_BLOCK_AWARE_PRUNE=ON \
  -DENABLE_DISPERSION_MONITOR=ON \
  -DCOLLECT_IO_STATS=ON

# 编译（使用所有CPU核心）
make -j$(nproc)

# 验证编译成功
ls -lh tests/thesis_benchmark tests/build_disk_index
```

编译成功后应看到以下可执行文件：
- `tests/thesis_benchmark` - 实验主程序
- `tests/build_disk_index` - 索引构建工具
- `tests/build_memory_index` - 内存索引构建工具

---

## 📦 步骤2: 准备数据集

### 2.1 数据集要求

实验需要以下文件：

| 文件类型 | 格式 | 说明 |
|---------|------|------|
| 基础数据集 | `.bin`/`.fbin`/`.bbin` | 向量数据（float/uint8/int8） |
| 查询集 | `.bin`/`.fbin`/`.bbin` | 查询向量 |
| 真值集 | `.bin` | Ground truth（用于计算召回率） |

### 2.2 示例数据集路径

假设数据位于 `/mnt/data/`:

```bash
# SIFT1M 示例（小数据集，适合快速测试）
BASE_DATA=/mnt/data/sift/sift_base.fbin       # 1M个128维float向量
QUERY_DATA=/mnt/data/sift/sift_query.fbin     # 查询向量
GT_DATA=/mnt/data/sift/sift_groundtruth.bin   # 真值

# SIFT100M 示例（百万级数据集）
BASE_DATA=/mnt/nvme/data/bigann/100M.bbin     # 100M个128维uint8向量
QUERY_DATA=/mnt/nvme/data/bigann/bigann_query.bbin
GT_DATA=/mnt/nvme/data/bigann/100M_gt.bin

# DEEP100M 示例
BASE_DATA=/mnt/nvme/data/deep/100M.fbin       # 100M个96维float向量
QUERY_DATA=/mnt/nvme/data/deep/queries.fbin
GT_DATA=/mnt/nvme/data/deep/100M_gt.bin
```

### 2.3 数据格式说明

二进制文件格式：
```
[num_points: 4 bytes] [dim: 4 bytes] [vector_data: num_points * dim * sizeof(T)]
```

---

## 🏗️ 步骤3: 构建索引

### 3.1 构建磁盘索引

```bash
cd /home/latir/WorkSpace/PipeANN

# 设置变量
DATA_TYPE=float           # 数据类型: float/uint8/int8
BASE_DATA=/path/to/base.fbin
INDEX_PREFIX=/path/to/output/index_name
R=64                      # 图度数
L=100                     # 构建时搜索列表长度
PQ_BYTES=32               # PQ压缩字节数
M=16                      # 聚簇数
T=32                      # 线程数
METRIC=l2                 # 距离度量: l2/cosine
NBR_TYPE=pq               # 邻居类型: pq/rabitq

# 执行构建
./build/tests/build_disk_index \
  $DATA_TYPE \
  $BASE_DATA \
  $INDEX_PREFIX \
  $R \
  $L \
  $PQ_BYTES \
  $M \
  $T \
  $METRIC \
  $NBR_TYPE

# 示例：SIFT1M
./build/tests/build_disk_index \
  float \
  /mnt/data/sift/sift_base.fbin \
  /mnt/indices/sift1m \
  64 100 32 16 32 l2 pq
```

构建完成后生成文件：
- `index_name_disk.index` - 磁盘图索引
- `index_name_pq_compressed.bin` - PQ压缩向量
- `index_name_pq_pivots.bin` - PQ中心点

### 3.2 构建内存索引（可选，用于混合检索）

```bash
MEM_L=100
MEM_ALPHA=1.2
NUM_FROZEN=0

./build/tests/build_memory_index \
  $DATA_TYPE \
  $BASE_DATA \
  $INDEX_PREFIX \
  $R \
  $MEM_L \
  $MEM_ALPHA \
  $T \
  $NUM_FROZEN \
  $METRIC

# 示例
./build/tests/build_memory_index \
  float \
  /mnt/data/sift/sift_base.fbin \
  /mnt/indices/sift1m \
  64 100 1.2 32 0 l2
```

生成文件：
- `index_name_mem.index` - 内存图
- `index_name_mem.index.tags` - 标签映射

---

## 🧪 步骤4: 运行实验

### 实验8: DC-PDI物理离散度评估 ⭐

**实验目的**: 评估动态聚簇和块感知剪枝对物理离散度的影响

**运行命令**:

```bash
cd /home/latir/WorkSpace/PipeANN

# 基础参数
EXP_TYPE=8                # 实验类型：物理离散度
DATA_TYPE=float           # 数据类型
INDEX_PREFIX=/mnt/indices/sift1m
QUERY_FILE=/mnt/data/sift/sift_query.fbin
GT_FILE=/mnt/data/sift/sift_groundtruth.bin
OUTPUT_FILE=./results/exp8_dispersion.csv
NUM_THREADS=32            # 搜索线程数
BEAM_WIDTH=16             # 流水线宽度
RECALL_AT=10              # 计算Recall@K
MEM_L=10                  # 内存索引候选列表长度

# 执行实验
./build/tests/thesis_benchmark \
  $EXP_TYPE \
  $DATA_TYPE \
  $INDEX_PREFIX \
  $QUERY_FILE \
  $GT_FILE \
  $OUTPUT_FILE \
  $NUM_THREADS \
  $BEAM_WIDTH \
  $RECALL_AT \
  $MEM_L

# 完整示例
./build/tests/thesis_benchmark \
  8 \
  float \
  /mnt/indices/sift1m \
  /mnt/data/sift/sift_query.fbin \
  /mnt/data/sift/sift_groundtruth.bin \
  ./results/exp8_dispersion.csv \
  32 16 10 10
```

**输出文件**:

1. `results/exp8_dispersion.csv` - 主要统计数据:
   ```csv
   metric,value
   avg_physical_dispersion,0.35
   avg_page_local_edge_ratio,0.68
   total_sampled_nodes,156789
   total_queries,10000
   global_total_pages,8192
   global_fragmented_pages,245
   global_avg_fragmentation,0.12
   ```

2. `results/exp8_dispersion.csv.pages.csv` - 页面级详细统计（如果启用ENABLE_DISPERSION_MONITOR）:
   ```csv
   page_id,cross_page_neighbors,total_neighbors,fragmentation_ratio,last_update_time
   0,12,64,0.187,1705933824
   1,8,64,0.125,1705933825
   ...
   ```

**输出指标说明**:

| 指标 | 说明 | 期望值 |
|------|------|--------|
| avg_physical_dispersion | 平均物理离散度 $D_p(u)$ | <0.5（块感知剪枝应降低此值） |
| avg_page_local_edge_ratio | 页面内邻居比例 | >0.6（聚簇分配应提高此值） |
| global_fragmented_pages | 需要重组织的页面数 | 越少越好 |
| global_avg_fragmentation | 全局平均碎片化程度 | <0.2 |

**对比实验**:

要对比DC-PDI vs 普通方法，需要编译两个版本：

```bash
# 版本1: DC-PDI完整版（已编译）
# ENABLE_BLOCK_AWARE_PRUNE=ON, ENABLE_DISPERSION_MONITOR=ON

# 版本2: 禁用块感知剪枝
cd build
cmake .. -DENABLE_BLOCK_AWARE_PRUNE=OFF -DENABLE_DISPERSION_MONITOR=ON
make -j$(nproc)
./tests/thesis_benchmark 8 ... ./results/exp8_dispersion_no_blockaware.csv ...

# 版本3: 完全禁用DC-PDI
cmake .. -DENABLE_BLOCK_AWARE_PRUNE=OFF -DENABLE_DISPERSION_MONITOR=OFF
make -j$(nproc)
./tests/thesis_benchmark 8 ... ./results/exp8_dispersion_baseline.csv ...
```

---

### 实验1: 搜索延迟分布

**实验目的**: 测试不同Recall@10下的延迟分布

```bash
./build/tests/thesis_benchmark \
  1 \
  float \
  /mnt/indices/sift1m \
  /mnt/data/sift/sift_query.fbin \
  /mnt/data/sift/sift_groundtruth.bin \
  ./results/exp1_latency.csv \
  32 16 10 10
```

**输出文件**: `exp1_latency.csv`
```csv
L,recall,qps,avg_lat_us,p50_lat_us,p90_lat_us,p95_lat_us,p99_lat_us,mean_ios,avg_compute_us,avg_prefetch_us,io_amplification,overlap_ratio
10,0.671,1952.03,490.99,450,800,950,1200,22.28,120,370,1.15,0.75
20,0.845,1717.53,547.84,500,900,1050,1350,31.11,145,402,1.18,0.73
...
```

---

### 实验5: I/O放大率测试

**实验目的**: 测量I/O效率和放大率

```bash
./build/tests/thesis_benchmark \
  5 \
  float \
  /mnt/indices/sift1m \
  /mnt/data/sift/sift_query.fbin \
  /mnt/data/sift/sift_groundtruth.bin \
  ./results/exp5_io_amp.csv \
  32 16 10 10
```

**输出**: I/O统计数据（需要COLLECT_IO_STATS=ON）

---

### 实验6: 流水线宽度敏感性

**实验目的**: 测试不同流水线宽度(beam_width)对性能的影响

```bash
./build/tests/thesis_benchmark \
  6 \
  float \
  /mnt/indices/sift1m \
  /mnt/data/sift/sift_query.fbin \
  /mnt/data/sift/sift_groundtruth.bin \
  ./results/exp6_pipeline.csv \
  32 16 10 10
```

**输出**: 测试beam_width={1,2,4,8,16,32,64}的性能曲线

---

### 实验7: 资源开销评估

**实验目的**: 评估内存和磁盘资源使用

```bash
./build/tests/thesis_benchmark \
  7 \
  float \
  /mnt/indices/sift1m \
  /mnt/data/sift/sift_query.fbin \
  /mnt/data/sift/sift_groundtruth.bin \
  ./results/exp7_resource.csv \
  32 16 10 10
```

---

## 📊 步骤5: 数据分析和绘图

### 5.1 生成图表

确保已安装matplotlib（在有pip的环境中）：

```bash
pip install matplotlib pandas numpy
```

运行绘图脚本：

```bash
cd /home/latir/WorkSpace/PipeANN

# 方法1: 直接运行Python脚本
python3 draw/plot_thesis_figures.py

# 方法2: 在Jupyter Notebook中逐步绘图
# 打开 scripts/tests-pipeann/plotting.ipynb
```

生成的图表保存在 `draw/` 目录：

**第3章图表**（DC-PDI核心设计）:
- `fig3_1_topology_strength.pdf/png` - 拓扑强度对比
- `fig3_2_dispersion_evolution.pdf/png` - 物理离散度演变
- `fig3_3_block_aware_edges.pdf/png` - 块感知边选择效果

**第4章图表**（流水线插入）:
- `fig4_1_clustering_io_efficiency.pdf/png` - 聚簇感知I/O效率
- `fig4_2_threshold_sensitivity.pdf/png` - 离散度阈值敏感性

**第5章图表**（系统性能）:
- `fig5_1_latency_distribution.pdf/png` - 延迟分布
- `fig5_2_dynamic_stability.pdf/png` - 动态稳定性
- `fig5_3_pipeline_sensitivity.pdf/png` - 流水线宽度敏感性
- `fig5_4_io_amplification.pdf/png` - I/O放大率
- `fig5_5_update_throughput.pdf/png` - 更新吞吐量
- `fig5_6_scalability.pdf/png` - 扩展性

### 5.2 手动分析实验8数据

使用Python分析物理离散度：

```python
import pandas as pd
import matplotlib.pyplot as plt

# 读取数据
df_dcpdi = pd.read_csv('results/exp8_dispersion.csv')
df_baseline = pd.read_csv('results/exp8_dispersion_baseline.csv')

# 提取关键指标
dispersion_dcpdi = df_dcpdi[df_dcpdi['metric']=='avg_physical_dispersion']['value'].values[0]
dispersion_baseline = df_baseline[df_baseline['metric']=='avg_physical_dispersion']['value'].values[0]

print(f"DC-PDI物理离散度: {dispersion_dcpdi:.3f}")
print(f"基线物理离散度: {dispersion_baseline:.3f}")
print(f"改进比例: {(dispersion_baseline - dispersion_dcpdi) / dispersion_baseline * 100:.1f}%")

# 读取页面级统计
pages = pd.read_csv('results/exp8_dispersion.csv.pages.csv')
fragmented = pages[pages['fragmentation_ratio'] > 0.5]
print(f"高度碎片化页面数: {len(fragmented)} / {len(pages)} ({len(fragmented)/len(pages)*100:.1f}%)")
```

---

## 🔍 步骤6: 结果验证

### 6.1 检查实验输出

```bash
# 查看输出文件
ls -lh results/

# 检查CSV格式
head results/exp8_dispersion.csv

# 验证页面统计文件
wc -l results/exp8_dispersion.csv.pages.csv
```

### 6.2 预期结果范围

| 实验 | 关键指标 | DC-PDI期望值 | 基线值 |
|------|---------|-------------|--------|
| 实验8 | 物理离散度 | 0.3-0.45 | 0.7-0.85 |
| 实验8 | 页面局部性 | 0.6-0.75 | 0.15-0.30 |
| 实验1 | P99延迟@R95% | <1.5ms | >2.5ms |
| 实验5 | I/O放大率 | 3-4x | 15-20x |

### 6.3 调试技巧

如果结果不符合预期：

1. **检查编译选项**:
   ```bash
   # 确认编译时的宏定义
   grep -E "ENABLE_BLOCK_AWARE_PRUNE|ENABLE_DISPERSION_MONITOR" build/CMakeCache.txt
   ```

2. **查看运行时日志**:
   ```bash
   # 带详细输出运行
   PIPEANN_LOG_LEVEL=DEBUG ./build/tests/thesis_benchmark ...
   ```

3. **验证索引完整性**:
   ```bash
   ls -lh /mnt/indices/sift1m*
   # 应包含: _disk.index, _pq_compressed.bin, _pq_pivots.bin
   ```

4. **检查数据格式**:
   ```bash
   # 使用hexdump查看二进制文件头（前8字节是num_points和dim）
   hexdump -n 8 -e '2/4 "%u " "\n"' /mnt/data/sift/sift_base.fbin
   ```

---

## 📝 步骤7: 批量运行脚本

创建自动化脚本 `run_all_dc_pdi_experiments.sh`：

```bash
#!/bin/bash
# DC-PDI实验批量运行脚本

set -e  # 遇到错误立即退出

# 配置
PROJECT_ROOT=/home/latir/WorkSpace/PipeANN
RESULTS_DIR=$PROJECT_ROOT/results
mkdir -p $RESULTS_DIR

# 数据集路径（根据实际情况修改）
INDEX_PREFIX=/mnt/indices/sift1m
QUERY_FILE=/mnt/data/sift/sift_query.fbin
GT_FILE=/mnt/data/sift/sift_groundtruth.bin
DATA_TYPE=float

cd $PROJECT_ROOT

echo "=========================================="
echo "DC-PDI实验批量运行"
echo "索引: $INDEX_PREFIX"
echo "结果目录: $RESULTS_DIR"
echo "=========================================="

# 实验8: 物理离散度（核心实验）
echo "[1/5] 运行实验8: 物理离散度评估..."
./build/tests/thesis_benchmark \
  8 $DATA_TYPE $INDEX_PREFIX $QUERY_FILE $GT_FILE \
  $RESULTS_DIR/exp8_dispersion.csv \
  32 16 10 10
echo "✓ 实验8完成"

# 实验1: 搜索延迟分布
echo "[2/5] 运行实验1: 搜索延迟分布..."
./build/tests/thesis_benchmark \
  1 $DATA_TYPE $INDEX_PREFIX $QUERY_FILE $GT_FILE \
  $RESULTS_DIR/exp1_latency.csv \
  32 16 10 10
echo "✓ 实验1完成"

# 实验5: I/O放大率
echo "[3/5] 运行实验5: I/O放大率测试..."
./build/tests/thesis_benchmark \
  5 $DATA_TYPE $INDEX_PREFIX $QUERY_FILE $GT_FILE \
  $RESULTS_DIR/exp5_io_amp.csv \
  32 16 10 10
echo "✓ 实验5完成"

# 实验6: 流水线宽度
echo "[4/5] 运行实验6: 流水线宽度敏感性..."
./build/tests/thesis_benchmark \
  6 $DATA_TYPE $INDEX_PREFIX $QUERY_FILE $GT_FILE \
  $RESULTS_DIR/exp6_pipeline.csv \
  32 16 10 10
echo "✓ 实验6完成"

# 实验7: 资源评估
echo "[5/5] 运行实验7: 资源开销评估..."
./build/tests/thesis_benchmark \
  7 $DATA_TYPE $INDEX_PREFIX $QUERY_FILE $GT_FILE \
  $RESULTS_DIR/exp7_resource.csv \
  32 16 10 10
echo "✓ 实验7完成"

echo "=========================================="
echo "所有实验完成！结果保存在: $RESULTS_DIR"
echo "=========================================="
ls -lh $RESULTS_DIR
```

运行批量脚本：

```bash
chmod +x run_all_dc_pdi_experiments.sh
./run_all_dc_pdi_experiments.sh
```

---

## 🚀 快速开始示例

**最小化示例**（使用SIFT1M小数据集，<30分钟）：

```bash
# 1. 编译
cd /home/latir/WorkSpace/PipeANN
bash build.sh

# 2. 构建索引（假设数据在/mnt/data/sift/）
./build/tests/build_disk_index \
  float /mnt/data/sift/sift_base.fbin /tmp/sift1m \
  64 100 32 16 8 l2 pq

# 3. 运行核心实验（实验8）
mkdir -p results
./build/tests/thesis_benchmark \
  8 float /tmp/sift1m \
  /mnt/data/sift/sift_query.fbin \
  /mnt/data/sift/sift_groundtruth.bin \
  results/exp8_dispersion.csv \
  8 8 10 0

# 4. 查看结果
cat results/exp8_dispersion.csv
```

---

## ❓ 常见问题

### Q1: 编译错误 "io_uring.h: No such file or directory"

**原因**: 系统不支持io_uring，需要使用libaio。

**解决**: 确保 `CMakeLists.txt` 中 `USE_AIO=ON`，并注释掉USE_URING相关代码。

### Q2: 运行时错误 "Cannot open index file"

**解决**: 检查索引路径是否正确，确保 `${INDEX_PREFIX}_disk.index` 存在。

### Q3: 实验8输出的物理离散度为0

**原因**: 可能未启用ENABLE_DISPERSION_MONITOR编译选项。

**解决**: 重新编译，确保`cmake -DENABLE_DISPERSION_MONITOR=ON`。

### Q4: 绘图脚本报错 "No module named matplotlib"

**解决**: 在有pip的环境中安装：
```bash
pip install matplotlib pandas numpy
# 或使用系统包管理器
sudo apt install python3-matplotlib python3-pandas
```

### Q5: 实验运行时间过长

**优化**:
- 减少查询数量：修改query_file只保留前100-1000个查询
- 减少线程数：将NUM_THREADS改为8-16
- 使用小数据集：先在SIFT1M上验证，再扩展到SIFT100M

---

## 📚 附录

### A. 参数说明表

| 参数 | 说明 | 典型值 | 范围 |
|------|------|--------|------|
| R | 图度数 | 64 | 32-128 |
| L | 构建时搜索列表长度 | 100 | 50-200 |
| PQ_BYTES | PQ压缩字节数 | 32 | 16-64 |
| M | 聚簇数 | 16 | 8-32 |
| T | 线程数 | 32 | 8-64 |
| beam_width | 流水线宽度 | 16 | 4-64 |
| mem_L | 内存索引L | 10 | 0-50 |
| recall_at | 计算Recall@K | 10 | 1-100 |

### B. 文件格式参考

**索引文件结构**:
```
${INDEX_PREFIX}_disk.index        # 图结构：邻接表
${INDEX_PREFIX}_pq_compressed.bin # PQ压缩向量
${INDEX_PREFIX}_pq_pivots.bin     # PQ码本
${INDEX_PREFIX}_mem.index         # 内存图（可选）
${INDEX_PREFIX}_mem.index.tags    # 标签映射（可选）
```

**实验输出格式**:
```
results/
├── exp8_dispersion.csv           # 实验8主统计
├── exp8_dispersion.csv.pages.csv # 实验8页面详情
├── exp1_latency.csv              # 实验1延迟数据
├── exp5_io_amp.csv               # 实验5 I/O统计
├── exp6_pipeline.csv             # 实验6性能曲线
└── exp7_resource.csv             # 实验7资源使用
```

---

**文档版本**: v1.0  
**最后更新**: 2026-01-22  
**维护者**: DC-PDI项目组
