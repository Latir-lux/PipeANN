# 消融实验脚本使用文档

## 概述

本文档说明如何使用 `run_ablation_study.sh` 脚本运行 DC-PDI 系统的四个消融实验。这些实验用于验证系统各组件的有效性。

## 实验说明

| 实验编号 | 实验名称 | 验证目标 |
|---------|---------|---------|
| 1 | 聚类数据分配消融实验 | 验证动态聚类数据分配策略的有效性 |
| 2 | 重组机制消融实验 | 验证后台重组机制对搜索性能的影响 |
| 3 | 流水线搜索消融实验 | 验证流水线搜索架构的性能优势 |
| 4 | 可扩展性实验 | 验证系统在不同并发度下的性能表现 |

## 脚本位置

```
/home/latir/WorkSpace/PipeANN/scripts/run_ablation_study.sh
```

## 基本用法

```bash
./run_ablation_study.sh [实验编号] [数据集] [选项...]
```

## 必需参数

### 实验编号 (第一个参数)
指定要运行的实验编号，可选值：`1`、`2`、`3`、`4`

| 值 | 实验名称 |
|---|---------|
| `1` | 聚类数据分配消融实验 |
| `2` | 重组机制消融实验 |
| `3` | 流水线搜索消融实验 |
| `4` | 可扩展性实验 |

### 数据集 (第二个参数)
指定使用的数据集，可选值：`sift`、`deep`、`gist`

| 数据集 | 维度 | 数据类型 | 说明 |
|-------|-----|---------|-----|
| `sift` | 128 | uint8 | SIFT1B 数据集，最常用的测试数据集 |
| `deep` | 96 | float | DEEP1B 数据集，深度学习特征向量 |
| `gist` | 960 | float | GIST 数据集，高维图像特征 |

## 可选参数

### 通用选项

| 参数 | 说明 | 默认值 | 示例 |
|-----|-----|-------|-----|
| `--base-dir <路径>` | 数据文件的基础目录 | `/mnt/data` | `--base-dir /data/datasets` |
| `--output-dir <路径>` | 实验结果输出目录 | `./ablation_results` | `--output-dir /results/exp1` |

### 实验配置选项

| 参数 | 说明 | 默认值 | 适用实验 |
|-----|-----|-------|---------|
| `--num-threads <数量>` | 搜索线程数 | 8 | 实验1、2、3 |
| `--insert-count <数量>` | 插入向量数量 | 1000000 | 实验1、2、3、4 |
| `--duration <秒>` | 实验持续时间 | 120 | 实验2 |
| `--insert-rate <速率>` | 每秒插入数量 | 1000 | 实验1、2 |
| `--query-count <数量>` | 查询次数 | 10000 | 实验1、2、3、4 |
| `--L <值>` | 搜索列表长度 | 100 | 实验1、2、3、4 |
| `--K <值>` | 返回近邻数 | 10 | 实验1、2、3、4 |

### 实验4专用选项

| 参数 | 说明 | 默认值 | 示例 |
|-----|-----|-------|-----|
| `--max-threads <数量>` | 最大测试线程数 | 32 | `--max-threads 64` |
| `--thread-step <步长>` | 线程数增长步长 | 4 | `--thread-step 2` |

## 使用示例

### 示例1：运行聚类消融实验（使用SIFT数据集）
```bash
./run_ablation_study.sh 1 sift --base-dir /mnt/ssd/data --output-dir ./results/clustering
```

### 示例2：运行重组机制消融实验（使用DEEP数据集，自定义持续时间）
```bash
./run_ablation_study.sh 2 deep --base-dir /mnt/ssd/data --duration 300 --insert-rate 2000
```

### 示例3：运行流水线搜索消融实验（使用SIFT数据集，16线程）
```bash
./run_ablation_study.sh 3 sift --base-dir /mnt/ssd/data --num-threads 16 --L 200
```

### 示例4：运行可扩展性实验（测试1到64线程）
```bash
./run_ablation_study.sh 4 sift --base-dir /mnt/ssd/data --max-threads 64 --thread-step 8
```

### 示例5：完整运行所有实验
```bash
# 创建结果目录
mkdir -p ./ablation_results

# 依次运行四个实验
for exp in 1 2 3 4; do
    ./run_ablation_study.sh $exp sift --base-dir /mnt/ssd/data --output-dir ./ablation_results
done
```

## 输出文件

### 实验1：聚类消融实验
| 文件名 | 内容说明 |
|-------|---------|
| `clustering_ablation_sift_results.csv` | 聚类策略对比结果 |
| `clustering_ablation_sift.png` | 对比图表 |

