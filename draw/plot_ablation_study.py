#!/usr/bin/env python3
"""
plot_ablation_study.py - 可视化DC-PDI系统消融实验结果

本脚本为毕业论文exp-chapter4and3-3.docx中定义的四个消融实验生成可视化图表：

实验一：拓扑感知分配 vs. 随机分配
  - 图1a: I/O放大率对比
  - 图1b: 页内边比例对比
  - 图1c: 搜索延迟分布对比
  - 图1d: QPS-Recall曲线对比

实验二：重组织机制的性能恢复能力
  - 图2a: P99延迟随时间变化
  - 图2b: 吞吐量随时间变化
  - 图2c: 性能稳定性对比（箱线图）

实验三：异步流水线 vs. 同步阻塞
  - 图3a: QPS对比
  - 图3b: 延迟分布对比
  - 图3c: 加速比分析

实验四：并发控制扩展性
  - 图4a: 吞吐量随线程数扩展
  - 图4b: 线性扩展效率
  - 图4c: 读写性能分解

Usage:
    python plot_ablation_study.py <results_dir> [--output <output_dir>]
"""

import os
import sys
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# 设置中文字体和样式
plt.rcParams["font.sans-serif"] = ["SimHei", "DejaVu Sans", "Arial"]
plt.rcParams["axes.unicode_minus"] = False
plt.rcParams["figure.figsize"] = (12, 8)
plt.rcParams["font.size"] = 11
plt.rcParams["axes.titlesize"] = 14
plt.rcParams["axes.labelsize"] = 12
plt.rcParams["legend.fontsize"] = 11
plt.rcParams["xtick.labelsize"] = 10
plt.rcParams["ytick.labelsize"] = 10

# 配色方案
COLORS = {
    "clustered": "#0072B2",      # 蓝色 - 聚类分配
    "random": "#C73E1D",         # 红色 - 随机分配
    "with_reorg": "#0072B2",     # 蓝色 - 有重组织
    "no_reorg": "#C73E1D",       # 红色 - 无重组织
    "pipe_search": "#0072B2",    # 蓝色 - 流水线搜索
    "beam_search": "#C73E1D",    # 红色 - 同步搜索
    "DC-PDI": "#0072B2",         # 蓝色
    "Baseline": "#C73E1D",       # 红色
}

MARKERS = {
    "clustered": "o",
    "random": "D",
    "with_reorg": "o",
    "no_reorg": "D",
    "pipe_search": "o",
    "beam_search": "D",
}

LINESTYLES = {
    "clustered": "-",
    "random": "--",
    "with_reorg": "-",
    "no_reorg": "--",
    "pipe_search": "-",
    "beam_search": "--",
}

LABELS = {
    "clustered": "DC-PDI (聚类分配)",
    "random": "Baseline (随机分配)",
    "with_reorg": "DC-PDI (有重组织)",
    "no_reorg": "Baseline (无重组织)",
    "pipe_search": "DC-PDI (流水线)",
    "beam_search": "Baseline (同步)",
}


def filter_outliers(df: pd.DataFrame, columns: List[str], 
                    iqr_factor: float = 1.5, min_points: int = 4) -> pd.DataFrame:
    """使用IQR方法过滤异常值"""
    if df.empty:
        return df
    
    mask = pd.Series(True, index=df.index)
    for col in columns:
        if col not in df.columns:
            continue
        series = df[col].dropna()
        if series.size < 4:
            continue
        q1 = series.quantile(0.25)
        q3 = series.quantile(0.75)
        iqr = q3 - q1
        if iqr == 0:
            continue
        lower = q1 - iqr_factor * iqr
        upper = q3 + iqr_factor * iqr
        mask &= df[col].between(lower, upper) | df[col].isna()
    
    filtered = df[mask].copy()
    return filtered if filtered.shape[0] >= min_points else df


