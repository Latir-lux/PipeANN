#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <fstream>
#include <vector>
#include <mutex>

namespace pipeann {

/**
 * I/O统计收集器 - 用于论文第5章实验
 * 收集I/O放大率、页面访问模式等数据
 */
struct IOStats {
  // 读取统计
  std::atomic<uint64_t> total_bytes_read{0};      // 实际从磁盘读取的字节数
  std::atomic<uint64_t> effective_bytes{0};       // 有效使用的数据字节数
  std::atomic<uint64_t> total_pages_read{0};      // 总读取页面数
  std::atomic<uint64_t> unique_pages_read{0};     // 去重后的页面数
  std::atomic<uint64_t> sequential_reads{0};      // 顺序读取次数
  std::atomic<uint64_t> random_reads{0};          // 随机读取次数
  
  // 写入统计
  std::atomic<uint64_t> total_bytes_written{0};   // 实际写入字节数
  std::atomic<uint64_t> total_pages_written{0};   // 写入页面数
  
  // 查询级统计
  std::atomic<uint64_t> total_queries{0};         // 总查询数
  
  void reset() {
    total_bytes_read = 0;
    effective_bytes = 0;
    total_pages_read = 0;
    unique_pages_read = 0;
    sequential_reads = 0;
    random_reads = 0;
    total_bytes_written = 0;
    total_pages_written = 0;
    total_queries = 0;
  }
  
  // I/O放大率 = 实际读取 / 有效数据
  double get_io_amplification() const {
    if (effective_bytes == 0) return 0.0;
    return static_cast<double>(total_bytes_read) / static_cast<double>(effective_bytes);
  }
  
  // 页面利用率 = 有效数据 / 总读取
  double get_page_utilization() const {
    if (total_bytes_read == 0) return 0.0;
    return static_cast<double>(effective_bytes) / static_cast<double>(total_bytes_read) * 100.0;
  }
  
  // 平均每查询读取页面数
  double get_pages_per_query() const {
    if (total_queries == 0) return 0.0;
    return static_cast<double>(total_pages_read) / static_cast<double>(total_queries);
  }
  
  // 顺序读取比例
  double get_sequential_ratio() const {
    uint64_t total = sequential_reads + random_reads;
    if (total == 0) return 0.0;
    return static_cast<double>(sequential_reads) / static_cast<double>(total) * 100.0;
  }
  
  void add_read(uint64_t bytes, uint64_t effective, bool is_sequential = false) {
    total_bytes_read += bytes;
    effective_bytes += effective;
    total_pages_read++;
    if (is_sequential) {
      sequential_reads++;
    } else {
      random_reads++;
    }
  }
  
  void add_write(uint64_t bytes) {
    total_bytes_written += bytes;
    total_pages_written++;
  }
  
  void add_query() {
    total_queries++;
  }
  
  std::string to_csv_header() const {
    return "total_bytes_read,effective_bytes,total_pages,io_amplification,page_utilization,pages_per_query,sequential_ratio";
  }
  
  std::string to_csv_row() const {
    char buf[512];
    snprintf(buf, sizeof(buf), "%lu,%lu,%lu,%.4f,%.2f,%.2f,%.2f",
             total_bytes_read.load(),
             effective_bytes.load(),
             total_pages_read.load(),
             get_io_amplification(),
             get_page_utilization(),
             get_pages_per_query(),
             get_sequential_ratio());
    return std::string(buf);
  }
};

// 全局I/O统计实例
#ifdef COLLECT_IO_STATS
inline IOStats global_io_stats;
#endif

/**
 * 延迟时间序列记录器 - 用于性能波动分析(5.2.2节)
 */
class LatencyRecorder {
public:
  struct Sample {
    double timestamp_s;      // 时间戳(秒)
    double latency_ms;       // 延迟(毫秒)
    double throughput;       // 吞吐量
    uint64_t num_vectors;    // 当前向量数
  };
  
private:
  std::vector<Sample> samples_;
  std::mutex mutex_;
  double start_time_s_ = 0;
  
public:
  void set_start_time(double t) { start_time_s_ = t; }
  
  void record(double timestamp_s, double latency_ms, double throughput = 0, uint64_t num_vectors = 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.push_back({timestamp_s - start_time_s_, latency_ms, throughput, num_vectors});
  }
  
  double get_latency_std_dev() const {
    if (samples_.size() < 2) return 0.0;
    
    double sum = 0, sum_sq = 0;
    for (const auto& s : samples_) {
      sum += s.latency_ms;
      sum_sq += s.latency_ms * s.latency_ms;
    }
    double mean = sum / samples_.size();
    double variance = sum_sq / samples_.size() - mean * mean;
    return std::sqrt(variance);
  }
  
  double get_mean_latency() const {
    if (samples_.empty()) return 0.0;
    double sum = 0;
    for (const auto& s : samples_) {
      sum += s.latency_ms;
    }
    return sum / samples_.size();
  }
  
  void save_to_csv(const std::string& filename) const {
    std::ofstream ofs(filename);
    ofs << "timestamp_s,latency_ms,throughput,num_vectors\n";
    for (const auto& s : samples_) {
      ofs << s.timestamp_s << "," << s.latency_ms << "," 
          << s.throughput << "," << s.num_vectors << "\n";
    }
    ofs.close();
  }
  
  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.clear();
  }
  
  size_t size() const { return samples_.size(); }
};

/**
 * 更新操作统计 - 用于更新吞吐量评估(5.3.1节)
 */
struct UpdateStats {
  double search_phase_us = 0;     // 搜索邻居阶段耗时
  double prune_phase_us = 0;      // 邻居剪枝阶段耗时
  double io_read_phase_us = 0;    // 磁盘读取阶段耗时
  double io_write_phase_us = 0;   // 磁盘写入阶段耗时
  double lock_phase_us = 0;       // 锁等待阶段耗时
  double total_us = 0;            // 总耗时
  
  void reset() {
    search_phase_us = 0;
    prune_phase_us = 0;
    io_read_phase_us = 0;
    io_write_phase_us = 0;
    lock_phase_us = 0;
    total_us = 0;
  }
};

/**
 * 缓存统计 - 用于缓存命中率分析(5.4.2节)
 */
struct CacheStats {
  std::atomic<uint64_t> hits{0};
  std::atomic<uint64_t> misses{0};
  std::atomic<uint64_t> evictions{0};
  
  void reset() {
    hits = 0;
    misses = 0;
    evictions = 0;
  }
  
  void add_hit() { hits++; }
  void add_miss() { misses++; }
  void add_eviction() { evictions++; }
  
  double get_hit_rate() const {
    uint64_t total = hits + misses;
    if (total == 0) return 0.0;
    return static_cast<double>(hits) / static_cast<double>(total) * 100.0;
  }
  
  std::string to_csv_header() const {
    return "hits,misses,evictions,hit_rate";
  }
  
  std::string to_csv_row() const {
    char buf[256];
    snprintf(buf, sizeof(buf), "%lu,%lu,%lu,%.2f",
             hits.load(), misses.load(), evictions.load(), get_hit_rate());
    return std::string(buf);
  }
};

#ifdef COLLECT_IO_STATS
inline CacheStats global_cache_stats;
#endif

}  // namespace pipeann
