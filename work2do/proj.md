# DC-PDI系统实验构建指南 - 第五章实验方案

## 概述

本文档基于论文《面向大模型的磁盘动态图索引技术研究》第五章的实验设计，详细说明各个实验的实施方案、所需代码修改和数据收集方法。

---

## 实验1: 搜索延迟分布与尾延迟对比 (对应5.2.1节)

### 1.1 实验目的

验证DC-PDI的细粒度流水线架构在搜索延迟方面的优势：
- 评估平均搜索延迟与P99尾延迟
- 与DiskANN、FreshDiskANN、IP-DiskANN进行对比
- 验证Search-Prefetch-Compute三级流水线对延迟的改善效果

### 1.2 实验设计

1. **数据集**: SIFT1B (128维, uint8)
2. **测试规模**: 10亿向量预热后进行纯搜索
3. **目标Recall**: Recall@10=95%
4. **对比系统**:
   - DiskANN (mode=0): 静态基准
   - IP-DiskANN (mode=0 + 动态插入后): 朴素原地更新
   - FreshDiskANN (缓冲-合并策略): 官方动态扩展
   - DC-PDI (mode=2): 本文方法

### 1.3 需要修改的代码

#### A. 增强QueryStats统计 (`include/utils/percentile_stats.h`)

需要添加以下统计字段：
```cpp
double search_phase_us = 0;    // 搜索阶段耗时
double prefetch_phase_us = 0;  // 预取阶段耗时
double compute_phase_us = 0;   // 计算阶段耗时
double overlap_ratio = 0;      // I/O与计算重叠率
```

#### B. 在pipe_search.cpp中埋点

在 `src/search/pipe_search.cpp` 中添加阶段耗时统计：
- 搜索发起时间
- I/O预取耗时
- 距离计算耗时
- 三阶段重叠率计算

### 1.4 需要收集的数据

| 指标 | 说明 |
|------|------|
| avg_latency_ms | 平均搜索延迟(ms) |
| p50_latency_ms | 50分位延迟 |
| p90_latency_ms | 90分位延迟 |
| p95_latency_ms | 95分位延迟 |
| p99_latency_ms | 99分位延迟(尾延迟) |
| recall_at_10 | Recall@10 |
| qps | 每秒查询数 |
| mean_ios | 平均I/O次数 |

### 1.5 实验脚本

见 `scripts/thesis_experiments/exp1_latency.sh`

---

## 实验2: 动态更新场景下的性能波动分析 (对应5.2.2节)

### 2.1 实验目的

评估DC-PDI在持续动态更新场景下的稳定性：
- 验证直接插入策略避免性能抖动的能力
- 与FreshDiskANN的缓冲-合并策略进行对比
- 记录长时间运行下P99延迟的时间序列变化

### 2.2 实验设计

1. **持续时间**: 4小时
2. **初始索引**: 5亿向量
3. **插入速率**: 10,000 vectors/s
4. **查询负载**: 2,000 QPS背景查询
5. **采样间隔**: 每5秒记录一次P99延迟

### 2.3 需要修改的代码

#### A. test_insert_search.cpp 增强

在 `tests/test_insert_search.cpp` 中添加：
- 时间序列延迟记录功能
- 周期性采样和输出
- 延迟标准差计算

#### B. 添加延迟波动记录器

创建 `include/utils/latency_recorder.h`:
```cpp
class LatencyRecorder {
  std::vector<std::pair<double, double>> time_latency_pairs;
  void record(double timestamp_s, double latency_ms);
  void save_to_csv(const std::string& filename);
  double get_std_dev();
};
```

### 2.4 需要收集的数据

| 指标 | 说明 |
|------|------|
| timestamp_s | 时间戳(秒) |
| p99_latency_ms | P99延迟 |
| latency_std_dev | 延迟标准差 |
| insert_throughput | 插入吞吐量 |
| search_qps | 搜索QPS |

---

## 实验3: 更新吞吐量评估 (对应5.3.1节)

### 3.1 实验目的

评估DC-PDI在纯更新模式下的最大更新吞吐量：
- 对比DC-PDI、FreshDiskANN、IP-DiskANN的更新速率
- 分析流水线更新架构的贡献

