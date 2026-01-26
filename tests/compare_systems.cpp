/**
 * compare_systems.cpp - 对比DC-PDI、IP-DiskANN和FreshDiskANN三个系统
 *
 * 系统配置:
 * - DC-PDI: PIPE_SEARCH + 动态聚类 + 原地更新
 * - IP-DiskANN: BEAM_SEARCH + 原地更新 (无聚类优化)
 * - FreshDiskANN: BEAM_SEARCH + 缓冲-合并策略
 */

#include "ssd_index.h"
#include "v2/dynamic_index.h"
#include "linux_aligned_file_reader.h"
#include "nbr/pq_nbr.h"

#include <index.h>
#include <algorithm>
#include <cstddef>
#include <future>
#include <numeric>
#include <omp.h>
#include <string.h>
#include <time.h>
#include "utils/timer.h"
#include <cstring>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <random>
#include <thread>
#include <atomic>
#include <chrono>
#include <limits>

#include "aux_utils.h"
#include "index.h"
#include "math_utils.h"
#include "partition.h"
#include "utils.h"

#include <filesystem>
#include <system_error>

// 系统类型枚举
enum SystemType {
  DC_PDI = 0,        // DC-PDI: Pipe search + 动态聚类
  IP_DISKANN = 1,    // IP-DiskANN: Beam search + 原地更新
  FRESH_DISKANN = 2  // FreshDiskANN: Beam search + 缓冲-合并
};

const char *system_names[] = {"DC-PDI", "IP-DiskANN", "FreshDiskANN"};

namespace {
  bool load_truthset_ivecs(const std::string &gt_file, size_t expected_queries, uint32_t *&ids, float *&dists,
                           size_t &npts, size_t &dim) {
    std::ifstream reader(gt_file, std::ios::binary | std::ios::ate);
    if (!reader.is_open()) {
      std::cerr << "Error: Unable to open ground truth file: " << gt_file << std::endl;
      return false;
    }

    const size_t file_size = static_cast<size_t>(reader.tellg());
    reader.seekg(0);

    int32_t k = 0;
    reader.read(reinterpret_cast<char *>(&k), sizeof(int32_t));
    if (!reader || k <= 0) {
      std::cerr << "Error: Invalid ivecs ground truth header in " << gt_file << std::endl;
      return false;
    }

    const size_t record_size = (static_cast<size_t>(k) + 1) * sizeof(int32_t);
    if (file_size % record_size != 0) {
      std::cerr << "Error: ivecs ground truth file size mismatch for " << gt_file << std::endl;
      return false;
    }

    npts = file_size / record_size;
    dim = static_cast<size_t>(k);

    if (expected_queries > 0 && npts != expected_queries) {
      std::cerr << "Error: Ground truth query count (" << npts << ") does not match expected queries ("
                << expected_queries << ") in " << gt_file << std::endl;
      return false;
    }

    ids = new uint32_t[npts * dim];
    dists = nullptr;

    reader.seekg(0);
    for (size_t i = 0; i < npts; i++) {
      int32_t row_dim = 0;
      reader.read(reinterpret_cast<char *>(&row_dim), sizeof(int32_t));
      if (!reader || row_dim != k) {
        std::cerr << "Error: ivecs row dimension mismatch at row " << i << " in " << gt_file << std::endl;
        delete[] ids;
        ids = nullptr;
        return false;
      }
      reader.read(reinterpret_cast<char *>(ids + i * dim), dim * sizeof(uint32_t));
      if (!reader) {
        std::cerr << "Error: Failed reading ivecs data at row " << i << " in " << gt_file << std::endl;
        delete[] ids;
        ids = nullptr;
        return false;
      }
    }

    return true;
  }

  bool load_truthset_auto(const std::string &gt_file, size_t expected_queries, uint32_t *&ids, float *&dists,
                          size_t &npts, size_t &dim) {
    const size_t actual_file_size = get_file_size(gt_file);

    auto safe_mul = [](size_t a, size_t b, size_t &out) -> bool {
      if (a == 0 || b == 0) {
        out = 0;
        return true;
      }
      if (a > std::numeric_limits<size_t>::max() / b) {
        return false;
      }
      out = a * b;
      return true;
    };

    std::ifstream reader(gt_file, std::ios::binary);
    if (!reader.is_open()) {
      std::cerr << "Error: Unable to open ground truth file: " << gt_file << std::endl;
      return false;
    }

    int npts_i32 = 0;
    int dim_i32 = 0;
    reader.read(reinterpret_cast<char *>(&npts_i32), sizeof(int));
    reader.read(reinterpret_cast<char *>(&dim_i32), sizeof(int));
    if (!reader) {
      std::cerr << "Error: Failed reading ground truth header from " << gt_file << std::endl;
      return false;
    }

    if (npts_i32 <= 0 || dim_i32 <= 0) {
      return load_truthset_ivecs(gt_file, expected_queries, ids, dists, npts, dim);
    }

    const size_t npts_candidate = static_cast<size_t>(static_cast<unsigned>(npts_i32));
    const size_t dim_candidate = static_cast<size_t>(static_cast<unsigned>(dim_i32));
    size_t npts_dim = 0;
    if (!safe_mul(npts_candidate, dim_candidate, npts_dim)) {
      return load_truthset_ivecs(gt_file, expected_queries, ids, dists, npts, dim);
    }

    size_t expected_ids_only = 0;
    size_t expected_with_dists = 0;
    size_t expected_with_tags = 0;
    if (!safe_mul(npts_dim, sizeof(uint32_t), expected_ids_only)) {
      return load_truthset_ivecs(gt_file, expected_queries, ids, dists, npts, dim);
    }
    if (!safe_mul(npts_dim, sizeof(uint32_t) * 2, expected_with_dists)) {
      return load_truthset_ivecs(gt_file, expected_queries, ids, dists, npts, dim);
    }
    expected_ids_only += 2 * sizeof(uint32_t);
    expected_with_dists += 2 * sizeof(uint32_t);

    size_t npts_dim_times_three = 0;
    if (safe_mul(npts_dim, 3, npts_dim_times_three) &&
        safe_mul(npts_dim_times_three, sizeof(uint32_t), expected_with_tags)) {
      expected_with_tags += 2 * sizeof(uint32_t);
    }

    if (actual_file_size == expected_with_dists || actual_file_size == expected_ids_only ||
        actual_file_size == expected_with_tags) {
      uint32_t *tags = nullptr;
      pipeann::load_truthset(gt_file, ids, dists, npts, dim, actual_file_size == expected_with_tags ? &tags : nullptr);
      delete[] tags;
      return true;
    }

    return load_truthset_ivecs(gt_file, expected_queries, ids, dists, npts, dim);
  }
}  // namespace

