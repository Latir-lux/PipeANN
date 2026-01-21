# chapter3-3-1 实验实现方案（基于现有代码与脚本）

## 结论性判断与映射
- 本仓库已包含 PipeANN/OdinANN 的实验入口与脚本，动态更新相关实验主要对应 OdinANN 脚本（`scripts/tests-odinann/*.sh`，`tests/test_insert_search.cpp`，`tests/overall_performance.cpp`）。
- 搜索 I/O 与延迟指标由 `tests/search_disk_index.cpp` 输出（Mean IOs、P99、QPS、Recall）。
- 更新负载稳定性由 `tests/test_insert_search.cpp`/`tests/overall_performance.cpp` 输出（P99/99.9、QPS、Disk IOs）。
- H1/H2/H4 可直接复用现有测试程序与脚本；H3（页缓存/LLC/IPC）需要外部 perf/eBPF 工具。
- 基线系统：脚本已集成 DiskANN 与 SPFresh/FreshDiskANN（`scripts/tests-odinann/fig6.sh`、`fig8.sh`、`fig12.sh` 中调用 `/mnt/nvme2/DiskANN` 与 `/mnt/nvme2/SPFresh`）。

## 实验一：长期动态更新稳定性（H2）
### 现成入口
- 代码：`tests/test_insert_search.cpp`（insert-search），`tests/overall_performance.cpp`（insert-delete-search）。
- 脚本：
  - `scripts/tests-odinann/fig6.sh`（SIFT100M insert-search，含 DiskANN/SPFresh）
  - `scripts/tests-odinann/fig8.sh`（SIFT1B 大规模长期 insert-search）
  - `scripts/tests-odinann/fig12.sh`（insert-delete-search）

### 需要的采集指标
- P99 / 99.9 / 平均延迟、QPS、Disk IOs：测试程序已输出（见 `test_insert_search.cpp`/`overall_performance.cpp` 的表头）。
- 每 10 分钟采样：通过脚本或外层日志解析（定时抓取输出行或追加 `tee` 输出）。

### 操作流程建议
1. 构建环境与索引（参考 `README-OdinANN.md` 快速开始与脚本内路径）。
2. 运行 insert-search（长期）
   - 100M 规模：`scripts/tests-odinann/fig6.sh`
   - 1B 规模：`scripts/tests-odinann/fig8.sh`
3. 运行 insert-delete-search：`scripts/tests-odinann/fig12.sh`
4. 解析输出日志，按时间序列提取 P99/QPS/Disk IOs。

## 实验二：搜索 I/O 效率与放大率（H1）
### 现成入口
- `tests/search_disk_index.cpp`：输出 Mean IOs、P99、QPS、Recall。
- 相关脚本：`scripts/tests-pipeann/*.sh`（搜索-only 评测）。

### 关键问题与补充
- I/O 数据量、放大率、页内邻居数在当前输出中未直接打印。
- `QueryStats` 已包含 `read_size`、`n_ios`（`include/utils/percentile_stats.h`），但 `read_size` 在现有搜索路径中未累计，需要补充统计逻辑。

### 建议改动（仅用于实验统计，不改变算法）
- 在 `src/search/beam_search.cpp`、`src/search/page_search.cpp`、`src/search/pipe_search.cpp` 中：
  - 每次发起 I/O 时累加 `stats->read_size += request_len`（当前已有 `stats->n_ios++`）。
  - 记录 `stats->n_cache_hits`：缓存命中分支增加计数（可在 `linux_aligned_file_reader.cpp` 的缓存路径加钩子，或由上层统计 `n_ios` 与请求数差值）。
- 在 `tests/search_disk_index.cpp` 中额外输出：
  - `mean_read_size`（每 query 读取字节数）
  - `io_amplification = mean_read_size / (recall_at * vector_bytes)`
  - `neighbors_per_page`（建议用“有效邻居数 / 读取页数”估算，可用 `n_cmps / n_ios`，或在 page_search 中统计每页实际处理节点数）。

### 执行流程
1. 先运行实验一，得到“老化索引”。
2. 暂停写入，执行 `tests/search_disk_index`（按 Recall@10=90/95/98 调 L/beam）。
3. 对比 PipeANN 与基线（DiskANN、SPFresh/FreshDiskANN、IP-DiskANN）。
4. 解析输出，计算 I/O 放大与页内邻居数曲线。

## 实验三：页缓存命中率与微观架构指标（H3）
### 需要外部工具
- perf：LLC、IPC、page faults
- eBPF/BCC：`cachestat`, `biolatency`（页缓存命中/IO 延迟）

### 建议采集命令（示意）
- `perf stat -e LLC-loads,LLC-load-misses,cycles,instructions,major-faults,minor-faults -p <pid>`
  - 计算 `LLC Miss Rate = misses / loads`，`IPC = instructions / cycles`。
- `cachestat` / `biolatency`：采样 Page Cache Hit Ratio 与 IO 延迟分布。

### 内存限制
- 使用 cgroup 或 `systemd-run --property=MemoryMax=` 限制 page cache（5%/10%/20% 索引大小）。
- 每个内存档位运行同样的 `search_disk_index`。

## 实验四：写入吞吐与并发开销（H4）
### 现成入口
- `tests/test_insert_search.cpp`（插入吞吐、延迟分位数输出）。

### 设计
- 逐步增大 `NUM_INSERT_THREADS`（通过命令参数）。
- 对照组策略：
  - Group A：默认动态聚类 + 后台重组（当前默认实现）。
  - Group B：关闭后台重组（需在 `dynamic_index`/merge 触发处加开关）。
  - Group C：随机分配（需在插入路径中关闭聚类/分配策略，可能需要小改动）。

## 关键文件索引（便于实现统计与脚本落地）
- 实验脚本：`scripts/tests-odinann/fig6.sh`、`scripts/tests-odinann/fig8.sh`、`scripts/tests-odinann/fig12.sh`
- 动态更新主程序：`tests/test_insert_search.cpp`、`tests/overall_performance.cpp`
- 搜索与 I/O 统计：`tests/search_disk_index.cpp`、`include/utils/percentile_stats.h`
- 搜索实现：`src/search/beam_search.cpp`、`src/search/page_search.cpp`、`src/search/pipe_search.cpp`

## 风险与缺口
- IP-DiskANN/FreshDiskANN 不在本仓库，需要外部 repo（脚本已假设路径 `/mnt/nvme2/DiskANN` 与 `/mnt/nvme2/SPFresh`）。
- I/O Volume、Neighbors per Page 当前未直接输出，需要小规模统计代码改动或外部采样工具。
- 24-48 小时负载需要长跑环境与稳定的数据路径配置，建议先用 100M 规模验证流程。
