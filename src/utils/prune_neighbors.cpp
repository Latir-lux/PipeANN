#include "aligned_file_reader.h"
#include "utils/libcuckoo/cuckoohash_map.hh"
#include "ssd_index.h"
#include <malloc.h>
#include <algorithm>
#include <filesystem>

#include <omp.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <tuple>
#include "utils/timer.h"
#include "utils/tsl/robin_map.h"
#include "utils.h"
#include "utils/page_cache.h"

#include <unistd.h>
#include <sys/syscall.h>
#include "linux_aligned_file_reader.h"

#ifdef ENABLE_BLOCK_AWARE_PRUNE
#include "utils/clustering.h"
#endif

namespace pipeann {
  template<typename T, typename TagT>
  void SSDIndex<T, TagT>::occlude_list(std::vector<Neighbor> &pool, const tsl::robin_map<uint32_t, T *> &coord_map,
                                       std::vector<Neighbor> &result, std::vector<float> &occlude_factor) {
    if (pool.empty())
      return;

    std::set<Neighbor> result_set;  // deduplication.
    float cur_alpha = 1;
    while (cur_alpha <= alpha && result_set.size() < range) {
      uint32_t start = 0;
      while (result_set.size() < range && (start) < pool.size() && start < maxc) {
        auto &p = pool[start];
        if (occlude_factor[start] > cur_alpha) {
          start++;
          continue;
        }
        occlude_factor[start] = std::numeric_limits<float>::max();
        result_set.insert(p);
        for (uint32_t t = start + 1; t < pool.size() && t < maxc; t++) {
          if (occlude_factor[t] > alpha)
            continue;
          auto iter_right = coord_map.find(p.id);
          auto iter_left = coord_map.find(pool[t].id);
          float djk = this->dist_cmp->compare(iter_left->second, iter_right->second, this->data_dim);
          occlude_factor[t] = (std::max)(occlude_factor[t], pool[t].distance / djk);
        }
        start++;
      }
      cur_alpha *= 1.2f;
    }
    for (auto &x : result_set) {
      result.push_back(x);
    }
  }

  template<typename T, typename TagT>
  void SSDIndex<T, TagT>::occlude_list_pq(std::vector<Neighbor> &pool, std::vector<Neighbor> &result,
                                          std::vector<float> &occlude_factor, uint8_t *scratch) {
    if (pool.empty())
      return;
    assert(std::is_sorted(pool.begin(), pool.end()));
    assert(!pool.empty());

    std::set<Neighbor> result_set;  // deduplication, and keep distance sorted.
    float cur_alpha = 1;
    while (cur_alpha <= alpha && result_set.size() < range) {
      uint32_t start = 0;
      while (result_set.size() < range && (start) < pool.size() && start < maxc) {
        auto &p = pool[start];
        if (occlude_factor[start] > cur_alpha) {
          start++;
          continue;
        }
        occlude_factor[start] = std::numeric_limits<float>::max();
        result_set.insert(p);
        // dynamic programming, if p (current) is included,
        // then D(t, p0) / D(t, p) should be updated.
        for (uint32_t t = start + 1; t < pool.size() && t < maxc; t++) {
          if (occlude_factor[t] > alpha)
            continue;
          // djk = dist(p.id, pool[t.id])
          float djk;
          nbr_handler->compute_dists(p.id, &(pool[t].id), 1, &djk, scratch);
          // LOG(INFO) << pool[t].distance << " " << djk << " " << alpha << " " << result_set.size();
          occlude_factor[t] = (std::max)(occlude_factor[t], pool[t].distance / djk);
        }
        start++;
      }
      cur_alpha *= 1.2f;
    }
    for (auto &x : result_set) {
      result.push_back(x);
    }
  }

  template<typename T, typename TagT>
  void SSDIndex<T, TagT>::prune_neighbors_pq(std::vector<Neighbor> &pool, std::vector<uint32_t> &pruned_list,
                                             uint8_t *scratch) {
    if (pool.size() == 0)
      return;

    // sort the pool based on distance to query

    std::vector<Neighbor> result;
    result.reserve(this->range);
    std::vector<float> occlude_factor(pool.size(), 0);

    occlude_list_pq(pool, result, occlude_factor, scratch);

    pruned_list.clear();
    assert(result.size() <= range);
    for (auto iter : result) {
      pruned_list.emplace_back(iter.id);
    }

    if (alpha > 1) {
      for (uint32_t i = 0; i < pool.size() && pruned_list.size() < range; i++) {
        if (std::find(pruned_list.begin(), pruned_list.end(), pool[i].id) == pruned_list.end())
          pruned_list.emplace_back(pool[i].id);
      }
    }
  }

