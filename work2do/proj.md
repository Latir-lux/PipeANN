# 3.2.2 Clu-Alloc 实验实现方案

目标：在 PipeANN 的动态插入路径中实现“基于连接强度的聚类感知分配规则（Clu-Alloc）”，并对比 Append-Only 与 Random-Alloc 的布局与性能差异，生成折线图结果。

## 1. 代码关联与可复用组件

- 动态插入入口：`/home/latir/WorkSpace/PipeANN/src/update/dynamic_index.cpp` 的 `DynamicSSDIndex::insert` 调用 `SSDIndex::insert_in_place`。
- 直接插入逻辑：`/home/latir/WorkSpace/PipeANN/src/update/direct_insert.cpp`。
  - 当前逻辑中 `do_beam_search` 返回 `page_ref`，并传入 `alloc_loc` 作为“hint pages”。
- 分配器基础实现：`/home/latir/WorkSpace/PipeANN/include/ssd_index.h` 中 `alloc_loc`。
  - 三段式分配：空页 -> hint pages -> 尾部新页（Append）。
- 页面/位置映射：`id2loc_` 与 `loc2id_`（同文件）。
- 查询统计：`/home/latir/WorkSpace/PipeANN/include/utils/percentile_stats.h` 的 `QueryStats`，包含 `n_ios`, `read_size`, `total_us`。
- 实验脚本与绘图：
  - `scripts/tests-odinann/fig*.sh` 与 `tests/test_insert_search.cpp`, `tests/overall_performance.cpp`
  - `scripts/tests-pipeann/plotting.py`
  - `scripts/tests-odinann/plotting.ipynb`

## 2. 需要新增/修改的核心点

### 2.1 页内位图（Bitmap）与空槽跟踪
目标：在 Page Header 中维护 slot bitmap，支持局部“填空”而非只尾追加。

建议实现路径：
- 在 `SSDIndex` 的页面布局管理中新增 bitmap 数据结构（内存侧），对应每页 `nnodes_per_sector` 个槽位。
- 加载/保存：
  - 通过分区/映射文件扩展或额外元数据文件（如 `${index_prefix}_page_bitmap.bin`）持久化。
- 插入/删除时更新 bitmap：
  - 插入：分配 slot 后置位；
  - 删除/merge：释放 slot 并检查页空闲数。

### 2.2 分配器接口扩展

新增 Allocator 抽象（可以先在 `SSDIndex` 内部实现）：
- `Alloc_Tail(n)`：等价于当前 `alloc_loc` 的“new pages”分支。
- `Alloc_Random(n)`：在可用页集合中均匀采样 page。
- `Alloc_Cluster(neighbors)`：核心策略（见 2.3）。

建议做法：
- 将 `alloc_loc` 拆分为可复用子函数，保留原逻辑作为 Append-Only baseline。
- 新增配置开关（例如 `Parameters::alloc_strategy` 或宏）用于实验切换。

### 2.3 Clu-Alloc 规则实现

插入前流程改造（`direct_insert.cpp`）：
1. `do_beam_search` 返回 Top-K 邻居集合 `N`（已存在）。
2. 统计 `N` 对应的页分布直方图（通过 `id2page` 或 `id2loc -> page`）。
3. 依据论文 3.2.2 的连接强度公式计算每页得分 `S_score`。
4. 调用 `Alloc_Cluster`，优先分配高分页的空槽；页满则按策略触发：
   - 分裂（Split）：选择目标页溢出到新页并更新局部映射。
   - 溢出（Overflow）：直接落到尾部页，并记录溢出比例。

说明：当前 `alloc_loc` 已支持传入 `hint_pages`，可将 `hint_pages` 替换为按 `S_score` 排序后的候选页列表（降序）。

## 3. 实验流程与指标输出

### 3.1 工作负载流程
- Phase 1：50% 数据构建静态图。
- Phase 2：插入剩余 50%，三组策略：
  - Append-Only（现有 alloc_loc new pages 分支）
  - Random-Alloc（新增）
  - Clu-Alloc（新增）
- Phase 3：插入后固定 10 万查询，收集指标。

### 3.2 指标采集
- APA（Average Page Accesses）：
  - 由 `QueryStats::n_ios` 或 `read_size / 4096` 统计。
- I/O 放大：
  - `read_size / (有效数据量)`；有效数据量可按 topK * 向量尺寸估计。
- P99 搜索延迟：
  - `QueryStats::total_us` 统计分位数。
- 写入放大与空间开销：
  - 记录磁盘索引大小变化（`_disk.index` 文件大小）。

### 3.3 使用现有测试入口
- 插入+搜索：`build/tests/test_insert_search`（与 `scripts/tests-odinann/fig6.sh` 一致）。
- 插入+删除+搜索：`build/tests/overall_performance`。

## 4. 图像绘制方案

折线图要求：多数据集规模、多策略对比。

建议做法：
- 复用 `scripts/tests-pipeann/plotting.py` 的 matplotlib 基础框架。
- 新增一个 `plot_clu_alloc.py`，输入为实验日志解析后的 CSV：
  - x 轴：数据集规模或插入进度（例如 100M、200M、...）；
  - y 轴：APA / I/O 放大 / P99 延迟。
  - 曲线：Append-Only / Random-Alloc / Clu-Alloc。
- 输出路径：`/home/latir/WorkSpace/PipeANN/work2do/figures/`。

## 5. 具体落地步骤（实现顺序）

1. 在 `SSDIndex` 增加页内 bitmap 管理与持久化。
2. 抽象分配策略（Append/Random/Cluster）并接入 `alloc_loc` 调用链。
3. 在 `direct_insert.cpp` 插入前增加 Top-K 邻居页统计与 `S_score` 计算。
4. 新增实验参数（命令行或配置）用于切换策略。
5. 扩展测试程序，记录 APA/I/O/延迟/空间开销。
6. 生成日志 -> CSV -> 绘图脚本输出折线图。

## 6. 风险与验证点

- 并发插入时 bitmap/loc2id_ 更新需保持原子性（复用 `alloc_lock` 与锁表）。
- 新分配策略必须保证 `loc2id_` 与 `id2loc_` 一致性（可复用 `verify_id2loc()`）。
- 插入时的页引用 `page_ref` 目前来自搜索路径，适合直接转为 Clu-Alloc 的候选页。

## 7. 产出物

- 代码变更：SSDIndex 分配策略 + direct_insert 插入逻辑。
- 实验脚本与日志解析脚本。
- 折线图：APA、I/O 放大、P99 延迟随数据规模变化的对比图。
