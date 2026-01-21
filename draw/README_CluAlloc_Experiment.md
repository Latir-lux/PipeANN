# Clu-Alloc 实验指南 (Section 3.2.2)

## 概述

本实验实现了论文 3.2.2 节中提出的"基于连接强度的聚类感知分配规则（Clu-Alloc）"，并对比 Append-Only 与 Random-Alloc 的布局与性能差异。

## 数据集位置配置

在运行实验前，需要配置以下数据集路径。这些路径在以下文件中定义：

### 1. 实验脚本中的数据路径

**文件**: [scripts/tests-odinann/run_clu_alloc.sh](scripts/tests-odinann/run_clu_alloc.sh)

```bash
# ========== DATA PATH CONFIGURATION ==========
# MODIFY THESE PATHS TO MATCH YOUR ENVIRONMENT:

# Data files
DATA_BIN="/mnt/nvme/data/bigann/bigann_200M.bbin"    # Binary data file for insertion
QUERY_FILE="/mnt/nvme/data/bigann/bigann_query.bbin" # Query file

# Index files (pre-built with 50% data)
INDEX_PREFIX="/mnt/nvme/indices_upd/bigann/100M"     # Index prefix path

# Output directory for results
OUTPUT_DIR="/home/latir/WorkSpace/PipeANN/draw"
```

### 2. 已有脚本中的数据路径参考

参考现有脚本中的数据路径配置：

- **hello_world.sh**: `/mnt/nvme/data/bigann/bigann_2M.bbin`
- **fig6.sh**: `/mnt/nvme/data/bigann/bigann_200M.bbin`
- **hello_world.sh (pipeann)**: `/mnt/nvme2/indices/bigann/100m`

## 代码修改位置

### 主要修改的文件：

1. **[include/ssd_index.h](include/ssd_index.h)**
   - 添加了 `AllocStrategy` 枚举（第 25-30 行）
   - 添加了 `CluAllocStats` 统计结构（第 476-500 行）
   - 添加了分配策略配置变量 `alloc_strategy_`（第 467 行）
   - 实现了 `compute_page_scores()` 函数计算页面连接强度得分（第 670-695 行）
   - 实现了 `alloc_loc_random()` 随机分配策略（第 707-765 行）
   - 实现了 `alloc_loc_cluster()` 聚类分配策略（第 768-836 行）
   - 实现了 `alloc_loc_strategy()` 统一分配接口（第 839-855 行）
   - 实现了 `update_intra_page_stats()` 统计页内边比例（第 858-866 行）

2. **[include/v2/dynamic_index.h](include/v2/dynamic_index.h)**
   - 添加了 `set_alloc_strategy()` 和 `get_alloc_strategy()` API（第 80-88 行）
   - 添加了 `get_clu_alloc_stats()` 和 `reset_clu_alloc_stats()` API（第 90-98 行）
   - 添加了 `get_disk_index_size()` 获取磁盘索引大小（第 100-107 行）

3. **[src/update/direct_insert.cpp](src/update/direct_insert.cpp)**
   - 修改了分配逻辑，使用 `alloc_loc_strategy()` 替代原来的 `alloc_loc()`（第 72-76 行）

4. **[tests/test_clu_alloc.cpp](tests/test_clu_alloc.cpp)** (新增)
   - 完整的实验测试程序

5. **[tests/CMakeLists.txt](tests/CMakeLists.txt)**
   - 添加了 `test_clu_alloc` 编译目标

## 编译和运行

### 编译项目

```bash
cd /home/latir/WorkSpace/PipeANN
mkdir -p build && cd build
cmake ..
make -j$(nproc) test_clu_alloc
```

### 运行实验

**方法一**: 使用实验脚本（推荐）

```bash
cd /home/latir/WorkSpace/PipeANN
bash scripts/tests-odinann/run_clu_alloc.sh
```

**方法二**: 手动运行测试程序

```bash
# 运行 Append-Only 策略 (strategy=0)
./build/tests/test_clu_alloc uint8 /mnt/nvme/data/bigann/bigann_200M.bbin 128 \
    1000000 10 10 32 0 /mnt/nvme/indices_upd/bigann/100M \
    /mnt/nvme/data/bigann/bigann_query.bbin 10 4 4 0 20 \
    /home/latir/WorkSpace/PipeANN/draw 0

# 运行 Random-Alloc 策略 (strategy=1)
./build/tests/test_clu_alloc uint8 ... 1

# 运行 Clu-Alloc 策略 (strategy=2)
./build/tests/test_clu_alloc uint8 ... 2
```

