#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include <algorithm>
#include <cmath>

namespace pipeann {
  struct QueryStats {
    double total_us = 0;        // total time to process query in micros
    double n_4k = 0;            // # of 4kB reads
    double n_8k = 0;            // # of 8kB reads
    double n_12k = 0;           // # of 12kB reads
    double n_ios = 0;           // total # of IOs issued
    double read_size = 0;       // total # of bytes read
    double io_us = 0;           // total time spent in IO
    double io_us1 = 0;          // total time spent in IO
    double head_us = 0;         // total time spent in in-memory index
    double cpu_us = 0;          // total time spent in CPU
    double cpu_us1 = 0;         // total time spent in CPU
    double cpu_us2 = 0;         // total time spent in CPU
    double n_cmps_saved = 0;    // # cmps saved
    double n_cmps = 0;          // # cmps
    double n_cache_hits = 0;    // # cache_hits
    double n_hops = 0;          // # search hops
    double n_current_used = 0;  // # force return for latency limit
    
    // 论文第5章实验新增统计字段
    double search_phase_us = 0;    // 搜索阶段耗时 (5.2.1节)
    double prefetch_phase_us = 0;  // 预取阶段耗时 (5.2.1节)
    double compute_phase_us = 0;   // 距离计算阶段耗时 (5.2.1节)
    double overlap_ratio = 0;      // I/O与计算重叠率 (5.2.1节)
    
    // I/O放大率相关 (5.4.1节)
    double bytes_read = 0;         // 实际读取字节数
    double effective_bytes = 0;    // 有效数据字节数
    double io_amplification = 0;   // I/O放大率
    double page_utilization = 0;   // 页面利用率(%)
    
    // 计算I/O放大率
    void calc_io_amplification() {
      if (effective_bytes > 0) {
        io_amplification = bytes_read / effective_bytes;
        page_utilization = (effective_bytes / bytes_read) * 100.0;
      }
    }
    
    // 计算重叠率: 1 - (总时间 / (各阶段时间之和))
    void calc_overlap_ratio() {
      double sum_phases = search_phase_us + prefetch_phase_us + compute_phase_us;
      if (sum_phases > 0 && total_us > 0) {
        // overlap_ratio = 1.0 表示完全串行，> 1.0 表示有并行
        overlap_ratio = sum_phases / total_us;
      }
    }
  };

  inline double get_percentile_stats(QueryStats *stats, uint64_t len, float percentile,
                                     const std::function<double(const QueryStats &)> &member_fn) {
    std::vector<double> vals(len);
    for (uint64_t i = 0; i < len; i++) {
      vals[i] = member_fn(stats[i]);
    }

    std::sort(vals.begin(), vals.end(), [](const double &left, const double &right) { return left < right; });

    auto retval = vals[(uint64_t) (percentile * ((float) len))];
    vals.clear();
    return retval;
  }

  inline double get_mean_stats(QueryStats *stats, uint64_t len,
                               const std::function<double(const QueryStats &)> &member_fn) {
    double avg = 0;
    for (uint64_t i = 0; i < len; i++) {
      avg += member_fn(stats[i]);
    }
    return avg / ((double) len);
  }
}  // namespace pipeann