**CSV列说明：**
- `strategy`: 分配策略 (clustered/random)
- `insert_count`: 插入向量数量
- `search_qps`: 搜索吞吐量
- `mean_latency`: 平均延迟(μs)
- `p99_latency`: P99延迟(μs)
- `recall`: 召回率

### 实验2：重组机制消融实验
| 文件名 | 内容说明 |
|-------|---------|
| `reorganization_ablation_sift_results.csv` | 重组机制对比结果 |
| `reorganization_ablation_sift.png` | 对比图表 |

**CSV列说明：**
- `reorg_enabled`: 是否启用重组 (true/false)
- `time_elapsed`: 经过时间(秒)
- `search_qps`: 搜索吞吐量
- `mean_latency`: 平均延迟(μs)
- `p99_latency`: P99延迟(μs)
- `recall`: 召回率
- `page_read_count`: 页面读取次数

### 实验3：流水线搜索消融实验
| 文件名 | 内容说明 |
|-------|---------|
| `pipeline_ablation_sift_results.csv` | 流水线对比结果 |
| `pipeline_ablation_sift.png` | 对比图表 |

**CSV列说明：**
- `search_mode`: 搜索模式 (PIPE_SEARCH/BEAM_SEARCH)
- `num_threads`: 线程数
- `search_qps`: 搜索吞吐量
- `mean_latency`: 平均延迟(μs)
- `p99_latency`: P99延迟(μs)
- `io_wait_time`: IO等待时间(μs)
- `compute_time`: 计算时间(μs)

### 实验4：可扩展性实验
| 文件名 | 内容说明 |
|-------|---------|
| `scalability_sift_results.csv` | 可扩展性测试结果 |
| `scalability_sift.png` | 可扩展性图表 |

**CSV列说明：**
- `num_threads`: 线程数
- `search_qps`: 搜索吞吐量
- `mean_latency`: 平均延迟(μs)
- `p99_latency`: P99延迟(μs)
- `cpu_utilization`: CPU利用率(%)

## 生成图表

实验完成后，使用画图脚本生成可视化图表：

```bash
cd /home/latir/WorkSpace/PipeANN/draw

# 生成单个实验图表
python3 plot_ablation_study.py --exp 1 --data-dir ../ablation_results --output-dir ../ablation_results

# 生成所有实验图表和汇总报告
python3 plot_ablation_study.py --all --data-dir ../ablation_results --output-dir ../ablation_results
```

### 画图脚本参数

| 参数 | 说明 |
|-----|-----|
| `--exp <编号>` | 指定生成哪个实验的图表 (1-4) |
| `--all` | 生成所有实验的图表 |
| `--data-dir <路径>` | CSV数据文件目录 |
| `--output-dir <路径>` | 图表输出目录 |
| `--dataset <名称>` | 指定数据集名称 (sift/deep/gist) |

## 前置条件

### 1. 编译程序
确保已编译 `ablation_study` 可执行文件：
```bash
cd /home/latir/WorkSpace/PipeANN/build
cmake ..
make ablation_study
```

### 2. 准备数据
确保数据目录结构正确：
```
<base-dir>/
├── sift/
│   ├── index_sift1b_R64_L100_A1.2/  # 基础索引
│   ├── learn.100M.u8bin              # 学习数据
│   ├── bigann_query.u8bin            # 查询数据
│   └── gt100M.u8bin.crop_nb_10       # Ground truth
├── deep/
│   ├── index_deep1b_R64_L100_A1.2/
│   ├── base.1B.fbin
│   └── ...
└── gist/
    └── ...
```

### 3. Python依赖
用于生成图表：
```bash
pip3 install matplotlib pandas numpy
```

## 常见问题

### Q: 实验运行时间很长怎么办？
A: 可以减小 `--insert-count`、`--query-count` 或 `--duration` 参数来缩短实验时间。

### Q: 如何只测试特定线程数？
A: 对于实验4，调整 `--max-threads` 和 `--thread-step` 参数。

### Q: 结果文件在哪里？
A: 默认在 `./ablation_results` 目录，可通过 `--output-dir` 自定义。

### Q: 如何对比多个数据集？
A: 分别运行不同数据集的实验，结果会以数据集名称区分。

## 联系与支持

如遇问题，请检查：
1. 数据路径是否正确
2. 索引文件是否存在
3. 编译是否成功

更多信息请参考项目 README 文件。
