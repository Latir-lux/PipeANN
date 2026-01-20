# PipeANN 逻辑空间局部性验证实验

## 代码修改说明

本实验对 PipeANN 代码进行了以下修改，用于验证 Beam Search 算法中的逻辑空间局部性。

### 1. 新增文件

#### `include/access_tracer.h`
追踪数据结构定义：
- `AccessStep`: 记录单步搜索行为（pivot节点、逻辑邻居、缓存命中、I/O请求等）
- `QueryTrace`: 记录单次查询的完整追踪信息
- `AccessTracer`: 追踪管理器，支持输出 JSONL/CSV 格式

### 2. 修改文件

#### `include/ssd_index.h`
- 添加 `#include "access_tracer.h"`
- 修改 `pipe_search()` 函数签名，添加 `QueryTrace *trace = nullptr` 参数
- 添加碎片化模拟相关函数声明：
  - `apply_fragmentation(float ratio, uint32_t seed)`: 应用碎片化模拟
  - `reset_fragmentation()`: 重置碎片化
  - `is_fragmentation_enabled()`, `get_fragmentation_ratio()`: 状态查询

#### `src/ssd_index.cpp`
- 添加 `#include <random>`
- 实现 `apply_fragmentation()`: 随机打乱部分节点的逻辑-物理映射
- 实现 `reset_fragmentation()`: 恢复原始映射

#### `src/search/pipe_search.cpp`
- 添加 `#include "access_tracer.h"`
- 修改 `pipe_search()` 函数签名
- 在函数内添加追踪逻辑：
  - 在节点展开时记录 pivot 信息
  - 在邻居遍历时记录所有逻辑邻居和缓存命中
  - 在发送 I/O 请求时记录
  - 在 I/O 完成时记录时间戳
  - 在函数结束时完成追踪统计

#### `tests/search_disk_index.cpp`
- 添加全局追踪器和配置变量
- 添加命令行参数解析：
  - `--trace`: 启用追踪
  - `--trace-output <dir>`: 输出目录
  - `--fragmentation <ratio>`: 碎片化比例
  - `--frag-seed <seed>`: 随机种子
- 在搜索时创建追踪对象并传递给 `pipe_search()`
- 在搜索完成后保存追踪结果

### 3. 新增脚本

#### `scripts/run_locality_experiment.sh`
实验执行脚本，支持：
- 基线实验（无碎片化）
- 碎片化实验（30%/50%）
- Pipeline Width 敏感性分析
- 结果分析

#### `scripts/test_compile.sh`
编译测试脚本

#### `draw/analyze_traces.py`
数据分析与可视化脚本，计算：
- NCR (Neighbor Conversion Rate) - 邻居转化率
- TAW (Temporal Access Window) - 时间访问窗口
- LPMS (Logical-Physical Mismatch Score) - 逻辑-物理失配度

生成图表：
- NCR 分布直方图
- TAW 分布直方图
- LPMS 对比图
- QPS/Latency 对比图

---

## 数据集路径变量位置

以下是需要修改的数据集路径变量位置：

### 1. `scripts/run_locality_experiment.sh` (第15-18行)
```bash
# 数据集路径 (需要修改为实际路径)
INDEX_PREFIX="/mnt/nvme2/indices/bigann/100m"           # 索引文件前缀
QUERY_BIN="/mnt/nvme/data/bigann/bigann_query.bbin"     # 查询向量文件
TRUTHSET_BIN="/mnt/nvme/data/bigann/100M_gt.bin"        # Ground truth 文件
```

### 2. `scripts/tests-pipeann/hello_world.sh` (第2行)
```bash
build/tests/search_disk_index uint8 /mnt/nvme2/indices/bigann/100m 1 32 \
    /mnt/nvme/data/bigann/bigann_query.bbin \
    /mnt/nvme/data/bigann/100M_gt.bin 10 l2 pq 2 10 10 20 30 40
```

### 3. `tests/search_disk_index.cpp` (命令行参数)
搜索时通过命令行传入：
- `argv[2]`: `index_prefix_path` - 索引文件前缀
- `argv[5]`: `query_bin` - 查询向量文件
- `argv[6]`: `truthset_bin` - Ground truth 文件

---

## 使用说明

### 1. 编译项目
```bash
cd /home/latir/WorkSpace/PipeANN
./scripts/test_compile.sh
```

### 2. 运行实验

#### 完整实验流程
```bash
./scripts/run_locality_experiment.sh all
```

#### 分步执行
```bash
# 基线实验
./scripts/run_locality_experiment.sh baseline

# 50% 碎片化实验
./scripts/run_locality_experiment.sh frag

# 分析结果
./scripts/run_locality_experiment.sh analyze
```

#### 直接命令行运行
```bash
./build/tests/search_disk_index float /path/to/index 1 8 \
    /path/to/query.bin /path/to/gt.bin 10 l2 pq 2 0 50 \
    --trace --trace-output ./draw --fragmentation 0.5
```

### 3. 分析结果
```bash
cd draw
python3 analyze_traces.py --data-dir . --output-dir .
```

---

## 输出文件说明

实验会在 `draw/` 目录下生成以下文件：

### 追踪数据
- `trace_static.jsonl`: 静态（无碎片化）追踪数据
- `trace_frag50.jsonl`: 50% 碎片化追踪数据
- `trace_summary_*.csv`: 步骤级别统计
- `neighbor_details_*.csv`: 邻居访问详情
- `page_access_*.csv`: 页面访问序列

### 分析结果
- `analysis_summary.csv`: 统计摘要
- `ncr_histogram.png`: NCR 分布图
- `taw_histogram.png`: TAW 分布图
- `lpms_comparison.png`: LPMS 对比图
- `qps_latency_comparison.png`: QPS/Latency 对比图

---

## 预期结论

1. **NCR 在 Static/Dynamic 组保持一致**: 证明逻辑局部性是算法固有的，与物理布局无关
2. **TAW 峰值集中在 1-3 步**: 证明预取策略的有效性
3. **LPMS 在 Dynamic 组显著升高**: 证明碎片化破坏了物理局部性
4. **Trace 开关关闭时无性能回退**: 追踪功能设计为可选，默认不影响性能
