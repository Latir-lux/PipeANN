/**
 * @file test_clu_alloc.cpp
 * @brief Clu-Alloc Experiment: Compare Append-Only, Random-Alloc, and Clu-Alloc strategies
 * 
 * This test implements the experiment described in section 3.2.2:
 * 1. Phase 1: Build static index with 50% data
 * 2. Phase 2: Insert remaining 50% using different allocation strategies
 * 3. Phase 3: Run queries and collect performance metrics
 * 
 * Output: CSV file with APA, I/O amplification, P99 latency, and intra-page edge ratio
 */

#include "ssd_index.h"
#include "v2/dynamic_index.h"

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
#include <dirent.h>
#include <sys/stat.h>

#include "aux_utils.h"
#include "index.h"
#include "math_utils.h"
#include "partition.h"
#include "utils.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ========== Configuration ==========
int NUM_INSERT_THREADS = 10;
int NUM_SEARCH_THREADS = 32;
int search_mode = BEAM_SEARCH;

// ========== Experiment Result Structure ==========
struct ExperimentResult {
  std::string strategy_name;
  uint64_t num_inserted;
  double avg_page_accesses;     // APA
  double io_amplification;      // I/O amplification
  double p50_latency_ms;        // P50 latency in ms
  double p99_latency_ms;        // P99 latency in ms
  double recall;                // Recall@K
  double intra_page_ratio;      // Intra-page edge ratio
  uint64_t disk_index_size_mb;  // Disk index size in MB
  uint64_t cluster_hits;        // Cluster allocation hits (for Clu-Alloc)
  uint64_t overflow_count;      // Overflow count
  
  std::string to_csv_header() const {
    return "strategy,num_inserted,avg_page_accesses,io_amplification,"
           "p50_latency_ms,p99_latency_ms,recall,intra_page_ratio,"
           "disk_size_mb,cluster_hits,overflow_count";
  }
  
  std::string to_csv_row() const {
    std::stringstream ss;
    ss << strategy_name << "," << num_inserted << ","
       << std::fixed << std::setprecision(4)
       << avg_page_accesses << "," << io_amplification << ","
       << p50_latency_ms << "," << p99_latency_ms << ","
       << recall << "," << intra_page_ratio << ","
       << disk_index_size_mb << "," << cluster_hits << "," << overflow_count;
    return ss.str();
  }
};

pipeann::Timer globalTimer;

// ========== Helper Functions ==========
std::string get_strategy_name(AllocStrategy strategy) {
  switch (strategy) {
    case ALLOC_APPEND: return "Append-Only";
    case ALLOC_RANDOM: return "Random-Alloc";
    case ALLOC_CLUSTER: return "Clu-Alloc";
    default: return "Unknown";
  }
}

template<typename T, typename TagT>
ExperimentResult run_search_benchmark(
    pipeann::DynamicSSDIndex<T, TagT>& sync_index,
    T* query, size_t query_num, size_t query_dim,
    int recall_at, uint32_t mem_L, uint64_t L,
    uint32_t beam_width, const std::string& strategy_name,
    uint64_t num_inserted) {
  
  LOG(INFO) << "Starting search benchmark: " << query_num << " queries, recall@" << recall_at 
            << ", L=" << L << ", beam_width=" << beam_width;
  
  ExperimentResult result;
  result.strategy_name = strategy_name;
  result.num_inserted = num_inserted;
  
  float* query_result_dists = new float[recall_at * query_num];
  TagT* query_result_tags = new TagT[recall_at * query_num];
  
  for (uint32_t q = 0; q < query_num; q++) {
    for (uint32_t r = 0; r < (uint32_t)recall_at; r++) {
      query_result_tags[q * recall_at + r] = std::numeric_limits<TagT>::max();
      query_result_dists[q * recall_at + r] = std::numeric_limits<float>::max();
    }
  }
  
  std::vector<double> latency_stats(query_num, 0);
  pipeann::QueryStats* stats = new pipeann::QueryStats[query_num];
  
  auto s = std::chrono::high_resolution_clock::now();
  
  #pragma omp parallel for num_threads(NUM_SEARCH_THREADS) schedule(dynamic)
  for (int64_t i = 0; i < (int64_t)query_num; i++) {
    sync_index.search(query + i * query_dim, recall_at, mem_L, L, beam_width,
                      query_result_tags + i * recall_at,
                      query_result_dists + i * recall_at, stats + i, true);
    latency_stats[i] = stats[i].total_us / 1000.0;  // Convert to ms
  }
  
  auto e = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed = e - s;
  LOG(INFO) << "Search completed in " << elapsed.count() << " seconds";
  
  // Calculate APA (Average Page Accesses)
  double total_ios = 0;
  double total_read_size = 0;
  for (size_t i = 0; i < query_num; i++) {
    total_ios += stats[i].n_ios;
    total_read_size += stats[i].read_size;
  }
  result.avg_page_accesses = total_ios / query_num;
  
  // Calculate I/O amplification
  // Effective data = recall_at * vector_dim * sizeof(T) per query
  double effective_data_per_query = recall_at * query_dim * sizeof(T);
  result.io_amplification = (total_read_size / query_num) / effective_data_per_query;
  
  // Latency percentiles
  std::sort(latency_stats.begin(), latency_stats.end());
  result.p50_latency_ms = latency_stats[(uint64_t)(0.50 * query_num)];
  result.p99_latency_ms = latency_stats[(uint64_t)(0.99 * query_num)];
  
  // Recall (set to 0 if no ground truth available)
  result.recall = 0.0;
  
  // Get allocation stats
  auto& alloc_stats = sync_index.get_clu_alloc_stats();
  result.intra_page_ratio = alloc_stats.get_intra_page_ratio();
  result.cluster_hits = alloc_stats.cluster_alloc_hits.load();
  result.overflow_count = alloc_stats.overflow_count.load();
  
  // Disk index size
  result.disk_index_size_mb = sync_index.get_disk_index_size() / (1024 * 1024);
  
  delete[] query_result_dists;
  delete[] query_result_tags;
  delete[] stats;
  
  return result;
}

