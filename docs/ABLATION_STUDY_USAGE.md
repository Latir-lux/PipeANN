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
./run_ablation_study.sh [实验编号] [数据集] [base_dir] [output_dir] [选项...]
```

## 位置参数

| 参数 | 说明 | 默认值 |
|-----|-----|-------|
| 实验编号 | 1=聚类, 2=重组, 3=流水线, 4=扩展性, all=全部 | all |
| 数据集 | sift/deep/gist | sift |
| base_dir | 数据目录 | /mnt/xiaoxuanx/dataset |
| output_dir | 输出目录 | /mnt/xiaoxuanx/dataset/exp/thesis_results/ablation_study |

## 可选参数

| 参数 | 说明 | 默认值 | 适用实验 |
|-----|-----|-------|---------|
| `--num-threads <n>` | 线程数 | 32 | 1、2、3 |
| `--insert-count <n>` | 插入数量 | 100000 | 1 |
| `--duration <sec>` | 持续时间(秒) | 120 | 2、4 |
| `--insert-rate <n>` | 插入速率 | 1000 | 2 |
| `--base-ratio <r>` | 基础索引比例 | 0.5 | 1、2、4 |
| `--L-values <list>` | L值列表(逗号分隔) | 50,100,150,200,250,300 | 1、3 |
| `--thread-list <list>` | 线程数列表(逗号分隔) | 1,4,8,16,32,64 | 4 |
| `--recall-at <k>` | Recall@k | 10 | 1、3 |

## 使用示例

### 示例1：运行全部实验（使用SIFT，默认路径）
```bash
./scripts/run_ablation_study.sh all sift
```

### 示例2：运行聚类消融实验（自定义路径）
```bash
./scripts/run_ablation_study.sh 1 sift /mnt/data /results --insert-count 500000
```

### 示例3：运行重组机制消融（DEEP数据集）
```bash
./scripts/run_ablation_study.sh 2 deep /data /output --duration 300 --insert-rate 2000
```

### 示例4：运行流水线消融（16线程）
```bash
./scripts/run_ablation_study.sh 3 sift /data /output --num-threads 16 --L-values 100,200,300
```

### 示例5：运行扩展性实验（自定义线程列表）
```bash
./scripts/run_ablation_study.sh 4 sift /data /output --thread-list 2,4,8,16,32
```

## 输出文件

所有结果保存在指定的输出目录，文件名格式：

| 实验 | CSV文件 | 内容 |
|-----|---------|------|
| 1 | `exp_ablation_clustering.csv` | 聚类策略对比(mode, insert_count, L, recall, qps, latency...) |
| 2 | `exp_ablation_reorganization.csv` | 重组机制对比(mode, time_sec, qps, latency...) |
| 3 | `exp_ablation_pipeline.csv` | 流水线对比(search_mode, L, recall, qps, speedup...) |
| 4 | `exp_ablation_scalability.csv` | 扩展性测试(num_threads, qps, latency, efficiency...) |

图表自动生成在 `${OUTPUT_DIR}/figures/` 目录。

## 生成图表

脚本会自动调用 `plot_ablation_study.py` 生成图表。手动生成：

```bash
python3 ./draw/plot_ablation_study.py <结果目录>
```

## 前置条件

1. **编译程序**
```bash
cd build && cmake .. && make ablation_study
```

2. **准备数据** - 确保以下文件存在（以SIFT为例）：
   - 数据文件：`{base_dir}/sift/base.1B.u8bin`
   - 查询文件：`{base_dir}/sift/bigann_query.u8bin`
   - GT文件：`{base_dir}/sift/bigann_gt100_crop10.bin`
   - 索引文件：`{base_dir}/sift/index_sift1b_{params}_disk.index`（或脚本自动生成）

3. **Python依赖**
```bash
pip3 install matplotlib pandas numpy
```

## 常见问题

**Q: 实验运行时间过长？**  
A: 减小 `--insert-count`、`--duration` 或 `--L-values` 参数。

**Q: 基础索引不存在？**  
A: 脚本会自动构建，或手动放置完整索引到 `{base_dir}/{dataset}/` 目录。

**Q: 结果在哪？**  
A: 默认在指定的 `output_dir`，查看 CSV 文件和 `figures/` 目录。