def plot_clustering_ablation(results_dir: str, output_dir: str, 
                             dataset_label: Optional[str] = None) -> None:
    """
    实验一：拓扑感知分配 vs. 随机分配
    绘制动态聚类消融实验的可视化图表
    """
    print("绘制实验一：动态聚类消融实验图表...")
    
    csv_file = os.path.join(results_dir, "exp_ablation_clustering.csv")
    if not os.path.exists(csv_file):
        print(f"Warning: {csv_file} not found, skipping")
        return
    
    df = pd.read_csv(csv_file)
    df = filter_outliers(df, ["qps", "avg_lat_us", "p99_lat_us", "io_amplification"])
    
    if df.empty:
        print("No data available for clustering ablation")
        return
    
    # 按模式分组
    modes = df["mode"].unique()
    
    fig, axes = plt.subplots(2, 2, figsize=(14, 12))
    
    # 图1a: I/O放大率对比
    ax1 = axes[0, 0]
    for mode in modes:
        mode_df = df[df["mode"] == mode]
        ax1.plot(mode_df["recall"], mode_df["io_amplification"],
                 marker=MARKERS.get(mode, "o"),
                 color=COLORS.get(mode, "#333333"),
                 linestyle=LINESTYLES.get(mode, "-"),
                 linewidth=2, markersize=8,
                 label=LABELS.get(mode, mode))
    ax1.set_xlabel("召回率 (Recall@10)")
    ax1.set_ylabel("I/O放大率")
    ax1.set_title("(a) I/O放大率对比", fontweight="bold")
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    ax1.axhline(y=1.0, color="gray", linestyle=":", linewidth=1)
    
    # 图1b: 页内边比例对比
    ax2 = axes[0, 1]
    if "page_local_edge_ratio" in df.columns:
        for mode in modes:
            mode_df = df[df["mode"] == mode]
            ax2.plot(mode_df["recall"], mode_df["page_local_edge_ratio"] * 100,
                     marker=MARKERS.get(mode, "o"),
                     color=COLORS.get(mode, "#333333"),
                     linestyle=LINESTYLES.get(mode, "-"),
                     linewidth=2, markersize=8,
                     label=LABELS.get(mode, mode))
        ax2.set_xlabel("召回率 (Recall@10)")
        ax2.set_ylabel("页内边比例 (%)")
        ax2.set_title("(b) 页内边比例对比", fontweight="bold")
        ax2.legend()
        ax2.grid(True, alpha=0.3)
    else:
        ax2.text(0.5, 0.5, "No page_local_edge_ratio data", 
                 ha="center", va="center", transform=ax2.transAxes)
    
    # 图1c: 搜索延迟对比 (P99)
    ax3 = axes[1, 0]
    for mode in modes:
        mode_df = df[df["mode"] == mode]
        ax3.plot(mode_df["recall"], mode_df["p99_lat_us"] / 1000,
                 marker=MARKERS.get(mode, "o"),
                 color=COLORS.get(mode, "#333333"),
                 linestyle=LINESTYLES.get(mode, "-"),
                 linewidth=2, markersize=8,
                 label=LABELS.get(mode, mode))
    ax3.set_xlabel("召回率 (Recall@10)")
    ax3.set_ylabel("P99延迟 (ms)")
    ax3.set_title("(c) P99延迟对比", fontweight="bold")
    ax3.legend()
    ax3.grid(True, alpha=0.3)
    
    # 图1d: QPS-Recall曲线对比
    ax4 = axes[1, 1]
    for mode in modes:
        mode_df = df[df["mode"] == mode]
        ax4.plot(mode_df["recall"], mode_df["qps"],
                 marker=MARKERS.get(mode, "o"),
                 color=COLORS.get(mode, "#333333"),
                 linestyle=LINESTYLES.get(mode, "-"),
                 linewidth=2, markersize=8,
                 label=LABELS.get(mode, mode))
    ax4.set_xlabel("召回率 (Recall@10)")
    ax4.set_ylabel("吞吐量 (QPS)")
    ax4.set_title("(d) 吞吐量-召回率权衡", fontweight="bold")
    ax4.legend()
    ax4.grid(True, alpha=0.3)
    
    # 设置总标题
    title = "实验一：拓扑感知分配 vs. 随机分配消融实验"
    if dataset_label:
        title = f"{dataset_label} - {title}"
    fig.suptitle(title, fontsize=16, fontweight="bold")
    plt.tight_layout(rect=[0, 0, 1, 0.96])
    
    output_file = os.path.join(output_dir, "fig_ablation_clustering.pdf")
    plt.savefig(output_file, dpi=300, bbox_inches="tight")
    print(f"Saved: {output_file}")
    plt.close()
    
    # 同时保存PNG格式
    output_file_png = os.path.join(output_dir, "fig_ablation_clustering.png")
    fig, axes = plt.subplots(2, 2, figsize=(14, 12))
    # 重新绘制...（简化，实际可以复用）
    plt.savefig(output_file_png, dpi=150, bbox_inches="tight")
    plt.close()


