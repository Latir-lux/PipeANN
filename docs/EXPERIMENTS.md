# 实验运行指南

## 一、数据准备

### 1. 原始数据格式 (FVECS/BVECS)
- 格式：[4字节维度][向量数据]×N
- 无全局头部，每条记录单独存储维度

### 2. 数据转换工具

#### a. 转换为二进制格式 (FVECS → BIN)
```bash
./build/tests/utils/vecs_to_bin <input.fvecs> <output.bin> <elem_size>
# elem_size: 4 (float) | 1 (uint8/int8)
```

#### b. 提取数据子集
```bash
./build/tests/change_pts uint8 <data.bin> <num_points>
# 从data.bin提取前num_points个向量，输出到data.bin<num_points>
```

#### c. 向量归一化 (用于余弦相似度)
```bash
./build/tests/normalize_data uint8 <data.bin>
# 输出到data.bin_normalized
```

#### d. 生成标签文件
```bash
./build/tests/gen_tags uint8 <data.bin> <index_prefix>
# 自动生成<index_prefix>_disk.index.tags文件
```

#### e. 计算Ground Truth
```bash
./build/tests/utils/compute_groundtruth uint8 \
  <data.bin> <query.bin> <k> <output_gt.bin>
# 计算top-k最近邻，k建议>=1000
```

### 3. 完整数据准备流程示例

```bash
# 1. 转换原始数据为二进制格式
./build/tests/utils/vecs_to_bin gist.fvecs gist.bin 1

# 2. 提取1M子集（GIST通常已经是1M）
./build/tests/change_pts uint8 gist.bin 1000000
mv gist.bin1000000 1M.bin

# 3. 计算Ground Truth (top-1000邻近)
./build/tests/utils/compute_groundtruth uint8 1M.bin gist_query.bin 1000 1M_gt.bin

# 4. 生成标签
./build/tests/gen_tags uint8 1M.bin /path/to/index

# 5. 构建磁盘索引
./build/tests/build_disk_index uint8 1M.bin \
  /path/to/index 96 128 32 256 32 l2 pq
```

参数说明:
- R=96: 最大出度邻接表大小
- L=128: 构建时候选列表大小  
- PQ_bytes=32: PQ量化表字节数
- M=256: 最大内存使用(GB)
- T=32: 构建线程数
- metric=l2: 相似度度量 (l2 或 cosine)
- nbr=pq: 邻接表类型 (pq 或 rabitq)

## 二、二进制格式说明

### 二进制数据文件格式
```
[4字节整数: 点数n][4字节整数: 维度d][n*d*sizeof(T)字节: 向量数据]
```

示例:
```cpp
// 读取方式
std::ifstream reader(file, std::ios::binary);
int32_t n, d;
reader.read((char*)&n, 4);        // 点数
reader.read((char*)&d, 4);        // 维度
T* data = new T[n*d];
reader.read((char*)data, n*d*sizeof(T));  // 向量数据
```

### Ground Truth格式
```
[4字节: 查询数q][4字节: top-k值k]
[q*k个4字节整数: 邻接ID][q*k个4字节浮点: 距离]
```

## 四、运行实验

### 方式1: 自动化脚本（推荐）
```bash
# 运行所有三系统对比实验
./scripts/run_system_comparison.sh sift /mnt/nvme/data ./results/comparison

# 参数说明:
# - sift: 数据集名称
# - /mnt/nvme/data: 数据目录
# - ./results/comparison: 输出目录
```

### 方式2: 单独运行
```bash
# DC-PDI搜索延迟测试
./build/tests/compare_systems uint8 \
  /mnt/nvme/indices/bigann/100m_dc-pdi \
  /mnt/nvme/data/bigann/bigann_query.bbin \
  /mnt/nvme/data/bigann/100M_gt.bin \
  /mnt/nvme/data/bigann/bigann_learn.bbin \
  0 1 ./results 32 10 100 200 300 400 500
```

参数: `<数据类型> <索引> <查询> <GT> <插入数据> <系统类型> <实验类型> <输出目录> <线程数> <Recall@K> <L值列表>`
- 系统类型: 0=DC-PDI, 1=IP-DiskANN, 2=FreshDiskANN
- 实验类型: 1=搜索延迟, 2=更新吞吐量, 3=读写并发

## 五、实验结果

### 输出文件
```
results/comparison/
├── exp1_search_latency_dc-pdi.csv          # DC-PDI搜索性能
├── exp1_search_latency_ip-diskann.csv      # IP-DiskANN搜索性能
├── exp1_search_latency_fresh-diskann.csv   # FreshDiskANN搜索性能
├── exp2_update_throughput_dc-pdi.csv       # DC-PDI更新性能
├── exp2_update_throughput_ip-diskann.csv   
├── exp2_update_throughput_fresh-diskann.csv
├── exp3_concurrent_dc-pdi.csv              # DC-PDI并发性能
├── exp3_concurrent_ip-diskann.csv
└── exp3_concurrent_fresh-diskann.csv
```

### CSV格式

**exp1 (搜索延迟)**
```csv
system,L,recall,qps,avg_lat_us,p50_lat_us,p90_lat_us,p95_lat_us,p99_lat_us,mean_ios,io_amplification
DC-PDI,200,0.95,5200,192,180,230,250,280,12.5,1.25
```

**exp2 (更新吞吐量)**
```csv
system,time_sec,num_inserts,throughput_ops,memory_rss_mb,merge_triggered
DC-PDI,1.0,1500,1500,2048,0
```

**exp3 (并发性能)**
```csv
system,time_sec,search_qps,search_p99_us,insert_ops,insert_tput,memory_rss_mb
DC-PDI,1.0,4500,250,1200,1200,2048
```

## 六、可视化

### 使用plot_system_comparison.py（新增）
```bash
# 生成三系统对比图表
python3 draw/plot_system_comparison.py ./results/comparison

# 输出:
# - figures/fig_search_latency_comparison.pdf
# - figures/fig_update_throughput_comparison.pdf  
# - figures/fig_concurrent_performance_comparison.pdf
# - system_comparison_summary.csv
```

### 使用plot_thesis_figures.py（原有）
**不兼容**。原有绘图脚本用于DC-PDI专有实验（exp2, exp8-11），数据格式不同。

对比实验数据需要使用`plot_system_comparison.py`。

## 七、系统说明

| 系统 | 搜索模式 | 更新策略 |
|------|---------|---------|
| DC-PDI | PIPE_SEARCH | 原地更新+聚类优化 |
| IP-DiskANN | BEAM_SEARCH | 朴素原地更新 |
| FreshDiskANN | BEAM_SEARCH | 原地更新+周期性合并 |

## 八、快速测试

```bash
# 5分钟快速验证（仅测试一个L值）
./scripts/quick_start_comparison.sh
```
