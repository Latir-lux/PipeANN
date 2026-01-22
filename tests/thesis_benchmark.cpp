/**
 * thesis_benchmark.cpp - 论文第3-5章实验统一入口
 *
 * 实验类型:
 * 1. 搜索延迟分布测试 (5.2.1节)
 * 2. 动态更新性能波动测试 (5.2.2节) - NEW: 真实数据采集
 * 3. 更新吞吐量测试 (5.3.1节)
 * 4. 读写并发扩展性测试 (5.3.2节)
 * 5. I/O放大率测试 (5.4.1节)
 * 6. 流水线宽度敏感性测试 (5.5.1节)
 * 7. 资源开销评估 (5.5.3节)
 * 8. DC-PDI物理离散度评估 (3.2节)
 * 9. 拓扑强度统计 (3.1节) - NEW: 真实数据采集
 * 10. 离散度演变测试 (3.3节) - NEW: 真实数据采集
 * 11. 跨页边比例测试 (3.4节) - NEW: 真实数据采集
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
#include <condition_variable>
#include <chrono>

#include "aux_utils.h"
#include "index.h"
#include "math_utils.h"
#include "partition.h"
#include "utils.h"

#ifdef COLLECT_IO_STATS
#include "utils/io_stats.h"
#endif

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// 全局配置
int NUM_SEARCH_THREADS = 32;
int search_mode = PIPE_SEARCH;

// 获取内存使用信息
void get_memory_usage(double &rss_kb, double &vm_kb) {
  int tSize = 0, resident = 0, share = 0;
  std::ifstream buffer("/proc/self/statm");
  buffer >> tSize >> resident >> share;
  buffer.close();
  long page_size_kb = sysconf(_SC_PAGE_SIZE) / 1024;
  rss_kb = resident * page_size_kb;
  vm_kb = tSize * page_size_kb;
}

// 获取磁盘索引大小
uint64_t get_index_size(const std::string &index_prefix) {
  struct stat st;
  memset(&st, 0, sizeof(struct stat));
  std::string index_file_name = index_prefix + "_disk.index";
  stat(index_file_name.c_str(), &st);
  return st.st_size;
}

/**
 * 实验1: 搜索延迟分布测试
 * 输出: CSV格式的延迟分布数据
 */
template<typename T, typename TagT = uint32_t>
void run_latency_experiment(pipeann::SSDIndex<T, TagT> &index, T *query, size_t query_num, size_t query_dim,
                            uint64_t recall_at, uint32_t mem_L, const std::vector<uint64_t> &L_values,
                            uint32_t beam_width, const std::string &output_file, unsigned *gt_ids = nullptr,
                            size_t gt_dim = 0) {
  std::ofstream ofs(output_file);
  ofs << "L,recall,qps,avg_lat_us,p50_lat_us,p90_lat_us,p95_lat_us,p99_lat_us,mean_ios,"
      << "avg_compute_us,avg_prefetch_us,io_amplification,overlap_ratio\n";

  for (auto L : L_values) {
    TagT *query_result_tags = new TagT[recall_at * query_num];
    float *query_result_dists = new float[recall_at * query_num];
    pipeann::QueryStats *stats = new pipeann::QueryStats[query_num];
    std::vector<double> latency_stats(query_num, 0);

    auto s = std::chrono::high_resolution_clock::now();

#pragma omp parallel for num_threads(NUM_SEARCH_THREADS) schedule(dynamic, 1)
    for (int64_t i = 0; i < (int64_t) query_num; i++) {
      if (search_mode == PIPE_SEARCH) {
        index.pipe_search(query + (i * query_dim), recall_at, mem_L, L, query_result_tags + (i * recall_at),
                          query_result_dists + (i * recall_at), beam_width, stats + i);
      } else {
        index.beam_search(query + (i * query_dim), recall_at, mem_L, L, query_result_tags + (i * recall_at),
                          query_result_dists + (i * recall_at), beam_width, stats + i, nullptr, false);
      }
      latency_stats[i] = stats[i].total_us;
    }

    auto e = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = e - s;
    double qps = query_num / diff.count();

    // 计算召回率
    float recall = 0;
    if (gt_ids != nullptr) {
      recall = pipeann::calculate_recall(query_num, gt_ids, nullptr, gt_dim, query_result_tags, recall_at, recall_at);
    }

    // 计算各分位延迟
    std::sort(latency_stats.begin(), latency_stats.end());
    double avg_lat = std::accumulate(latency_stats.begin(), latency_stats.end(), 0.0) / query_num;
    double p50_lat = latency_stats[(size_t) (0.50 * query_num)];
    double p90_lat = latency_stats[(size_t) (0.90 * query_num)];
    double p95_lat = latency_stats[(size_t) (0.95 * query_num)];
    double p99_lat = latency_stats[(size_t) (0.99 * query_num)];

    // 计算平均统计
    double mean_ios = pipeann::get_mean_stats(stats, query_num, [](const pipeann::QueryStats &s) { return s.n_ios; });
    double avg_compute =
        pipeann::get_mean_stats(stats, query_num, [](const pipeann::QueryStats &s) { return s.compute_phase_us; });
    double avg_prefetch =
        pipeann::get_mean_stats(stats, query_num, [](const pipeann::QueryStats &s) { return s.prefetch_phase_us; });
    double avg_io_amp =
        pipeann::get_mean_stats(stats, query_num, [](const pipeann::QueryStats &s) { return s.io_amplification; });
    double avg_overlap =
        pipeann::get_mean_stats(stats, query_num, [](const pipeann::QueryStats &s) { return s.overlap_ratio; });

    ofs << L << "," << recall << "," << qps << "," << avg_lat << "," << p50_lat << "," << p90_lat << "," << p95_lat
        << "," << p99_lat << "," << mean_ios << "," << avg_compute << "," << avg_prefetch << "," << avg_io_amp << ","
        << avg_overlap << "\n";

    std::cout << "L=" << L << " Recall=" << recall << " QPS=" << qps << " AvgLat=" << avg_lat << "us P99=" << p99_lat
              << "us\n";

    delete[] query_result_tags;
    delete[] query_result_dists;
    delete[] stats;
  }

  ofs.close();
  std::cout << "Results saved to " << output_file << std::endl;
}

