#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <string>
#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace pipeann {

/**
 * @brief 记录单步搜索行为的结构体
 * 
 * 用于追踪 Beam Search 算法在单次节点展开时的详细信息
 */
struct AccessStep {
  uint32_t step_id;                         // 搜索步数 (Iteration)
  uint32_t pivot_node_id;                   // 当前被展开的节点 (u)
  std::vector<uint32_t> logic_neighbors;    // u 的所有出边邻居 (N_out(u))
  std::vector<uint32_t> cache_hits;         // 在 Candidate Pool 中已存在或被访问过的邻居
  std::vector<uint32_t> io_requests;        // 触发了实际 I/O 请求的邻居
  double io_complete_ts;                    // I/O 完成时间戳（相对于 query 起始，单位微秒）
  
  // 用于计算 LPMS
  uint64_t pivot_page_id;                   // pivot 节点所在的物理页 ID
  std::vector<uint64_t> neighbor_page_ids;  // 邻居节点所在的物理页 ID 列表
  
  AccessStep() : step_id(0), pivot_node_id(0), io_complete_ts(0.0), pivot_page_id(0) {}
};

/**
 * @brief 记录单次查询完整追踪信息的结构体
 */
struct QueryTrace {
  uint32_t query_id;
  std::vector<AccessStep> steps;
  double total_time_us;                     // 整个查询的总时间（微秒）
  
  // 统计信息
  uint32_t total_ios;                       // 总 I/O 次数
  uint32_t total_cache_hits;                // 总缓存命中次数
  uint32_t total_neighbors_explored;        // 总探索邻居数
  
  QueryTrace() : query_id(0), total_time_us(0.0), total_ios(0), 
                 total_cache_hits(0), total_neighbors_explored(0) {}
  
  void reset(uint32_t qid) {
    query_id = qid;
    steps.clear();
    total_time_us = 0.0;
    total_ios = 0;
    total_cache_hits = 0;
    total_neighbors_explored = 0;
  }
  
  void finalize() {
    total_ios = 0;
    total_cache_hits = 0;
    total_neighbors_explored = 0;
    for (const auto& step : steps) {
      total_ios += step.io_requests.size();
      total_cache_hits += step.cache_hits.size();
      total_neighbors_explored += step.logic_neighbors.size();
    }
  }
};

/**
 * @brief 访问追踪器 - 管理多个查询的追踪信息并输出
 */
class AccessTracer {
public:
  bool enabled;
  std::string output_path;
  std::vector<std::unique_ptr<QueryTrace>> traces;
  
  AccessTracer() : enabled(false) {}
  
  AccessTracer(const std::string& path, bool enable = true) 
    : enabled(enable), output_path(path) {}
  
  void set_enabled(bool enable) { enabled = enable; }
  bool is_enabled() const { return enabled; }
  
  QueryTrace* new_trace(uint32_t query_id) {
    if (!enabled) return nullptr;
    traces.emplace_back(std::make_unique<QueryTrace>());
    traces.back()->reset(query_id);
    return traces.back().get();
  }
  
  /**
   * @brief 将追踪结果输出为 JSONL 格式
   */
  void save_to_jsonl(const std::string& filename) const {
    std::ofstream ofs(filename);
    if (!ofs.is_open()) {
      return;
    }
    
    for (const auto& trace_ptr : traces) {
      const auto& trace = *trace_ptr;
      ofs << "{\"query_id\":" << trace.query_id 
          << ",\"total_time_us\":" << std::fixed << std::setprecision(2) << trace.total_time_us
          << ",\"total_ios\":" << trace.total_ios
          << ",\"total_cache_hits\":" << trace.total_cache_hits
          << ",\"total_neighbors_explored\":" << trace.total_neighbors_explored
          << ",\"steps\":[";
      
      for (size_t i = 0; i < trace.steps.size(); ++i) {
        const auto& step = trace.steps[i];
        if (i > 0) ofs << ",";
        ofs << "{\"step_id\":" << step.step_id
            << ",\"pivot_node_id\":" << step.pivot_node_id
            << ",\"pivot_page_id\":" << step.pivot_page_id
            << ",\"io_complete_ts\":" << std::fixed << std::setprecision(2) << step.io_complete_ts
            << ",\"logic_neighbors\":[";
        for (size_t j = 0; j < step.logic_neighbors.size(); ++j) {
          if (j > 0) ofs << ",";
          ofs << step.logic_neighbors[j];
        }
        ofs << "],\"neighbor_page_ids\":[";
        for (size_t j = 0; j < step.neighbor_page_ids.size(); ++j) {
          if (j > 0) ofs << ",";
          ofs << step.neighbor_page_ids[j];
        }
        ofs << "],\"cache_hits\":[";
        for (size_t j = 0; j < step.cache_hits.size(); ++j) {
          if (j > 0) ofs << ",";
          ofs << step.cache_hits[j];
        }
        ofs << "],\"io_requests\":[";
        for (size_t j = 0; j < step.io_requests.size(); ++j) {
          if (j > 0) ofs << ",";
          ofs << step.io_requests[j];
        }
        ofs << "]}";
      }
      ofs << "]}\n";
    }
    ofs.close();
  }
  
