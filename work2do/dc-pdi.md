# DC-PDI系统实现方案

## 基于《面向大模型的磁盘动态图索引技术研究》第3章和第4章的设计

---

## 一、文档核心概念摘要

### 1.1 第三章：基于动态聚类的存储分布与空间优化方法

**核心问题**：逻辑拓扑与物理存储布局的严重失配导致物理存储退化

**关键概念**：
- **拓扑连接强度** $S(u, P_j)$：节点u与物理页面$P_j$之间的亲和力度量
  $$S(u, P_j) = \sum_{v \in V(P_j) \cap N(u)} \frac{1}{dist(u,v)^\alpha} \cdot \omega_{nav}$$
  
- **物理离散度** $D_p(u)$：量化节点邻居的物理分散程度
  $$D_p(u) = \sum_{v \in N(u)} \mathbb{1}_{[Page(u) \neq Page(v)]} \cdot Cost_{IO}$$

- **动态聚类感知的数据分配**：在插入时根据连接强度选择目标页面
- **离线定期聚类重组织**：当物理离散度超过阈值时触发局部重组

### 1.2 第四章：细粒度流水的向量直接插入策略与并发控制

**核心设计**：
- **向量直接插入策略**：摒弃LSM-Tree缓冲合并，采用原地更新
- **异步流水线处理模型**：三级流水线（搜索-预取、计算-剪枝、更新-持久化）
- **块感知边选择策略**：优先保留页内边，惩罚跨页边
  $$d'(v_{new}, u) = \begin{cases} d(v_{new}, u) & \text{if } u \text{ is page-local} \\ d(v_{new}, u) \times \beta & \text{otherwise} \end{cases}$$
- **并发控制协议**：搜索与更新冲突处理、更新与更新冲突处理
- **惰性删除策略**：逻辑软删除 + 后台物理回收

---

## 二、现有代码分析

### 2.1 当前PipeANN实现的相关组件

| 文件 | 功能 | 与论文设计的对应关系 |
|------|------|---------------------|
| `src/update/direct_insert.cpp` | 直接插入实现 | 对应4.2节向量直接插入 |
| `src/search/pipe_search.cpp` | 流水线搜索 | 对应4.3节异步流水线 |
| `src/utils/prune_neighbors.cpp` | 邻居剪枝 | 需要增加块感知策略 |
| `include/ssd_index.h` | 索引核心结构 | 需要增加连接强度和物理离散度 |
| `include/utils/lock_table.h` | 锁表 | 对应4.4节并发控制 |

### 2.2 现有`alloc_loc`函数分析

当前位置分配策略 (`ssd_index.h:536-606`)：
```cpp
std::vector<uint64_t> alloc_loc(int n, const std::vector<uint64_t> &hint_pages,
                                std::set<uint64_t> &page_need_to_read) {
  // 1. 使用空页面
  // 2. 使用hint_pages（搜索过程中访问的页面）
  // 3. 分配新页面
}
```

**问题**：hint_pages仅基于搜索路径，未考虑拓扑连接强度

### 2.3 现有剪枝策略分析

当前`prune_neighbors`和`delta_prune_neighbors_pq`：
- 基于α-RNG三角不等式
- 未考虑页面局部性

---

## 三、实现方案

### 3.1 新增数据结构

#### A. 拓扑连接强度计算器 (`include/utils/clustering.h`)