def plot_reorganization_ablation(results_dir: str, output_dir: str,
                                  dataset_label: Optional[str] = None) -> None:
    """
    实验二：重组织机制的性能恢复能力
    """
    print("绘制实验二：重组织机制消融实验图表...")
    
    csv_file = os.path.join(results_dir, "exp_ablation_reorganization.csv")
    if not os.path.exists(csv_file):
        print(f"Warning: {csv_file} not found, skipping")
        return
    
    df = pd.read_csv(csv_file)
    df = filter_outliers(df, ["search_qps", "search_p99_us"])
    
    if df.empty:
        print("No data available for reorganization ablation")
        return
    
    modes = df["mode"].unique()
    
    fig, axes = plt.subplots(1, 3, figsize=(18, 6))
    
    # 图2a: P99延迟随时间变化
    ax1 = axes[0]
    for mode in modes:
        mode_df = df[df["mode"] == mode]
        ax1.plot(mode_df["time_sec"], mode_df["search_p99_us"] / 1000,
                 color=COLORS.get(mode, "#333333"),
                 linestyle=LINESTYLES.get(mode, "-"),
                 linewidth=2,
                 label=LABELS.get(mode, mode))
        
        # 标记重组织触发点
        if mode == "with_reorg":
            reorg_points = mode_df[mode_df["reorg_triggered"].diff() > 0]
            if not reorg_points.empty:
                ax1.scatter(reorg_points["time_sec"], 
                           reorg_points["search_p99_us"] / 1000,
                           marker="v", s=100, color="green", zorder=5,
                           label="重组织触发")
    
    ax1.set_xlabel("时间 (秒)")
    ax1.set_ylabel("P99搜索延迟 (ms)")
    ax1.set_title("(a) P99延迟随时间变化", fontweight="bold")
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    
    # 图2b: 吞吐量随时间变化
    ax2 = axes[1]
    for mode in modes:
        mode_df = df[df["mode"] == mode]
        ax2.plot(mode_df["time_sec"], mode_df["search_qps"],
                 color=COLORS.get(mode, "#333333"),
                 linestyle=LINESTYLES.get(mode, "-"),
                 linewidth=2,
                 label=LABELS.get(mode, mode))
    ax2.set_xlabel("时间 (秒)")
    ax2.set_ylabel("搜索吞吐量 (QPS)")
    ax2.set_title("(b) 吞吐量随时间变化", fontweight="bold")
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    
    # 图2c: 性能稳定性对比（箱线图）
    ax3 = axes[2]
    data_for_box = []
    labels_for_box = []
    colors_for_box = []
    
    for mode in modes:
        mode_df = df[df["mode"] == mode]
        data_for_box.append(mode_df["search_p99_us"] / 1000)
        labels_for_box.append(LABELS.get(mode, mode))
        colors_for_box.append(COLORS.get(mode, "#333333"))
    
    bp = ax3.boxplot(data_for_box, labels=labels_for_box, patch_artist=True)
    for patch, color in zip(bp["boxes"], colors_for_box):
        patch.set_facecolor(color)
        patch.set_alpha(0.7)
    
    ax3.set_ylabel("P99延迟 (ms)")
    ax3.set_title("(c) 延迟稳定性对比", fontweight="bold")
    ax3.grid(True, alpha=0.3, axis="y")
    
    # 设置总标题
    title = "实验二：重组织机制性能恢复能力"
    if dataset_label:
        title = f"{dataset_label} - {title}"
    fig.suptitle(title, fontsize=16, fontweight="bold")
    plt.tight_layout(rect=[0, 0, 1, 0.95])
    
    output_file = os.path.join(output_dir, "fig_ablation_reorganization.pdf")
    plt.savefig(output_file, dpi=300, bbox_inches="tight")
    print(f"Saved: {output_file}")
    plt.close()