  template<typename T, typename TagT>
  void SSDIndex<T, TagT>::delta_prune_neighbors_pq(std::vector<TriangleNeighbor> &pool,
                                                   std::vector<uint32_t> &pruned_list, uint8_t *scratch, int tgt_idx) {
    if (unlikely(pool.size() != this->range + 1)) {
      LOG(ERROR) << "Pool size " << pool.size() << " not equal to " << this->range + 1;
    }
    pruned_list.clear();
    float cur_alpha = alpha;
    int to_evict = -1;
    float tgt_nbr_dis = pool[tgt_idx].distance;
    // step 1: fast path
    // determine which to evict using triangular inequality.
    while (cur_alpha >= (1 - 1e-5) && to_evict == -1) {
      for (int i = (int) pool.size() - 1; i >= 0; --i) {
        if (i == tgt_idx) {
          continue;
        }
        if (pool[i].distance > tgt_nbr_dis) {
          // pool[i] -> nbr is the longest edge.
          if (pool[i].distance / pool[i].tgt_dis > cur_alpha) {
            to_evict = i;
            break;
          }
        } else {
          // tgt -> nbr is the longest edge.
          if (tgt_nbr_dis / pool[i].tgt_dis > cur_alpha) {
            to_evict = tgt_idx;
            break;
          }
        }
      }
      cur_alpha /= 1.2f;
    }

    auto finish = [&]() {
      for (int i = 0; i < (int) pool.size(); i++) {
        if (i == to_evict) {
          continue;
        }
        pruned_list.emplace_back(pool[i].id);
      }
    };

    if (to_evict != -1) {
      finish();
      return;
    }
    // The point to insert is with high quality.
    // Step 2: Seek one with low quality to evict, early stop.

    std::vector<uint32_t> ids(pool.size());
    for (uint32_t i = 0; i < pool.size(); i++) {
      ids[i] = pool[i].id;
    }
    std::vector<float> dists(pool.size());

    for (int start = 0; start < (int) pool.size(); ++start) {
      if (start == tgt_idx) {
        continue;
      }
      auto &p = pool[start];
      nbr_handler->compute_dists(p.id, ids.data() + start + 1, pool.size() - start - 1, dists.data() + start + 1,
                                 scratch);
      for (uint32_t t = start + 1; t < pool.size(); t++) {
        if (pool[t].distance / dists[t] > alpha) {
          to_evict = t;
          finish();
          return;
        }
      }
    }

    // Step 3: all the points satisfy alpha-RNG, evict the farthest.
    to_evict = pool.size() - 1;
    finish();
  }

  template<typename T, typename TagT>
  void SSDIndex<T, TagT>::prune_neighbors(const tsl::robin_map<uint32_t, T *> &coord_map, std::vector<Neighbor> &pool,
                                          std::vector<uint32_t> &pruned_list) {
    if (pool.size() == 0)
      return;

    // sort the pool based on distance to query
    std::sort(pool.begin(), pool.end());

    std::vector<Neighbor> result;
    result.reserve(range);
    std::vector<float> occlude_factor(pool.size(), 0);

    occlude_list(pool, coord_map, result, occlude_factor);

    pruned_list.clear();

    // SPACEV1B frequently inserts medoid, which can not be excluded by triangular ineq.
    size_t medoid_threshold = result.size() * 3 / 4;
    for (size_t i = 0; i < result.size(); ++i) {
      if (i > medoid_threshold && result[i].id == medoid) {
        continue;
      }
      pruned_list.emplace_back(result[i].id);
    }

    if (alpha > 1) {
      for (uint32_t i = 0; i < pool.size() && pruned_list.size() < range; i++) {
        if (std::find(pruned_list.begin(), pruned_list.end(), pool[i].id) == pruned_list.end()) {
          pruned_list.emplace_back(pool[i].id);
        }
      }
    }
  }