```cpp
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>
#include <unordered_map>

namespace pipeann {

/**
 * 拓扑连接强度计算器
 * 对应论文3.2.2节：基于节点连接强度的聚类感知分配规则
 */
template<typename T>
class TopologyStrength {
public:
  // 距离衰减因子，论文建议α≥1
  static constexpr float kDistanceDecay = 1.5f;
  // 导航权重（枢纽节点加权）
  static constexpr float kNavWeight = 1.0f;
  
  /**
   * 计算节点u对页面P的连接强度
   * S(u,P) = Σ(1/dist(u,v)^α) * ω_nav, for v ∈ V(P) ∩ N(u)
   * 
   * @param u_neighbors 节点u的邻居集合
   * @param u_distances 节点u到各邻居的距离
   * @param page_nodes 页面P中的节点集合
   * @return 连接强度值
   */
  static float compute_strength(
      const std::vector<uint32_t>& u_neighbors,
      const std::vector<float>& u_distances,
      const std::vector<uint32_t>& page_nodes) {
    
    float strength = 0.0f;
    std::unordered_set<uint32_t> page_set(page_nodes.begin(), page_nodes.end());
    
    for (size_t i = 0; i < u_neighbors.size(); i++) {
      if (page_set.count(u_neighbors[i]) > 0) {
        float dist = std::max(u_distances[i], 1e-6f);  // 避免除零
        strength += kNavWeight / std::pow(dist, kDistanceDecay);
      }
    }
    return strength;
  }
  
  /**
   * 计算节点u的物理离散度
   * D_p(u) = |{v ∈ N(u) : Page(u) ≠ Page(v)}|
   */
  static uint32_t compute_physical_dispersion(
      uint64_t u_page,
      const std::vector<uint32_t>& neighbors,
      const std::function<uint64_t(uint32_t)>& node_to_page) {
    
    uint32_t dispersion = 0;
    for (auto v : neighbors) {
      if (node_to_page(v) != u_page) {
        dispersion++;
      }
    }
    return dispersion;
  }
};

/**
 * 物理离散度监控器
 * 对应论文3.3节：离线的定期聚类重组织机制
 */
class DispersionMonitor {
public:
  // 碎片化阈值
  static constexpr float kFragmentationThreshold = 0.6f;  // 60%邻居在其他页面
  
  struct PageStats {
    uint64_t page_id;
    float avg_dispersion;       // 平均物理离散度
    uint32_t node_count;        // 页面内节点数
    uint32_t valid_node_count;  // 有效节点数（非删除）
  };
  
  std::vector<PageStats> fragmented_pages;  // 碎片化页面列表
  
  /**
   * 检测是否需要触发重组织
   * 当碎片化页面比例超过20%时触发
   */
  bool should_reorganize(size_t total_pages) const {
    return fragmented_pages.size() > total_pages * 0.2;
  }
};

}  // namespace pipeann
```

#### B. 块感知边选择器 (`include/utils/block_aware.h`)

```cpp
#pragma once
#include <cstdint>
#include <vector>

namespace pipeann {

/**
 * 块感知边选择策略
 * 对应论文4.2节：I/O代价感知距离
 */
class BlockAwareSelector {
public:
  // 跨页惩罚系数β
  static constexpr float kCrossPagePenalty = 1.5f;
  
  /**
   * 计算I/O代价感知距离
   * d'(v_new, u) = d(v_new, u) * β  if Page(u) ≠ Page(v_new)
   *              = d(v_new, u)      otherwise
   */
  static float compute_io_aware_distance(
      float original_dist,
      uint64_t target_page,
      uint64_t neighbor_page) {
    
    if (target_page != neighbor_page) {
      return original_dist * kCrossPagePenalty;
    }
    return original_dist;
  }
  
  /**
   * 对候选邻居应用块感知惩罚
   * @param candidates 候选邻居ID
   * @param distances 原始距离
   * @param target_page 目标节点所在页面
   * @param node_to_page 节点到页面的映射函数
   * @return 调整后的距离
   */
  static std::vector<float> apply_block_penalty(
      const std::vector<uint32_t>& candidates,
      const std::vector<float>& distances,
      uint64_t target_page,
      const std::function<uint64_t(uint32_t)>& node_to_page) {
    
    std::vector<float> adjusted_distances(distances.size());
    for (size_t i = 0; i < candidates.size(); i++) {
      adjusted_distances[i] = compute_io_aware_distance(
          distances[i], target_page, node_to_page(candidates[i]));
    }
    return adjusted_distances;
  }
};

}  // namespace pipeann
```

### 3.2 修改`alloc_loc`函数实现聚类感知分配

**文件**: `include/ssd_index.h`

**修改思路**：
1. 在分配时计算候选页面的连接强度
2. 按连接强度排序选择最优页面
3. 保留超量空间机制

