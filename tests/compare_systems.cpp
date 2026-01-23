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
  if (gt_dim < recall_at) {
    std::cerr << "Error: Groundtruth k (" << gt_dim << ") is smaller than recall@" << recall_at << "." << std::endl;
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
  std::ofstream ofs(output_file);
  ofs << "system,L,recall,qps,avg_lat_us,p50_lat_us,p90_lat_us,p95_lat_us,p99_lat_us,mean_ios,io_amplification\n";

  // 测试不同L值
  for (auto L : L_values) {
    TagT *query_result_tags = new TagT[recall_at * query_num];
    float *query_result_dists = new float[recall_at * query_num];
    pipeann::QueryStats *stats = new pipeann::QueryStats[query_num];
    std::vector<double> latency_stats(query_num, 0);

    auto s = std::chrono::high_resolution_clock::now();

#pragma omp parallel for num_threads(NUM_SEARCH_THREADS) schedule(dynamic, 1)
    for (int64_t i = 0; i < (int64_t) query_num; i++) {
      auto qs = std::chrono::high_resolution_clock::now();

      if (search_mode == PIPE_SEARCH) {
        index.pipe_search(query + (i * query_dim), recall_at, 0, L, query_result_tags + (i * recall_at),
                          query_result_dists + (i * recall_at), beam_width, stats + i);
      } else {
        index.beam_search(query + (i * query_dim), recall_at, 0, L, query_result_tags + (i * recall_at),
                          query_result_dists + (i * recall_at), beam_width, stats + i);
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

    // 计算召回率
    double recall = pipeann::calculate_recall((uint32_t) query_num, gt_ids, gt_dists, (uint32_t) gt_dim,
                                              query_result_tags, (uint32_t) recall_at, (uint32_t) recall_at);

    // 计算平均IO次数
    double mean_ios = 0.0;
    for (size_t i = 0; i < query_num; i++) {
      mean_ios += stats[i].n_ios;
    }
    mean_ios /= query_num;

    // IO放大率 = 实际IO次数 / 理论最少IO次数
    double io_amplification = mean_ios / recall_at;

    // 输出结果
    ofs << system_names[system_type] << "," << L << "," << recall << "," << qps << "," << avg_lat << "," << p50 << ","
        << p90 << "," << p95 << "," << p99 << "," << mean_ios << "," << io_amplification << "\n";

    std::cout << system_names[system_type] << " L=" << L << ": Recall=" << recall << ", QPS=" << qps << ", P99=" << p99
              << "us, MeanIOs=" << mean_ios << std::endl;

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
    ofs << "system,time_sec,num_inserts,throughput_ops,memory_rss_mb,merge_triggered\n";
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

      ofs << system_names[system_type] << "," << elapsed_sec << "," << current_inserts << "," << throughput << ","
          << (rss_kb / 1024.0) << "," << (merge_done.load() ? 1 : 0) << "\n";
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
                                    uint64_t L, uint32_t beam_width, SystemType system_type,
                                    const std::string &output_file) {
  std::ofstream ofs(output_file, std::ios::app);
  if (ofs.tellp() == 0) {
    ofs << "system,time_sec,search_qps,search_p99_us,insert_ops,insert_tput,memory_rss_mb\n";
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
  auto insert_func = [&]() {
    while (!stop_test.load()) {
      uint64_t idx = insert_count.fetch_add(1);
      if (idx >= insert_num)
        break;

      TagT tag = static_cast<TagT>(idx + 2000000);
      index.insert(insert_data + idx * data_dim, tag);

      // FreshDiskANN模式: 周期性合并
      if (system_type == FRESH_DISKANN && idx % MERGE_INTERVAL == MERGE_INTERVAL - 1) {
        index.final_merge(NUM_SEARCH_THREADS / 2);
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

      ofs << system_names[system_type] << "," << elapsed_sec << "," << search_qps << "," << p99_lat << "," << inserts
          << "," << insert_tput << "," << (rss_kb / 1024.0) << "\n";
      ofs.flush();

      if (inserts >= insert_num) {
        stop_test.store(true);
        break;
      }
    }
  });

  // 等待完成
  for (auto &t : insert_threads) {
    t.join();
  }
  stop_test.store(true);

  for (auto &t : search_threads) {
    t.join();
  }
  monitor.join();

  ofs.close();

  std::cout << "[" << system_names[system_type] << "] Concurrent test completed" << std::endl;
}

/**
 * 主函数
 */
int main(int argc, char **argv) {
  if (argc < 10) {
    std::cout << "Usage: " << argv[0] << " <data_type> <index_prefix> <query_file> <gt_file>"
              << " <insert_data_file> <system_type> <experiment_type>"
              << " <output_dir> <num_threads> [recall_at] [L_values...]\n";
    std::cout << "\nParameters:\n";
    std::cout << "  data_type: uint8/int8/float\n";
    std::cout << "  system_type: 0=DC-PDI, 1=IP-DiskANN, 2=FreshDiskANN\n";
    std::cout << "  experiment_type: 1=search_latency, 2=update_throughput, 3=concurrent\n";
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

  std::vector<uint64_t> L_values;
  if (argc > arg_no) {
    for (int i = arg_no; i < argc; i++) {
      L_values.push_back(atoi(argv[i]));
    }
  } else {
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
      compare_search_latency<uint8_t, uint32_t>(index_prefix, query_file, gt_file, num_threads,
                                                (SystemType) system_type, recall_at, L_values, 4, output);
    } else if (data_type == "int8") {
      compare_search_latency<int8_t, uint32_t>(index_prefix, query_file, gt_file, num_threads, (SystemType) system_type,
                                               recall_at, L_values, 4, output);
    } else if (data_type == "float") {
      compare_search_latency<float, uint32_t>(index_prefix, query_file, gt_file, num_threads, (SystemType) system_type,
                                              recall_at, L_values, 4, output);
    } else {
      std::cerr << "Unsupported data type: " << data_type << std::endl;
      return -1;
    }
  }
  // 实验2和3需要DynamicSSDIndex，暂不实现
  else if (exp_type == 2) {
    std::cout << "Update throughput experiment not yet implemented" << std::endl;
    std::string output = output_dir + "/exp2_update_throughput_" +
                         std::string(system_type == 0   ? "dc-pdi"
                                     : system_type == 1 ? "ip-diskann"
                                                        : "fresh-diskann") +
                         ".csv";
    // Placeholder: create empty file
    std::ofstream ofs(output);
    ofs << "system,time_sec,num_inserts,throughput_ops,memory_rss_mb,merge_triggered\n";
    ofs << system_names[system_type] << ",0,0,0,0,0\n";
    ofs.close();
  } else if (exp_type == 3) {
    std::cout << "Concurrent performance experiment not yet implemented" << std::endl;
    std::string output = output_dir + "/exp3_concurrent_" +
                         std::string(system_type == 0   ? "dc-pdi"
                                     : system_type == 1 ? "ip-diskann"
                                                        : "fresh-diskann") +
                         ".csv";
    // Placeholder: create empty file
    std::ofstream ofs(output);
    ofs << "system,time_sec,search_qps,search_p99_us,insert_ops,insert_tput,memory_rss_mb\n";
    ofs << system_names[system_type] << ",0,0,0,0,0,0\n";
    ofs.close();
  } else {
    std::cerr << "Unknown experiment type: " << exp_type << std::endl;
    return -1;
  }

  std::cout << "Experiment completed successfully!" << std::endl;
  return 0;
}
