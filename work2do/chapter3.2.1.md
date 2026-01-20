# 章节3.2.1

实验项目名称：基于 PipeANN 的动态图索引逻辑空间局部性验证与特征提取实验

实验目的：通过对 PipeANN 搜索过程的插桩（Instrumentation）与追踪，量化 Beam Search 算法在遍历过程中节点访问的时空相关性，证明“逻辑空间局部性”的存在，即：访问节点u后，其逻辑邻居 $N_{out}(u)$ 在极短时间内被访问的概率显著高于随机概率。

## 代码修改与插桩方案 (Instrumentation Plan)

为了捕获访问模式，我们需要深入修改 PipeANN 的核心搜索逻辑。主要修改文件为 src/v2/pipe_search.cpp 和 tests/search_disk_index.cpp。

### 追踪数据结构

在 src/include 下新建 access_tracer.h，定义用于记录单次查询行为的结构体：

```c++
struct AccessStep {
   uint32_t step_id;     // 搜索步数 (Iteration)
   uint32_t pivot_node_id;  // 当前被展开的节点 (u)
   std::vector<uint32_t> logic_neighbors; // u 的所有出边邻居 (N_out(u))
   std::vector<uint32_t> cache_hits;   // 在 Candidate Pool 中已存在或被访问过的邻居
   std::vector<uint32_t> io_requests;   // 触发了实际 I/O 请求的邻居
 };

 struct QueryTrace {
   uint32_t query_id;
   std::vector<AccessStep> steps;
 };
```

### 核心算法插桩 (pipe_search.cpp)

PipeANN 的 PIPESEARCH 函数通过维护 unexplored_set 和 io_pipeline 来工作。我们需要在以下关键点插入探针：

1. **Pivot Selection **(节点展开时刻)：
    代码位置：在 while (P < E) 循环内部，从 unexplored_set (U) 中取出一个节点 v 进行扩展时。
    操作：记录 v 为 pivot_node_id。
2. **Neighbor Expansion** (邻居遍历时刻)：
    代码位置：在遍历 v.neighbors 并计算距离、更新 Candidate Pool (P) 的循环中。
    操作：记录所有 nbr 到 logic_neighbors。
    逻辑判断：如果 nbr 随后被加入了 io_pipeline（即触发了 prep_read），标记为 **潜在的物理访问**；如果 nbr 最终进入了 explored_set (E)，则标记为 **有效的逻辑局部性转化**。

3. **I/O Completion (I/O** **完成时刻)**：
    代码位置：io_uring 返回 completion queue entry (CQE) 时。
    操作：记录该节点被实际加载的时间戳。

## 模拟动态碎片化 (Fragmentation Simulation)

由于 PipeANN 原生支持的是静态索引搜索，为了模拟 **IP-DiskANN** 1 或 **ODINANN** 1 中的动态环境，我们需要人工引入“物理熵”，方式如下：

1. **修改 SSDIndex 类**：增加一个重映射表 std::vector<uint32_t> logical_to_physical_map。
2. **碎片化注入**：在加载索引后，随机打乱 30%~50% 节点的物理页映射关系。这模拟了经过大量 Direct Insert 后，逻辑相邻的节点（如 $u$ 和 $v$）被分散到不连续的物理页（Page ID $P_u$ 和 $P_v$ 相距甚远）的场景。

**对比组**：

- Group A (Static): 原始 DiskANN 布局（利用 BFS/DFS 排序，物理局部性好）。
- Group B (Dynamic): 随机打乱映射（模拟碎片化，物理局部性差）。

## 评价指标设计

为了量化逻辑局部性，我们定义以下核心指标：

1. 邻居转化率 (Neighbor Conversion Rate, NCR)：
    对于节点 $u$，其邻居 $v \in N_{out}(u)$ 在随后的 $k$ 步搜索中被访问的概率。
    $$
    NCR(u, k) = \frac{|N_{out}(u) \cap \bigcup_{i=1}^{k} VisitedSet_{t+i}|}{|N_{out}(u)|}
    $$
    *预期结果*：NCR 应显著高于随机选点的概率，证明 Beam Search 倾向于在局部子图中游走。

2. 时间访问窗口 (Temporal Access Window, TAW)：如果邻居 $v$ 被访问，它是作为 $u$ 的邻居被发现后第几步被访问的？
    $$
    TAW(u, v) = Step(v) - Step(u)
    $$
    *预期结果*：TAW 的分布应呈现长尾状，且峰值集中在 1-3 步内。这证明了**将逻辑邻居物理聚类**可以极大提升 I/O 效率（预取命中率）。

3. 逻辑-物理失配度 (Logical-Physical Mismatch Score, LPMS)：
    衡量逻辑上紧密的邻居在物理磁盘上的分散程度。
    $$
    LPMS(u) = \sum_{v \in N_{out}(u) \cap Visited} |PageID(u) - PageID(v)|
    $$
    *验证目标*：在 Dynamic 组中 LPMS 极高，但在 Static 组中较低。如果 NCR 在两组中保持一致（逻辑不变），而 LPMS 差异巨大，则证明了**必须引入动态聚类来弥合这一差距**。

## 实验步骤执行

1. **基线构建**：使用 SIFT100M 构建标准的 Vamana 索引（$R=64, L=128$）。
2. **数据采集**：运行 1000 次查询，收集 Trace Log。
3. **局部性分析**：
    统计所有 Pivot Node 的 NCR 分布。
    绘制 TAW 直方图，观察访问的时间紧凑性。

4. **碎片化压力测试**：应用 50% 随机映射，再次运行查询，记录 I/O 延迟（Latency）和吞吐量（QPS）的变化。
    重点观察：虽然搜索路径（逻辑访问序列）几乎不变，但 I/O 消耗是否成倍增加？

5. **PipeANN** **参数敏感性**：调整 PipeANN 的 Pipeline Width ($W=4, 8, 16, 32$)，观察 $W$ 的增加是否能掩盖部分物理碎片化的延迟，或者是否因为过度的 Speculative I/O 导致带宽饱和。

## 预期结论

实验预计将支撑以下论点，作为章节 3.2.1 的核心依据：

1. **逻辑局部性是固有的**：无论物理存储如何，Beam Search 算法决定了节点访问具有极强的局部聚类特征。
2. **物理退化是必然的**：在动态场景下，简单的原地更新策略（如 IP-DiskANN）破坏了物理局部性，导致 I/O 放大。

**聚类的必要性**：由于 NCR 高且 TAW 短，如果能将逻辑邻居物理重组（Dynamic Clustering），就能将多次随机 I/O 转化为单次顺序 I/O，这是解决动态图索引性能抖动的根本出路。