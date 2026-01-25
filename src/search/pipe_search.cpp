#include "aligned_file_reader.h"
#include "utils/libcuckoo/cuckoohash_map.hh"
#include "neighbor.h"
#include "ssd_index.h"
#include <malloc.h>
#include <algorithm>
#ifndef USE_AIO
#include "liburing.h"
#endif

#include <omp.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include "utils/timer.h"
#include "utils/tsl/robin_set.h"
#include "utils/tsl/robin_map.h"
#include "utils.h"
#include "utils/page_cache.h"

#ifdef COLLECT_IO_STATS
#include "utils/io_stats.h"
#endif

#include <unistd.h>
#include <sys/syscall.h>

namespace pipeann {
  struct io_t {
    Neighbor nbr;
    unsigned page_id;
    unsigned loc;
    IORequest *read_req;
    bool operator>(const io_t &rhs) const {
      return nbr.distance > rhs.nbr.distance;
    }

    bool operator<(const io_t &rhs) const {
      return nbr.distance < rhs.nbr.distance;
    }

    bool finished() {
      return read_req->finished;
    }
  };

  template<typename T, typename TagT>
  size_t SSDIndex<T, TagT>::pipe_search(const T *query1, const uint64_t k_search, const uint32_t mem_L,
                                        const uint64_t l_search, TagT *res_tags, float *distances,
                                        const uint64_t beam_width, QueryStats *stats) {
    QueryBuffer<T> *query_buf = pop_query_buf(query1);
#ifdef USE_AIO
    void *ctx = reader->get_ctx();
#else
    void *ctx = reader->get_ctx(IORING_SETUP_SQPOLL);  // use SQ polling only for pipe search.
#endif

    if (beam_width > MAX_N_SECTOR_READS) {
      LOG(ERROR) << "Beamwidth can not be higher than MAX_N_SECTOR_READS";
      crash();
    }

    // copy query to thread specific aligned and allocated memory (for distance
    // calculations we need aligned data)
    const T *query = query_buf->aligned_query_T;

    // reset query
    query_buf->reset();

    // pointers to current vector for comparison
    T *data_buf = query_buf->coord_scratch;
    _mm_prefetch((char *) data_buf, _MM_HINT_T1);

    // sector scratch
    char *sector_scratch = query_buf->sector_scratch;
    float *dist_scratch = query_buf->aligned_dist_scratch;

    Timer query_timer;
    std::vector<Neighbor> retset(mem_L + this->range + l_search * 10);
    auto &visited = *(query_buf->visited);
    unsigned cur_list_size = 0;

    std::vector<Neighbor> full_retset;
    full_retset.reserve(l_search * 10);

#ifndef OVERLAP_INIT
    nbr_handler->initialize_query(query, query_buf);
#endif

    auto compute_exact_dists_and_push = [&](const char *node_buf, const unsigned id) -> float {
      T *node_fp_coords_copy = data_buf;
      memcpy(node_fp_coords_copy, node_buf, data_dim * sizeof(T));
      float cur_expanded_dist = dist_cmp->compare(query, node_fp_coords_copy, (unsigned) aligned_dim);
      full_retset.push_back(Neighbor(id, cur_expanded_dist, true));
      return cur_expanded_dist;
    };

    uint64_t n_computes = 0;
    // DC-PDI优化: 完全移除物理离散度采样，将统计开销降至零
    // 该功能仅用于离线分析，不应在搜索热路径中启用

    auto compute_and_push_nbrs = [&](const char *node_buf, unsigned &nk, unsigned current_node_id) {
      unsigned *node_nbrs = offset_to_node_nhood(node_buf);
      unsigned nnbrs = *(node_nbrs++);
      unsigned nbors_cand_size = 0;

      // DC-PDI优化: 完全禁用搜索路径上的物理离散度统计
      // 原因: id2loc调用成本过高，即使采样也会拖慢搜索
      // 物理离散度可以在离线批量分析时单独计算
      std::ignore = current_node_id;  // 避免unused warning

      for (unsigned m = 0; m < nnbrs; ++m) {
        if (visited.find(node_nbrs[m]) == visited.end()) {
          node_nbrs[nbors_cand_size++] = node_nbrs[m];
          visited.insert(node_nbrs[m]);
        }
      }

      n_computes += nbors_cand_size;
      if (nbors_cand_size) {
        // DC-PDI优化: 仅在需要统计时才获取时间戳，减少系统调用开销
        nbr_handler->compute_dists(query_buf, node_nbrs, nbors_cand_size);
        for (unsigned m = 0; m < nbors_cand_size; ++m) {
          const int nbor_id = node_nbrs[m];
          const float nbor_dist = dist_scratch[m];
          if (nbor_dist >= retset[cur_list_size - 1].distance && (cur_list_size == l_search))
            continue;
          Neighbor nn(nbor_id, nbor_dist, true);
          // Return position in sorted list where nn inserted
          auto r = InsertIntoPool(retset.data(), cur_list_size, nn);  // may be overflow in retset...
          if (cur_list_size < l_search) {
            ++cur_list_size;
            if (unlikely(cur_list_size >= retset.size())) {
              retset.resize(2 * cur_list_size);
            }
          }
          // nk logs the best position in the retset that was updated due to
          // neighbors of n.
          if (r < nk)
            nk = r;
        }
        if (stats != nullptr) {
          stats->n_cmps += nbors_cand_size;
        }
      }
    };

    auto add_to_retset = [&](const unsigned *node_ids, const uint64_t n_ids, float *dists) {
      for (uint64_t i = 0; i < n_ids; ++i) {
        retset[cur_list_size++] = Neighbor(node_ids[i], dists[i], true);
        visited.insert(node_ids[i]);
      }
    };

    // stats.
    if (stats != nullptr) {
      stats->io_us = 0;
      stats->io_us1 = 0;
      stats->cpu_us = 0;
      stats->cpu_us1 = 0;
      stats->cpu_us2 = 0;
      // 论文第5章实验新增统计
      stats->search_phase_us = 0;
      stats->prefetch_phase_us = 0;
      stats->compute_phase_us = 0;
      stats->bytes_read = 0;
      stats->effective_bytes = 0;
      // DC-PDI: 物理离散度统计初始化
      stats->physical_dispersion = 0;
      stats->page_local_edge_ratio = 0;
      stats->sampled_nodes = 0;
    }
    // search in in-memory index.

    std::vector<unsigned> mem_tags(mem_L);
    std::vector<float> mem_dists(mem_L);

#ifdef OVERLAP_INIT
    if (mem_L) {
      mem_index_->search_with_tags_fast(query, mem_L, mem_tags.data(), mem_dists.data());
      add_to_retset(mem_tags.data(), std::min((uint64_t) mem_L, l_search), mem_dists.data());
    } else {
      // cannot overlap.
      nbr_handler->initialize_query(query, query_buf);
      nbr_handler->compute_dists(query_buf, &medoid, 1);
      add_to_retset(&medoid, 1, dist_scratch);
    }
#else
    if (mem_L) {
      mem_index_->search_with_tags_fast(query, mem_L, mem_tags.data(), mem_dists.data());
      nbr_handler->compute_dists(query_buf, mem_tags.data(), mem_L);
      add_to_retset(mem_tags.data(), std::min((uint64_t) mem_L, l_search), dist_scratch);
    } else {
      nbr_handler->compute_dists(query_buf, &medoid, 1);
      add_to_retset(&medoid, 1, dist_scratch);
    }
    std::sort(retset.begin(), retset.begin() + cur_list_size);
#endif

    std::queue<io_t> on_flight_ios;
    
    // DC-PDI优化: 使用robin_map替代std::unordered_map，提高查找性能
    // 必须在lambda定义之前声明，因为lambda会捕获它
    tsl::robin_map<unsigned, char *> id_buf_map;
    id_buf_map.reserve(l_search * 2);  // 预分配空间减少rehash
    
    // DC-PDI优化: 使用k指针跟踪搜索进度（类似beam_search）
    // k表示retset中第一个可能未完成处理的位置
    unsigned k = 0;
    
    // DC-PDI优化: 使用on_flight_set快速判断节点是否在发送中
    tsl::robin_set<unsigned> on_flight_set;
    on_flight_set.reserve(beam_width * 2);
    
    // DC-PDI: 记录上一轮发送时的marker位置，用于模拟beam_search的num_seen限制
    unsigned last_send_marker = 0;

    // DC-PDI优化: 重构为批量发送I/O请求
    // 设计思路：从retset中按顺序扫描未访问节点，批量发送I/O请求
    // 使用last_send_marker记录上次扫描位置，避免重复扫描
    auto send_batch_read_req = [&](uint32_t n, bool new_round) -> unsigned {
      if (n == 0) return 0;
      
      // 如果是新一轮（k指针回退），重置marker从k开始
      unsigned marker = new_round ? k : last_send_marker;
      
      // 1. 从marker开始收集需要发送的节点
      std::vector<std::pair<unsigned, Neighbor*>> to_send;
      to_send.reserve(n);
      
      while (marker < cur_list_size && to_send.size() < n) {
        auto& node = retset[marker];
        // 只检查未访问的节点
        if (!node.visited) {
          // 检查节点: 不在飞行中 + 未在id_buf_map中
          if (on_flight_set.find(node.id) == on_flight_set.end() 
              && id_buf_map.find(node.id) == id_buf_map.end()) {
            to_send.push_back({marker, &node});
            on_flight_set.insert(node.id);
          }
        }
        ++marker;
      }
      
      last_send_marker = marker;  // 记录位置，下次继续
      
      if (to_send.empty()) return 0;
      
      // 2. 批量加锁（按ID排序避免死锁）
      std::vector<uint32_t> ids_to_lock;
      ids_to_lock.reserve(to_send.size());
      for (auto& [_, nbr] : to_send) {
        ids_to_lock.push_back(nbr->id);
      }
      std::sort(ids_to_lock.begin(), ids_to_lock.end());
#ifndef READ_ONLY_TESTS
      for (auto& id : ids_to_lock) {
        idx_lock_table.rdlock(id);
      }
#endif
      
      // 3. 发送所有I/O请求
      for (auto& [_, nbr] : to_send) {
        const unsigned loc = id2loc(nbr->id), pid = loc_sector_no(loc);
        
        uint64_t &cur_buf_idx = query_buf->sector_idx;
        auto buf = sector_scratch + cur_buf_idx * size_per_io;
        auto &req = query_buf->reqs[cur_buf_idx];
        req = IORequest(static_cast<uint64_t>(pid) * SECTOR_LEN, size_per_io, buf, u_loc_offset(loc), max_node_len,
                        sector_scratch);
        reader->send_read_no_alloc(req, ctx);
        
        on_flight_ios.push(io_t{*nbr, pid, loc, &req});
        cur_buf_idx = (cur_buf_idx + 1) % MAX_N_SECTOR_READS;
        
        if (stats != nullptr) {
          stats->n_ios++;
          stats->bytes_read += size_per_io;
          stats->effective_bytes += max_node_len;
        }
#ifdef COLLECT_IO_STATS
        global_io_stats.add_read(size_per_io, max_node_len);
#endif
      }
      
      return to_send.size();
    };

    auto poll_all = [&]() -> std::pair<int, int> {
      // poll once.
      reader->poll_all(ctx);
      unsigned n_in = 0, n_out = 0;
      while (!on_flight_ios.empty() && on_flight_ios.front().finished()) {
        io_t &io = on_flight_ios.front();
        id_buf_map.insert(std::make_pair(io.nbr.id, offset_to_loc((char *) io.read_req->buf, io.loc)));
        on_flight_set.erase(io.nbr.id);  // DC-PDI: 从飞行集合中移除
        io.nbr.distance <= retset[cur_list_size - 1].distance ? ++n_in : ++n_out;
        // unlock the corresponding page.
#ifndef READ_ONLY_TESTS
        idx_lock_table.unlock(io.nbr.id);
#endif
        on_flight_ios.pop();
      }
      return std::make_pair(n_in, n_out);
    };

    // DC-PDI优化: 使用k指针优化calc_best_node，避免从头遍历
    // 返回值: nk - 本轮更新的最佳位置，用于更新k指针
    auto calc_best_node = [&]() -> unsigned {
      unsigned nk = cur_list_size;
      
      // 从k开始查找第一个已读取但未访问的节点
      for (unsigned marker = k; marker < cur_list_size; ++marker) {
        if (!retset[marker].visited) {
          auto it = id_buf_map.find(retset[marker].id);
          if (it != id_buf_map.end()) {
            retset[marker].visited = true;
            compute_exact_dists_and_push(it->second, it->first);
            compute_and_push_nbrs(it->second, nk, it->first);
            // 只处理一个节点，让I/O有机会完成
            break;
          }
        }
      }
      
      return nk;
    };

    // DC-PDI优化: 检查是否收敛（所有节点都已访问或正在飞行中）
    auto is_converged = [&]() -> bool {
      // 从k开始检查，因为k之前的节点都已处理
      for (unsigned i = k; i < cur_list_size; ++i) {
        if (!retset[i].visited) {
          return false;  // 还有未访问的节点
        }
      }
      return true;
    };

    auto print_state = [&]() {
      LOG(INFO) << "cur_list_size: " << cur_list_size;
      for (unsigned i = 0; i < cur_list_size; ++i) {
        LOG(INFO) << "retset[" << i << "]: " << retset[i].id << ", " << retset[i].distance << ", " << retset[i].flag
                  << ", " << retset[i].visited << ", " << (id_buf_map.find(retset[i].id) != id_buf_map.end());
      }
      LOG(INFO) << "On flight IOs: " << on_flight_ios.size();
      if (on_flight_ios.size() != 0) {
        auto &io = on_flight_ios.front();
        LOG(INFO) << "on_flight_io: " << io.nbr.id << ", " << io.nbr.distance << ", " << io.nbr.flag << ", "
                  << io.page_id << ", " << io.loc << ", " << io.finished();
      }
      usleep(500);
    };

    std::ignore = print_state;

    auto cpu2_st = std::chrono::high_resolution_clock::now();
    // 初始发送I/O请求（第一轮，new_round=true）
    send_batch_read_req(beam_width, true);
    
#ifdef OVERLAP_INIT
    if (likely(mem_L != 0)) {
      nbr_handler->initialize_query(query, query_buf);
      nbr_handler->compute_dists(query_buf, mem_tags.data(), mem_L);
      for (unsigned i = 0; i < cur_list_size; ++i) {
        retset[i].distance = dist_scratch[i];
      }
      std::sort(retset.begin(), retset.begin() + cur_list_size);
    }
#endif

    // DC-PDI优化: 主循环模仿beam_search的逻辑
    // 每处理完一个节点后，检查是否需要开始新一轮
    bool need_new_round = false;
    while (k < cur_list_size || !on_flight_ios.empty()) {
      // 1. 轮询已完成的I/O
      poll_all();
      
      // 2. 处理已读取的节点
      unsigned nk = calc_best_node();
      
      // 3. 更新k指针（类似beam_search的收敛检测）
      if (nk <= k) {
        k = nk;  // 发现更好的节点，回退k
        need_new_round = true;  // 需要重新从k开始发送
        last_send_marker = k;
      } else {
        // 尝试推进k到下一个未访问的节点
        while (k < cur_list_size && retset[k].visited) {
          ++k;
        }
      }
      
      // 4. 发送新的I/O请求（如果有空位）
      unsigned sent = 0;
      if (on_flight_ios.size() < beam_width) {
        sent = send_batch_read_req(beam_width - on_flight_ios.size(), need_new_round);
        need_new_round = false;
      }
      
      // 5. 检查是否收敛或需要重新扫描
      if (on_flight_ios.empty()) {
        if (is_converged()) {
          break;  // 所有节点都已访问，搜索完成
        }
        // 还有未访问节点但没发送I/O，强制从k重新开始
        if (sent == 0) {
          need_new_round = true;
          last_send_marker = k;
        }
      }
    }
    
    auto cpu2_ed = std::chrono::high_resolution_clock::now();
    if (stats != nullptr) {
      stats->cpu_us2 = std::chrono::duration_cast<std::chrono::microseconds>(cpu2_ed - cpu2_st).count();
      stats->cpu_us = n_computes;
      stats->search_phase_us = stats->cpu_us2;  // 搜索阶段包含整个主循环
      // 计算I/O放大率和重叠率
      stats->calc_io_amplification();
      stats->calc_overlap_ratio();
    }
#ifdef COLLECT_IO_STATS
    global_io_stats.add_query();
#endif
    std::sort(full_retset.begin(), full_retset.end(),
              [](const Neighbor &left, const Neighbor &right) { return left < right; });

    // copy k_search values
    uint64_t t = 0;
    for (uint64_t i = 0; i < full_retset.size() && t < k_search; i++) {
      if (i > 0 && full_retset[i].id == full_retset[i - 1].id) {
        continue;  // deduplicate.
      }
      res_tags[t] = id2tag(full_retset[i].id);
      if (distances != nullptr) {
        distances[t] = full_retset[i].distance;
      }
      t++;
    }

    push_query_buf(query_buf);

    if (stats != nullptr) {
      stats->total_us = (double) query_timer.elapsed();
    }
    return t;
  }

  template class SSDIndex<float>;
  template class SSDIndex<int8_t>;
  template class SSDIndex<uint8_t>;
}  // namespace pipeann