### 命令行参数说明

```
test_clu_alloc <type> <data_bin> <L_disk> <vecs_per_step> <num_steps> 
               <insert_threads> <search_threads> <search_mode> <index_prefix> 
               <query_file> <recall@> <beam_width> <search_beam_width> 
               <mem_L> <L_search> <output_dir> <strategy>

参数说明:
- type           : 数据类型 (int8, uint8, float)
- data_bin       : 插入数据的二进制文件路径
- L_disk         : 磁盘索引的 L 参数
- vecs_per_step  : 每步插入的向量数量
- num_steps      : 插入步数
- insert_threads : 插入线程数
- search_threads : 搜索线程数
- search_mode    : 搜索模式 (0=BEAM, 1=PAGE, 2=PIPE)
- index_prefix   : 索引文件前缀路径
- query_file     : 查询文件路径
- recall@        : recall@K 的 K 值
- beam_width     : 构建时的 beam 宽度
- search_beam_width: 搜索时的 beam 宽度
- mem_L          : 内存索引的 L (0 表示不使用)
- L_search       : 搜索的 L 参数
- output_dir     : 输出目录
- strategy       : 分配策略 (0=Append, 1=Random, 2=Cluster)
```

## 实验输出

### 结果文件

实验结果保存在 `/home/latir/WorkSpace/PipeANN/draw` 目录下：

- `clu_alloc_append.csv` - Append-Only 策略结果
- `clu_alloc_random.csv` - Random-Alloc 策略结果
- `clu_alloc_cluster.csv` - Clu-Alloc 策略结果

### CSV 列说明

| 列名 | 说明 |
|------|------|
| strategy | 策略名称 |
| num_inserted | 已插入向量数 |
| avg_page_accesses | 平均页面访问次数 (APA) |
| io_amplification | I/O 放大率 |
| p50_latency_ms | P50 延迟 (ms) |
| p99_latency_ms | P99 延迟 (ms) |
| recall | Recall 值 |
| intra_page_ratio | 页内边比例 |
| disk_size_mb | 磁盘索引大小 (MB) |
| cluster_hits | 聚类分配命中次数 |
| overflow_count | 溢出次数 |

## 生成图表

### 运行画图脚本

```bash
cd /home/latir/WorkSpace/PipeANN/draw
python3 plot_clu_alloc.py
```

### 生成的图表

1. **fig_apa_comparison.pdf/png** - APA 对比图
2. **fig_io_amplification.pdf/png** - I/O 放大率对比图
3. **fig_p99_latency.pdf/png** - P99 延迟对比图
4. **fig_intra_page_ratio.pdf/png** - 页内边比例对比图
5. **fig_disk_space.pdf/png** - 磁盘空间使用对比图
6. **fig_combined_metrics.pdf/png** - 组合指标图 (2x2)
7. **fig_bar_comparison.pdf/png** - 最终结果柱状图
8. **summary_table.csv** - 汇总表格

## 预期结果

根据论文 3.2.2 节的分析，预期实验结果如下：

1. **APA (Average Page Accesses)**
   - Clu-Alloc < Random-Alloc < Append-Only
   - Clu-Alloc 预期降低 30%-50%

2. **I/O 放大率**
   - Clu-Alloc 最低，因为更好的数据局部性

3. **P99 延迟**
   - Clu-Alloc 最低，搜索性能最好

4. **页内边比例**
   - Clu-Alloc 最高（预期 40%-60%）
   - Append-Only 最低（随机分布约 10%-15%）

5. **磁盘空间**
   - Clu-Alloc 可能略高（约 1.35x），但低于 ODINANN 的 2x

## 注意事项

1. 实验需要预先构建好的 50% 数据的静态索引
2. 确保有足够的磁盘空间存储实验结果
3. 建议使用 SSD 进行实验以获得准确的 I/O 性能数据
4. 如果没有真实数据集，画图脚本会自动生成示例数据用于测试
