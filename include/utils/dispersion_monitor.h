#pragma once

/**
 * dispersion_monitor.h - 物理离散度监控器
 * 
 * 对应论文3.3节：离线的定期聚类重组织机制
 * 
 * 功能：
 * 1. 实时监控索引区域的物理离散度
 * 2. 识别需要重组织的"碎片页"
 * 3. 触发局部重组织策略
 */

#include <atomic>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <algorithm>

namespace pipeann {

/**
 * 页面统计信息
 */
struct PageDispersionStats {
  std::atomic<uint64_t> total_dispersion{0};    // 累计离散度
  std::atomic<uint32_t> sample_count{0};        // 采样次数
  std::atomic<uint32_t> valid_node_count{0};    // 有效节点数（非删除）
  std::atomic<uint32_t> deleted_node_count{0};  // 已删除节点数
  
  PageDispersionStats() = default;
  
  PageDispersionStats(const PageDispersionStats& other)
      : total_dispersion(other.total_dispersion.load())
      , sample_count(other.sample_count.load())
      , valid_node_count(other.valid_node_count.load())
      , deleted_node_count(other.deleted_node_count.load()) {}
  
  float get_avg_dispersion() const {
    uint32_t count = sample_count.load();
    if (count == 0) return 0.0f;
    return static_cast<float>(total_dispersion.load()) / count;
  }
  
  float get_fragmentation_ratio(uint32_t max_neighbors) const {
    if (max_neighbors == 0) return 0.0f;
    return get_avg_dispersion() / max_neighbors;
  }
};

/**
 * 物理离散度监控器
 */
class DispersionMonitor {
public:
  // 碎片化阈值：当60%以上的邻居在其他页面时标记为碎片页
  static constexpr float kFragmentationThreshold = 0.6f;
  
  // 触发重组织的碎片页比例阈值
  static constexpr float kReorganizeTriggerRatio = 0.2f;  // 20%
  
  // 采样率（每100次访问采样一次）
  static constexpr uint32_t kSampleRate = 100;
  
private:
  std::unordered_map<uint64_t, PageDispersionStats> page_stats_;
  mutable std::mutex stats_mutex_;
  uint32_t max_neighbors_ = 64;  // 默认最大邻居数
  std::atomic<uint64_t> total_samples_{0};
  std::atomic<bool> enabled_{true};
  
public:
  DispersionMonitor() = default;
  
  explicit DispersionMonitor(uint32_t max_neighbors) 
      : max_neighbors_(max_neighbors) {}
  
  void set_max_neighbors(uint32_t max_neighbors) {
    max_neighbors_ = max_neighbors;
  }
  
  void enable() { enabled_ = true; }
  void disable() { enabled_ = false; }
  bool is_enabled() const { return enabled_.load(); }
  
  /**
   * 更新页面的离散度统计
   * 
   * @param page_id 页面ID
   * @param dispersion 本次采样的离散度（跨页邻居数）
   */
  void update_dispersion(uint64_t page_id, uint32_t dispersion) {
    if (!enabled_.load()) return;
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    auto& stats = page_stats_[page_id];
    stats.total_dispersion += dispersion;
    stats.sample_count++;
    total_samples_++;
  }
  
  /**
   * 更新页面的节点计数
   */
  void update_node_count(uint64_t page_id, uint32_t valid_count, uint32_t deleted_count) {
    if (!enabled_.load()) return;
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    auto& stats = page_stats_[page_id];
    stats.valid_node_count = valid_count;
    stats.deleted_node_count = deleted_count;
  }
  
  /**
   * 获取页面的平均离散度
   */
  float get_avg_dispersion(uint64_t page_id) const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    auto it = page_stats_.find(page_id);
    if (it == page_stats_.end()) return 0.0f;
    return it->second.get_avg_dispersion();
  }
  
  /**
   * 获取页面的碎片化比例
   */
  float get_fragmentation_ratio(uint64_t page_id) const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    auto it = page_stats_.find(page_id);
    if (it == page_stats_.end()) return 0.0f;
    return it->second.get_fragmentation_ratio(max_neighbors_);
  }
  
  /**
   * 检测是否需要触发重组织
   */
  bool should_reorganize() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    if (page_stats_.empty()) return false;
    
    size_t fragmented_count = 0;
    for (const auto& [page_id, stats] : page_stats_) {
      if (stats.get_fragmentation_ratio(max_neighbors_) > kFragmentationThreshold) {
        fragmented_count++;
      }
    }
    
    return static_cast<float>(fragmented_count) / page_stats_.size() > kReorganizeTriggerRatio;
  }
  
  /**
   * 获取需要重组织的碎片页列表
   */
  std::vector<uint64_t> get_fragmented_pages() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    std::vector<uint64_t> result;
    
    for (const auto& [page_id, stats] : page_stats_) {
      if (stats.get_fragmentation_ratio(max_neighbors_) > kFragmentationThreshold) {
        result.push_back(page_id);
      }
    }
    
    return result;
  }
  
  /**
   * 获取按碎片化程度排序的页面列表
   */
  std::vector<std::pair<uint64_t, float>> get_pages_sorted_by_fragmentation() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    std::vector<std::pair<uint64_t, float>> result;
    
    for (const auto& [page_id, stats] : page_stats_) {
      result.emplace_back(page_id, stats.get_fragmentation_ratio(max_neighbors_));
    }
    
    std::sort(result.begin(), result.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });
    
    return result;
  }
  
  /**
   * 获取全局统计摘要
   */
  struct GlobalStats {
    size_t total_pages;
    size_t fragmented_pages;
    float avg_fragmentation;
    uint64_t total_samples;
  };
  
  GlobalStats get_global_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    GlobalStats stats{};
    stats.total_pages = page_stats_.size();
    stats.total_samples = total_samples_.load();
    
    if (page_stats_.empty()) return stats;
    
    float total_frag = 0.0f;
    for (const auto& [page_id, page_stats] : page_stats_) {
      float frag = page_stats.get_fragmentation_ratio(max_neighbors_);
      total_frag += frag;
      if (frag > kFragmentationThreshold) {
        stats.fragmented_pages++;
      }
    }
    stats.avg_fragmentation = total_frag / page_stats_.size();
    
    return stats;
  }
  
  /**
   * 保存统计数据到CSV文件
   */
  void save_to_csv(const std::string& filename) const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    std::ofstream ofs(filename);
    ofs << "page_id,avg_dispersion,fragmentation_ratio,sample_count,valid_nodes,deleted_nodes\n";
    
    for (const auto& [page_id, stats] : page_stats_) {
      ofs << page_id << ","
          << stats.get_avg_dispersion() << ","
          << stats.get_fragmentation_ratio(max_neighbors_) << ","
          << stats.sample_count.load() << ","
          << stats.valid_node_count.load() << ","
          << stats.deleted_node_count.load() << "\n";
    }
    ofs.close();
  }
  
  /**
   * 重置所有统计
   */
  void reset() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    page_stats_.clear();
    total_samples_ = 0;
  }
  
  /**
   * 清除指定页面的统计
   */
  void clear_page(uint64_t page_id) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    page_stats_.erase(page_id);
  }
};

}  // namespace pipeann
