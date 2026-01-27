#include "neighbor.h"
#include "utils/timer.h"
#include "utils/tsl/robin_set.h"
#include "utils/tsl/robin_map.h"
#include "utils.h"
#include "v2/dynamic_index.h"
#include <csignal>
#include <cstdint>
#include <mutex>
#include <vector>
#include <future>
#include <thread>

#include <algorithm>
#include <filesystem>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <omp.h>
#include <shared_mutex>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <gperftools/malloc_extension.h>

#include "aux_utils.h"
#include "ssd_index.h"
#include "nbr/pq_nbr.h"
#include "nbr/rabitq_nbr.h"

#include "linux_aligned_file_reader.h"

namespace pipeann {
  namespace {
    inline void update_bin_header(const std::string &path, int npts, int dim) {
      std::fstream io(path, std::ios::in | std::ios::out | std::ios::binary);
      io.write(reinterpret_cast<const char *>(&npts), sizeof(int));
      io.write(reinterpret_cast<const char *>(&dim), sizeof(int));
      io.close();
    }

    template<typename T>
    std::unique_ptr<pipeann::AbstractNeighbor<T>> create_neighbor_handler(
        const pipeann::AbstractNeighbor<T> *existing) {
      if (dynamic_cast<const pipeann::RaBitQNeighbor<T> *>(existing) != nullptr) {
        return std::unique_ptr<pipeann::AbstractNeighbor<T>>(new pipeann::RaBitQNeighbor<T>());
      }
      return std::unique_ptr<pipeann::AbstractNeighbor<T>>(new pipeann::PQNeighbor<T>());
    }
  }  // namespace
  template<typename T, typename TagT>
  DynamicSSDIndex<T, TagT>::DynamicSSDIndex(Parameters &parameters, const std::string disk_prefix_in,
                                            const std::string disk_prefix_out, Distance<T> *dist,
                                            pipeann::Metric dist_metric, int search_mode, bool use_mem_index,
                                            bool use_buffered_updates, size_t buffer_max_points,
                                            uint32_t buffer_search_L, uint32_t build_ram_gb) {
    // check if file exists.
    if (!std::filesystem::exists(disk_prefix_in + "_disk.index")) {
      LOG(ERROR) << "Disk index file does not exist: " << disk_prefix_in << "_disk.index";
      exit(-1);
    }
    if (use_mem_index && !std::filesystem::exists(disk_prefix_in + "_mem.index")) {
      LOG(ERROR) << "In-memory index file does not exist: " << disk_prefix_in << "_mem.index";
      exit(-1);
    }

    this->active_del[0] = true;
    this->active_del[1] = false;
    this->_dist_metric = dist_metric;
    this->journal = new v2::Journal<TagT>(disk_prefix_out + "_journal");

    _paras_disk = parameters;
    _num_threads = parameters.num_threads;
    _beamwidth = parameters.beam_width;

    _disk_index_prefix_in = disk_prefix_in;
    _disk_index_prefix_out = disk_prefix_out;
    _dist_comp = dist;

    reader.reset(new LinuxAlignedFileReader());
    AbstractNeighbor<T> *nbr_handler = new PQNeighbor<T>();
    _disk_index = new pipeann::SSDIndex<T, TagT>(this->_dist_metric, reader, nbr_handler, true, &_paras_disk);

#ifndef NO_POLLUTE_ORIGINAL
    std::string disk_index_prefix_shadow = _disk_index_prefix_in + "_shadow";
    _disk_index->copy_index(_disk_index_prefix_in, disk_index_prefix_shadow);
    LOG(INFO) << "Copy disk index file to " << disk_index_prefix_shadow << "_disk.index";
    _disk_index_prefix_in = disk_index_prefix_shadow;
#endif

    if (search_mode == BEAM_SEARCH || search_mode == PAGE_SEARCH || search_mode == PIPE_SEARCH) {
      this->search_mode = search_mode;
      // DC-PDI优化控制：将搜索模式传递给底层SSDIndex
      // 这样SSDIndex可以根据搜索模式决定是否使用DC-PDI特有的优化
      _disk_index->set_search_mode(search_mode);
    } else {
      LOG(ERROR) << "Invalid search mode: " << search_mode
                 << ". Must be one of BEAM_SEARCH, PAGE_SEARCH, or PIPE_SEARCH.";
      exit(-1);
    }
    bool use_page_search = (search_mode == PAGE_SEARCH);
    _use_page_search = use_page_search;
    int res = _disk_index->load(_disk_index_prefix_in.c_str(), _num_threads, true, use_page_search);
    if (res != 0) {
      LOG(INFO) << "Failed to load disk index in DynamicSSDIndex constructor";
      exit(-1);
    }

    this->_use_mem_index = use_mem_index;
    if (use_mem_index) {
      std::string mem_index_path = disk_prefix_in + "_mem.index";  // use the original one.
      LOG(INFO) << "Use static in-memory index for acceleration, path: " << mem_index_path;
      _disk_index->load_mem_index(this->_dist_metric, _disk_index->data_dim, mem_index_path);
    }

    _dim = _disk_index->data_dim;
    _use_buffered_updates = use_buffered_updates;
    if (_use_buffered_updates) {
      _buffer_params = parameters;
      _buffer_aligned_dim = ROUND_UP(_dim, 8);
      _build_ram_gb = build_ram_gb > 0 ? build_ram_gb : 32;

      if (buffer_max_points > 0) {
        _buffer_max_points = buffer_max_points;
      } else {
        size_t base_points = _disk_index->num_points;
        size_t default_points = std::max<size_t>(100000, base_points / 100);
        _buffer_max_points = std::min<size_t>(default_points, 5000000);
      }

      if (buffer_search_L > 0) {
        _buffer_search_L = buffer_search_L;
      } else {
        _buffer_search_L = std::max<uint32_t>(_buffer_params.R + 16, _buffer_params.L);
      }

      _buffer.reset(new pipeann::Index<T, TagT>(_dist_metric, _dim, _buffer_max_points, true, false, true));
      _buffer->enable_delete();
    }
  }