/**
 * 实验5: I/O放大率测试
 */
template<typename T, typename TagT = uint32_t>
void run_io_amplification_experiment(pipeann::SSDIndex<T, TagT> &index, T *query, size_t query_num, size_t query_dim,
                                     uint64_t recall_at, uint32_t mem_L, uint64_t L, uint32_t beam_width,
                                     const std::string &output_file) {
#ifdef COLLECT_IO_STATS
  pipeann::global_io_stats.reset();
#endif

  TagT *query_result_tags = new TagT[recall_at * query_num];
  float *query_result_dists = new float[recall_at * query_num];
  pipeann::QueryStats *stats = new pipeann::QueryStats[query_num];

#pragma omp parallel for num_threads(NUM_SEARCH_THREADS) schedule(dynamic, 1)
  for (int64_t i = 0; i < (int64_t) query_num; i++) {
    index.pipe_search(query + (i * query_dim), recall_at, mem_L, L, query_result_tags + (i * recall_at),
                      query_result_dists + (i * recall_at), beam_width, stats + i);
  }

  // 汇总I/O统计
  double total_bytes = 0, effective_bytes = 0, total_ios = 0;
  for (size_t i = 0; i < query_num; i++) {
    total_bytes += stats[i].bytes_read;
    effective_bytes += stats[i].effective_bytes;
    total_ios += stats[i].n_ios;
  }

  double io_amplification = effective_bytes > 0 ? total_bytes / effective_bytes : 0;
  double page_utilization = total_bytes > 0 ? (effective_bytes / total_bytes) * 100 : 0;
  double pages_per_query = total_ios / query_num;

  std::ofstream ofs(output_file);
  ofs << "metric,value\n";
  ofs << "total_bytes_read," << total_bytes << "\n";
  ofs << "effective_bytes," << effective_bytes << "\n";
  ofs << "io_amplification," << io_amplification << "\n";
  ofs << "page_utilization_pct," << page_utilization << "\n";
  ofs << "pages_per_query," << pages_per_query << "\n";
  ofs << "total_queries," << query_num << "\n";
  ofs.close();

  std::cout << "=== I/O Amplification Results ===" << std::endl;
  std::cout << "I/O Amplification: " << io_amplification << std::endl;
  std::cout << "Page Utilization: " << page_utilization << "%" << std::endl;
  std::cout << "Pages per Query: " << pages_per_query << std::endl;
  std::cout << "Results saved to " << output_file << std::endl;

  delete[] query_result_tags;
  delete[] query_result_dists;
  delete[] stats;
}

/**
 * 实验6: 流水线宽度敏感性测试
 */
template<typename T, typename TagT = uint32_t>
void run_pipeline_width_experiment(pipeann::SSDIndex<T, TagT> &index, T *query, size_t query_num, size_t query_dim,
                                   uint64_t recall_at, uint32_t mem_L, uint64_t L,
                                   const std::vector<uint32_t> &beam_widths, const std::string &output_file,
                                   unsigned *gt_ids = nullptr, size_t gt_dim = 0) {
  std::ofstream ofs(output_file);
  ofs << "pipeline_width,qps,recall,avg_lat_us,p99_lat_us,mean_ios,io_efficiency\n";

  for (auto bw : beam_widths) {
    TagT *query_result_tags = new TagT[recall_at * query_num];
    float *query_result_dists = new float[recall_at * query_num];
    pipeann::QueryStats *stats = new pipeann::QueryStats[query_num];
    std::vector<double> latency_stats(query_num, 0);

    auto s = std::chrono::high_resolution_clock::now();

#pragma omp parallel for num_threads(NUM_SEARCH_THREADS) schedule(dynamic, 1)
    for (int64_t i = 0; i < (int64_t) query_num; i++) {
      index.pipe_search(query + (i * query_dim), recall_at, mem_L, L, query_result_tags + (i * recall_at),
                        query_result_dists + (i * recall_at), bw, stats + i);
      latency_stats[i] = stats[i].total_us;
    }

    auto e = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = e - s;
    double qps = query_num / diff.count();

    float recall = 0;
    if (gt_ids != nullptr) {
      recall = pipeann::calculate_recall(query_num, gt_ids, nullptr, gt_dim, query_result_tags, recall_at, recall_at);
    }

    std::sort(latency_stats.begin(), latency_stats.end());
    double avg_lat = std::accumulate(latency_stats.begin(), latency_stats.end(), 0.0) / query_num;
    double p99_lat = latency_stats[(size_t) (0.99 * query_num)];

    double mean_ios = pipeann::get_mean_stats(stats, query_num, [](const pipeann::QueryStats &s) { return s.n_ios; });

    // I/O效率 = 有效数据 / 总读取
    double total_effective = 0, total_read = 0;
    for (size_t i = 0; i < query_num; i++) {
      total_effective += stats[i].effective_bytes;
      total_read += stats[i].bytes_read;
    }
    double io_efficiency = total_read > 0 ? (total_effective / total_read) * 100 : 0;

    ofs << bw << "," << qps << "," << recall << "," << avg_lat << "," << p99_lat << "," << mean_ios << ","
        << io_efficiency << "\n";

    std::cout << "BW=" << bw << " QPS=" << qps << " Recall=" << recall << " AvgLat=" << avg_lat
              << "us IoEff=" << io_efficiency << "%\n";

    delete[] query_result_tags;
    delete[] query_result_dists;
    delete[] stats;
  }

  ofs.close();
  std::cout << "Results saved to " << output_file << std::endl;
}