def plot_pipeline_ablation(results_dir: str, output_dir: str,
                           dataset_label: Optional[str] = None) -> None:
    """
    实验三：异步流水线 vs. 同步阻塞
    """
    print("绘制实验三：流水线消融实验图表...")
    
    csv_file = os.path.join(results_dir, "exp_ablation_pipeline.csv")
    if not os.path.exists(csv_file):
        print(f"Warning: {csv_file} not found, skipping")
        return
    
    df = pd.read_csv(csv_file)
    df = filter_outliers(df, ["qps", "avg_lat_us", "p99_lat_us"])
    
    if df.empty:
        print("No data available for pipeline ablation")
        return
    
    modes = df["search_mode"].unique()
    
    fig, axes = plt.subplots(1, 3, figsize=(18, 6))
    
    # 图3a: QPS对比
    ax1 = axes[0]
    for mode in modes:
        mode_df = df[df["search_mode"] == mode]
        ax1.plot(mode_df["recall"], mode_df["qps"],
                 marker=MARKERS.get(mode, "o"),
                 color=COLORS.get(mode, "#333333"),
                 linestyle=LINESTYLES.get(mode, "-"),
                 linewidth=2, markersize=8,
                 label=LABELS.get(mode, mode))
    ax1.set_xlabel("召回率 (Recall@10)")
    ax1.set_ylabel("吞吐量 (QPS)")
    ax1.set_title("(a) 吞吐量对比", fontweight="bold")
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    
    # 图3b: 延迟分布对比
    ax2 = axes[1]
    target_recall = 0.95
    width = 0.35
    x = np.arange(len(modes))
    
    latency_data = {"p50": [], "p95": [], "p99": []}
    mode_labels = []
    
    for mode in modes:
        mode_df = df[df["search_mode"] == mode]
        if mode_df.empty:
            continue
        closest_idx = (mode_df["recall"] - target_recall).abs().idxmin()
        latency_data["p50"].append(mode_df.loc[closest_idx, "p50_lat_us"] / 1000)
        latency_data["p95"].append(mode_df.loc[closest_idx, "p95_lat_us"] / 1000)
        latency_data["p99"].append(mode_df.loc[closest_idx, "p99_lat_us"] / 1000)
        mode_labels.append(LABELS.get(mode, mode))
    
    x = np.arange(len(mode_labels))
    ax2.bar(x - width, latency_data["p50"], width, label="P50", alpha=0.8)
    ax2.bar(x, latency_data["p95"], width, label="P95", alpha=0.8)
    ax2.bar(x + width, latency_data["p99"], width, label="P99", alpha=0.8)
    
    ax2.set_xticks(x)
    ax2.set_xticklabels(mode_labels, fontsize=10)
    ax2.set_ylabel("延迟 (ms)")
    ax2.set_title(f"(b) 延迟分布对比 (Recall≈{target_recall:.0%})", fontweight="bold")
    ax2.legend()
    ax2.grid(True, alpha=0.3, axis="y")
    
    # 图3c: 加速比分析
    ax3 = axes[2]
    if "speedup" in df.columns:
        pipe_df = df[df["search_mode"] == "pipe_search"]
        if not pipe_df.empty:
            ax3.bar(range(len(pipe_df)), pipe_df["speedup"], 
                   color=COLORS["pipe_search"], alpha=0.8)
            ax3.set_xticks(range(len(pipe_df)))
            ax3.set_xticklabels([f"L={int(l)}" for l in pipe_df["L"]], rotation=45)
            ax3.axhline(y=1.0, color="gray", linestyle="--", linewidth=2, label="基准线")
            ax3.set_ylabel("加速比")
            ax3.set_title("(c) 流水线加速比", fontweight="bold")
            ax3.legend()
            ax3.grid(True, alpha=0.3, axis="y")
    else:
        ax3.text(0.5, 0.5, "No speedup data", ha="center", va="center", 
                transform=ax3.transAxes)
    
    # 设置总标题
    title = "实验三：异步流水线 vs. 同步阻塞搜索"
    if dataset_label:
        title = f"{dataset_label} - {title}"
    fig.suptitle(title, fontsize=16, fontweight="bold")
    plt.tight_layout(rect=[0, 0, 1, 0.95])
    
    output_file = os.path.join(output_dir, "fig_ablation_pipeline.pdf")
    plt.savefig(output_file, dpi=300, bbox_inches="tight")
    print(f"Saved: {output_file}")
    plt.close()