// 全局配置
const int NUM_SEARCH_THREADS = 32;
const int NUM_INSERT_THREADS = 10;
const int NUM_DELETE_THREADS = 1;
const int MERGE_INTERVAL = 10000;  // FreshDiskANN每10000次更新合并一次

// 获取内存使用
void get_memory_usage(double &rss_kb, double &vm_kb) {
  int tSize = 0, resident = 0, share = 0;
  std::ifstream buffer("/proc/self/statm");
  buffer >> tSize >> resident >> share;
  buffer.close();
  long page_size_kb = sysconf(_SC_PAGE_SIZE) / 1024;
  rss_kb = resident * page_size_kb;
  vm_kb = tSize * page_size_kb;
}

uint64_t get_directory_size(const std::filesystem::path &dir_path) {
  std::error_code ec;
  uint64_t total = 0;
  for (auto const &entry : std::filesystem::recursive_directory_iterator(dir_path, ec)) {
    if (ec) {
      return total;
    }
    if (entry.is_regular_file(ec)) {
      total += entry.file_size(ec);
    }
  }
  return total;
}

uint64_t get_disk_usage_bytes(const std::string &index_prefix) {
  std::filesystem::path prefix_path(index_prefix);
  std::filesystem::path dir = prefix_path.parent_path();
  std::string prefix_name = prefix_path.filename().string();

  if (dir.empty()) {
    dir = ".";
  }
  if (!std::filesystem::exists(dir)) {
    return 0;
  }

  uint64_t total = 0;
  std::error_code ec;
  for (auto const &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      break;
    }
    const std::string name = entry.path().filename().string();
    if (name.rfind(prefix_name, 0) != 0) {
      continue;
    }
    if (entry.is_regular_file(ec)) {
      total += entry.file_size(ec);
    } else if (entry.is_directory(ec)) {
      total += get_directory_size(entry.path());
    }
  }
  return total;
}

/**
 * 实验1: 搜索延迟对比 (对应5.2.1节)
 * 测试三个系统在相同更新负载后的搜索性能
 */
template<typename T, typename TagT = uint32_t>
void compare_search_latency(const std::string &index_prefix, const std::string &query_file, const std::string &gt_file,
                            size_t num_threads, SystemType system_type, uint64_t recall_at,
                            const std::vector<uint64_t> &L_values, uint32_t beam_width,
                            const std::string &output_file) {
  // 验证文件存在性
  std::string disk_index_file = index_prefix + "_disk.index";
  std::string pq_compressed_file = index_prefix + "_pq_compressed.bin";
  std::string pq_pivots_file = index_prefix + "_pq_pivots.bin";

  if (!std::filesystem::exists(query_file)) {
    std::cerr << "Error: Query file not found: " << query_file << std::endl;
    return;
  }
  if (!std::filesystem::exists(gt_file)) {
    std::cerr << "Error: Ground truth file not found: " << gt_file << std::endl;
    return;
  }
  if (!std::filesystem::exists(disk_index_file)) {
    std::cerr << "Error: Disk index file not found: " << disk_index_file << std::endl;
    return;
  }
  if (!std::filesystem::exists(pq_compressed_file)) {
    std::cerr << "Error: PQ compressed file not found: " << pq_compressed_file << std::endl;
    std::cerr << "Hint: Make sure index files follow naming convention {prefix}_pq_compressed.bin" << std::endl;
    return;
  }
  if (!std::filesystem::exists(pq_pivots_file)) {
    std::cerr << "Error: PQ pivots file not found: " << pq_pivots_file << std::endl;
    std::cerr << "Hint: Make sure index files follow naming convention {prefix}_pq_pivots.bin" << std::endl;
    return;
  }

  // 加载查询
  T *query = nullptr;
  size_t query_num, query_dim;
  pipeann::load_bin<T>(query_file, query, query_num, query_dim);
  std::cout << "Loaded " << query_num << " queries with dimension " << query_dim << std::endl;

  // 加载groundtruth
  unsigned *gt_ids = nullptr;
  float *gt_dists = nullptr;
  size_t gt_num, gt_dim;
  if (!load_truthset_auto(gt_file, query_num, gt_ids, gt_dists, gt_num, gt_dim)) {
    std::cerr << "Error: Failed to load ground truth file: " << gt_file << std::endl;
    delete[] query;
    return;
  }
  std::cout << "Loaded groundtruth: " << gt_num << " queries, k=" << gt_dim << std::endl;
  if (query_num == 0 || query_dim == 0 || gt_num == 0 || gt_dim == 0) {
    std::cerr << "Error: Query or groundtruth metadata is invalid." << std::endl;
    delete[] query;
    delete[] gt_ids;
    delete[] gt_dists;
    return;
  }
  if (query_num != gt_num) {
    std::cerr << "Error: Query count (" << query_num << ") does not match groundtruth count (" << gt_num << ")."
              << std::endl;
    delete[] query;
    delete[] gt_ids;
    delete[] gt_dists;
    return;
  }
  int recall_min = 10;
  int recall_max = std::min(99, static_cast<int>(gt_dim));
  recall_max = std::min(recall_max, static_cast<int>(recall_at));
  if (recall_max < recall_min) {
    std::cerr << "Error: Groundtruth k (" << gt_dim << ") is smaller than recall@" << recall_min << "." << std::endl;
    delete[] query;
    delete[] gt_ids;
    delete[] gt_dists;
    return;
  }

  // 设置搜索模式
  int search_mode = (system_type == DC_PDI) ? PIPE_SEARCH : BEAM_SEARCH;
  // 注意：只有 PAGE_SEARCH (Starling) 需要使用 page layout，BEAM_SEARCH 和 PIPE_SEARCH 都不需要
  bool use_page_search = false;  // DC-PDI和IP-DiskANN都不使用page search

  // 创建索引读取器和邻居处理器
  std::shared_ptr<AlignedFileReader> reader;
  reader.reset(new LinuxAlignedFileReader());
  auto nbr_handler = new pipeann::PQNeighbor<T>();

  // 加载索引
  pipeann::SSDIndex<T, TagT> index(pipeann::L2, reader, nbr_handler, false);
  int load_result = index.load(index_prefix.c_str(), num_threads, true, use_page_search);

  if (load_result != 0) {
    std::cerr << "Failed to load index for " << system_names[system_type] << std::endl;
    delete[] query;
    delete[] gt_ids;
    delete[] gt_dists;
    return;
  }
  if (index.data_dim != query_dim) {
    std::cerr << "Error: Query dimension (" << query_dim << ") does not match index dimension (" << index.data_dim
              << ")." << std::endl;
    delete[] query;
    delete[] gt_ids;
    delete[] gt_dists;
    return;
  }

  std::cout << "Loaded index for " << system_names[system_type] << ", num_points=" << index.num_points << std::endl;

  // 输出文件
  std::ofstream ofs(output_file, std::ios::app);
  if (ofs.tellp() == 0) {
    ofs << "system,recall_at,L,recall,qps,avg_lat_us,p50_lat_us,p90_lat_us,p95_lat_us,p99_lat_us,mean_ios,"
           "io_amplification,reorg_running\n";
  }

  // 测试不同L值
  for (auto L : L_values) {
    TagT *query_result_tags = new TagT[recall_max * query_num];
    float *query_result_dists = new float[recall_max * query_num];
    pipeann::QueryStats *stats = new pipeann::QueryStats[query_num];
    std::vector<double> latency_stats(query_num, 0);

    auto s = std::chrono::high_resolution_clock::now();

#pragma omp parallel for num_threads(NUM_SEARCH_THREADS) schedule(dynamic, 1)
    for (int64_t i = 0; i < (int64_t) query_num; i++) {
      auto qs = std::chrono::high_resolution_clock::now();

      if (search_mode == PIPE_SEARCH) {
        index.pipe_search(query + (i * query_dim), recall_max, 0, L, query_result_tags + (i * recall_max),
                          query_result_dists + (i * recall_max), beam_width, stats + i);
      } else {
        index.beam_search(query + (i * query_dim), recall_max, 0, L, query_result_tags + (i * recall_max),
                          query_result_dists + (i * recall_max), beam_width, stats + i);
      }

      auto qe = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> diff = qe - qs;
      latency_stats[i] = diff.count() * 1000000;  // 转换为微秒
    }

    auto e = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = e - s;

    // 计算统计信息
    double qps = query_num / diff.count();
    std::sort(latency_stats.begin(), latency_stats.end());
    double avg_lat = std::accumulate(latency_stats.begin(), latency_stats.end(), 0.0) / query_num;
    double p50 = latency_stats[query_num * 0.5];
    double p90 = latency_stats[query_num * 0.9];
    double p95 = latency_stats[query_num * 0.95];
    double p99 = latency_stats[query_num * 0.99];

    // 计算平均IO次数
    double mean_ios = 0.0;
    for (size_t i = 0; i < query_num; i++) {
      mean_ios += stats[i].n_ios;
    }
    mean_ios /= query_num;

    int reorg_running = 0;
#ifdef ENABLE_DISPERSION_MONITOR
    reorg_running = index.is_reorganizing() ? 1 : 0;
#endif
    for (int recall_k = recall_min; recall_k <= recall_max; ++recall_k) {
      double recall = pipeann::calculate_recall((uint32_t) query_num, gt_ids, gt_dists, (uint32_t) gt_dim,
                                                query_result_tags, (uint32_t) recall_max, (uint32_t) recall_k);
      double io_amplification = mean_ios / recall_k;

      ofs << system_names[system_type] << "," << recall_k << "," << L << "," << recall << "," << qps << "," << avg_lat
          << "," << p50 << "," << p90 << "," << p95 << "," << p99 << "," << mean_ios << "," << io_amplification << ","
          << reorg_running << "\n";

      std::cout << system_names[system_type] << " L=" << L << ": Recall@" << recall_k << "=" << recall
                << ", QPS=" << qps << ", P99=" << p99 << "us, MeanIOs=" << mean_ios << std::endl;
    }

    delete[] query_result_tags;
    delete[] query_result_dists;
    delete[] stats;
  }

  ofs.close();
  delete[] query;
  delete[] gt_ids;
  delete[] gt_dists;
}