  /**
   * @brief 将追踪结果输出为 CSV 格式（用于后续分析）
   */
  void save_to_csv(const std::string& filename) const {
    std::ofstream ofs(filename);
    if (!ofs.is_open()) {
      return;
    }
    
    // CSV header
    ofs << "query_id,step_id,pivot_node_id,pivot_page_id,num_neighbors,"
        << "num_cache_hits,num_io_requests,io_complete_ts\n";
    
    for (const auto& trace_ptr : traces) {
      const auto& trace = *trace_ptr;
      for (const auto& step : trace.steps) {
        ofs << trace.query_id << ","
            << step.step_id << ","
            << step.pivot_node_id << ","
            << step.pivot_page_id << ","
            << step.logic_neighbors.size() << ","
            << step.cache_hits.size() << ","
            << step.io_requests.size() << ","
            << std::fixed << std::setprecision(2) << step.io_complete_ts << "\n";
      }
    }
    ofs.close();
  }
  
  /**
   * @brief 输出邻居访问详情（用于计算 NCR 和 TAW）
   */
  void save_neighbor_details(const std::string& filename) const {
    std::ofstream ofs(filename);
    if (!ofs.is_open()) {
      return;
    }
    
    // CSV header: 记录每个邻居的访问情况
    ofs << "query_id,step_id,pivot_node_id,neighbor_id,neighbor_page_id,"
        << "is_cache_hit,is_io_request\n";
    
    for (const auto& trace_ptr : traces) {
      const auto& trace = *trace_ptr;
      for (const auto& step : trace.steps) {
        for (size_t i = 0; i < step.logic_neighbors.size(); ++i) {
          uint32_t nbr_id = step.logic_neighbors[i];
          uint64_t nbr_page = (i < step.neighbor_page_ids.size()) ? 
                              step.neighbor_page_ids[i] : 0;
          
          // 检查是否为 cache hit
          bool is_cache_hit = false;
          for (uint32_t ch : step.cache_hits) {
            if (ch == nbr_id) {
              is_cache_hit = true;
              break;
            }
          }
          
          // 检查是否触发 I/O
          bool is_io_request = false;
          for (uint32_t io : step.io_requests) {
            if (io == nbr_id) {
              is_io_request = true;
              break;
            }
          }
          
          ofs << trace.query_id << ","
              << step.step_id << ","
              << step.pivot_node_id << ","
              << nbr_id << ","
              << nbr_page << ","
              << (is_cache_hit ? 1 : 0) << ","
              << (is_io_request ? 1 : 0) << "\n";
        }
      }
    }
    ofs.close();
  }
  
  /**
   * @brief 输出页面访问序列（用于计算 LPMS）
   */
  void save_page_access_sequence(const std::string& filename) const {
    std::ofstream ofs(filename);
    if (!ofs.is_open()) {
      return;
    }
    
    // CSV header
    ofs << "query_id,step_id,pivot_page_id,neighbor_page_ids\n";
    
    for (const auto& trace_ptr : traces) {
      const auto& trace = *trace_ptr;
      for (const auto& step : trace.steps) {
        ofs << trace.query_id << ","
            << step.step_id << ","
            << step.pivot_page_id << ",\"[";
        for (size_t i = 0; i < step.neighbor_page_ids.size(); ++i) {
          if (i > 0) ofs << ",";
          ofs << step.neighbor_page_ids[i];
        }
        ofs << "]\"\n";
      }
    }
    ofs.close();
  }
  
  /**
   * @brief 清空所有追踪数据
   */
  void clear() {
    traces.clear();
  }
  
  size_t size() const {
    return traces.size();
  }
};

}  // namespace pipeann