  /**
   * DC-PDI优化v4: 块感知邻居剪枝（论文4.2节）
   * 
   * 新策略：不修改距离，而是在相同距离时优先选择同页邻居
   * 原因：即使1.05的惩罚也会影响图质量，导致召回率下降
   * 
   * 优化策略：
   * 1. 保持原始距离不变，确保图质量
   * 2. 在occlude_list中使用自定义比较器，同距离时优先同页
   * 3. 作为最终的tie-breaker，对图质量影响最小
   */
  template<typename T, typename TagT>
  void SSDIndex<T, TagT>::prune_neighbors_block_aware(
      const tsl::robin_map<uint32_t, T*>& coord_map,
      std::vector<Neighbor>& pool,
      std::vector<uint32_t>& pruned_list,
      uint64_t target_page) {
    
    if (pool.empty()) return;
    
    // 首先对pool排序
    std::sort(pool.begin(), pool.end());
    
    // DC-PDI优化v4: 使用极小的epsilon作为tie-breaker
    // 只有在距离差异小于epsilon时才考虑页面局部性
    constexpr float kEpsilon = 1e-6f;
    
    // 对距离相近的候选进行重排序：同页优先
    // 使用稳定排序保持原有相对顺序
    const size_t pool_size = pool.size();
    const size_t process_count = std::min(pool_size, static_cast<size_t>(maxc));
    
    // DC-PDI优化v4: 使用lambda进行局部重排
    // 对于距离差异在epsilon内的连续段，将同页邻居排在前面
    for (size_t i = 0; i < process_count; ) {
      // 找到距离相近的段
      size_t j = i + 1;
      float base_dist = pool[i].distance;
      while (j < process_count && (pool[j].distance - base_dist) < kEpsilon * base_dist) {
        ++j;
      }
      
      // 如果段长度大于1，在段内将同页邻居移到前面
      if (j - i > 1) {
        // 使用stable_partition保持同页和跨页各自的相对顺序
        std::stable_partition(pool.begin() + i, pool.begin() + j, 
          [this, target_page](const Neighbor& nbr) {
            return node_sector_no(nbr.id) == target_page;
          });
      }
      i = j;
    }
    
    // 使用原有剪枝逻辑
    std::vector<Neighbor> result;
    result.reserve(range);
    std::vector<float> occlude_factor(pool.size(), 0);
    occlude_list(pool, coord_map, result, occlude_factor);
    
    pruned_list.clear();
    
    // 填充结果
    size_t medoid_threshold = result.size() * 3 / 4;
    for (size_t i = 0; i < result.size(); ++i) {
      if (i > medoid_threshold && result[i].id == medoid) {
        continue;
      }
      pruned_list.emplace_back(result[i].id);
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

  /**
   * DC-PDI: 块感知PQ邻居剪枝（论文4.2节）
   * 优化v2: 
   * - 降低惩罚系数到1.1
   * - 只处理前maxc个候选
   * - 移除不必要的距离恢复
   */
  template<typename T, typename TagT>
  void SSDIndex<T, TagT>::prune_neighbors_pq_block_aware(
      std::vector<Neighbor>& pool,
      std::vector<uint32_t>& pruned_list,
      uint8_t* scratch,
      uint64_t target_page) {
    
    if (pool.empty()) return;
    
    // DC-PDI优化v2: 降低惩罚系数
    constexpr float kCrossPagePenalty = 1.1f;
    
    const size_t pool_size = pool.size();
    
    // DC-PDI优化v2: 只处理前maxc个候选
    const size_t process_count = std::min(pool_size, static_cast<size_t>(maxc));
    
    for (size_t i = 0; i < process_count; i++) {
      uint64_t nbr_page = node_sector_no(pool[i].id);
      if (nbr_page != target_page) {
        pool[i].distance *= kCrossPagePenalty;
      }
    }
    
    // 重新排序
    std::sort(pool.begin(), pool.end());
    
    // 使用原有PQ剪枝逻辑
    std::vector<Neighbor> result;
    result.reserve(this->range);
    std::vector<float> occlude_factor(pool.size(), 0);
    
    occlude_list_pq(pool, result, occlude_factor, scratch);
    
    pruned_list.clear();
    assert(result.size() <= range);
    for (auto iter : result) {
      pruned_list.emplace_back(iter.id);
    }
    
    if (alpha > 1) {
      for (uint32_t i = 0; i < pool.size() && pruned_list.size() < range; i++) {
        if (std::find(pruned_list.begin(), pruned_list.end(), pool[i].id) == pruned_list.end()) {
          pruned_list.emplace_back(pool[i].id);
        }
      }
    }
    
    // DC-PDI优化v2: 不恢复距离，节省开销
  }

  template class SSDIndex<float>;
  template class SSDIndex<int8_t>;
  template class SSDIndex<uint8_t>;
}  // namespace pipeann