template<typename T, typename TagT>
void insertion_kernel(T* data_load, pipeann::DynamicSSDIndex<T, TagT>& sync_index,
                      std::vector<TagT>& insert_vec, size_t dim) {
  pipeann::Timer timer;
  size_t npts = insert_vec.size();
  
  LOG(INFO) << "Begin Insert: " << npts << " points";
  
  #pragma omp parallel for num_threads(NUM_INSERT_THREADS)
  for (int64_t i = 0; i < (int64_t)insert_vec.size(); i++) {
    sync_index.insert(data_load + dim * i, insert_vec[i]);
  }
  
  float time_secs = timer.elapsed() / 1.0e6f;
  LOG(INFO) << "Inserted " << insert_vec.size() << " points in " << time_secs << "s";
  LOG(INFO) << "Insertion throughput: " << (npts / time_secs) << " ops/sec";
}

template<typename T, typename TagT>
bool get_insertion_data(const std::string& data_bin, uint64_t start_idx, uint64_t count,
                        std::vector<TagT>& insert_tags, std::vector<T>& data_load, size_t& data_dim) {
  int npts_i32, dim_i32;
  std::ifstream reader(data_bin, std::ios::binary);
  if (!reader.is_open()) {
    LOG(ERROR) << "Failed to open data file: " << data_bin;
    return false;
  }
  
  reader.read((char*)&npts_i32, sizeof(int));
  reader.read((char*)&dim_i32, sizeof(int));
  
  data_dim = dim_i32;
  size_t total_pts = npts_i32;
  
  LOG(INFO) << "Data file: " << data_bin << " has " << total_pts << " points, dim=" << data_dim;
  
  // Check if we have enough data
  if (start_idx >= total_pts) {
    LOG(ERROR) << "Start index " << start_idx << " exceeds data file size " << total_pts;
    reader.close();
    return false;
  }
  
  // Adjust count if it exceeds available data
  uint64_t available = total_pts - start_idx;
  if (count > available) {
    LOG(WARNING) << "Requested " << count << " vectors but only " << available << " available. Adjusting.";
    count = available;
  }
  
  if (count == 0) {
    LOG(WARNING) << "No data to insert";
    reader.close();
    return false;
  }
  
  for (uint64_t i = start_idx; i < start_idx + count; ++i) {
    insert_tags.push_back(i);
  }
  
  data_load.resize(count * data_dim);
  reader.seekg(2 * sizeof(int) + start_idx * data_dim * sizeof(T), reader.beg);
  reader.read((char*)data_load.data(), sizeof(T) * count * data_dim);
  
  if (!reader) {
    LOG(ERROR) << "Failed to read data from file";
    reader.close();
    return false;
  }
  
  reader.close();
  LOG(INFO) << "Loaded " << count << " vectors starting from index " << start_idx;
  return true;
}