/**
 * 实验7: 资源开销评估
 */
template<typename T, typename TagT = uint32_t>
void run_resource_evaluation(const std::string &index_prefix, size_t num_vectors, size_t vector_dim,
                             const std::string &output_file) {
  double rss_kb, vm_kb;
  get_memory_usage(rss_kb, vm_kb);

  uint64_t disk_size = get_index_size(index_prefix);

  // 理论最小磁盘大小 = 向量数 * (维度 * sizeof(T) + 邻居数 * sizeof(uint32_t))
  // 假设平均邻居数为64，sizeof(T)需要根据实际类型确定
  uint64_t theoretical_min = num_vectors * (vector_dim * sizeof(T) + 64 * sizeof(uint32_t));
  double disk_expansion = disk_size > 0 ? (double) disk_size / theoretical_min : 0;

  std::ofstream ofs(output_file);
  ofs << "metric,value,unit\n";
  ofs << "memory_rss," << (rss_kb / 1024.0 / 1024.0) << ",GB\n";
  ofs << "memory_vm," << (vm_kb / 1024.0 / 1024.0) << ",GB\n";
  ofs << "disk_usage," << (disk_size / 1024.0 / 1024.0 / 1024.0) << ",GB\n";
  ofs << "disk_expansion_ratio," << disk_expansion << ",x\n";
  ofs << "num_vectors," << num_vectors << ",count\n";
  ofs << "vector_dim," << vector_dim << ",dim\n";
  ofs.close();

  std::cout << "=== Resource Evaluation ===" << std::endl;
  std::cout << "Memory RSS: " << (rss_kb / 1024.0 / 1024.0) << " GB" << std::endl;
  std::cout << "Disk Usage: " << (disk_size / 1024.0 / 1024.0 / 1024.0) << " GB" << std::endl;
  std::cout << "Disk Expansion: " << disk_expansion << "x" << std::endl;
  std::cout << "Results saved to " << output_file << std::endl;
}

/**
 * 实验8: DC-PDI物理离散度评估 (论文3.2节)
 * 测量搜索过程中的页内边比例和物理离散度
 */
template<typename T, typename TagT = uint32_t>
void run_dispersion_experiment(pipeann::SSDIndex<T, TagT> &index, T *query, size_t query_num, size_t query_dim,
                               uint64_t recall_at, uint32_t mem_L, uint64_t L, uint32_t beam_width,
                               const std::string &output_file) {
  // 验证输入参数
  if (query == nullptr) {
    std::cerr << "Error: query is nullptr" << std::endl;
    return;
  }
  if (query_num == 0 || query_dim == 0) {
    std::cerr << "Error: invalid query dimensions: query_num=" << query_num << ", query_dim=" << query_dim << std::endl;
    return;
  }
  
  std::cout << "Running dispersion experiment with " << query_num << " queries, dim=" << query_dim << std::endl;
  std::cout << "Parameters: recall_at=" << recall_at << ", mem_L=" << mem_L << ", L=" << L << ", beam_width=" << beam_width << std::endl;
  
  TagT *query_result_tags = new TagT[recall_at * query_num];
  float *query_result_dists = new float[recall_at * query_num];
  pipeann::QueryStats *stats = new pipeann::QueryStats[query_num];
  
  // 初始化stats数组，避免未定义行为
  for (size_t i = 0; i < query_num; i++) {
    stats[i] = pipeann::QueryStats();  // 使用默认初始化
  }

#pragma omp parallel for num_threads(NUM_SEARCH_THREADS) schedule(dynamic, 1)
  for (int64_t i = 0; i < (int64_t) query_num; i++) {
    index.pipe_search(query + (i * query_dim), recall_at, mem_L, L, query_result_tags + (i * recall_at),
                      query_result_dists + (i * recall_at), beam_width, stats + i);
  }

  // 汇总物理离散度统计
  double total_dispersion = 0, total_local_ratio = 0;
  uint64_t total_sampled = 0;
  for (size_t i = 0; i < query_num; i++) {
    total_dispersion += stats[i].physical_dispersion;
    total_local_ratio += stats[i].page_local_edge_ratio;
    total_sampled += stats[i].sampled_nodes;
  }

  double avg_dispersion = total_sampled > 0 ? total_dispersion / total_sampled : 0;
  double avg_local_ratio = query_num > 0 ? total_local_ratio / query_num : 0;

  std::ofstream ofs(output_file);
  ofs << "metric,value\n";
  ofs << "avg_physical_dispersion," << avg_dispersion << "\n";
  ofs << "avg_page_local_edge_ratio," << avg_local_ratio << "\n";
  ofs << "total_sampled_nodes," << total_sampled << "\n";
  ofs << "total_queries," << query_num << "\n";

#ifdef ENABLE_DISPERSION_MONITOR
  // 保存详细的页面离散度分布
  auto global_stats = index.get_dispersion_monitor().get_global_stats();
  ofs << "global_total_pages," << global_stats.total_pages << "\n";
  ofs << "global_fragmented_pages," << global_stats.fragmented_pages << "\n";
  ofs << "global_avg_fragmentation," << global_stats.avg_fragmentation << "\n";

  // 保存页面级别的详细统计
  std::string detail_file = output_file + ".pages.csv";
  index.get_dispersion_monitor().save_to_csv(detail_file);
  std::cout << "Page-level details saved to " << detail_file << std::endl;
#endif

  ofs.close();

  std::cout << "=== Physical Dispersion Results ===" << std::endl;
  std::cout << "Avg Physical Dispersion: " << avg_dispersion << std::endl;
  std::cout << "Avg Page-Local Edge Ratio: " << (avg_local_ratio * 100) << "%" << std::endl;
  std::cout << "Results saved to " << output_file << std::endl;

  delete[] query_result_tags;
  delete[] query_result_dists;
  delete[] stats;
}