### 3.2 实验设计

1. **测试模式**: 纯更新(关闭在线查询)
2. **数据集**: SIFT1B和DEEP1B
3. **持续时间**: 30分钟
4. **测量指标**: vectors/s

### 3.3 需要修改的代码

#### A. 添加更新吞吐量统计

在 `src/update/direct_insert.cpp` 中添加：
- 插入耗时细分统计
- 吞吐量实时监控

#### B. UpdateStats结构体

```cpp
struct UpdateStats {
  double search_phase_us = 0;     // 搜索邻居阶段
  double prune_phase_us = 0;      // 剪枝阶段
  double io_phase_us = 0;         // 磁盘I/O阶段
  double total_us = 0;            // 总耗时
};
```

### 3.4 需要收集的数据

| 指标 | 说明 |
|------|------|
| update_throughput | 更新吞吐量(vectors/s) |
| avg_insert_latency_us | 平均插入延迟 |
| p99_insert_latency_us | P99插入延迟 |

---

## 实验4: 读写并发度测试 (对应5.3.2节)

### 4.1 实验目的

评估DC-PDI在混合负载下的扩展性：
- 测试CPU核心数从8到64的扩展趋势
- 验证无锁快照获取机制的有效性

### 4.2 实验设计

1. **核心数范围**: 8, 16, 32, 48, 64
2. **读写比例**: 混合负载(搜索+更新)
3. **测量指标**: 总体吞吐量

### 4.3 需要修改的代码

#### A. 扩展性测试框架

在 `tests/test_insert_search.cpp` 中添加：
- 多线程配置支持
- 扩展比计算

### 4.4 需要收集的数据

| 指标 | 说明 |
|------|------|
| num_threads | 线程数 |
| total_throughput | 总吞吐量 |
| scaling_ratio | 扩展比(相对于8线程) |
| lock_contention_rate | 锁竞争率 |

---

## 实验5: I/O放大率与页面访问模式分析 (对应5.4.1节)

### 5.1 实验目的

从物理存储层面验证动态聚类策略的效果：
- 测量I/O放大率
- 分析页面有效利用率
- 对比动态聚类前后的存储局部性

### 5.2 实验设计

1. **操作**: 5亿次随机插入后
2. **测量**: 10,000次随机查询的I/O统计
3. **对比**: DC-PDI vs IP-DiskANN vs DiskANN

### 5.3 需要修改的代码

#### A. I/O统计收集 (`include/utils/io_stats.h`)

创建I/O统计模块：
```cpp
struct IOStats {
  uint64_t total_bytes_read = 0;       // 实际读取字节数
  uint64_t effective_bytes = 0;         // 有效数据字节数
  uint64_t total_pages_read = 0;        // 读取页面数
  uint64_t unique_pages_read = 0;       // 去重后的页面数
  
  double get_io_amplification() const;  // I/O放大率
  double get_page_utilization() const;  // 页面利用率
};
```

#### B. 在pipe_search.cpp和beam_search.cpp中埋点

添加I/O统计收集：
- 每次磁盘读取记录
- 有效数据量统计

### 5.4 需要收集的数据

| 指标 | 说明 |
|------|------|
| io_amplification | I/O放大率 = total_read / theoretical_min |
| pages_per_query | 每次查询平均读取页面数 |
| page_utilization | 页面有效利用率(%) |
| sequential_ratio | 顺序读取比例 |

---

## 实验6: 聚类策略对缓存命中率的影响 (对应5.4.2节)

### 6.1 实验目的

验证动态聚类策略对操作系统Page Cache的影响：
- 对比DC-PDI与IP-DiskANN的缓存命中率
- 分析预读机制的有效性

### 6.2 实验设计

1. **测试环境**: 监控/proc/meminfo
2. **负载**: 相同查询负载
3. **指标**: Page Cache命中率

### 6.3 需要修改的代码

#### A. 缓存命中率监控