template<typename T, typename TagT>
void run_experiment(const std::string& data_bin, const unsigned L_disk,
                    int vecs_per_step, int num_steps,
                    const std::string& index_prefix,
                    const std::string& query_file,
                    const int recall_at,
                    const uint64_t L_search,
                    const unsigned beam_width,
                    const uint32_t search_beam_width,
                    const uint32_t search_mem_L,
                    pipeann::Distance<T>* dist_cmp,
                    const std::string& output_dir,
                    AllocStrategy strategy) {
  
  pipeann::Parameters paras;
  paras.set(0, L_disk, 384, 1.2, NUM_SEARCH_THREADS + NUM_INSERT_THREADS, true, beam_width);
  
  // Load query data
  T* query = nullptr;
  size_t query_num, query_dim;
  pipeann::load_bin<T>(query_file, query, query_num, query_dim);
  
  pipeann::Metric metric = pipeann::Metric::L2;
  
  // Create output file
  std::string strategy_name = get_strategy_name(strategy);
  std::string csv_filename = output_dir + "/clu_alloc_" + 
      (strategy == ALLOC_APPEND ? "append" : 
       (strategy == ALLOC_RANDOM ? "random" : "cluster")) + ".csv";
  
  std::ofstream csv_file(csv_filename);
  ExperimentResult dummy;
  csv_file << dummy.to_csv_header() << std::endl;
  
  LOG(INFO) << "=== Running experiment with strategy: " << strategy_name << " ===";
  
  // Create dynamic index with specified strategy
  pipeann::DynamicSSDIndex<T, TagT> sync_index(paras, index_prefix, index_prefix + "_merge",
                                                dist_cmp, metric, search_mode, (search_mem_L > 0));
  
  // Set allocation strategy
  sync_index.set_alloc_strategy(strategy);
  sync_index.reset_clu_alloc_stats();
  
  uint64_t index_npts = sync_index._disk_index->num_points;
  LOG(INFO) << "Initial index size: " << index_npts << " points";
  
  // Run benchmark before insertions
  LOG(INFO) << "Running initial search benchmark...";
  ExperimentResult result = run_search_benchmark(sync_index, query, query_num, query_dim,
                                                  recall_at, search_mem_L, L_search,
                                                  search_beam_width, strategy_name, 0);
  csv_file << result.to_csv_row() << std::endl;
  csv_file.flush();
  LOG(INFO) << "Initial benchmark: APA=" << result.avg_page_accesses 
            << ", P99=" << result.p99_latency_ms << "ms";
  
  // Phase 2: Insert data in steps
  uint64_t total_inserted = 0;
  for (int step = 0; step < num_steps; step++) {
    LOG(INFO) << "=== Step " << (step + 1) << "/" << num_steps << " ===";
    
    std::vector<TagT> insert_tags;
    std::vector<T> insert_data;
    size_t data_dim_check = 0;
    
    uint64_t start_idx = index_npts + step * vecs_per_step;
    bool success = get_insertion_data<T, TagT>(data_bin, start_idx, vecs_per_step, insert_tags, insert_data, data_dim_check);
    
    if (!success || insert_tags.empty()) {
      LOG(WARNING) << "No more data to insert at step " << step << ". Ending experiment early.";
      break;
    }
    
    // Perform insertions
    insertion_kernel(insert_data.data(), sync_index, insert_tags, query_dim);
    total_inserted += insert_tags.size();
    
    // Run search benchmark after this step
    LOG(INFO) << "Running search benchmark after " << total_inserted << " insertions...";
    result = run_search_benchmark(sync_index, query, query_num, query_dim,
                                   recall_at, search_mem_L, L_search,
                                   search_beam_width, strategy_name, total_inserted);
    csv_file << result.to_csv_row() << std::endl;
    csv_file.flush();
    
    // Log progress
    LOG(INFO) << "Results: APA=" << result.avg_page_accesses
              << ", IO_amp=" << result.io_amplification
              << ", P99=" << result.p99_latency_ms << "ms"
              << ", IntraPage=" << result.intra_page_ratio;
  }
  
  csv_file.close();
  LOG(INFO) << "Results saved to: " << csv_filename;
  
  // Print final statistics
  auto& final_stats = sync_index.get_clu_alloc_stats();
  LOG(INFO) << "=== Final Allocation Statistics ===";
  LOG(INFO) << "Total insertions: " << final_stats.total_insertions.load();
  LOG(INFO) << "Cluster alloc hits: " << final_stats.cluster_alloc_hits.load();
  LOG(INFO) << "Random alloc count: " << final_stats.random_alloc_count.load();
  LOG(INFO) << "Append alloc count: " << final_stats.append_alloc_count.load();
  LOG(INFO) << "Overflow count: " << final_stats.overflow_count.load();
  LOG(INFO) << "Intra-page edge ratio: " << final_stats.get_intra_page_ratio();
  
  delete[] query;
}