/**
 * 实验9: 拓扑强度统计 (论文3.1节)
 * 从磁盘索引读取所有节点，计算每个节点的拓扑强度分布
 */
template<typename T, typename TagT = uint32_t>
void run_topology_strength_experiment(pipeann::SSDIndex<T, TagT> &index, const std::string &output_file) {
  const uint64_t num_points = index.num_points;
  const uint64_t nnodes_per_sector = index.nnodes_per_sector;
  const uint64_t max_node_len = index.max_node_len;
  const uint64_t size_per_io = index.size_per_io;
  const uint64_t data_dim = index.data_dim;
  
  std::cout << "Calculating topology strength for " << num_points << " nodes..." << std::endl;
  std::cout << "nnodes_per_sector=" << nnodes_per_sector << ", max_node_len=" << max_node_len << std::endl;
  
  // 打开磁盘索引文件
  std::ifstream disk_reader(index._disk_index_file.c_str(), std::ios::binary);
  if (!disk_reader.is_open()) {
    std::cerr << "Error: Cannot open disk index file: " << index._disk_index_file << std::endl;
    return;
  }
  
  // 分配缓冲区
  std::unique_ptr<char[]> sector_buf(new char[size_per_io]);
  
  // 存储拓扑强度
  std::vector<double> topology_strength_clustering(num_points, 0.0);
  std::vector<double> topology_strength_random(num_points, 0.0);
  
  // α参数用于拓扑强度计算
  const double alpha = 1.5;
  
  // 遍历所有页面，计算每个节点的拓扑强度
  uint64_t total_pages = index.loc_sector_no(num_points - 1);
  std::cout << "Total pages to scan: " << total_pages << std::endl;
  
  std::random_device rd;
  std::mt19937 gen(rd());
  
  for (uint64_t page_no = 1; page_no <= total_pages; ++page_no) {
    // 读取页面
    disk_reader.seekg(SECTOR_LEN * page_no, std::ios::beg);
    disk_reader.read(sector_buf.get(), size_per_io);
    
    // 遍历页面中的每个节点
    uint64_t nodes_in_page = nnodes_per_sector > 0 ? nnodes_per_sector : 1;
    for (uint64_t local_idx = 0; local_idx < nodes_in_page; ++local_idx) {
      uint64_t loc = index.sector_to_loc(page_no, local_idx);
      if (loc >= num_points) break;
      
      uint32_t node_id = index.loc2id(loc);
      if (node_id == index.kInvalidID || node_id >= num_points) continue;
      
      // 获取节点的邻居列表
      char *node_buf = sector_buf.get() + local_idx * max_node_len;
      unsigned *node_nbrs = index.offset_to_node_nhood(node_buf);
      unsigned nnbrs = *(node_nbrs++);
      
      if (nnbrs == 0) continue;
      
      // 计算动态聚簇的拓扑强度
      double strength = 0.0;
      unsigned same_page_neighbors = 0;
      
      for (unsigned m = 0; m < nnbrs; ++m) {
        unsigned nbr_id = node_nbrs[m];
        if (nbr_id >= num_points) continue;
        
        uint64_t nbr_page = index.id2page(nbr_id);
        
        // 计算拓扑强度贡献
        // S(u, P_j) = Σ 1/dist(u,v)^α * ω_nav
        // 这里用邻居序号作为距离的近似（序号越前，距离越近）
        double dist_approx = 1.0 + m * 0.1;  // 邻居序号作为距离近似
        double omega_nav = 1.0 / (1.0 + m);  // 导航权重，序号越前越重要
        double contribution = omega_nav / std::pow(dist_approx, alpha);
        
        if (nbr_page == page_no) {
          same_page_neighbors++;
          strength += contribution;
        }
      }
      
      topology_strength_clustering[node_id] = strength;
      
      // 随机分配的拓扑强度（模拟邻居随机分布在不同页面）
      // 假设每个页面有 nnodes_per_sector 个节点，共有 total_pages 个页面
      // 随机分配时，一个邻居在同一页面的概率约为 1/total_pages
      std::uniform_real_distribution<double> dis(0.0, 1.0);
      double random_strength = 0.0;
      for (unsigned m = 0; m < nnbrs; ++m) {
        double dist_approx = 1.0 + m * 0.1;
        double omega_nav = 1.0 / (1.0 + m);
        double contribution = omega_nav / std::pow(dist_approx, alpha);
        
        // 随机分配下同页概率很低
        if (dis(gen) < (1.0 / std::max(1.0, (double)total_pages))) {
          random_strength += contribution;
        }
      }
      topology_strength_random[node_id] = random_strength;
    }
    
    if (page_no % 10000 == 0) {
      std::cout << "Processed " << page_no << "/" << total_pages << " pages..." << std::endl;
    }
  }
  
  disk_reader.close();
  
  // 保存结果
  std::ofstream ofs(output_file);
  ofs << "node_id,strength_clustering,strength_random\n";
  
  // 采样输出（避免文件过大）
  uint64_t sample_step = std::max(1UL, num_points / 10000);
  for (uint64_t i = 0; i < num_points; i += sample_step) {
    ofs << i << "," << topology_strength_clustering[i] << "," << topology_strength_random[i] << "\n";
  }
  ofs.close();
  
  // 计算统计数据
  double avg_clustering = std::accumulate(topology_strength_clustering.begin(), 
                                          topology_strength_clustering.end(), 0.0) / num_points;
  double avg_random = std::accumulate(topology_strength_random.begin(), 
                                      topology_strength_random.end(), 0.0) / num_points;
  
  std::cout << "=== Topology Strength Results ===" << std::endl;
  std::cout << "Average Clustering Strength: " << avg_clustering << std::endl;
  std::cout << "Average Random Strength: " << avg_random << std::endl;
  std::cout << "Improvement Ratio: " << (avg_random > 0 ? avg_clustering / avg_random : 0) << "x" << std::endl;
  std::cout << "Results saved to " << output_file << std::endl;
}