在 `include/utils/page_cache.h` 中添加统计：
```cpp
struct CacheStats {
  std::atomic<uint64_t> hits = 0;
  std::atomic<uint64_t> misses = 0;
  
  double get_hit_rate() const {
    return (double)hits / (hits + misses);
  }
};
```

### 6.4 需要收集的数据

| 指标 | 说明 |
|------|------|
| cache_hit_rate | 缓存命中率(%) |
| os_page_cache_usage | 操作系统Page Cache使用量 |

---

## 实验7: 流水线并发度敏感性分析 (对应5.5.1节)

### 7.1 实验目的

分析流水线宽度(pipeline width)对搜索性能的影响：
- 确定最优I/O并发度
- 分析过大并发度的负面效果

### 7.2 实验设计

1. **流水线宽度范围**: 1, 2, 4, 8, 16, 32, 64
2. **固定参数**: L_search=50, num_threads=32
3. **测量指标**: QPS, Recall, I/O效率

### 7.3 需要修改的代码

无需额外修改，使用现有beam_width参数即可。

### 7.4 需要收集的数据

| 指标 | 说明 |
|------|------|
| pipeline_width | 流水线宽度 |
| qps | 搜索吞吐量 |
| recall | 召回率 |
| avg_latency_ms | 平均延迟 |
| io_efficiency | I/O有效率 |

---

## 实验8: 系统资源开销评估 (对应5.5.3节)

### 8.1 实验目的

评估DC-PDI的资源消耗：
- 内存占用
- 磁盘空间膨胀率
- 与纯内存方案对比

### 8.2 实验设计

1. **索引规模**: 10亿向量
2. **测量**: 运行时内存、磁盘占用

### 8.3 需要修改的代码

#### A. 内存监控

在测试代码中添加内存监控：
```cpp
void show_memory_usage() {
  // 读取 /proc/self/statm
  // 计算RSS
}
```

### 8.4 需要收集的数据

| 指标 | 说明 |
|------|------|
| memory_usage_gb | 内存使用量(GB) |
| disk_usage_gb | 磁盘占用(GB) |
| disk_expansion_ratio | 磁盘膨胀率 |

---

## 代码修改总结

### 需要创建的新文件

1. `include/utils/io_stats.h` - I/O统计结构体
2. `include/utils/latency_recorder.h` - 延迟时间序列记录器
3. `tests/thesis_benchmark.cpp` - 论文实验统一入口
4. `scripts/thesis_experiments/` - 实验脚本目录

### 需要修改的现有文件

1. `include/utils/percentile_stats.h` - 扩展QueryStats
2. `src/search/pipe_search.cpp` - 添加阶段耗时埋点
3. `src/search/beam_search.cpp` - 添加I/O统计
4. `src/update/direct_insert.cpp` - 添加更新统计
5. `tests/test_insert_search.cpp` - 添加波动测试功能
6. `include/utils/page_cache.h` - 添加缓存统计

### libaio兼容性修改

由于目标环境不支持io_uring但支持libaio，需要:
1. 在CMakeLists.txt中添加 `-DUSE_AIO` 编译选项
2. 确保 `src/utils/linux_aligned_file_reader.cpp` 中libaio路径正确编译

---

## 编译配置

### CMakeLists.txt修改

```cmake
# 启用论文实验相关宏
add_definitions(-DCOLLECT_IO_STATS)        # 收集I/O统计
add_definitions(-DCOLLECT_PHASE_TIMING)    # 收集阶段耗时

# 使用libaio
option(USE_AIO "Use AIO instead of liburing" ON)
```

### 编译命令

```bash
mkdir -p build && cd build
cmake .. -DUSE_AIO=ON -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

---

## 数据输出格式

所有实验数据以CSV格式输出，便于后续绘图分析：

```csv
# exp1_latency_results.csv
system,dataset,L,recall,qps,avg_lat_ms,p50_lat_ms,p90_lat_ms,p95_lat_ms,p99_lat_ms,mean_ios
DC-PDI,SIFT1B,50,0.95,24000,0.82,0.65,1.02,1.21,1.50,72
DiskANN,SIFT1B,50,0.95,18000,0.91,0.72,1.15,1.45,2.10,85
...
```
