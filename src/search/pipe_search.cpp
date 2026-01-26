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
    tsl::robin_map<unsigned, char *> id_buf_map;
    id_buf_map.reserve(l_search * 2);

    // DC-PDI: 使用robin_set跟踪已发送但未完成的节点ID
    tsl::robin_set<unsigned> on_flight_ids;
    on_flight_ids.reserve(beam_width * 2);

    // DC-PDI优化: 借鉴beam_search的k指针策略
    // k表示下一个需要检查的位置，避免重复扫描已处理的节点
    unsigned k = 0;

    // DC-PDI优化: 批量发送I/O请求，从k开始扫描 + num_seen限制
    // 核心优化：借鉴beam_search的双重限制策略，减少无效扫描
    auto send_batch_read_req = [&](uint32_t n) -> unsigned {
      if (n == 0)
        return 0;

      std::vector<std::pair<unsigned, Neighbor *>> to_send;
      to_send.reserve(n);

      // 从k开始扫描，使用num_seen限制扫描范围（beam_search策略）
      uint32_t marker = k;
      uint32_t num_seen = 0;

      // 关键优化：num_seen < beam_width限制扫描范围，避免扫描过多节点
      while (marker < cur_list_size && to_send.size() < n && num_seen < beam_width) {
        unsigned id = retset[marker].id;
        // 只计数未访问的节点
        if (!retset[marker].visited) {
          num_seen++;
          // 跳过已发送（在飞行中）或已读取的节点
          if (on_flight_ids.find(id) == on_flight_ids.end() && id_buf_map.find(id) == id_buf_map.end()) {
            to_send.push_back({marker, &retset[marker]});
            on_flight_ids.insert(id);
          }
        }
        ++marker;
      }

      if (to_send.empty())
        return 0;

      // 批量加锁
      std::vector<uint32_t> ids_to_lock;
      ids_to_lock.reserve(to_send.size());
      for (auto &[_, nbr] : to_send) {
        ids_to_lock.push_back(nbr->id);
      }
      std::sort(ids_to_lock.begin(), ids_to_lock.end());
#ifndef READ_ONLY_TESTS
      for (auto &id : ids_to_lock) {
        idx_lock_table.rdlock(id);
      }
#endif

      // 发送所有I/O请求
      for (auto &[_, nbr] : to_send) {
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

    // DC-PDI: 轮询I/O完成
    auto poll_all = [&]() -> unsigned {
      reader->poll_all(ctx);
      unsigned n_completed = 0;
      while (!on_flight_ios.empty() && on_flight_ios.front().finished()) {
        io_t &io = on_flight_ios.front();
        id_buf_map.insert(std::make_pair(io.nbr.id, offset_to_loc((char *) io.read_req->buf, io.loc)));
        on_flight_ids.erase(io.nbr.id);  // 从飞行集合中移除
#ifndef READ_ONLY_TESTS
        idx_lock_table.unlock(io.nbr.id);
#endif
        on_flight_ios.pop();
        ++n_completed;
      }
      return n_completed;
    };

    // DC-PDI优化: 处理已读取的节点，从k开始扫描
    // 返回<处理节点数, 最佳更新位置nk>，nk用于更新k指针
    auto calc_best_nodes = [&]() -> std::pair<unsigned, unsigned> {
      unsigned n_processed = 0;
      unsigned nk = cur_list_size;

      // 从k开始扫描，处理所有已读取但未访问的节点
      for (unsigned marker = k; marker < cur_list_size; ++marker) {
        if (!retset[marker].visited) {
          auto it = id_buf_map.find(retset[marker].id);
          if (it != id_buf_map.end()) {
            retset[marker].visited = true;
            compute_exact_dists_and_push(it->second, it->first);
            ++n_processed;

            // 处理邻居，nk记录最佳插入位置
            unsigned local_nk = cur_list_size;
            compute_and_push_nbrs(it->second, local_nk, it->first);
            if (local_nk < nk) {
              nk = local_nk;
            }
          }
        }
      }

      return std::make_pair(n_processed, nk);
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
    // 初始发送I/O请求
    send_batch_read_req(beam_width);

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

    // DC-PDI优化: 主循环 - 借鉴beam_search的k指针策略
    // 终止条件：k >= cur_list_size 且所有飞行I/O都完成
    while (k < cur_list_size || !on_flight_ios.empty()) {
      // 1. 轮询已完成的I/O
      unsigned n_completed = poll_all();

      // 2. 处理所有已读取的节点
      auto [n_processed, nk] = calc_best_nodes();

      // 3. 更新k指针（修复：与beam_search保持一致的逻辑）
      // 关键修复：不论是否处理了节点，都需要更新k
      if (nk <= k) {
        k = nk;  // 发现更好的节点，回退k
      } else if (n_processed > 0) {
        // 推进k：跳过已访问的节点
        while (k < cur_list_size && retset[k].visited) {
          ++k;
        }
      }

      // 4. 发送新的I/O请求（如果有空位）
      unsigned n_sent = 0;
      if (on_flight_ios.size() < beam_width) {
        n_sent = send_batch_read_req(beam_width - on_flight_ios.size());
      }

      // 5. 如果没有任何进度：
      //    - 有飞行I/O则等待完成
      //    - 无飞行I/O则直接退出，避免死循环
      if (n_completed == 0 && n_processed == 0 && n_sent == 0) {
        if (!on_flight_ios.empty()) {
          reader->poll_wait(ctx);
        } else {
          break;
        }
      }

      // 6. 终止条件：k到达末尾且无飞行I/O
      if (k >= cur_list_size && on_flight_ios.empty()) {
        break;
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