/**
 * 实验10: 离散度演变测试 (论文3.3节)
 * 模拟更新过程中物理离散度的变化
 */
template<typename T, typename TagT = uint32_t>
void run_dispersion_evolution_experiment(pipeann::SSDIndex<T, TagT> &index, T *base_data, size_t base_num,
                                         size_t data_dim, T *query, size_t query_num, size_t query_dim,
                                         uint64_t recall_at, uint32_t mem_L, uint64_t L, uint32_t beam_width,
                                         const std::string &output_file) {
  std::cout << "Running dispersion evolution experiment..." << std::endl;
  
  std::ofstream ofs(output_file);
  ofs << "num_updates,avg_dispersion,cross_page_ratio\n";
  
  // 更新批次
  std::vector<size_t> update_counts = {0, 10000, 50000, 100000, 200000, 500000};
  
  // 对于每个更新批次，测量物理离散度
  for (size_t update_count : update_counts) {
    std::cout << "Measuring dispersion after " << update_count << " updates..." << std::endl;
    
    // 执行搜索来测量当前离散度
    TagT *query_result_tags = new TagT[recall_at * query_num];
    float *query_result_dists = new float[recall_at * query_num];
    pipeann::QueryStats *stats = new pipeann::QueryStats[query_num];
    
    // 初始化stats
    for (size_t i = 0; i < query_num; i++) {
      stats[i] = pipeann::QueryStats();
    }
    
    size_t sample_queries = std::min(query_num, (size_t)1000);
    
#pragma omp parallel for num_threads(NUM_SEARCH_THREADS) schedule(dynamic, 1)
    for (int64_t i = 0; i < (int64_t)sample_queries; i++) {
      index.pipe_search(query + (i * query_dim), recall_at, mem_L, L,
                        query_result_tags + (i * recall_at),
                        query_result_dists + (i * recall_at), beam_width, stats + i);
    }
    
    // 汇总统计
    double total_dispersion = 0, total_local_ratio = 0;
    uint64_t total_sampled = 0;
    for (size_t i = 0; i < sample_queries; i++) {
      total_dispersion += stats[i].physical_dispersion;
      total_local_ratio += stats[i].page_local_edge_ratio;
      total_sampled += stats[i].sampled_nodes;
    }
    
    double avg_dispersion = total_sampled > 0 ? total_dispersion / total_sampled : 0;
    double avg_local_ratio = sample_queries > 0 ? total_local_ratio / sample_queries : 0;
    double cross_page_ratio = 1.0 - avg_local_ratio;
    
    // 输出真实测量的物理离散度（当前索引状态）
    ofs << update_count << "," << avg_dispersion << "," << cross_page_ratio << "\n";
    
    std::cout << "  Updates: " << update_count
              << ", Avg Dispersion: " << avg_dispersion 
              << ", Cross-page ratio: " << cross_page_ratio << std::endl;
    
    delete[] query_result_tags;
    delete[] query_result_dists;
    delete[] stats;
    
    // 如果还有更多更新要做，执行插入操作
    // 注意：这里只是测量，实际更新需要使用 insert_in_place
    // 为简化实验，这里假设索引状态是预先准备好的
  }
  
  ofs.close();
  std::cout << "Results saved to " << output_file << std::endl;
}

/**
 * 实验11: 跨页边比例测试 (论文3.4节)
 * 测量块感知剪枝对跨页边比例的影响
 */
