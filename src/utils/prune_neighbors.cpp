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
   * DC-PDI: 块感知邻居剪枝（论文4.2节）
   * 
   * 对跨页边应用惩罚系数，优先保留页内边
   * d'(v_new, u) = d(v_new, u) * β  if Page(u) ≠ Page(v_new)
   *              = d(v_new, u)      otherwise
   */
  template<typename T, typename TagT>
  void SSDIndex<T, TagT>::prune_neighbors_block_aware(
      const tsl::robin_map<uint32_t, T*>& coord_map,
      std::vector<Neighbor>& pool,
      std::vector<uint32_t>& pruned_list,
      uint64_t target_page) {
    
    if (pool.empty()) return;
    
    // DC-PDI优化: 使用更温和的惩罚系数，减少对距离分布的扰动
    // 原来1.5太激进，导致过度偏向页内边，反而损害图质量
    constexpr float kCrossPagePenalty = 1.15f;  // 降低惩罚系数
    
    // DC-PDI优化: 使用就地修改避免额外内存分配
    // 通过标记位而非保存完整距离数组来标识跨页边
    uint64_t cross_page_mask = 0;  // 使用位图标记（假设pool.size() < 64）
    const size_t use_bitmap = pool.size() <= 64;
    
    // 应用块感知惩罚调整距离
    if (use_bitmap) {
      for (size_t i = 0; i < pool.size(); i++) {
        uint64_t nbr_page = node_sector_no(pool[i].id);
        if (nbr_page != target_page) {
          cross_page_mask |= (1ULL << i);
          pool[i].distance *= kCrossPagePenalty;
        }
      }
    } else {
      // 大pool使用简化版: 不恢复距离
      for (size_t i = 0; i < pool.size(); i++) {
        uint64_t nbr_page = node_sector_no(pool[i].id);
        if (nbr_page != target_page) {
          pool[i].distance *= kCrossPagePenalty;
        }
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
    
    // DC-PDI优化: 仅在使用位图时恢复距离
    if (use_bitmap && cross_page_mask != 0) {
      constexpr float kInversePenalty = 1.0f / kCrossPagePenalty;
      for (size_t i = 0; i < pool.size(); i++) {
        if (cross_page_mask & (1ULL << i)) {
          pool[i].distance *= kInversePenalty;
        }
      }
    }
  }

  /**
   * DC-PDI: 块感知PQ邻居剪枝（论文4.2节）
   * 优化: 减少内存分配，使用更温和的惩罚系数
   */
  template<typename T, typename TagT>
  void SSDIndex<T, TagT>::prune_neighbors_pq_block_aware(
      std::vector<Neighbor>& pool,
      std::vector<uint32_t>& pruned_list,
      uint8_t* scratch,
      uint64_t target_page) {
    
    if (pool.empty()) return;
    
    // DC-PDI优化: 使用更温和的惩罚系数
    constexpr float kCrossPagePenalty = 1.15f;
    
    // DC-PDI优化: 使用位图避免额外内存分配
    uint64_t cross_page_mask = 0;
    const bool use_bitmap = pool.size() <= 64;
    
    if (use_bitmap) {
      for (size_t i = 0; i < pool.size(); i++) {
        uint64_t nbr_page = node_sector_no(pool[i].id);
        if (nbr_page != target_page) {
          cross_page_mask |= (1ULL << i);
          pool[i].distance *= kCrossPagePenalty;
        }
      }
    } else {
      for (size_t i = 0; i < pool.size(); i++) {
        uint64_t nbr_page = node_sector_no(pool[i].id);
        if (nbr_page != target_page) {
          pool[i].distance *= kCrossPagePenalty;
        }
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
    
    // DC-PDI优化: 仅在使用位图时恢复距离
    if (use_bitmap && cross_page_mask != 0) {
      constexpr float kInversePenalty = 1.0f / kCrossPagePenalty;
      for (size_t i = 0; i < pool.size(); i++) {
        if (cross_page_mask & (1ULL << i)) {
          pool[i].distance *= kInversePenalty;
        }
      }
    }
  }

  template class SSDIndex<float>;
  template class SSDIndex<int8_t>;
  template class SSDIndex<uint8_t>;
}  // namespace pipeann