```cpp
// 新增方法：基于连接强度的位置分配
std::vector<uint64_t> alloc_loc_clustering_aware(
    int n, 
    const std::vector<uint32_t>& new_neighbors,
    const std::vector<float>& neighbor_dists,
    std::set<uint64_t>& page_need_to_read) {
  
  std::lock_guard<std::mutex> lock(alloc_lock);
  std::vector<uint64_t> ret;
  
  // 1. 收集候选页面（邻居所在页面）
  std::unordered_map<uint64_t, float> page_strength;
  for (size_t i = 0; i < new_neighbors.size(); i++) {
    uint64_t page = node_sector_no(new_neighbors[i]);
    // 累加连接强度
    float dist = std::max(neighbor_dists[i], 1e-6f);
    page_strength[page] += 1.0f / std::pow(dist, 1.5f);
  }
  
  // 2. 按连接强度排序
  std::vector<std::pair<uint64_t, float>> sorted_pages(
      page_strength.begin(), page_strength.end());
  std::sort(sorted_pages.begin(), sorted_pages.end(),
      [](const auto& a, const auto& b) { return a.second > b.second; });
  
  int cur = 0;
  uint32_t threshold = (nnodes_per_sector + kIndexSizeFactor - 1) / kIndexSizeFactor;
  
  // 3. 优先使用高连接强度页面的空槽
  for (auto& [page, strength] : sorted_pages) {
    if (cur >= n) break;
    
    auto st = sector_to_loc(page, 0);
    auto ed = nnodes_per_sector == 0 ? st + 1 : st + nnodes_per_sector;
    
    uint32_t empty_count = 0;
    for (uint32_t i = st; i < ed; i++) {
      if (loc2id_[i] == kInvalidID) empty_count++;
    }
    
    if (empty_count < threshold) continue;  // 空槽不足
    
    if (empty_count < nnodes_per_sector) {
      page_need_to_read.insert(page);
    }
    
    for (uint32_t i = st; i < ed && cur < n; i++) {
      if (loc2id_[i] == kInvalidID) {
        loc2id_[i] = kAllocatedID;
        ret.push_back(i);
        cur++;
      }
    }
  }
  
  // 4. 空页面回退
  if (cur < n) {
    uint32_t empty_page;
    while ((empty_page = empty_pages.pop()) != kInvalidID && cur < n) {
      auto st = sector_to_loc(empty_page, 0);
      auto ed = nnodes_per_sector == 0 ? st + 1 : st + nnodes_per_sector;
      for (uint32_t i = st; i < ed && cur < n; i++) {
        loc2id_[i] = kAllocatedID;
        ret.push_back(i);
        cur++;
      }
    }
  }
  
  // 5. 分配新页面
  int remaining = n - cur;
  for (int i = 0; i < remaining; i++) {
    set_loc2id(cur_loc + i, kAllocatedID);
    ret.push_back(cur_loc + i);
  }
  
  cur_loc += remaining;
  while (nnodes_per_sector != 0 && cur_loc % nnodes_per_sector != 0) {
    set_loc2id(cur_loc++, kInvalidID);
  }
  
  return ret;
}
```

### 3.3 修改剪枝算法实现块感知边选择

**文件**: `src/utils/prune_neighbors.cpp`

**修改思路**：
1. 在剪枝时对跨页边应用惩罚系数
2. 优先保留页内边

```cpp
// 新增：块感知剪枝函数
template<typename T, typename TagT>
void SSDIndex<T, TagT>::prune_neighbors_block_aware(
    const tsl::robin_map<uint32_t, T*>& coord_map,
    std::vector<Neighbor>& pool,
    std::vector<uint32_t>& pruned_list,
    uint64_t target_page) {
  
  if (pool.empty()) return;
  
  // 应用块感知惩罚调整距离
  constexpr float kCrossPagePenalty = 1.5f;
  for (auto& nbr : pool) {
    uint64_t nbr_page = node_sector_no(nbr.id);
    if (nbr_page != target_page) {
      nbr.distance *= kCrossPagePenalty;
    }
  }
  
  // 重新排序
  std::sort(pool.begin(), pool.end());
  
  // 使用原有剪枝逻辑
  std::vector<Neighbor> result;
  result.reserve(range);
  std::vector<float> occlude_factor(pool.size(), 0);
  occlude_list(pool, coord_map, result, occlude_factor);
  
  pruned_list.clear();
  for (auto& r : result) {
    // 恢复原始距离（用于后续计算）
    uint64_t r_page = node_sector_no(r.id);
    if (r_page != target_page) {
      r.distance /= kCrossPagePenalty;
    }
    pruned_list.emplace_back(r.id);
  }
  
  // 填充到range
  if (alpha > 1) {
    for (uint32_t i = 0; i < pool.size() && pruned_list.size() < range; i++) {
      if (std::find(pruned_list.begin(), pruned_list.end(), pool[i].id) == pruned_list.end()) {
        pruned_list.emplace_back(pool[i].id);
      }
    }
  }
}
```