/**
 * 实验2: 更新吞吐量对比 (对应5.3.1节)
 * 测试三个系统的插入性能
 */
template<typename T, typename TagT = uint32_t>
void compare_update_throughput(pipeann::DynamicSSDIndex<T, TagT> &index, T *insert_data, size_t num_inserts,
                               size_t data_dim, SystemType system_type, bool trigger_merge,
                               const std::string &output_file) {
  std::ofstream ofs(output_file, std::ios::app);
  if (ofs.tellp() == 0) {
    ofs << "system,time_sec,num_inserts,throughput_ops,memory_rss_mb,disk_usage_mb,merge_triggered,reorg_running\n";
  }

  std::atomic<uint64_t> insert_count(0);
  std::atomic<bool> merge_done(false);
  std::atomic<bool> stop_insert(false);

  pipeann::Timer timer;

  // 插入线程
  auto insert_func = [&](int thread_id) {
    size_t local_count = 0;
    while (!stop_insert.load()) {
      uint64_t idx = insert_count.fetch_add(1);
      if (idx >= num_inserts)
        break;

      TagT tag = static_cast<TagT>(idx + 1000000);  // 避免与现有数据冲突
      index.insert(insert_data + idx * data_dim, tag);
      local_count++;

      // FreshDiskANN模式: 周期性触发合并
      if (system_type == FRESH_DISKANN && trigger_merge && idx % MERGE_INTERVAL == MERGE_INTERVAL - 1) {
        std::cout << "[" << system_names[system_type] << "] Triggering merge at " << idx << " inserts" << std::endl;
        index.final_merge(NUM_SEARCH_THREADS);
        merge_done.store(true);
      }
    }
  };

  // 启动插入线程
  std::vector<std::thread> insert_threads;
  for (int i = 0; i < NUM_INSERT_THREADS; i++) {
    insert_threads.emplace_back(insert_func, i);
  }

  // 监控线程：每秒记录一次
  std::thread monitor([&]() {
    while (!stop_insert.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));

      uint64_t current_inserts = insert_count.load();
      double elapsed_sec = timer.elapsed() / 1e6;
      double throughput = current_inserts / elapsed_sec;

      double rss_kb, vm_kb;
      get_memory_usage(rss_kb, vm_kb);
      double disk_mb = static_cast<double>(get_disk_usage_bytes(index._disk_index_prefix_in)) / (1024.0 * 1024.0);

      int reorg_running = 0;
#ifdef ENABLE_DISPERSION_MONITOR
      reorg_running = index.is_reorganizing() ? 1 : 0;
#endif
      ofs << system_names[system_type] << "," << elapsed_sec << "," << current_inserts << "," << throughput << ","
          << (rss_kb / 1024.0) << "," << disk_mb << "," << (merge_done.load() ? 1 : 0) << "," << reorg_running << "\n";
      ofs.flush();

      if (current_inserts >= num_inserts) {
        stop_insert.store(true);
        break;
      }
    }
  });

  // 等待完成
  for (auto &t : insert_threads) {
    t.join();
  }
  stop_insert.store(true);
  monitor.join();

  double total_time = timer.elapsed() / 1e6;
  double final_throughput = num_inserts / total_time;

  std::cout << "[" << system_names[system_type] << "] Insert completed: " << num_inserts << " ops in " << total_time
            << "s, throughput=" << final_throughput << " ops/s" << std::endl;

  ofs.close();
}

