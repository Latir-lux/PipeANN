# DC-PDI实验快速启动

本文档提供DC-PDI（Dynamic Clustering & Pipelined Direct Insertion）实验的快速启动指南。

---

## 📚 文档索引

| 文档 | 说明 |
|------|------|
| [experiment_guide.md](experiment_guide.md) | 完整实验执行指南（推荐详细阅读） |
| [dc-pdi.md](dc-pdi.md) | DC-PDI系统实现方案 |
| 本文档 | 快速启动指南 |

---

## ⚡ 快速启动（3步）

### 步骤1: 编译项目

```bash
cd /home/latir/WorkSpace/PipeANN
bash build.sh
```

编译会自动启用DC-PDI所需的所有选项：
- ✅ `ENABLE_BLOCK_AWARE_PRUNE=ON` - 块感知边剪枝
- ✅ `ENABLE_DISPERSION_MONITOR=ON` - 物理离散度监控
- ✅ `USE_AIO=ON` - libaio支持
- ✅ `COLLECT_IO_STATS=ON` - I/O统计收集

### 步骤2: 准备数据和索引

确保以下文件存在：

```bash
# 索引文件
${INDEX_PREFIX}_disk.index
${INDEX_PREFIX}_pq_compressed.bin
${INDEX_PREFIX}_pq_pivots.bin

# 查询和真值文件
${QUERY_FILE}
${GT_FILE}
```