void print_usage(const char* prog_name) {
  LOG(INFO) << "Usage: " << prog_name
            << " <type[int8/uint8/float]> <data_bin> <L_disk>"
            << " <vecs_per_step> <num_steps> <insert_threads> <search_threads>"
            << " <search_mode> <index_prefix> <query_file> <recall@>"
            << " <beam_width> <search_beam_width> <mem_L> <L_search>"
            << " <output_dir> <strategy[0=append/1=random/2=cluster]>";
  LOG(INFO) << "";
  LOG(INFO) << "Arguments:";
  LOG(INFO) << "  type           : Data type (int8, uint8, float)";
  LOG(INFO) << "  data_bin       : Path to binary data file for insertion";
  LOG(INFO) << "  L_disk         : L parameter for disk index";
  LOG(INFO) << "  vecs_per_step  : Number of vectors to insert per step";
  LOG(INFO) << "  num_steps      : Number of insertion steps";
  LOG(INFO) << "  insert_threads : Number of insertion threads";
  LOG(INFO) << "  search_threads : Number of search threads";
  LOG(INFO) << "  search_mode    : Search mode (0=BEAM, 1=PAGE, 2=PIPE)";
  LOG(INFO) << "  index_prefix   : Prefix path for the index files";
  LOG(INFO) << "  query_file     : Path to query file";
  LOG(INFO) << "  recall@        : K for recall@K metric";
  LOG(INFO) << "  beam_width     : Beam width for index building";
  LOG(INFO) << "  search_beam_width: Beam width for search";
  LOG(INFO) << "  mem_L          : L for in-memory index (0 if not used)";
  LOG(INFO) << "  L_search       : L parameter for search";
  LOG(INFO) << "  output_dir     : Directory to save CSV results";
  LOG(INFO) << "  strategy       : Allocation strategy (0=append, 1=random, 2=cluster)";
}

int main(int argc, char** argv) {
  if (argc < 18) {
    print_usage(argv[0]);
    exit(-1);
  }
  
  int arg_no = 2;
  std::string data_bin = std::string(argv[arg_no++]);
  unsigned L_disk = (unsigned)atoi(argv[arg_no++]);
  int vecs_per_step = (int)std::atoi(argv[arg_no++]);
  int num_steps = (int)std::atoi(argv[arg_no++]);
  NUM_INSERT_THREADS = (int)std::atoi(argv[arg_no++]);
  NUM_SEARCH_THREADS = (int)std::atoi(argv[arg_no++]);
  search_mode = std::atoi(argv[arg_no++]);
  std::string index_prefix(argv[arg_no++]);
  std::string query_file(argv[arg_no++]);
  int recall_at = (int)std::atoi(argv[arg_no++]);
  unsigned beam_width = (unsigned)std::atoi(argv[arg_no++]);
  unsigned search_beam_width = (unsigned)std::atoi(argv[arg_no++]);
  unsigned search_mem_L = (unsigned)std::atoi(argv[arg_no++]);
  uint64_t L_search = (uint64_t)std::atoll(argv[arg_no++]);
  std::string output_dir(argv[arg_no++]);
  int strategy_int = std::atoi(argv[arg_no++]);
  
  AllocStrategy strategy = static_cast<AllocStrategy>(strategy_int);
  
  LOG(INFO) << "=== Clu-Alloc Experiment Configuration ===";
  LOG(INFO) << "Data file: " << data_bin;
  LOG(INFO) << "Index prefix: " << index_prefix;
  LOG(INFO) << "Query file: " << query_file;
  LOG(INFO) << "Vecs per step: " << vecs_per_step;
  LOG(INFO) << "Num steps: " << num_steps;
  LOG(INFO) << "Insert threads: " << NUM_INSERT_THREADS;
  LOG(INFO) << "Search threads: " << NUM_SEARCH_THREADS;
  LOG(INFO) << "Allocation strategy: " << get_strategy_name(strategy);
  LOG(INFO) << "Output directory: " << output_dir;
  
  // Create output directory if not exists
  mkdir(output_dir.c_str(), 0755);
  
  if (std::string(argv[1]) == std::string("int8")) {
    pipeann::DistanceL2Int8 dist_cmp;
    run_experiment<int8_t, unsigned>(data_bin, L_disk, vecs_per_step, num_steps,
                                      index_prefix, query_file, recall_at, L_search,
                                      beam_width, search_beam_width, search_mem_L,
                                      &dist_cmp, output_dir, strategy);
  } else if (std::string(argv[1]) == std::string("uint8")) {
    pipeann::DistanceL2UInt8 dist_cmp;
    run_experiment<uint8_t, unsigned>(data_bin, L_disk, vecs_per_step, num_steps,
                                       index_prefix, query_file, recall_at, L_search,
                                       beam_width, search_beam_width, search_mem_L,
                                       &dist_cmp, output_dir, strategy);
  } else if (std::string(argv[1]) == std::string("float")) {
    pipeann::DistanceL2 dist_cmp;
    run_experiment<float, unsigned>(data_bin, L_disk, vecs_per_step, num_steps,
                                     index_prefix, query_file, recall_at, L_search,
                                     beam_width, search_beam_width, search_mem_L,
                                     &dist_cmp, output_dir, strategy);
  } else {
    LOG(ERROR) << "Unsupported type. Use float/int8/uint8";
    exit(-1);
  }
  
  return 0;
}