### 3.4 物理离散度监控实现

**文件**: `include/utils/dispersion_monitor.h`

```cpp
#pragma once
#include <atomic>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace pipeann {

/**
 * 物理离散度监控器
 * 用于检测需要重组织的碎片化页面
 */
class DispersionMonitor {
public:
  struct PageStats {
    std::atomic<uint32_t> total_dispersion{0};  // 总离散度
    std::atomic<uint32_t> node_count{0};        // 节点数
  };
  
private:
  std::unordered_map<uint64_t, PageStats> page_stats_;
  std::mutex stats_mutex_;
  float fragmentation_threshold_ = 0.6f;  // 60%阈值
  
public:
  void update_dispersion(uint64_t page_id, uint32_t dispersion) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    page_stats_[page_id].total_dispersion += dispersion;
    page_stats_[page_id].node_count++;
  }
  
  float get_avg_dispersion(uint64_t page_id, uint32_t max_neighbors) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    auto it = page_stats_.find(page_id);
    if (it == page_stats_.end() || it->second.node_count == 0) {
      return 0.0f;
    }
    float avg = (float)it->second.total_dispersion / it->second.node_count;
    return avg / max_neighbors;  // 归一化
  }
  
  std::vector<uint64_t> get_fragmented_pages(uint32_t max_neighbors) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    std::vector<uint64_t> result;
    for (auto& [page_id, stats] : page_stats_) {
      if (stats.node_count > 0) {
        float avg = (float)stats.total_dispersion / stats.node_count / max_neighbors;
        if (avg > fragmentation_threshold_) {
          result.push_back(page_id);
        }
      }
    }
    return result;
  }
  
  void reset() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    page_stats_.clear();
  }
};

}  // namespace pipeann
```

---

## 四、修改现有代码

### 4.1 修改`direct_insert.cpp`

**修改点1**：使用聚类感知位置分配

在`insert_in_place`函数中，将现有的`alloc_loc`调用替换：

```cpp
// 原代码（约第67行）：
auto locs = this->alloc_loc(new_nhood.size() + 1, page_ref, pages_need_to_read);

// 修改为：
std::vector<float> neighbor_dists;
for (auto& nbr : exp_node_info) {
  if (std::find(new_nhood.begin(), new_nhood.end(), nbr.id) != new_nhood.end()) {
    neighbor_dists.push_back(nbr.distance);
  }
}
auto locs = this->alloc_loc_clustering_aware(
    new_nhood.size() + 1, new_nhood, neighbor_dists, pages_need_to_read);
```

**修改点2**：使用块感知剪枝

```cpp
// 原代码（约第50行）：
prune_neighbors(coord_map, exp_node_info, new_nhood);

// 修改为：
// 获取目标页面（使用连接强度最高的页面）
uint64_t target_page = 0;
if (!page_ref.empty()) {
  target_page = page_ref[0];  // 假设page_ref已按连接强度排序
}
prune_neighbors_block_aware(coord_map, exp_node_info, new_nhood, target_page);
```

### 4.2 在`ssd_index.h`中添加声明

```cpp
// 在public区域添加：
void prune_neighbors_block_aware(const tsl::robin_map<uint32_t, T*>& coord_map,
                                  std::vector<Neighbor>& pool,
                                  std::vector<uint32_t>& pruned_list,
                                  uint64_t target_page);

std::vector<uint64_t> alloc_loc_clustering_aware(
    int n,
    const std::vector<uint32_t>& new_neighbors,
    const std::vector<float>& neighbor_dists,
    std::set<uint64_t>& page_need_to_read);

// 添加物理离散度监控器
#ifdef ENABLE_DISPERSION_MONITOR
DispersionMonitor dispersion_monitor_;
#endif
```

### 4.3 增强统计收集

在`pipe_search.cpp`中添加物理离散度采样（已在上轮实现I/O统计的基础上）：