def plot_scalability_exp(results_dir: str, output_dir: str,
                         dataset_label: Optional[str] = None) -> None:
    """
    实验四：并发控制扩展性
    """
    print("绘制实验四：并发扩展性测试图表...")
    
    csv_file = os.path.join(results_dir, "exp_ablation_scalability.csv")
    if not os.path.exists(csv_file):
        print(f"Warning: {csv_file} not found, skipping")
        return
    
    df = pd.read_csv(csv_file)
    
    if df.empty:
        print("No data available for scalability experiment")
        return
    
    fig, axes = plt.subplots(1, 3, figsize=(18, 6))
    
    # 图4a: 吞吐量随线程数扩展
    ax1 = axes[0]
    ax1.plot(df["num_threads"], df["total_qps"],
             marker="o", color=COLORS["DC-PDI"],
             linestyle="-", linewidth=2, markersize=8,
             label="实际吞吐量")
    
    # 理想线性扩展
    if len(df) > 0:
        base_qps = df.iloc[0]["total_qps"]
        ideal_qps = [base_qps * t / df.iloc[0]["num_threads"] for t in df["num_threads"]]
        ax1.plot(df["num_threads"], ideal_qps,
                 marker="", color="gray",
                 linestyle="--", linewidth=2,
                 label="理想线性扩展")
    
    ax1.set_xlabel("线程数")
    ax1.set_ylabel("总吞吐量 (ops/s)")
    ax1.set_title("(a) 吞吐量随线程数扩展", fontweight="bold")
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    ax1.set_xscale("log", base=2)
    
    # 图4b: 线性扩展效率
    ax2 = axes[1]
    ax2.bar(range(len(df)), df["linear_efficiency"] * 100,
           color=COLORS["DC-PDI"], alpha=0.8)
    ax2.set_xticks(range(len(df)))
    ax2.set_xticklabels([str(int(t)) for t in df["num_threads"]])
    ax2.axhline(y=100, color="gray", linestyle="--", linewidth=2)
    ax2.set_xlabel("线程数")
    ax2.set_ylabel("线性效率 (%)")
    ax2.set_title("(b) 线性扩展效率", fontweight="bold")
    ax2.grid(True, alpha=0.3, axis="y")
    
    # 添加数值标签
    for i, (idx, row) in enumerate(df.iterrows()):
        ax2.text(i, row["linear_efficiency"] * 100 + 2, 
                f"{row['linear_efficiency']*100:.1f}%",
                ha="center", va="bottom", fontsize=9)
    
    # 图4c: 读写性能分解
    ax3 = axes[2]
    width = 0.35
    x = np.arange(len(df))
    
    ax3.bar(x - width/2, df["search_qps"], width, label="搜索", 
           color=COLORS["DC-PDI"], alpha=0.8)
    ax3.bar(x + width/2, df["insert_tps"], width, label="插入",
           color=COLORS["Baseline"], alpha=0.8)
    
    ax3.set_xticks(x)
    ax3.set_xticklabels([str(int(t)) for t in df["num_threads"]])
    ax3.set_xlabel("线程数")
    ax3.set_ylabel("吞吐量 (ops/s)")
    ax3.set_title("(c) 读写性能分解", fontweight="bold")
    ax3.legend()
    ax3.grid(True, alpha=0.3, axis="y")
    
    # 设置总标题
    title = "实验四：并发控制协议扩展性测试"
    if dataset_label:
        title = f"{dataset_label} - {title}"
    fig.suptitle(title, fontsize=16, fontweight="bold")
    plt.tight_layout(rect=[0, 0, 1, 0.95])
    
    output_file = os.path.join(output_dir, "fig_ablation_scalability.pdf")
    plt.savefig(output_file, dpi=300, bbox_inches="tight")
    print(f"Saved: {output_file}")
    plt.close()