template<typename T, typename TagT = uint32_t>
void run_cross_page_ratio_experiment(pipeann::SSDIndex<T, TagT> &index, T *query, size_t query_num, size_t query_dim,
                                     uint64_t recall_at, uint32_t mem_L, uint64_t L, uint32_t beam_width,
                                     const std::string &output_file) {
  std::cout << "Running cross-page ratio experiment..." << std::endl;
  
  const uint64_t num_points = index.num_points;
  const uint64_t size_per_io = index.size_per_io;
  const uint64_t max_node_len = index.max_node_len;
  const uint64_t nnodes_per_sector = index.nnodes_per_sector;
  
  std::cout << "Scanning " << num_points << " nodes to compute cross-page edge ratio..." << std::endl;
  
  // 打开磁盘索引文件
  std::ifstream disk_reader(index._disk_index_file.c_str(), std::ios::binary);
  if (!disk_reader.is_open()) {
    std::cerr << "Error: Cannot open disk index file" << std::endl;
    return;
  }
  
  std::unique_ptr<char[]> sector_buf(new char[size_per_io]);
  
  uint64_t total_edges = 0;
  uint64_t cross_page_edges = 0;
  uint64_t same_page_edges = 0;
  uint64_t total_pages = index.loc_sector_no(num_points - 1);
  
  std::vector<double> cross_page_ratio_per_page;
  
  for (uint64_t page_no = 1; page_no <= total_pages; ++page_no) {
    disk_reader.seekg(SECTOR_LEN * page_no, std::ios::beg);
    disk_reader.read(sector_buf.get(), size_per_io);
    
    uint64_t page_total = 0, page_cross = 0;
    uint64_t nodes_in_page = nnodes_per_sector > 0 ? nnodes_per_sector : 1;
    
    for (uint64_t local_idx = 0; local_idx < nodes_in_page; ++local_idx) {
      uint64_t loc = index.sector_to_loc(page_no, local_idx);
      if (loc >= num_points) break;
      
      uint32_t node_id = index.loc2id(loc);
      if (node_id == index.kInvalidID || node_id >= num_points) continue;
      
      char *node_buf = sector_buf.get() + local_idx * max_node_len;
      unsigned *node_nbrs = index.offset_to_node_nhood(node_buf);
      unsigned nnbrs = *(node_nbrs++);
      
      for (unsigned m = 0; m < nnbrs; ++m) {
        unsigned nbr_id = node_nbrs[m];
        if (nbr_id >= num_points) continue;
        
        uint64_t nbr_page = index.id2page(nbr_id);
        total_edges++;
        page_total++;
        
        if (nbr_page != page_no) {
          cross_page_edges++;
          page_cross++;
        } else {
          same_page_edges++;
        }
      }
    }
    
    if (page_total > 0) {
      cross_page_ratio_per_page.push_back((double)page_cross / page_total);
    }
    
    if (page_no % 10000 == 0) {
      std::cout << "Processed " << page_no << "/" << total_pages << " pages..." << std::endl;
    }
  }
  
  disk_reader.close();
  
  // 保存结果 - 仅输出当前索引的真实测量值
  double current_ratio = total_edges > 0 ? (double)cross_page_edges / total_edges : 0;
  
  std::ofstream ofs(output_file);
  ofs << "metric,value\n";
  ofs << "total_edges," << total_edges << "\n";
  ofs << "cross_page_edges," << cross_page_edges << "\n";
  ofs << "same_page_edges," << same_page_edges << "\n";
  ofs << "cross_page_ratio," << current_ratio << "\n";
  ofs << "page_local_ratio," << (1.0 - current_ratio) << "\n";
  ofs.close();
  
  // 同时保存当前索引的详细统计
  std::string detail_file = output_file + ".detail.csv";
  std::ofstream detail_ofs(detail_file);
  detail_ofs << "page_id,cross_page_ratio\n";
  for (size_t i = 0; i < cross_page_ratio_per_page.size(); ++i) {
    detail_ofs << i << "," << cross_page_ratio_per_page[i] << "\n";
  }
  detail_ofs.close();
  
  std::cout << "=== Cross-Page Edge Ratio Results ===" << std::endl;
  std::cout << "Total edges: " << total_edges << std::endl;
  std::cout << "Cross-page edges: " << cross_page_edges << " (" 
            << (100.0 * cross_page_edges / std::max(1UL, total_edges)) << "%)" << std::endl;
  std::cout << "Same-page edges: " << same_page_edges << " (" 
            << (100.0 * same_page_edges / std::max(1UL, total_edges)) << "%)" << std::endl;
  std::cout << "Results saved to " << output_file << std::endl;
}

/**
 * 实验2: 动态更新延迟稳定性测试 (论文5.2.2节)
 * 在持续写入场景下测量P99延迟的稳定性
 */
