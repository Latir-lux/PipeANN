# chapter3-2-2

## 实验目标

本实验旨在验证论文3.2.2节提出的“基于连接强度的聚类感知分配规则”（以下简称**Clu-Alloc**）在动态插入场景下，相较于传统的“追加写入”（Append-Only）和“随机分配”（Random-Alloc）策略，在维持图索引物理局部性、降低I/O放大率及提升搜索性能方面的有效性。

## 实验环境与基准代码

●    **代码库:** PipeANN (Github: https://github.com/thustorage/PipeANN)，即本项目代码 。

●    **基础架构:** 基于PipeANN的SSDIndex类进行改造。PipeANN原生支持异步搜索与静态图构建，需扩展其update模块以支持动态分配逻辑。

## 代码修改与实现计划

为了支持3.2.2的方案验证，需要对PipeANN代码进行以下非破坏性扩展：

#### **存储引擎扩展 (src/ssd_index.cpp)**:

**增加页内位图 (Bitmap):** 修改Page结构，在Page Header中增加位图以跟踪空闲槽位（Slot），支持ODINANN式的非原地更新。

**实现分配器接口 (Allocator):**

- Alloc_Tail(): 基准策略，总是分配在文件末尾。
- Alloc_Cluster(vector<NodeID> neighbors): 实验策略，根据邻居所在的PageID计算亲和度得分，返回最佳PageID。

#### **插入逻辑重构 (src/update/direct_insert.cpp)**:

- 在插入新节点$V_{new}$前，先执行Search(V_{new})获取Top-K邻居集合 $\mathcal{N}$。
- 统计 $\mathcal{N}$ 中节点的物理Page分布直方图。
- 计算每个候选Page的**连接强度得分 $S_{score}$**（公式见正文3.2.2）。
- 调用Alloc_Cluster尝试写入高分Page；若满，则触发**分裂（Split）**或**溢出（Overflow）**逻辑。

## 实验数据集与工作负载

#### **数据集:**

**SIFT1B (10亿规模):** 用于验证大规模下的I/O性能。

**DEEP1B:** 高维数据，测试空间利用率。

#### **工作负载 (Workload):**

1. **Phase 1 (****构建):** 使用50%数据构建初始静态图。
2. **Phase 2 (****碎片化注入):** 模拟动态流，连续插入剩余50%数据：*
    Group A:* 使用 Append-Only 策略。
    *Group B:* 使用 Clu-Alloc 策略 (本方案)。
3. **Phase 3 (****查询测试):** 在完成插入后的图上执行10万次查询，记录性能指标。

## 关键评估指标 (Metrics)

#### **平均页面访问次数 (Average Page Accesses, APA):**

- 定义：单次搜索请求平均读取的物理SSD Page数量。
- 预期：Clu-Alloc策略下的APA应显著低于Append-Only（预期降低30%-50%），证明物理局部性得到改善。

#### **I/O** **放大率 (I/O Amplification):**

- 定义：读取的数据量 / 有效数据量。
- 关联：BAMG指出块利用率直接影响此指标。

#### **搜索延迟 (Search Latency - P99):**

- 验证PipeANN的流水线是否因更好的数据布局而运行得更流畅。

#### **写入放大与空间开销:**

- 记录因预留空间（Overprovisioning）导致的额外磁盘占用，验证是否在ODINANN提到的1.5倍范围内。

## 预期实验结果记录折线图 (模拟)

实验预计将展示，虽然磁盘空间占用因预留机制增加了约35%（低于ODINANN的100%预留，因为我们采用了更智能的填空策略），但换取了极大的读取性能提升。页内边比例的提升直接证明了“连接强度”规则的有效性。注意使用折线图表现各个baseline在不同的数据集规模下的指标。