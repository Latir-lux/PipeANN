#pragma once

/**
 * clustering.h - 动态聚类感知数据分配
 * 
 * 对应论文第3章：基于动态聚类的存储分布与空间优化方法
 * 
 * 核心概念：
 * 1. 拓扑连接强度 S(u, P): 节点u与物理页面P之间的亲和力
 * 2. 物理离散度 D_p(u): 节点邻居的物理分散程度
 * 3. 块感知边选择: 优先保留页内边，惩罚跨页边
 */

#include <cmath>
#include <cstdint>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <algorithm>

namespace pipeann {

/**
 * 拓扑连接强度计算器
 * 
 * 公式: S(u, P_j) = Σ (1/dist(u,v)^α) * ω_nav
 *       其中 v ∈ V(P_j) ∩ N(u)
 */
class TopologyStrength {
public:
  // 距离衰减因子，论文建议 α ≥ 1
  static constexpr float kDistanceDecay = 1.5f;
  
  // 导航权重（枢纽节点加权，默认为1.0）
  static constexpr float kNavWeight = 1.0f;
  
  /**
   * 计算节点u对页面P的连接强度
   * 
   * @param u_neighbors 节点u的邻居ID集合
   * @param u_distances 节点u到各邻居的距离（与u_neighbors一一对应）
   * @param page_nodes 页面P中的节点ID集合
   * @return 连接强度值（越大表示亲和力越强）
   */
  static float compute_strength(
      const std::vector<uint32_t>& u_neighbors,
      const std::vector<float>& u_distances,
      const std::vector<uint32_t>& page_nodes) {
    
    if (u_neighbors.empty() || page_nodes.empty()) {
      return 0.0f;
    }
    
    std::unordered_set<uint32_t> page_set(page_nodes.begin(), page_nodes.end());
    float strength = 0.0f;
    
    for (size_t i = 0; i < u_neighbors.size() && i < u_distances.size(); i++) {
      if (page_set.count(u_neighbors[i]) > 0) {
        // 避免除零，设置最小距离
        float dist = std::max(u_distances[i], 1e-6f);
        strength += kNavWeight / std::pow(dist, kDistanceDecay);
      }
    }
    return strength;
  }
  
  /**
   * 批量计算节点对多个页面的连接强度
   * 
   * @param u_neighbors 节点u的邻居ID集合
   * @param u_distances 节点u到各邻居的距离
   * @param neighbor_to_page 邻居ID到页面ID的映射函数
   * @return 页面ID到连接强度的映射
   */
  static std::unordered_map<uint64_t, float> compute_all_strengths(
      const std::vector<uint32_t>& u_neighbors,
      const std::vector<float>& u_distances,
      const std::function<uint64_t(uint32_t)>& neighbor_to_page) {
    
    std::unordered_map<uint64_t, float> page_strengths;
    
    for (size_t i = 0; i < u_neighbors.size() && i < u_distances.size(); i++) {
      uint64_t page = neighbor_to_page(u_neighbors[i]);
      float dist = std::max(u_distances[i], 1e-6f);
      page_strengths[page] += kNavWeight / std::pow(dist, kDistanceDecay);
    }
    
    return page_strengths;
  }
  
  /**
   * 获取按连接强度排序的页面列表
   */
  static std::vector<std::pair<uint64_t, float>> get_sorted_pages(
      const std::vector<uint32_t>& u_neighbors,
      const std::vector<float>& u_distances,
      const std::function<uint64_t(uint32_t)>& neighbor_to_page) {
    
    auto page_strengths = compute_all_strengths(u_neighbors, u_distances, neighbor_to_page);
    
    std::vector<std::pair<uint64_t, float>> sorted_pages(
        page_strengths.begin(), page_strengths.end());
    
    std::sort(sorted_pages.begin(), sorted_pages.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });
    
    return sorted_pages;
  }
};

/**
 * 物理离散度计算器
 * 
 * 公式: D_p(u) = Σ 1[Page(u) ≠ Page(v)] * Cost_IO
 *       其中 v ∈ N(u)
 */
class PhysicalDispersion {
public:
  /**
   * 计算节点u的物理离散度（跨页邻居数量）
   * 
   * @param u_page 节点u所在的页面ID
   * @param neighbors 节点u的邻居ID集合
   * @param node_to_page 节点ID到页面ID的映射函数
   * @return 跨页邻居数量
   */
  static uint32_t compute(
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
  
  /**
   * 计算归一化的物理离散度（0.0 - 1.0）
   */
  static float compute_normalized(
      uint64_t u_page,
      const std::vector<uint32_t>& neighbors,
      const std::function<uint64_t(uint32_t)>& node_to_page) {
    
    if (neighbors.empty()) return 0.0f;
    return static_cast<float>(compute(u_page, neighbors, node_to_page)) / neighbors.size();
  }
  
  /**
   * 计算页内边比例
   */
  static float compute_page_local_ratio(
      uint64_t u_page,
      const std::vector<uint32_t>& neighbors,
      const std::function<uint64_t(uint32_t)>& node_to_page) {
    
    if (neighbors.empty()) return 1.0f;
    uint32_t local_count = 0;
    for (auto v : neighbors) {
      if (node_to_page(v) == u_page) {
        local_count++;
      }
    }
    return static_cast<float>(local_count) / neighbors.size();
  }
};

/**
 * 块感知边选择器
 * 
 * 对应论文4.2节：I/O代价感知距离
 * d'(v_new, u) = d(v_new, u) * β  if Page(u) ≠ Page(v_new)
 *              = d(v_new, u)      otherwise
 */
class BlockAwareSelector {
public:
  // 跨页惩罚系数 β
  static constexpr float kCrossPagePenalty = 1.5f;
  
  /**
   * 计算I/O代价感知距离
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
   * 
   * @param candidates 候选邻居ID列表
   * @param distances 原始距离列表
   * @param target_page 目标节点所在页面
   * @param node_to_page 节点到页面的映射函数
   * @return 调整后的距离列表
   */
  static std::vector<float> apply_block_penalty(
      const std::vector<uint32_t>& candidates,
      const std::vector<float>& distances,
      uint64_t target_page,
      const std::function<uint64_t(uint32_t)>& node_to_page) {
    
    std::vector<float> adjusted_distances(distances.size());
    for (size_t i = 0; i < candidates.size() && i < distances.size(); i++) {
      adjusted_distances[i] = compute_io_aware_distance(
          distances[i], target_page, node_to_page(candidates[i]));
    }
    return adjusted_distances;
  }
  
  /**
   * 原地应用块感知惩罚（修改原始距离）
   */
  static void apply_block_penalty_inplace(
      const std::vector<uint32_t>& candidates,
      std::vector<float>& distances,
      uint64_t target_page,
      const std::function<uint64_t(uint32_t)>& node_to_page) {
    
    for (size_t i = 0; i < candidates.size() && i < distances.size(); i++) {
      uint64_t nbr_page = node_to_page(candidates[i]);
      if (nbr_page != target_page) {
        distances[i] *= kCrossPagePenalty;
      }
    }
  }
  
  /**
   * 恢复被惩罚的距离
   */
  static void restore_distances(
      const std::vector<uint32_t>& candidates,
      std::vector<float>& distances,
      uint64_t target_page,
      const std::function<uint64_t(uint32_t)>& node_to_page) {
    
    for (size_t i = 0; i < candidates.size() && i < distances.size(); i++) {
      uint64_t nbr_page = node_to_page(candidates[i]);
      if (nbr_page != target_page) {
        distances[i] /= kCrossPagePenalty;
      }
    }
  }
};

}  // namespace pipeann