```cpp
// 在compute_and_push_nbrs lambda中添加：
#ifdef ENABLE_DISPERSION_MONITOR
// 采样物理离散度
if (stats != nullptr && rand() % 100 < 5) {  // 5%采样率
  uint32_t dispersion = 0;
  uint64_t cur_page = loc_sector_no(loc);
  for (unsigned m = 0; m < nnbrs; m++) {
    if (node_sector_no(node_nbrs[m]) != cur_page) {
      dispersion++;
    }
  }
  dispersion_monitor_.update_dispersion(cur_page, dispersion);
}
#endif
```

---

## 五、与上轮实验代码的契合性分析

### 5.1 上轮已实现的组件

| 组件 | 状态 | 兼容性 |
|------|------|--------|
| `io_stats.h` | ✅ 已实现 | ✅ 完全兼容 |
| `percentile_stats.h` 扩展 | ✅ 已实现 | ✅ 需要添加dispersion字段 |
| `pipe_search.cpp` 插桩 | ✅ 已实现 | ⚠️ 需要增加块感知统计 |
| `thesis_benchmark.cpp` | ✅ 已实现 | ⚠️ 需要添加聚类效果测试 |
| `plot_thesis_figures.py` | ✅ 已实现 | ⚠️ 需要添加离散度图表 |

### 5.2 需要调整的实验代码

#### A. `include/utils/percentile_stats.h` 添加字段

```cpp
// 在QueryStats结构体中添加：
double physical_dispersion = 0;     // 平均物理离散度
double page_local_edge_ratio = 0;   // 页内边比例
```

#### B. `tests/thesis_benchmark.cpp` 添加实验

```cpp
// 添加实验类型：动态聚类效果评估
// 实验2: 物理离散度变化测试
// 实验3: 聚类前后I/O对比测试
```

#### C. 画图代码补充

在`draw/plot_thesis_figures.py`中添加：
- 物理离散度随时间变化曲线
- 页内边比例柱状图
- 聚类前后I/O放大率对比图

---

## 六、编译配置

### 6.1 CMake选项

在`CMakeLists.txt`中添加：
```cmake
option(ENABLE_DISPERSION_MONITOR "Enable physical dispersion monitoring" ON)
option(ENABLE_BLOCK_AWARE_PRUNE "Enable block-aware neighbor pruning" ON)
option(USE_AIO "Use libaio instead of io_uring" ON)

if(ENABLE_DISPERSION_MONITOR)
  add_definitions(-DENABLE_DISPERSION_MONITOR)
endif()

if(ENABLE_BLOCK_AWARE_PRUNE)
  add_definitions(-DENABLE_BLOCK_AWARE_PRUNE)
endif()
```

### 6.2 编译命令

```bash
cd /home/latir/WorkSpace/PipeANN
mkdir -p build && cd build
cmake .. -DUSE_AIO=ON -DENABLE_DISPERSION_MONITOR=ON -DENABLE_BLOCK_AWARE_PRUNE=ON -DCOLLECT_IO_STATS=ON
make -j$(nproc)
```

---

## 七、实现优先级与里程碑

### Phase 1: 核心功能 (高优先级)
1. ✅ 创建`clustering.h`拓扑连接强度计算
2. ⬜ 实现`alloc_loc_clustering_aware`函数
3. ⬜ 实现`prune_neighbors_block_aware`函数
4. ⬜ 修改`direct_insert.cpp`使用新策略

### Phase 2: 监控与统计 (中优先级)
5. ⬜ 实现`dispersion_monitor.h`物理离散度监控
6. ⬜ 扩展统计字段
7. ⬜ 更新实验代码

### Phase 3: 验证与调优 (低优先级)
8. ⬜ 实现离线重组织机制（论文3.3节）
9. ⬜ 参数调优（α, β, threshold等）

---

## 八、验证方案

### 8.1 正确性验证

1. **回归测试**：确保搜索召回率不下降
2. **插入测试**：验证插入后图结构完整性
3. **并发测试**：验证并发插入和搜索的正确性

### 8.2 性能验证

1. **I/O放大率**：对比聚类前后的I/O放大率
2. **物理离散度**：监控长期运行中物理离散度的变化
3. **尾延迟稳定性**：验证P99延迟的稳定性