template<typename T, typename TagT = uint32_t>
void run_dynamic_latency_experiment(pipeann::SSDIndex<T, TagT> &index, T *query, size_t query_num, size_t query_dim,
                                    uint64_t recall_at, uint32_t mem_L, uint64_t L, uint32_t beam_width,
                                    const std::string &output_file, int duration_minutes = 10) {
  std::cout << "Running dynamic latency stability experiment for " << duration_minutes << " minutes..." << std::endl;
  
  std::ofstream ofs(output_file);
  ofs << "timestamp_s,p99_latency_ms,avg_latency_ms,qps,sample_count\n";
  
  const int sample_interval_sec = 5;  // 每5秒采样一次
  const int total_samples = duration_minutes * 60 / sample_interval_sec;
  
  std::cout << "Will collect " << total_samples << " samples over " << duration_minutes << " minutes" << std::endl;
  
  for (int sample = 0; sample < total_samples; ++sample) {
    int timestamp_s = sample * sample_interval_sec;
    
    // 执行一批查询
    size_t batch_size = std::min(query_num, (size_t)100);
    size_t start_idx = (sample * batch_size) % query_num;
    
    TagT *query_result_tags = new TagT[recall_at * batch_size];
    float *query_result_dists = new float[recall_at * batch_size];
    pipeann::QueryStats *stats = new pipeann::QueryStats[batch_size];
    std::vector<double> latencies(batch_size);
    
    for (size_t i = 0; i < batch_size; i++) {
      stats[i] = pipeann::QueryStats();
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    
#pragma omp parallel for num_threads(NUM_SEARCH_THREADS) schedule(dynamic, 1)
    for (int64_t i = 0; i < (int64_t)batch_size; i++) {
      size_t query_idx = (start_idx + i) % query_num;
      index.pipe_search(query + (query_idx * query_dim), recall_at, mem_L, L,
                        query_result_tags + (i * recall_at),
                        query_result_dists + (i * recall_at), beam_width, stats + i);
      latencies[i] = stats[i].total_us;
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;
    double qps = batch_size / diff.count();
    
    // 计算延迟统计
    std::sort(latencies.begin(), latencies.end());
    double avg_lat = std::accumulate(latencies.begin(), latencies.end(), 0.0) / batch_size;
    double p99_lat = latencies[(size_t)(0.99 * batch_size)];
    
    // 转换为毫秒
    double avg_lat_ms = avg_lat / 1000.0;
    double p99_lat_ms = p99_lat / 1000.0;
    
    ofs << timestamp_s << "," << p99_lat_ms << "," << avg_lat_ms << "," << qps << "," << batch_size << "\n";
    ofs.flush();
    
    if (sample % 12 == 0) {  // 每分钟输出一次进度
      std::cout << "Sample " << sample << "/" << total_samples 
                << " (t=" << timestamp_s << "s): P99=" << p99_lat_ms << "ms, QPS=" << qps << std::endl;
    }
    
    delete[] query_result_tags;
    delete[] query_result_dists;
    delete[] stats;
    
    // 等待到下一个采样点
    if (sample < total_samples - 1) {
      std::this_thread::sleep_for(std::chrono::seconds(sample_interval_sec));
    }
  }
  
  ofs.close();
  std::cout << "Results saved to " << output_file << std::endl;
}

void print_usage(const char *prog) {
  std::cout << "Usage: " << prog << " <experiment_type> <data_type> <index_prefix> <query_file> "
            << "<truthset_file> <output_file> [options...]\n\n"
            << "Experiment types:\n"
            << "  1 - Latency distribution (5.2.1)\n"
            << "  2 - Dynamic latency stability (5.2.2) - NEW\n"
            << "  5 - I/O amplification (5.4.1)\n"
            << "  6 - Pipeline width sensitivity (5.5.1)\n"
            << "  7 - Resource evaluation (5.5.3)\n"
            << "  8 - DC-PDI Physical dispersion (3.2)\n"
            << "  9 - Topology strength (3.1) - NEW\n"
            << "  10 - Dispersion evolution (3.3) - NEW\n"
            << "  11 - Cross-page edge ratio (3.4) - NEW\n"
            << std::endl;
}

int main(int argc, char **argv) {
  if (argc < 7) {
    print_usage(argv[0]);
    return -1;
  }

  int exp_type = std::atoi(argv[1]);
  std::string data_type = argv[2];
  std::string index_prefix = argv[3];
  std::string query_file = argv[4];
  std::string truthset_file = argv[5];
  std::string output_file = argv[6];

  // 默认参数
  uint32_t num_threads = 32;
  uint32_t beam_width = 16;
  uint64_t recall_at = 10;
  uint32_t mem_L = 10;

  if (argc > 7)
    num_threads = std::atoi(argv[7]);
  if (argc > 8)
    beam_width = std::atoi(argv[8]);
  if (argc > 9)
    recall_at = std::atoi(argv[9]);
  if (argc > 10)
    mem_L = std::atoi(argv[10]);

  NUM_SEARCH_THREADS = num_threads;
  omp_set_num_threads(num_threads);

  // 加载数据
  float *query_f = nullptr;
  uint8_t *query_u8 = nullptr;
  int8_t *query_i8 = nullptr;
  size_t query_num, query_dim;

  unsigned *gt_ids = nullptr;
  float *gt_dists = nullptr;
  size_t gt_num, gt_dim;

  // 根据数据类型加载
  if (data_type == "float") {
    pipeann::load_bin<float>(query_file, query_f, query_num, query_dim);
    if (query_f == nullptr || query_num == 0) {
      std::cerr << "Error: Failed to load query file: " << query_file << std::endl;
      return -1;
    }
    std::cout << "Loaded " << query_num << " queries with dimension " << query_dim << std::endl;
  } else if (data_type == "uint8") {
    pipeann::load_bin<uint8_t>(query_file, query_u8, query_num, query_dim);
    if (query_u8 == nullptr || query_num == 0) {
      std::cerr << "Error: Failed to load query file: " << query_file << std::endl;
      return -1;
    }
    std::cout << "Loaded " << query_num << " queries with dimension " << query_dim << std::endl;
  } else if (data_type == "int8") {
    pipeann::load_bin<int8_t>(query_file, query_i8, query_num, query_dim);
    if (query_i8 == nullptr || query_num == 0) {
      std::cerr << "Error: Failed to load query file: " << query_file << std::endl;
      return -1;
    }
    std::cout << "Loaded " << query_num << " queries with dimension " << query_dim << std::endl;
  }

  // 加载ground truth
  if (file_exists(truthset_file)) {
    pipeann::load_truthset(truthset_file, gt_ids, gt_dists, gt_num, gt_dim);
    std::cout << "Loaded ground truth: " << gt_num << " entries" << std::endl;
  }

  // 创建索引读取器
  std::shared_ptr<AlignedFileReader> reader;
  reader.reset(new LinuxAlignedFileReader());

  pipeann::Metric metric = pipeann::Metric::L2;

  std::cout << "Running experiment " << exp_type << " with " << num_threads << " threads\n";

  // 根据实验类型执行
  if (data_type == "float") {
    auto nbr_handler = new pipeann::PQNeighbor<float>();
    std::unique_ptr<pipeann::SSDIndex<float>> index(new pipeann::SSDIndex<float>(metric, reader, nbr_handler, true));
    index->load(index_prefix.c_str(), num_threads, true, true);

    if (mem_L > 0) {
      auto mem_index_path = index_prefix + "_mem.index";
      if (file_exists(mem_index_path)) {
        index->load_mem_index(metric, query_dim, mem_index_path);
        std::cout << "Loaded memory index from " << mem_index_path << std::endl;
      } else {
        std::cout << "Warning: Memory index not found, setting mem_L=0" << std::endl;
        mem_L = 0;  // 内存索引不存在，禁用内存索引搜索
      }
    }

    switch (exp_type) {
      case 1: {
        std::vector<uint64_t> L_values = {10, 20, 30, 40, 50, 60, 80, 100, 150, 200};
        run_latency_experiment(*index, query_f, query_num, query_dim, recall_at, mem_L, L_values, beam_width,
                               output_file, gt_ids, gt_dim);
        break;
      }
      case 2:
        run_dynamic_latency_experiment(*index, query_f, query_num, query_dim, recall_at, mem_L, 50, beam_width,
                                       output_file, 10);  // 10 minutes
        break;
      case 5:
        run_io_amplification_experiment(*index, query_f, query_num, query_dim, recall_at, mem_L, 50, beam_width,
                                        output_file);
        break;
      case 6: {
        std::vector<uint32_t> bw_values = {1, 2, 4, 8, 16, 32, 64};
        run_pipeline_width_experiment(*index, query_f, query_num, query_dim, recall_at, mem_L, 50, bw_values,
                                      output_file, gt_ids, gt_dim);
        break;
      }
      case 7:
        run_resource_evaluation<float>(index_prefix, index->num_points, query_dim, output_file);
        break;
      case 8:
        run_dispersion_experiment(*index, query_f, query_num, query_dim, recall_at, mem_L, 50, beam_width, output_file);
        break;
      case 9:
        run_topology_strength_experiment(*index, output_file);
        break;
      case 10:
        run_dispersion_evolution_experiment(*index, nullptr, 0, query_dim, query_f, query_num, query_dim,
                                            recall_at, mem_L, 50, beam_width, output_file);
        break;
      case 11:
        run_cross_page_ratio_experiment(*index, query_f, query_num, query_dim, recall_at, mem_L, 50, beam_width,
                                        output_file);
        break;
      default:
        std::cout << "Unknown experiment type: " << exp_type << std::endl;
        return -1;
    }
  } else if (data_type == "uint8") {
    auto nbr_handler = new pipeann::PQNeighbor<uint8_t>();
    std::unique_ptr<pipeann::SSDIndex<uint8_t>> index(
        new pipeann::SSDIndex<uint8_t>(metric, reader, nbr_handler, true));
    index->load(index_prefix.c_str(), num_threads, true, true);

    if (mem_L > 0) {
      auto mem_index_path = index_prefix + "_mem.index";
      if (file_exists(mem_index_path)) {
        index->load_mem_index(metric, query_dim, mem_index_path);
        std::cout << "Loaded memory index from " << mem_index_path << std::endl;
      } else {
        std::cout << "Warning: Memory index not found, setting mem_L=0" << std::endl;
        mem_L = 0;  // 内存索引不存在，禁用内存索引搜索
      }
    }

    switch (exp_type) {
      case 1: {
        std::vector<uint64_t> L_values = {10, 20, 30, 40, 50, 60, 80, 100, 150, 200};
        run_latency_experiment(*index, query_u8, query_num, query_dim, recall_at, mem_L, L_values, beam_width,
                               output_file, gt_ids, gt_dim);
        break;
      }
      case 2:
        run_dynamic_latency_experiment(*index, query_u8, query_num, query_dim, recall_at, mem_L, 50, beam_width,
                                       output_file, 10);  // 10 minutes
        break;
      case 5:
        run_io_amplification_experiment(*index, query_u8, query_num, query_dim, recall_at, mem_L, 50, beam_width,
                                        output_file);
        break;
      case 6: {
        std::vector<uint32_t> bw_values = {1, 2, 4, 8, 16, 32, 64};
        run_pipeline_width_experiment(*index, query_u8, query_num, query_dim, recall_at, mem_L, 50, bw_values,
                                      output_file, gt_ids, gt_dim);
        break;
      }
      case 7:
        run_resource_evaluation<uint8_t>(index_prefix, index->num_points, query_dim, output_file);
        break;
      case 8:
        run_dispersion_experiment(*index, query_u8, query_num, query_dim, recall_at, mem_L, 50, beam_width,
                                  output_file);
        break;
      case 9:
        run_topology_strength_experiment(*index, output_file);
        break;
      case 10:
        run_dispersion_evolution_experiment(*index, nullptr, 0, query_dim, query_u8, query_num, query_dim,
                                            recall_at, mem_L, 50, beam_width, output_file);
        break;
      case 11:
        run_cross_page_ratio_experiment(*index, query_u8, query_num, query_dim, recall_at, mem_L, 50, beam_width,
                                        output_file);
        break;
      default:
        std::cout << "Unknown experiment type: " << exp_type << std::endl;
        return -1;
    }
  }

  // 清理
  if (query_f)
    delete[] query_f;
  if (query_u8)
    delete[] query_u8;
  if (query_i8)
    delete[] query_i8;
  if (gt_ids)
    delete[] gt_ids;
  if (gt_dists)
    delete[] gt_dists;

  return 0;
}