如果没有索引，参见 [experiment_guide.md 步骤3](experiment_guide.md#-步骤3-构建索引) 构建索引。

### 步骤3: 运行实验

#### 选项A: 运行单个实验（推荐先试）

编辑 `run_exp8_dispersion.sh` 修改数据路径，然后运行：

```bash
./run_exp8_dispersion.sh
```

#### 选项B: 批量运行所有实验

编辑 `run_all_dc_pdi_experiments.sh` 修改数据路径，然后运行：

```bash
./run_all_dc_pdi_experiments.sh
```

---

## 📋 实验列表

### 核心实验

**实验8: 物理离散度评估** ⭐（DC-PDI第3-4章核心实验）
- **目的**: 评估动态聚簇分配和块感知剪枝的效果
- **输出**: 物理离散度、页面局部性、碎片化页面统计
- **运行**: `./run_exp8_dispersion.sh`
- **预期结果**: 
  - `avg_physical_dispersion` < 0.5（DC-PDI应显著低于baseline）
  - `avg_page_local_edge_ratio` > 0.6（页面内邻居比例高）

### 性能评估实验

**实验1: 搜索延迟分布**
- **目的**: 测试不同召回率下的延迟
- **输出**: 延迟分布曲线数据

**实验5: I/O放大率**
- **目的**: 测量I/O效率
- **输出**: I/O统计数据

**实验6: 流水线宽度敏感性**
- **目的**: 测试不同beam_width的性能
- **输出**: 性能曲线

**实验7: 资源开销**
- **目的**: 评估内存和磁盘使用
- **输出**: 资源统计

---

## 🔧 自定义实验参数

### 方法1: 修改脚本

编辑 `run_exp8_dispersion.sh` 或 `run_all_dc_pdi_experiments.sh`:

```bash
# 修改这些变量
INDEX_PREFIX=/your/path/to/index
QUERY_FILE=/your/path/to/query.fbin
GT_FILE=/your/path/to/groundtruth.bin
DATA_TYPE=float                # float/uint8/int8
NUM_THREADS=32                 # 搜索线程数
BEAM_WIDTH=16                  # 流水线宽度
```

### 方法2: 直接命令行

```bash
./build/tests/thesis_benchmark \
  8 \                          # 实验类型
  float \                      # 数据类型
  /path/to/index \             # 索引前缀
  /path/to/query.fbin \        # 查询文件
  /path/to/gt.bin \            # 真值文件
  results/output.csv \         # 输出文件
  32 \                         # 线程数
  16 \                         # 流水线宽度
  10 \                         # recall_at
  10                           # mem_L
```

---

## 📊 结果分析

### 查看实验8结果

```bash
# 主要统计
cat results/exp8_dispersion.csv

# 页面级详情（如果生成）
head results/exp8_dispersion.csv.pages.csv
```

### 关键指标解读

| 指标 | 含义 | DC-PDI目标 | Baseline典型值 |
|------|------|-----------|---------------|
| `avg_physical_dispersion` | 平均物理离散度 $D_p(u)$ | < 0.45 | 0.7-0.85 |
| `avg_page_local_edge_ratio` | 页面内邻居比例 | > 0.65 | 0.15-0.30 |
| `global_fragmented_pages` | 需重组织页面数 | 越少越好 | - |
| `global_avg_fragmentation` | 全局碎片化程度 | < 0.20 | - |

### 绘制图表

如果环境支持Python和matplotlib：

```bash
# 安装依赖（如果需要）
pip install matplotlib pandas numpy

# 运行绘图脚本
python3 draw/plot_thesis_figures.py
```

生成的图表保存在 `draw/` 目录，包括：
- 图3-1: 拓扑强度对比
- 图3-2: 物理离散度演变
- 图3-3: 块感知边选择效果
- 图4-1: 聚簇感知I/O效率
- 图4-2: 离散度阈值敏感性
- 图5-1~5-6: 系统性能图表

---

## 🔍 验证DC-PDI是否生效

### 检查1: 编译选项

```bash
grep -E "ENABLE_BLOCK_AWARE_PRUNE|ENABLE_DISPERSION_MONITOR" build/CMakeCache.txt
```

应该看到：
```
ENABLE_BLOCK_AWARE_PRUNE:BOOL=ON
ENABLE_DISPERSION_MONITOR:BOOL=ON
```

### 检查2: 运行输出

实验8运行时应该看到：
```
=== Physical Dispersion Results ===
Avg Physical Dispersion: 0.35
Avg Page-Local Edge Ratio: 68.5%
Page-level details saved to results/exp8_dispersion.csv.pages.csv
```

如果没有看到这些输出，说明监控未启用，需要重新编译。

### 检查3: 对比实验

运行两个版本对比：

```bash
# 版本1: DC-PDI完整版
cmake .. -DENABLE_BLOCK_AWARE_PRUNE=ON -DENABLE_DISPERSION_MONITOR=ON
make -j$(nproc)
./build/tests/thesis_benchmark 8 ... results/dcpdi.csv ...

# 版本2: 禁用块感知剪枝
cmake .. -DENABLE_BLOCK_AWARE_PRUNE=OFF -DENABLE_DISPERSION_MONITOR=ON
make -j$(nproc)
./build/tests/thesis_benchmark 8 ... results/baseline.csv ...

# 对比
echo "DC-PDI:"
grep avg_physical_dispersion results/dcpdi.csv
echo "Baseline:"
grep avg_physical_dispersion results/baseline.csv
```

预期DC-PDI的离散度应该**显著低于**baseline。

---

## 🐛 常见问题

### Q1: 编译错误 "io_uring.h not found"

**解决**: 系统不支持io_uring，已自动切换到libaio（USE_AIO=ON）。这是预期行为。

### Q2: 实验8输出的离散度为0或非常小

**原因**: 可能是索引刚构建，还未经过更新操作。

**解决**: 
- 在已有更新负载的索引上运行实验
- 或者先运行插入操作（需要动态更新功能）

### Q3: 找不到索引文件

**错误信息**: `Cannot open index file`

**解决**: 
1. 检查 `INDEX_PREFIX` 是否正确
2. 确认 `${INDEX_PREFIX}_disk.index` 存在
3. 参见 [experiment_guide.md](experiment_guide.md) 构建索引

### Q4: 绘图脚本报错

**错误**: `No module named matplotlib`

**解决**: 
```bash
# 使用pip安装
pip install matplotlib pandas numpy

# 或使用系统包管理器
sudo apt install python3-matplotlib python3-pandas python3-numpy
```

### Q5: 实验运行时间过长

**优化建议**:
- 减少查询数量（只用前100-1000条查询）
- 使用小数据集（SIFT1M而非SIFT100M）
- 减少线程数（NUM_THREADS=8）

---

## 📁 文件结构

```
/home/latir/WorkSpace/PipeANN/
├── work2do/
│   ├── README.md                        # 本文档
│   ├── experiment_guide.md              # 完整实验指南 ⭐
│   └── dc-pdi.md                        # 实现方案文档
├── run_exp8_dispersion.sh               # 单实验运行脚本
├── run_all_dc_pdi_experiments.sh        # 批量实验脚本
├── draw/
│   └── plot_thesis_figures.py           # 绘图脚本
├── build/
│   └── tests/
│       └── thesis_benchmark             # 实验主程序
├── results/                             # 实验结果（运行后生成）
│   ├── exp8_dispersion.csv
│   ├── exp8_dispersion.csv.pages.csv
│   ├── exp1_latency.csv
│   └── ...
└── include/
    └── utils/
        ├── clustering.h                 # 动态聚簇实现
        └── dispersion_monitor.h         # 离散度监控
```

---

## 📞 获取帮助

1. **详细文档**: 参见 [experiment_guide.md](experiment_guide.md)
2. **实现细节**: 参见 [dc-pdi.md](dc-pdi.md)
3. **代码问题**: 检查编译错误和运行日志
4. **参数调优**: 参考 experiment_guide.md 附录A

---

## ✅ 快速检查清单

在运行实验前，确认：

- [ ] 项目已编译（`bash build.sh`）
- [ ] 索引文件存在（`${INDEX_PREFIX}_disk.index`）
- [ ] 查询文件存在（`${QUERY_FILE}`）
- [ ] 真值文件存在（`${GT_FILE}`）
- [ ] 编辑了脚本中的路径配置
- [ ] 创建了结果目录（`mkdir -p results`）

然后运行：
```bash
./run_exp8_dispersion.sh          # 单实验
# 或
./run_all_dc_pdi_experiments.sh   # 全部实验
```

---

**祝实验顺利！** 🚀

如有问题，请查阅 [experiment_guide.md](experiment_guide.md) 获取详细帮助。