  template<typename T, typename TagT>
  DynamicSSDIndex<T, TagT>::~DynamicSSDIndex() {
    wait_merge();
    if (_disk_index != nullptr) {
      delete _disk_index;
      _disk_index = nullptr;
    }
    if (journal != nullptr) {
      delete journal;
      journal = nullptr;
    }
  }

  template<typename T, typename TagT>
  bool DynamicSSDIndex<T, TagT>::is_reorganizing() const {
    if (_merge_in_progress.load(std::memory_order_relaxed)) {
      return true;
    }
#ifdef ENABLE_DISPERSION_MONITOR
    if (_disk_index != nullptr) {
      return _disk_index->is_reorganizing();
    }
#endif
    return false;
  }

  template<typename T, typename TagT>
  void DynamicSSDIndex<T, TagT>::checkpoint() {
    // TODO(gh): checkpoint the index.
    journal->checkpoint();
  }

  template<typename T, typename TagT>
  int DynamicSSDIndex<T, TagT>::insert(const T *point, const TagT &tag) {
    if (!_use_buffered_updates) {
      std::shared_lock<std::shared_timed_mutex> lock(_merge_lock);  // prevent merge during insert
      journal->append(v2::TxType::kInsert, tag);
      auto *deletion_set = &deletion_sets[active_delete_set];
      return _disk_index->insert_in_place(point, tag, deletion_set);
    }

    int ret = -1;
    {
      std::shared_lock<std::shared_timed_mutex> lock(_merge_lock);
      journal->append(v2::TxType::kInsert, tag);
      ret = _buffer->insert_point(point, _buffer_params, tag);
      if (ret == 0) {
        _buffer_live.fetch_add(1, std::memory_order_relaxed);
      }
    }

    if (_buffer_max_points > 0 && _buffer_live.load(std::memory_order_relaxed) >= _buffer_max_points) {
      request_merge_async(_num_threads);
    }
    return ret;
  }