def generate_ablation_summary(results_dir: str, output_dir: str,
                              dataset_label: Optional[str] = None) -> None:
    """生成消融实验汇总表格"""
    print("生成消融实验汇总表格...")
    
    summary = []
    
    # 实验一数据
    exp1_file = os.path.join(results_dir, "exp_ablation_clustering.csv")
    if os.path.exists(exp1_file):
        df1 = pd.read_csv(exp1_file)
        for mode in df1["mode"].unique():
            mode_df = df1[df1["mode"] == mode]
            if mode_df.empty:
                continue
            # 取95%召回率的数据
            target_idx = (mode_df["recall"] - 0.95).abs().idxmin()
            row = {
                "Experiment": "Exp1: Clustering",
                "Mode": LABELS.get(mode, mode),
                "Recall@95%": f"{mode_df.loc[target_idx, 'recall']:.4f}",
                "QPS": int(mode_df.loc[target_idx, "qps"]),
                "P99 (ms)": f"{mode_df.loc[target_idx, 'p99_lat_us']/1000:.2f}",
                "IO-Amp": f"{mode_df.loc[target_idx, 'io_amplification']:.2f}",
            }
            summary.append(row)
    
    # 实验二数据
    exp2_file = os.path.join(results_dir, "exp_ablation_reorganization.csv")
    if os.path.exists(exp2_file):
        df2 = pd.read_csv(exp2_file)
        for mode in df2["mode"].unique():
            mode_df = df2[df2["mode"] == mode]
            if mode_df.empty:
                continue
            row = {
                "Experiment": "Exp2: Reorganization",
                "Mode": LABELS.get(mode, mode),
                "Avg QPS": int(mode_df["search_qps"].mean()),
                "P99 Std (ms)": f"{mode_df['search_p99_us'].std()/1000:.2f}",
                "Stability": f"{(1 - mode_df['search_qps'].std()/mode_df['search_qps'].mean())*100:.1f}%",
            }
            summary.append(row)
    
    # 实验三数据
    exp3_file = os.path.join(results_dir, "exp_ablation_pipeline.csv")
    if os.path.exists(exp3_file):
        df3 = pd.read_csv(exp3_file)
        for mode in df3["search_mode"].unique():
            mode_df = df3[df3["search_mode"] == mode]
            if mode_df.empty:
                continue
            target_idx = (mode_df["recall"] - 0.95).abs().idxmin()
            speedup = mode_df.loc[target_idx, "speedup"] if "speedup" in mode_df.columns else 1.0
            row = {
                "Experiment": "Exp3: Pipeline",
                "Mode": LABELS.get(mode, mode),
                "QPS@95%": int(mode_df.loc[target_idx, "qps"]),
                "P99 (ms)": f"{mode_df.loc[target_idx, 'p99_lat_us']/1000:.2f}",
                "Speedup": f"{speedup:.2f}x",
            }
            summary.append(row)
    
    # 实验四数据
    exp4_file = os.path.join(results_dir, "exp_ablation_scalability.csv")
    if os.path.exists(exp4_file):
        df4 = pd.read_csv(exp4_file)
        for _, row_data in df4.iterrows():
            row = {
                "Experiment": "Exp4: Scalability",
                "Threads": int(row_data["num_threads"]),
                "Total QPS": int(row_data["total_qps"]),
                "Linear Eff": f"{row_data['linear_efficiency']*100:.1f}%",
            }
            summary.append(row)
    
    if summary:
        summary_df = pd.DataFrame(summary)
        output_file = os.path.join(output_dir, "ablation_summary.csv")
        summary_df.to_csv(output_file, index=False)
        print(f"Saved: {output_file}")
        
        # 打印汇总
        print("\n" + "=" * 80)
        print("消融实验汇总")
        print("=" * 80)
        print(summary_df.to_string(index=False))
        print("=" * 80)


def main():
    if len(sys.argv) < 2:
        print("Usage: python plot_ablation_study.py <results_dir> [--output <output_dir>]")
        sys.exit(1)
    
    results_dir = sys.argv[1]
    
    # 解析输出目录
    output_dir = results_dir
    if "--output" in sys.argv:
        output_idx = sys.argv.index("--output")
        if output_idx + 1 < len(sys.argv):
            output_dir = sys.argv[output_idx + 1]
    
    # 创建figures子目录
    figures_dir = os.path.join(output_dir, "figures")
    Path(figures_dir).mkdir(parents=True, exist_ok=True)
    
    print(f"Results directory: {results_dir}")
    print(f"Output directory: {figures_dir}")
    print("")
    
    # 检测数据集
    dataset_label = None
    parent_name = os.path.basename(results_dir).lower()
    if "sift" in parent_name:
        dataset_label = "SIFT1B"
    elif "deep" in parent_name:
        dataset_label = "DEEP1B"
    elif "gist" in parent_name:
        dataset_label = "GIST"
    
    # 绘制各实验图表
    plot_clustering_ablation(results_dir, figures_dir, dataset_label)
    plot_reorganization_ablation(results_dir, figures_dir, dataset_label)
    plot_pipeline_ablation(results_dir, figures_dir, dataset_label)
    plot_scalability_exp(results_dir, figures_dir, dataset_label)
    
    # 生成汇总表格
    generate_ablation_summary(results_dir, output_dir, dataset_label)
    
    print("\nAll ablation plots generated successfully!")


if __name__ == "__main__":
    main()
