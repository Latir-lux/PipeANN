# PipeANN 3.2.1 实验修改与执行计划

## 代码修改计划
- 新增追踪结构体
  - 新文件: `include/access_tracer.h`
  - 定义 `AccessStep` / `QueryTrace`，支持 step_id、pivot_node_id、logic_neighbors、cache_hits、io_requests、io_complete_ts 等字段
- 在 PipeSearch 路径插桩
  - 文件: `src/search/pipe_search.cpp`
  - Pivot 选择: 在 `calc_best_node()` 中确定首次 `retset[marker]` 被展开时记录 `pivot_node_id` 与 `step_id`
  - 邻居遍历: 在 `compute_and_push_nbrs()` 中记录所有 `node_nbrs` 到 `logic_neighbors`
  - 缓存/候选命中: 对已在 `visited` 或已在候选池的邻居标记为 `cache_hits`
  - I/O 触发: 在 `send_read_req()` / `send_best_read_req()` 处记录 `io_requests`
  - I/O 完成: 在 `poll_all()` 中记录完成时间戳（相对 query 起始）
- 追踪数据挂载与输出
  - 方案: 为 `pipe_search` 增加可选 `QueryTrace*` 参数或在 `QueryStats` 内新增 trace 指针（按需启用，默认不影响性能）
  - 在 `tests/search_disk_index.cpp` 中增加 trace 开关和输出路径，按 query 输出 JSONL/CSV
- 碎片化模拟
  - 文件: `include/ssd_index.h`, `src/ssd_index.cpp`
  - 增加 `logical_to_physical_map` 或等效映射表
  - 在 `load_page_layout()` 之后提供 `apply_fragmentation(ratio, seed)`
    - 随机打乱 30%~50% 的逻辑节点映射，保证映射一一对应
    - 更新 `id2loc_` / `loc2id_` 一致性
  - 在 `tests/search_disk_index.cpp` 增加 CLI 参数控制是否启用碎片化和比例

## 实验执行计划
- 基线
  - 使用 SIFT100M 构建标准 Vamana（R=64, L=128）
  - 运行 1000 查询，开启 trace，收集访问序列
- 逻辑局部性分析
  - 计算 NCR、TAW 的分布
  - 输出 NCR 直方图、TAW 直方图
- 碎片化对比
  - 启用 50% 映射扰动，重复查询
  - 对比 QPS、Latency、LPMS
- 参数敏感性
  - PipeANN pipeline width: 4/8/16/32
  - 比较在碎片化下的吞吐与延迟变化

## 需要重点验证
- trace 开关关闭时无性能回退
- NCR 在 Static/Dynamic 组均保持高值
- LPMS 在 Dynamic 组显著升高
- TAW 峰值集中在 1-3 步