  template<typename T, typename TagT>
  void DynamicSSDIndex<T, TagT>::search(const T *query, const uint64_t K, const uint32_t mem_L, const uint64_t search_L,
                                        const uint32_t beam_width, TagT *tags, float *distances, QueryStats *stats,
                                        bool dyn_search_l) {
    // Acquire shared lock on _merge_lock to prevent starvation of merge operations
    // Without this, continuous search operations could starve the merge_lock.lock() in merge_deletes
    std::shared_lock<std::shared_timed_mutex> merge_lk(_merge_lock);

    if (!_use_buffered_updates) {
      std::vector<TagT> result_tags(4096);
      std::vector<float> result_distances(4096);
      auto *deletion_set = &deletion_sets[active_delete_set];
      size_t n = 0;
      if (search_mode == BEAM_SEARCH) {
        n = _disk_index->beam_search(query, search_L, mem_L, search_L, result_tags.data(), result_distances.data(),
                                     beam_width, stats, deletion_set, dyn_search_l);
      } else if (search_mode == PAGE_SEARCH) {
        n = _disk_index->page_search(query, search_L, mem_L, search_L, result_tags.data(), result_distances.data(),
                                     beam_width, stats);
      } else if (search_mode == PIPE_SEARCH) {
        n = _disk_index->pipe_search(query, search_L, mem_L, search_L, result_tags.data(), result_distances.data(),
                                     beam_width, stats);
      }
      std::vector<NeighborTag<TagT>> best_vec;
      best_vec.reserve(n);
      for (size_t i = 0; i < n; i++) {
        best_vec.emplace_back(result_tags[i], result_distances[i]);
      }
      std::shared_lock<std::shared_timed_mutex> lock(delete_lock);
      size_t pos = 0;

      for (auto iter : best_vec) {
        if (deletion_set->find(iter.tag) == deletion_set->end()) {
          tags[pos] = iter.tag;
          distances[pos] = iter.dist;
          pos++;
        }
        if (pos == K) {
          return;
        }
      }
      return;
    }

    std::vector<TagT> base_tags(4096);
    std::vector<float> base_dists(4096);
    auto *deletion_set = &deletion_sets[active_delete_set];
    size_t base_n = 0;
    if (search_mode == BEAM_SEARCH) {
      base_n = _disk_index->beam_search(query, search_L, mem_L, search_L, base_tags.data(), base_dists.data(),
                                        beam_width, stats, deletion_set, dyn_search_l);
    } else if (search_mode == PAGE_SEARCH) {
      base_n = _disk_index->page_search(query, search_L, mem_L, search_L, base_tags.data(), base_dists.data(),
                                        beam_width, stats);
    } else if (search_mode == PIPE_SEARCH) {
      base_n = _disk_index->pipe_search(query, search_L, mem_L, search_L, base_tags.data(), base_dists.data(),
                                        beam_width, stats);
    }

    std::vector<TagT> buffer_tags;
    std::vector<float> buffer_dists;
    size_t buffer_n = 0;
    if (_buffer_live.load(std::memory_order_relaxed) > 0) {
      buffer_tags.resize(K);
      buffer_dists.resize(K);
      buffer_n = _buffer->search_with_tags(query, K, _buffer_search_L, buffer_tags.data(), buffer_dists.data());
    }

    std::vector<TagT> pending_tags;
    std::vector<float> pending_dists;
    size_t pending_n = 0;
    auto pending_buffer = _buffer_pending;
    if (pending_buffer != nullptr) {
      pending_tags.resize(K);
      pending_dists.resize(K);
      pending_n =
          pending_buffer->search_with_tags(query, K, _buffer_search_L, pending_tags.data(), pending_dists.data());
    }

    tsl::robin_map<TagT, float> best_map;
    best_map.reserve(base_n + buffer_n + pending_n);

    auto update_best = [&](TagT tag, float dist) {
      auto it = best_map.find(tag);
      if (it == best_map.end() || dist < it->second) {
        best_map.insert_or_assign(tag, dist);
      }
    };

    {
      std::shared_lock<std::shared_timed_mutex> lock(delete_lock);
      for (size_t i = 0; i < base_n; i++) {
        TagT tag = base_tags[i];
        if (deletion_set->find(tag) == deletion_set->end()) {
          update_best(tag, base_dists[i]);
        }
      }
      for (size_t i = 0; i < buffer_n; i++) {
        TagT tag = buffer_tags[i];
        if (deletion_set->find(tag) == deletion_set->end()) {
          update_best(tag, buffer_dists[i]);
        }
      }
      for (size_t i = 0; i < pending_n; i++) {
        TagT tag = pending_tags[i];
        if (deletion_set->find(tag) == deletion_set->end()) {
          update_best(tag, pending_dists[i]);
        }
      }
    }

    std::vector<NeighborTag<TagT>> merged;
    merged.reserve(best_map.size());
    for (const auto &kv : best_map) {
      merged.emplace_back(kv.first, kv.second);
    }

    size_t out_n = std::min<size_t>(K, merged.size());
    if (out_n > 0) {
      std::partial_sort(merged.begin(), merged.begin() + out_n, merged.end());
    }
    for (size_t i = 0; i < out_n; i++) {
      tags[i] = merged[i].tag;
      distances[i] = merged[i].dist;
    }
  }