/**
 * 实验3: 读写并发性能 (对应5.3.2节)
 * 同时运行搜索和更新，测试性能稳定性
 */
template<typename T, typename TagT = uint32_t>
void compare_concurrent_performance(pipeann::DynamicSSDIndex<T, TagT> &index, T *query_data, T *insert_data,
                                    size_t query_num, size_t insert_num, size_t data_dim, uint64_t recall_at,
                                    uint64_t L, uint32_t beam_width, double duration_sec, double insert_rate,
                                    SystemType system_type, const std::string &output_file) {
  std::ofstream ofs(output_file, std::ios::app);
  if (ofs.tellp() == 0) {
    ofs << "system,time_sec,search_qps,search_p99_us,insert_ops,insert_tput,memory_rss_mb,disk_usage_mb,reorg_"
           "running\n";
  }

  std::atomic<uint64_t> search_count(0);
  std::atomic<uint64_t> insert_count(0);
  std::atomic<bool> stop_test(false);

  std::vector<double> search_latencies;
  std::mutex lat_mutex;

  pipeann::Timer timer;

  // 搜索线程
  auto search_func = [&]() {
    int search_mode = (system_type == DC_PDI) ? PIPE_SEARCH : BEAM_SEARCH;

    while (!stop_test.load()) {
      uint64_t idx = search_count.fetch_add(1);
      if (idx >= query_num) {
        search_count.store(0);  // 循环使用查询
        idx = 0;
      }

      TagT result_tags[recall_at];
      float result_dists[recall_at];
      pipeann::QueryStats stats;

      auto qs = std::chrono::high_resolution_clock::now();
      index.search(query_data + idx * data_dim, recall_at, 0, L, beam_width, result_tags, result_dists, &stats, true);
      auto qe = std::chrono::high_resolution_clock::now();

      double lat_us = std::chrono::duration<double>(qe - qs).count() * 1e6;

      {
        std::lock_guard<std::mutex> lock(lat_mutex);
        search_latencies.push_back(lat_us);
      }
    }
  };

  // 插入线程
  auto start_time = std::chrono::steady_clock::now();
  auto insert_func = [&]() {
    while (!stop_test.load()) {
      if (duration_sec > 0) {
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
        if (elapsed >= duration_sec) {
          stop_test.store(true);
          break;
        }
      }
      uint64_t idx = insert_count.fetch_add(1);
      if (idx >= insert_num) {
        // All inserts completed, log and exit
        if (idx == insert_num) {
          double elapsed_final = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
          std::cout << "[Exp3] All " << insert_num << " updates completed in " << elapsed_final 
                    << "s (target duration: " << duration_sec << "s)" << std::endl;
        }
        break;
      }

      TagT tag = static_cast<TagT>(idx + 2000000);
      index.insert(insert_data + idx * data_dim, tag);

      // FreshDiskANN模式: 周期性合并
      if (system_type == FRESH_DISKANN && idx % MERGE_INTERVAL == MERGE_INTERVAL - 1) {
        index.final_merge(NUM_SEARCH_THREADS / 2);
      }

      if (duration_sec > 0 && insert_rate > 0) {
        while (!stop_test.load()) {
          double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
          if (elapsed >= duration_sec) {
            stop_test.store(true);
            break;
          }
          double expected = insert_rate * elapsed;
          if (static_cast<double>(insert_count.load()) <= expected) {
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
    }
    
    // For FreshDiskANN, wait for any ongoing reorganization to complete before exiting
    if (system_type == FRESH_DISKANN) {
      while (index.is_reorganizing()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  };

  // 启动线程
  std::vector<std::thread> search_threads;
  for (int i = 0; i < NUM_SEARCH_THREADS; i++) {
    search_threads.emplace_back(search_func);
  }

  std::vector<std::thread> insert_threads;
  for (int i = 0; i < NUM_INSERT_THREADS; i++) {
    insert_threads.emplace_back(insert_func);
  }

  // 监控线程
  std::thread monitor([&]() {
    while (!stop_test.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));

      double elapsed_sec = timer.elapsed() / 1e6;
      uint64_t searches = search_count.load();
      uint64_t inserts = insert_count.load();

      double search_qps = searches / elapsed_sec;
      double insert_tput = inserts / elapsed_sec;

      // 计算P99延迟
      double p99_lat = 0;
      {
        std::lock_guard<std::mutex> lock(lat_mutex);
        if (!search_latencies.empty()) {
          std::sort(search_latencies.begin(), search_latencies.end());
          p99_lat = search_latencies[search_latencies.size() * 0.99];
          search_latencies.clear();  // 清空，避免内存暴涨
        }
      }

      double rss_kb, vm_kb;
      get_memory_usage(rss_kb, vm_kb);
      double disk_mb = static_cast<double>(get_disk_usage_bytes(index._disk_index_prefix_in)) / (1024.0 * 1024.0);

      int reorg_running = index.is_reorganizing() ? 1 : 0;
      ofs << system_names[system_type] << "," << elapsed_sec << "," << search_qps << "," << p99_lat << "," << inserts
          << "," << insert_tput << "," << (rss_kb / 1024.0) << "," << disk_mb << "," << reorg_running << "\n";
      ofs.flush();

      if ((duration_sec > 0 && elapsed_sec >= duration_sec) || inserts >= insert_num) {
        stop_test.store(true);
        break;
      }
    }
  });

  // 等待完成
  for (auto &t : insert_threads) {
    t.join();
  }
  
  // All insert threads completed - wait for any final operations to complete
  if (system_type == FRESH_DISKANN) {
    std::cout << "[Exp3] Waiting for FreshDiskANN reorganization to complete..." << std::endl;
    while (index.is_reorganizing()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "[Exp3] FreshDiskANN reorganization completed." << std::endl;
  }
  
  double final_elapsed = timer.elapsed() / 1e6;
  uint64_t final_insert_count = insert_count.load();
  std::cout << "[Exp3] Insert threads completed. Total inserts: " << final_insert_count 
            << "/" << insert_num << ", elapsed: " << final_elapsed << "s" << std::endl;
  
  stop_test.store(true);

  for (auto &t : search_threads) {
    t.join();
  }
  monitor.join();

  ofs.close();

  std::cout << "[" << system_names[system_type] << "] Concurrent test completed" << std::endl;
}

/**
 * 实验1: 搜索延迟+更新并发 (动态调整L值以达到目标召回率)
 * 
 * 核心改进：
 * 1. 使用自适应L值调整策略，根据当前recall动态调整搜索参数
 * 2. 当recall稳定在目标附近时，记录有效的性能指标
 * 3. 支持L值的上下限约束，避免极端值
 */
template<typename T, typename TagT = uint32_t>
void compare_search_update_latency(pipeann::DynamicSSDIndex<T, TagT> &index, T *query_data, unsigned *gt_ids,
                                   float *gt_dists, size_t query_num, size_t gt_dim, T *insert_data, size_t insert_num,
                                   size_t data_dim, uint64_t recall_at, uint64_t L_init, uint32_t beam_width,
                                   double duration_sec, double insert_rate, double target_recall,
                                   SystemType system_type, const std::string &output_file) {
  std::ofstream ofs(output_file, std::ios::app);
  if (ofs.tellp() == 0) {
    ofs << "system,time_sec,L,recall_at,recall_target,recall_pct,search_qps,p50_lat_us,p90_lat_us,p99_lat_us,"
           "mean_ios,memory_rss_mb,disk_usage_mb,reorg_running\n";
  }

  // L值动态调整参数
  constexpr uint64_t L_MIN = 30;         // L最小值
  constexpr uint64_t L_MAX = 500;        // L最大值
  constexpr uint64_t L_STEP = 5;         // 每次调整步长
  constexpr double RECALL_TOLERANCE = 2.0;  // 召回率容忍度（±2%）
  constexpr int STABLE_THRESHOLD = 3;    // 稳定判定所需的连续采样次数

  // 动态L值（原子变量，可被搜索线程安全读取）
  std::atomic<uint64_t> current_L(L_init);
  int stable_count = 0;  // 连续稳定的采样次数

  std::atomic<uint64_t> query_index(0);
  std::atomic<uint64_t> interval_queries(0);
  std::atomic<uint64_t> interval_recall_sum(0);
  std::atomic<uint64_t> interval_ios_sum(0);
  std::atomic<uint64_t> insert_count(0);
  std::atomic<bool> stop_test(false);

  std::vector<double> search_latencies;
  std::mutex lat_mutex;

  pipeann::Timer timer;
  auto start_time = std::chrono::steady_clock::now();
  auto last_sample = std::chrono::steady_clock::now();

  auto search_func = [&]() {
    while (!stop_test.load()) {
      uint64_t idx = query_index.fetch_add(1);
      if (idx >= query_num) {
        query_index.store(0);
        idx = 0;
      }

      // 使用当前动态L值
      uint64_t L_now = current_L.load();

      TagT result_tags[recall_at];
      float result_dists[recall_at];
      pipeann::QueryStats stats;

      auto qs = std::chrono::high_resolution_clock::now();
      index.search(query_data + idx * data_dim, recall_at, 0, L_now, beam_width, result_tags, result_dists, &stats, true);
      auto qe = std::chrono::high_resolution_clock::now();

      double lat_us = std::chrono::duration<double>(qe - qs).count() * 1e6;
      double recall_pct =
          pipeann::calculate_recall(1, gt_ids + idx * gt_dim, gt_dists + idx * gt_dim, static_cast<unsigned>(gt_dim),
                                    result_tags, static_cast<unsigned>(recall_at), static_cast<unsigned>(recall_at));

      interval_queries.fetch_add(1);
      interval_recall_sum.fetch_add(static_cast<uint64_t>(recall_pct * 1000.0));
      interval_ios_sum.fetch_add(static_cast<uint64_t>(stats.n_ios * 1000.0));

      {
        std::lock_guard<std::mutex> lock(lat_mutex);
        search_latencies.push_back(lat_us);
      }
    }
  };

  auto insert_func = [&]() {
    while (!stop_test.load()) {
      double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
      if (duration_sec > 0 && elapsed >= duration_sec) {
        stop_test.store(true);
        break;
      }
      uint64_t idx = insert_count.fetch_add(1);
      if (idx >= insert_num) {
        // All inserts completed, log and exit
        if (idx == insert_num) {
          double elapsed_final = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
          std::cout << "[Exp1] All " << insert_num << " updates completed in " << elapsed_final 
                    << "s (target duration: " << duration_sec << "s)" << std::endl;
        }
        break;
      }

      TagT tag = static_cast<TagT>(idx + 2000000);
      index.insert(insert_data + idx * data_dim, tag);

      if (system_type == FRESH_DISKANN && idx % MERGE_INTERVAL == MERGE_INTERVAL - 1) {
        index.final_merge(NUM_SEARCH_THREADS / 2);
      }

      if (duration_sec > 0 && insert_rate > 0) {
        while (!stop_test.load()) {
          double elapsed_now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
          if (elapsed_now >= duration_sec) {
            stop_test.store(true);
            break;
          }
          double expected = insert_rate * elapsed_now;
          if (static_cast<double>(insert_count.load()) <= expected) {
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
    }
    
    // For FreshDiskANN, wait for any ongoing reorganization to complete before exiting
    if (system_type == FRESH_DISKANN) {
      while (index.is_reorganizing()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  };

  std::vector<std::thread> search_threads;
  for (int i = 0; i < NUM_SEARCH_THREADS; i++) {
    search_threads.emplace_back(search_func);
  }

  std::vector<std::thread> insert_threads;
  for (int i = 0; i < NUM_INSERT_THREADS; i++) {
    insert_threads.emplace_back(insert_func);
  }

  std::thread monitor([&]() {
    bool first_log = true;
    while (!stop_test.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      auto now = std::chrono::steady_clock::now();
      double interval_sec = std::chrono::duration<double>(now - last_sample).count();
      last_sample = now;

      uint64_t queries = interval_queries.exchange(0);
      uint64_t recall_sum = interval_recall_sum.exchange(0);
      uint64_t ios_sum = interval_ios_sum.exchange(0);

      double recall_pct = 0.0;
      double mean_ios = 0.0;
      if (queries > 0) {
        recall_pct = (static_cast<double>(recall_sum) / 1000.0) / static_cast<double>(queries);
        mean_ios = (static_cast<double>(ios_sum) / 1000.0) / static_cast<double>(queries);
      }

      // ========= 动态调整L值以达到目标召回率 =========
      uint64_t L_now = current_L.load();
      double recall_diff = recall_pct - target_recall;
      
      if (queries > 0) {
        if (recall_diff < -RECALL_TOLERANCE) {
          // 当前recall低于目标，增加L值
          uint64_t new_L = std::min(L_now + L_STEP, L_MAX);
          if (new_L != L_now) {
            current_L.store(new_L);
            stable_count = 0;
            if (first_log) {
              std::cout << "[Exp1] Recall " << recall_pct << "% < target " << target_recall 
                        << "%, increasing L: " << L_now << " -> " << new_L << std::endl;
            }
          }
        } else if (recall_diff > RECALL_TOLERANCE) {
          // 当前recall高于目标，减小L值以降低延迟
          uint64_t new_L = (L_now > L_MIN + L_STEP) ? (L_now - L_STEP) : L_MIN;
          if (new_L != L_now) {
            current_L.store(new_L);
            stable_count = 0;
            if (first_log) {
              std::cout << "[Exp1] Recall " << recall_pct << "% > target " << target_recall 
                        << "%, decreasing L: " << L_now << " -> " << new_L << std::endl;
            }
          }
        } else {
          // 召回率在目标范围内，增加稳定计数
          stable_count++;
          if (stable_count == STABLE_THRESHOLD && first_log) {
            std::cout << "[Exp1] Recall stabilized at " << recall_pct << "% (target: " 
                      << target_recall << "%, L=" << L_now << ")" << std::endl;
            first_log = false;
          }
        }
      }
      // ========= 动态调整结束 =========

      double search_qps = (interval_sec > 0 && queries > 0) ? (queries / interval_sec) : 0.0;
      double p50_lat = 0.0;
      double p90_lat = 0.0;
      double p99_lat = 0.0;
      {
        std::lock_guard<std::mutex> lock(lat_mutex);
        if (!search_latencies.empty()) {
          std::sort(search_latencies.begin(), search_latencies.end());
          p50_lat = search_latencies[static_cast<size_t>(search_latencies.size() * 0.5)];
          p90_lat = search_latencies[static_cast<size_t>(search_latencies.size() * 0.9)];
          p99_lat = search_latencies[static_cast<size_t>(search_latencies.size() * 0.99)];
          search_latencies.clear();
        }
      }

      double rss_kb, vm_kb;
      get_memory_usage(rss_kb, vm_kb);
      double disk_mb = static_cast<double>(get_disk_usage_bytes(index._disk_index_prefix_in)) / (1024.0 * 1024.0);

      int reorg_running = index.is_reorganizing() ? 1 : 0;
      double elapsed_sec = timer.elapsed() / 1e6;
      
      // 使用当前实际的L值而非初始L值
      uint64_t L_current = current_L.load();
      ofs << system_names[system_type] << "," << elapsed_sec << "," << L_current << "," << recall_at << "," << target_recall
          << "," << recall_pct << "," << search_qps << "," << p50_lat << "," << p90_lat << "," << p99_lat << ","
          << mean_ios << "," << (rss_kb / 1024.0) << "," << disk_mb << "," << reorg_running << "\n";
      ofs.flush();

      if (duration_sec > 0 && elapsed_sec >= duration_sec) {
        stop_test.store(true);
        break;
      }
    }
  });

  for (auto &t : insert_threads) {
    t.join();
  }
  
  // All insert threads completed - wait a bit for any final operations to complete
  if (system_type == FRESH_DISKANN) {
    std::cout << "[Exp1] Waiting for FreshDiskANN reorganization to complete..." << std::endl;
    while (index.is_reorganizing()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "[Exp1] FreshDiskANN reorganization completed." << std::endl;
  }
  
  double final_elapsed = timer.elapsed() / 1e6;
  uint64_t final_insert_count = insert_count.load();
  uint64_t final_L = current_L.load();
  std::cout << "[Exp1] Insert threads completed. Total inserts: " << final_insert_count 
            << "/" << insert_num << ", elapsed: " << final_elapsed << "s, final L: " << final_L << std::endl;
  
  // Now it's safe to stop the test
  stop_test.store(true);
  
  for (auto &t : search_threads) {
    t.join();
  }
  monitor.join();
  ofs.close();
}

template<typename T, typename TagT = uint32_t>
void run_concurrent_exp(const std::string &index_prefix, const std::string &query_file, const std::string &insert_file,
                        SystemType system_type, uint64_t recall_at, const std::vector<uint64_t> &L_values,
                        uint32_t beam_width, double update_ratio, double duration_sec, const std::string &output_file) {
  T *query = nullptr;
  size_t query_num = 0, query_dim = 0;
  pipeann::load_bin<T>(query_file, query, query_num, query_dim);
  if (query_num == 0 || query_dim == 0) {
    std::cerr << "Error: Query file metadata is invalid: " << query_file << std::endl;
    delete[] query;
    return;
  }

  T *insert_data = nullptr;
  size_t insert_num = 0, insert_dim = 0;
  pipeann::load_bin<T>(insert_file, insert_data, insert_num, insert_dim);
  if (insert_num == 0 || insert_dim == 0) {
    std::cerr << "Error: Insert file metadata is invalid: " << insert_file << std::endl;
    delete[] query;
    delete[] insert_data;
    return;
  }
  if (insert_dim != query_dim) {
    std::cerr << "Error: Insert dim (" << insert_dim << ") does not match query dim (" << query_dim << ")" << std::endl;
    delete[] query;
    delete[] insert_data;
    return;
  }

  if (update_ratio > 0.0 && update_ratio < 1.0) {
    insert_num = static_cast<size_t>(insert_num * update_ratio);
    if (insert_num == 0) {
      std::cerr << "Error: update_ratio too small, no insertions to run." << std::endl;
      delete[] query;
      delete[] insert_data;
      return;
    }
  }

  pipeann::Parameters paras;
  uint64_t L_disk = L_values.empty() ? 100 : L_values.front();
  paras.set(0, static_cast<uint32_t>(L_disk), 384, 1.2f, NUM_SEARCH_THREADS + NUM_INSERT_THREADS, true, beam_width);

  pipeann::Metric metric = pipeann::Metric::L2;
  auto *dist_cmp = pipeann::get_distance_function<T>(metric);
  int search_mode = (system_type == DC_PDI) ? PIPE_SEARCH : BEAM_SEARCH;

  pipeann::DynamicSSDIndex<T, TagT> dyn_index(paras, index_prefix, index_prefix + "_merge", dist_cmp, metric,
                                              search_mode, false);

  double insert_rate = (duration_sec > 0) ? (static_cast<double>(insert_num) / duration_sec) : 0.0;
  compare_concurrent_performance<T, TagT>(dyn_index, query, insert_data, query_num, insert_num, query_dim, recall_at,
                                          L_disk, beam_width, duration_sec, insert_rate, system_type, output_file);

  delete[] query;
  delete[] insert_data;
  delete dist_cmp;
}

template<typename T, typename TagT = uint32_t>
void run_search_update_latency_exp(const std::string &index_prefix, const std::string &query_file,
                                   const std::string &gt_file, const std::string &insert_file, SystemType system_type,
                                   uint64_t recall_at, const std::vector<uint64_t> &L_values, uint32_t beam_width,
                                   double update_ratio, double duration_sec, double target_recall,
                                   const std::string &output_file) {
  T *query = nullptr;
  size_t query_num = 0, query_dim = 0;
  pipeann::load_bin<T>(query_file, query, query_num, query_dim);
  if (query_num == 0 || query_dim == 0) {
    std::cerr << "Error: Query file metadata is invalid: " << query_file << std::endl;
    delete[] query;
    return;
  }

  unsigned *gt_ids = nullptr;
  float *gt_dists = nullptr;
  size_t gt_num = 0, gt_dim = 0;
  if (!load_truthset_auto(gt_file, query_num, gt_ids, gt_dists, gt_num, gt_dim)) {
    std::cerr << "Error: Failed to load ground truth file: " << gt_file << std::endl;
    delete[] query;
    delete[] gt_ids;
    delete[] gt_dists;
    return;
  }
  if (gt_num != query_num) {
    std::cerr << "Error: Query count (" << query_num << ") does not match groundtruth count (" << gt_num << ")"
              << std::endl;
    delete[] query;
    delete[] gt_ids;
    delete[] gt_dists;
    return;
  }
  if (gt_dim < recall_at) {
    std::cerr << "Error: Groundtruth k (" << gt_dim << ") is smaller than recall@" << recall_at << "." << std::endl;
    delete[] query;
    delete[] gt_ids;
    delete[] gt_dists;
    return;
  }

  T *insert_data = nullptr;
  size_t insert_num = 0, insert_dim = 0;
  pipeann::load_bin<T>(insert_file, insert_data, insert_num, insert_dim);
  if (insert_num == 0 || insert_dim == 0) {
    std::cerr << "Error: Insert file metadata is invalid: " << insert_file << std::endl;
    delete[] query;
    delete[] gt_ids;
    delete[] insert_data;
    return;
  }
  if (insert_dim != query_dim) {
    std::cerr << "Error: Insert dim (" << insert_dim << ") does not match query dim (" << query_dim << ")" << std::endl;
    delete[] query;
    delete[] gt_ids;
    delete[] insert_data;
    return;
  }

  if (update_ratio > 0.0 && update_ratio < 1.0) {
    insert_num = static_cast<size_t>(insert_num * update_ratio);
    if (insert_num == 0) {
      std::cerr << "Error: update_ratio too small, no insertions to run." << std::endl;
      delete[] query;
      delete[] gt_ids;
      delete[] insert_data;
      return;
    }
  }

  pipeann::Parameters paras;
  uint64_t L_disk = L_values.empty() ? 100 : L_values.front();
  paras.set(0, static_cast<uint32_t>(L_disk), 384, 1.2f, NUM_SEARCH_THREADS + NUM_INSERT_THREADS, true, beam_width);

  pipeann::Metric metric = pipeann::Metric::L2;
  auto *dist_cmp = pipeann::get_distance_function<T>(metric);
  int search_mode = (system_type == DC_PDI) ? PIPE_SEARCH : BEAM_SEARCH;

  pipeann::DynamicSSDIndex<T, TagT> dyn_index(paras, index_prefix, index_prefix + "_merge", dist_cmp, metric,
                                              search_mode, false);

  double insert_rate = (duration_sec > 0) ? (static_cast<double>(insert_num) / duration_sec) : 0.0;
  compare_search_update_latency<T, TagT>(dyn_index, query, gt_ids, gt_dists, query_num, gt_dim, insert_data, insert_num,
                                         query_dim, recall_at, L_disk, beam_width, duration_sec, insert_rate,
                                         target_recall, system_type, output_file);

  delete[] query;
  delete[] gt_ids;
  delete[] gt_dists;
  delete[] insert_data;
  delete dist_cmp;
}

template<typename T, typename TagT = uint32_t>
void run_update_throughput_exp(const std::string &index_prefix, const std::string &insert_file, SystemType system_type,
                               const std::vector<uint64_t> &L_values, const std::string &output_file) {
  if (!std::filesystem::exists(insert_file)) {
    std::cerr << "Error: Insert file not found: " << insert_file << std::endl;
    return;
  }

  T *insert_data = nullptr;
  size_t insert_num = 0, insert_dim = 0;
  pipeann::load_bin<T>(insert_file, insert_data, insert_num, insert_dim);
  if (insert_num == 0 || insert_dim == 0) {
    std::cerr << "Error: Insert file metadata is invalid: " << insert_file << std::endl;
    delete[] insert_data;
    return;
  }

  pipeann::Parameters paras;
  uint64_t L_disk = L_values.empty() ? 100 : L_values.front();
  paras.set(0, static_cast<uint32_t>(L_disk), 384, 1.2f, NUM_SEARCH_THREADS + NUM_INSERT_THREADS, true, 4);

  pipeann::Metric metric = pipeann::Metric::L2;
  auto *dist_cmp = pipeann::get_distance_function<T>(metric);
  int search_mode = (system_type == DC_PDI) ? PIPE_SEARCH : BEAM_SEARCH;

  pipeann::DynamicSSDIndex<T, TagT> dyn_index(paras, index_prefix, index_prefix + "_merge", dist_cmp, metric,
                                              search_mode, false);

  if (dyn_index._disk_index != nullptr && dyn_index._disk_index->data_dim != insert_dim) {
    std::cerr << "Error: Insert dim (" << insert_dim << ") does not match index dim ("
              << dyn_index._disk_index->data_dim << ")" << std::endl;
    delete[] insert_data;
    delete dist_cmp;
    return;
  }

  bool trigger_merge = (system_type == FRESH_DISKANN);
  compare_update_throughput<T, TagT>(dyn_index, insert_data, insert_num, insert_dim, system_type, trigger_merge,
                                     output_file);

  delete[] insert_data;
  delete dist_cmp;
}

/**
 * 主函数
 */
int main(int argc, char **argv) {
  if (argc < 10) {
    std::cout << "Usage: " << argv[0] << " <data_type> <index_prefix> <query_file> <gt_file>"
              << " <insert_data_file> <system_type> <experiment_type>"
              << " <output_dir> <num_threads> [recall_at] [L_values...]"
              << " [--exp1-duration-sec <sec>] [--exp1-update-ratio <ratio>]"
              << " [--exp1-target-recall <recall_pct>]"
              << " [--exp3-duration-sec <sec>] [--exp3-update-ratio <ratio>]\n";
    std::cout << "\nParameters:\n";
    std::cout << "  data_type: uint8/int8/float\n";
    std::cout << "  system_type: 0=DC-PDI, 1=IP-DiskANN, 2=FreshDiskANN\n";
    std::cout << "  experiment_type: 1=search_latency, 2=update_throughput, 3=concurrent\n";
    std::cout << "  --exp1-duration-sec: fixed duration for experiment 1 (seconds)\n";
    std::cout << "  --exp1-update-ratio: fraction of insert dataset to use in experiment 1\n";
    std::cout << "  --exp1-target-recall: target recall@K percentage for reporting\n";
    std::cout << "  --exp3-duration-sec: fixed duration for experiment 3 (seconds)\n";
    std::cout << "  --exp3-update-ratio: fraction of insert dataset to use in experiment 3\n";
    return -1;
  }

  int arg_no = 1;
  std::string data_type(argv[arg_no++]);
  std::string index_prefix(argv[arg_no++]);
  std::string query_file(argv[arg_no++]);
  std::string gt_file(argv[arg_no++]);
  std::string insert_file(argv[arg_no++]);
  int system_type = atoi(argv[arg_no++]);
  int exp_type = atoi(argv[arg_no++]);
  std::string output_dir(argv[arg_no++]);
  size_t num_threads = atoi(argv[arg_no++]);

  uint64_t recall_at = (argc > arg_no) ? atoi(argv[arg_no++]) : 10;

  double exp1_duration_sec = 180.0;
  double exp1_update_ratio = 1.0;
  double exp1_target_recall = 90.0;
  double exp3_duration_sec = 120.0;
  double exp3_update_ratio = 1.0;

  std::vector<uint64_t> L_values;
  if (argc > arg_no) {
    for (int i = arg_no; i < argc; i++) {
      std::string arg(argv[i]);
      if (arg.rfind("--exp1-duration-sec=", 0) == 0) {
        exp1_duration_sec = std::stod(arg.substr(strlen("--exp1-duration-sec=")));
        continue;
      }
      if (arg == "--exp1-duration-sec" && i + 1 < argc) {
        exp1_duration_sec = std::stod(argv[++i]);
        continue;
      }
      if (arg.rfind("--exp1-update-ratio=", 0) == 0) {
        exp1_update_ratio = std::stod(arg.substr(strlen("--exp1-update-ratio=")));
        continue;
      }
      if (arg == "--exp1-update-ratio" && i + 1 < argc) {
        exp1_update_ratio = std::stod(argv[++i]);
        continue;
      }
      if (arg.rfind("--exp1-target-recall=", 0) == 0) {
        exp1_target_recall = std::stod(arg.substr(strlen("--exp1-target-recall=")));
        continue;
      }
      if (arg == "--exp1-target-recall" && i + 1 < argc) {
        exp1_target_recall = std::stod(argv[++i]);
        continue;
      }
      if (arg.rfind("--exp3-duration-sec=", 0) == 0) {
        exp3_duration_sec = std::stod(arg.substr(strlen("--exp3-duration-sec=")));
        continue;
      }
      if (arg == "--exp3-duration-sec" && i + 1 < argc) {
        exp3_duration_sec = std::stod(argv[++i]);
        continue;
      }
      if (arg.rfind("--exp3-update-ratio=", 0) == 0) {
        exp3_update_ratio = std::stod(arg.substr(strlen("--exp3-update-ratio=")));
        continue;
      }
      if (arg == "--exp3-update-ratio" && i + 1 < argc) {
        exp3_update_ratio = std::stod(argv[++i]);
        continue;
      }
      L_values.push_back(atoi(argv[i]));
    }
  }
  if (L_values.empty()) {
    L_values = {100, 200, 300, 400, 500};
  }

  std::cout << "Running comparison experiment for " << system_names[system_type] << std::endl;
  std::cout << "Experiment type: " << exp_type << std::endl;

  // 实验1: 搜索延迟对比
  if (exp_type == 1) {
    std::string output = output_dir + "/exp1_search_latency_" +
                         std::string(system_type == 0   ? "dc-pdi"
                                     : system_type == 1 ? "ip-diskann"
                                                        : "fresh-diskann") +
                         ".csv";

    if (data_type == "uint8") {
      run_search_update_latency_exp<uint8_t, uint32_t>(
          index_prefix, query_file, gt_file, insert_file, (SystemType) system_type, recall_at, L_values, 4,
          exp1_update_ratio, exp1_duration_sec, exp1_target_recall, output);
    } else if (data_type == "int8") {
      run_search_update_latency_exp<int8_t, uint32_t>(index_prefix, query_file, gt_file, insert_file,
                                                      (SystemType) system_type, recall_at, L_values, 4,
                                                      exp1_update_ratio, exp1_duration_sec, exp1_target_recall, output);
    } else if (data_type == "float") {
      run_search_update_latency_exp<float, uint32_t>(index_prefix, query_file, gt_file, insert_file,
                                                     (SystemType) system_type, recall_at, L_values, 4,
                                                     exp1_update_ratio, exp1_duration_sec, exp1_target_recall, output);
    } else {
      std::cerr << "Unsupported data type: " << data_type << std::endl;
      return -1;
    }
  }
  // 实验2和3需要DynamicSSDIndex，暂不实现
  else if (exp_type == 2) {
    std::string output = output_dir + "/exp2_update_throughput_" +
                         std::string(system_type == 0   ? "dc-pdi"
                                     : system_type == 1 ? "ip-diskann"
                                                        : "fresh-diskann") +
                         ".csv";
    if (data_type == "uint8") {
      run_update_throughput_exp<uint8_t, uint32_t>(index_prefix, insert_file, (SystemType) system_type, L_values,
                                                   output);
    } else if (data_type == "int8") {
      run_update_throughput_exp<int8_t, uint32_t>(index_prefix, insert_file, (SystemType) system_type, L_values,
                                                  output);
    } else if (data_type == "float") {
      run_update_throughput_exp<float, uint32_t>(index_prefix, insert_file, (SystemType) system_type, L_values, output);
    } else {
      std::cerr << "Unsupported data type: " << data_type << std::endl;
      return -1;
    }
  } else if (exp_type == 3) {
    std::string output = output_dir + "/exp3_concurrent_" +
                         std::string(system_type == 0   ? "dc-pdi"
                                     : system_type == 1 ? "ip-diskann"
                                                        : "fresh-diskann") +
                         ".csv";
    if (data_type == "uint8") {
      run_concurrent_exp<uint8_t, uint32_t>(index_prefix, query_file, insert_file, (SystemType) system_type, recall_at,
                                            L_values, 4, exp3_update_ratio, exp3_duration_sec, output);
    } else if (data_type == "int8") {
      run_concurrent_exp<int8_t, uint32_t>(index_prefix, query_file, insert_file, (SystemType) system_type, recall_at,
                                           L_values, 4, exp3_update_ratio, exp3_duration_sec, output);
    } else if (data_type == "float") {
      run_concurrent_exp<float, uint32_t>(index_prefix, query_file, insert_file, (SystemType) system_type, recall_at,
                                          L_values, 4, exp3_update_ratio, exp3_duration_sec, output);
    } else {
      std::cerr << "Unsupported data type: " << data_type << std::endl;
      return -1;
    }
  } else {
    std::cerr << "Unknown experiment type: " << exp_type << std::endl;
    return -1;
  }

  std::cout << "Experiment completed successfully!" << std::endl;
  return 0;
}