  template<typename T, typename TagT>
  void DynamicSSDIndex<T, TagT>::lazy_delete(const TagT &tag) {
    std::unique_lock<std::shared_timed_mutex> lock(delete_lock);
    journal->append(v2::TxType::kDelete, tag);

    if (active_del[active_delete_set].load() == false) {
      LOG(ERROR) << "Active deletion set indicated as _deletion_set_" << active_delete_set
                 << " but it cannot accept deletions";
    }
    if (deletion_sets[active_delete_set].find(tag) == deletion_sets[active_delete_set].end()) {
      deletion_sets[active_delete_set].insert(tag);
      deleted_tags[active_delete_set].push_back(tag);
    }

    if (_use_buffered_updates && _buffer != nullptr) {
      _buffer->lazy_delete(tag);
    }
  }

  template<typename T, typename TagT>
  void DynamicSSDIndex<T, TagT>::save_del_set() {
    int nxt_idx = 1 - active_delete_set, cur_idx = active_delete_set;
    std::unique_lock<std::shared_timed_mutex> lock(delete_lock);
    deletion_sets[nxt_idx].clear();
    deleted_tags[nxt_idx].clear();
    bool expected_active = false;
    if (active_del[nxt_idx].compare_exchange_strong(expected_active, true)) {
      LOG(INFO) << "Cleared deletion set " << nxt_idx << " - ready to accept new points";
    } else {
      LOG(INFO) << "Failed to clear deletion set " << nxt_idx;
    }
    active_delete_set = nxt_idx;
    active_del[cur_idx].store(false);
  }

  template<typename T, typename TagT>
  void DynamicSSDIndex<T, TagT>::request_merge_async(const uint32_t &nthreads) {
    if (!_use_buffered_updates) {
      return;
    }

    bool expected = false;
    if (!_merge_in_progress.compare_exchange_strong(expected, true)) {
      return;
    }

    {
      std::lock_guard<std::mutex> guard(_merge_thread_mu);
      if (_merge_future.valid()) {
        if (_merge_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
          _merge_in_progress.store(false, std::memory_order_relaxed);
          return;
        }
        _merge_future.get();
      }
    }

    uint32_t merge_threads = nthreads == 0 ? _num_threads : nthreads;
    std::shared_ptr<pipeann::Index<T, TagT>> pending_buffer;

    {
      std::unique_lock<std::shared_timed_mutex> lock(_merge_lock);
      if (_buffer_live.load(std::memory_order_relaxed) == 0) {
        _merge_in_progress.store(false, std::memory_order_relaxed);
        return;
      }

      save_del_set();

      pending_buffer = std::shared_ptr<pipeann::Index<T, TagT>>(std::move(_buffer));
      _buffer.reset(new pipeann::Index<T, TagT>(_dist_metric, _dim, _buffer_max_points, true, false, true));
      _buffer->enable_delete();
      _buffer_live.store(0, std::memory_order_relaxed);
      _buffer_pending = pending_buffer;
    }

    {
      std::lock_guard<std::mutex> guard(_merge_thread_mu);
      _merge_future = std::async(std::launch::async, [this, pending_buffer, merge_threads]() {
        (void) pending_buffer;
        this->rebuild_merge(merge_threads, std::numeric_limits<uint32_t>::max());
      });
    }
  }

  template<typename T, typename TagT>
  void DynamicSSDIndex<T, TagT>::wait_merge() {
    std::lock_guard<std::mutex> guard(_merge_thread_mu);
    if (_merge_future.valid()) {
      _merge_future.get();
    }
  }

  template<typename T, typename TagT>
  void DynamicSSDIndex<T, TagT>::final_merge(const uint32_t &nthreads, const uint32_t &n_sampled_nbrs) {
    if (_use_buffered_updates) {
      wait_merge();
      if (_buffer_live.load(std::memory_order_relaxed) > 0) {
        request_merge_async(nthreads == 0 ? _num_threads : nthreads);
        wait_merge();
        return;
      }
    }

    _merge_in_progress.store(true, std::memory_order_relaxed);
    std::unique_lock<std::shared_timed_mutex> lock(_merge_lock);  // only one merge at a time
    save_del_set();
    pipeann::Timer timer;
    merge(nthreads, n_sampled_nbrs);
    std::swap(_disk_index_prefix_in, _disk_index_prefix_out);
    _disk_index->reload(_disk_index_prefix_in.c_str(), _num_threads);

    LOG(INFO) << "Merge time : " << timer.elapsed() / 1000 << " ms";
    MallocExtension::instance()->ReleaseFreeMemory();
    _merge_in_progress.store(false, std::memory_order_relaxed);
  }

  template<typename T, typename TagT>
  void DynamicSSDIndex<T, TagT>::merge(const uint32_t &nthreads, const uint32_t &n_sampled_nbrs) {
    _disk_index->merge_deletes(_disk_index_prefix_in, _disk_index_prefix_out, deleted_tags[1 - active_delete_set],
                               deletion_sets[1 - active_delete_set], nthreads, n_sampled_nbrs);
  }

  template<typename T, typename TagT>
  void DynamicSSDIndex<T, TagT>::rebuild_merge(const uint32_t &nthreads, const uint32_t &n_sampled_nbrs) {
    (void) n_sampled_nbrs;
    uint32_t merge_threads = nthreads == 0 ? _num_threads : nthreads;
    const auto &deleted_set = deletion_sets[1 - active_delete_set];
    auto pending_buffer = _buffer_pending;

    std::string tmp_prefix = _disk_index_prefix_out + "_fresh";
    std::string tmp_data = tmp_prefix + ".bin";
    std::string tmp_tags = tmp_prefix + ".tags.bin";

    if (std::filesystem::exists(tmp_data)) {
      std::filesystem::remove(tmp_data);
    }
    if (std::filesystem::exists(tmp_tags)) {
      std::filesystem::remove(tmp_tags);
    }

    _disk_index->export_live_points(tmp_data, tmp_tags, deleted_set, merge_threads);

    size_t total_points = 0;
    size_t base_points = 0;
    size_t base_dim = 0;
    pipeann::get_bin_metadata(tmp_data, base_points, base_dim);
    total_points = base_points;

    if (pending_buffer != nullptr) {
      std::ofstream data_writer(tmp_data, std::ios::binary | std::ios::app);
      std::ofstream tags_writer(tmp_tags, std::ios::binary | std::ios::app);

      tsl::robin_set<TagT> active_tags;
      pending_buffer->get_active_tags(active_tags);
      std::vector<T> buf_vec(_buffer_aligned_dim);
      for (auto tag : active_tags) {
        if (deleted_set.find(tag) != deleted_set.end()) {
          continue;
        }
        if (pending_buffer->get_vector_by_tag(tag, buf_vec.data()) != 0) {
          continue;
        }
        data_writer.write(reinterpret_cast<const char *>(buf_vec.data()), _dim * sizeof(T));
        tags_writer.write(reinterpret_cast<const char *>(&tag), sizeof(TagT));
        total_points++;
      }
      data_writer.close();
      tags_writer.close();
    }

    update_bin_header(tmp_data, static_cast<int>(total_points), static_cast<int>(_dim));
    update_bin_header(tmp_tags, static_cast<int>(total_points), 1);

    pipeann::Metric build_metric = _dist_metric;
    if (_disk_index->is_data_normalized()) {
      build_metric = pipeann::Metric::L2;
    }

    uint32_t bytes_per_nbr = 32;
    std::string pq_file = _disk_index_prefix_in + "_pq_compressed.bin";
    if (std::filesystem::exists(pq_file)) {
      size_t pq_npts = 0;
      size_t pq_dim = 0;
      pipeann::get_bin_metadata(pq_file, pq_npts, pq_dim);
      if (pq_dim > 0) {
        bytes_per_nbr = static_cast<uint32_t>(pq_dim);
      }
    }

    auto build_nbr = create_neighbor_handler(_disk_index->nbr_handler);
    pipeann::build_disk_index<T, TagT>(tmp_data.c_str(), _disk_index_prefix_out.c_str(), _paras_disk.R, _paras_disk.L,
                                       _build_ram_gb, merge_threads, bytes_per_nbr, build_metric, tmp_tags.c_str(),
                                       build_nbr.get());

    auto load_nbr = create_neighbor_handler(_disk_index->nbr_handler);
    auto new_reader = std::shared_ptr<AlignedFileReader>(new LinuxAlignedFileReader());
    auto new_index = std::unique_ptr<SSDIndex<T, TagT>>(
        new SSDIndex<T, TagT>(this->_dist_metric, new_reader, load_nbr.release(), true, &_paras_disk));
    new_index->load(_disk_index_prefix_out.c_str(), merge_threads, true, _use_page_search);

    SSDIndex<T, TagT> *old_index = nullptr;
    {
      std::unique_lock<std::shared_timed_mutex> lock(_merge_lock);
      old_index = _disk_index;
      _disk_index = new_index.release();
      reader = new_reader;
      std::swap(_disk_index_prefix_in, _disk_index_prefix_out);
      _buffer_pending.reset();
    }

    if (old_index != nullptr) {
      delete old_index;
    }

    if (std::filesystem::exists(tmp_data)) {
      std::filesystem::remove(tmp_data);
    }
    if (std::filesystem::exists(tmp_tags)) {
      std::filesystem::remove(tmp_tags);
    }
    _merge_in_progress.store(false, std::memory_order_relaxed);
  }

  template<typename T, typename TagT>
  void DynamicSSDIndex<T, TagT>::maybe_trigger_merge() {
    request_merge_async(_num_threads);
  }

  template class DynamicSSDIndex<float>;
  template class DynamicSSDIndex<uint8_t>;
  template class DynamicSSDIndex<int8_t>;
}  // namespace pipeann
