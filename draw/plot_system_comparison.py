#!/usr/bin/env python3
"""
plot_system_comparison.py - 可视化DC-PDI、IP-DiskANN和FreshDiskANN的对比实验结果

Usage:
    python plot_system_comparison.py <results_dir> [--output <output_dir>]
"""

import os
import sys
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from pathlib import Path

# 设置中文字体和样式
plt.rcParams["font.sans-serif"] = ["SimHei", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False
plt.rcParams["figure.figsize"] = (12, 8)
plt.rcParams["font.size"] = 11

# 系统颜色配置（与plot_thesis_figures.py保持一致）
COLORS = {
    "DC-PDI": "#0072B2",  # 蓝色
    "IP-DiskANN": "#C73E1D",  # 红色
    "FreshDiskANN": "#F18F01",  # 橙色
}

MARKERS = {"DC-PDI": "o", "IP-DiskANN": "D", "FreshDiskANN": "^"}

LINESTYLES = {"DC-PDI": "-", "IP-DiskANN": "--", "FreshDiskANN": "-."}


def filter_outliers(df, columns, iqr_factor=1.5, min_points=4):
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
    if filtered.shape[0] < min_points:
        return df
    return filtered


def plot_search_latency_comparison(
    results_dir, output_dir, dataset_label=None, dataset_tag=None
):
    """
    绘制搜索延迟对比图（实验1）
    包含: QPS-Recall曲线, P99延迟对比, IO放大率对比
    """
    print("Plotting search latency comparison...")

    # 读取所有系统的数据
    data = {}
    for system in ["dc-pdi", "ip-diskann", "fresh-diskann"]:
        csv_file = os.path.join(results_dir, f"exp1_search_latency_{system}.csv")
        if os.path.exists(csv_file):
            df = pd.read_csv(csv_file)
            df = filter_outliers(
                df,
                [
                    "qps",
                    "avg_lat_us",
                    "p50_lat_us",
                    "p90_lat_us",
                    "p99_lat_us",
                    "io_amplification",
                ],
            )
            system_name = system.replace("-", "-").upper()
            if system == "fresh-diskann":
                system_name = "FreshDiskANN"
            elif system == "ip-diskann":
                system_name = "IP-DiskANN"
            data[system_name] = df
        else:
            print(f"Warning: {csv_file} not found, skipping {system}")

    if not data:
        print("No data found for search latency comparison")
        return

    suffix = f"_{dataset_tag}" if dataset_tag else ""
    has_concurrent_schema = all("recall_pct" in df.columns for df in data.values())
    if not has_concurrent_schema:
        print("Search latency data is legacy format; skipping exp1 plots.")
        return

    summary_rows = []
    for system_name, df in data.items():
        target_recall = df.get("recall_target", pd.Series([90.0])).iloc[0]
        closest_idx = (df["recall_pct"] - target_recall).abs().idxmin()
        row = df.loc[closest_idx]
        summary_rows.append(
            {
                "System": system_name,
                "Recall@10(%)": row["recall_pct"],
                "P50(ms)": row["p50_lat_us"] / 1000.0,
                "P90(ms)": row["p90_lat_us"] / 1000.0,
                "P99(ms)": row["p99_lat_us"] / 1000.0,
                "QPS": row["search_qps"],
                "Memory(MB)": row["memory_rss_mb"],
                "MeanIOs": row["mean_ios"],
            }
        )

    summary_df = pd.DataFrame(summary_rows)
    summary_csv = os.path.join(output_dir, f"exp1_target_recall_summary{suffix}.csv")
    summary_df.to_csv(summary_csv, index=False)

    fig, axes = plt.subplots(4, 2, figsize=(16, 16))
    metrics = [
        ("p50_lat_us", "P50延迟 (ms)", 1000.0),
        ("p90_lat_us", "P90延迟 (ms)", 1000.0),
        ("p99_lat_us", "P99延迟 (ms)", 1000.0),
        ("search_qps", "吞吐量 (QPS)", 1.0),
        ("memory_rss_mb", "内存 (MB)", 1.0),
        ("mean_ios", "平均页面访问数", 1.0),
        ("recall_pct", "Recall@10 (%)", 1.0),
    ]

    dc_reorg_color = "#56B4E9"
    dc_normal_color = COLORS["DC-PDI"]

    for idx, (metric_key, ylabel, scale) in enumerate(metrics):
        ax = axes[idx // 2, idx % 2]
        for system_name, df in data.items():
            if system_name == "DC-PDI":
                reorg_mask = (
                    df.get("reorg_running", pd.Series([0] * len(df))).astype(int) == 1
                )
                ax.plot(
                    df.loc[~reorg_mask, "time_sec"],
                    df.loc[~reorg_mask, metric_key] / scale,
                    color=dc_normal_color,
                    linestyle="-",
                    linewidth=1.8,
                    label="DC-PDI (normal)" if idx == 0 else None,
                )
                ax.plot(
                    df.loc[reorg_mask, "time_sec"],
                    df.loc[reorg_mask, metric_key] / scale,
                    color=dc_reorg_color,
                    linestyle="-",
                    linewidth=1.8,
                    label="DC-PDI (reorg)" if idx == 0 else None,
                )
            else:
                ax.plot(
                    df["time_sec"],
                    df[metric_key] / scale,
                    color=COLORS[system_name],
                    linestyle=LINESTYLES[system_name],
                    linewidth=1.8,
                    label=system_name if idx == 0 else None,
                )
        ax.set_ylabel(ylabel, fontsize=12)
        ax.set_xlabel("时间 (s)", fontsize=11)
        ax.set_title(metric_key, fontsize=13, fontweight="bold")
        ax.grid(True, alpha=0.3)
        if idx == 0:
            ax.legend(fontsize=10)

    axes[3, 1].axis("off")

    if dataset_label:
        fig.suptitle(f"{dataset_label}", fontsize=15, fontweight="bold")
        plt.tight_layout(rect=[0, 0, 1, 0.96])
    else:
        plt.tight_layout()

    output_file = os.path.join(output_dir, f"fig_search_latency_comparison{suffix}.pdf")
    plt.savefig(output_file, dpi=300, bbox_inches="tight")
    print(f"Saved: {output_file}")
    plt.close()


def plot_update_throughput_comparison(
    results_dir, output_dir, dataset_label=None, dataset_tag=None
):
    """
    绘制更新吞吐量对比图（实验2）
    """
    print("Plotting update throughput comparison...")

    data = {}
    for system in ["dc-pdi", "ip-diskann", "fresh-diskann"]:
        csv_file = os.path.join(results_dir, f"exp2_update_throughput_{system}.csv")
        if os.path.exists(csv_file):
            df = pd.read_csv(csv_file)
            df = filter_outliers(df, ["throughput_ops", "memory_rss_mb"])
            system_name = system.replace("-", "-").upper()
            if system == "fresh-diskann":
                system_name = "FreshDiskANN"
            elif system == "ip-diskann":
                system_name = "IP-DiskANN"
            data[system_name] = df
        else:
            print(f"Warning: {csv_file} not found")

    if not data:
        print("No data found for update throughput comparison")
        return

    fig, axes = plt.subplots(1, 2, figsize=(16, 6))

    # 1. 吞吐量随时间变化
    ax1 = axes[0]
    for system_name, df in data.items():
        ax1.plot(
            df["time_sec"],
            df["throughput_ops"],
            color=COLORS[system_name],
            linestyle=LINESTYLES[system_name],
            linewidth=2,
            label=system_name,
        )

        # 标记merge点
        merge_points = df[df["merge_triggered"] == 1]
        if not merge_points.empty:
            ax1.scatter(
                merge_points["time_sec"],
                merge_points["throughput_ops"],
                color=COLORS[system_name],
                marker="x",
                s=100,
                zorder=5,
            )

    ax1.set_xlabel("时间 (秒)", fontsize=13)
    ax1.set_ylabel("更新吞吐量 (ops/s)", fontsize=13)
    ax1.set_title("(a) 更新吞吐量随时间变化", fontsize=14, fontweight="bold")
    ax1.legend(fontsize=12)
    ax1.grid(True, alpha=0.3)

    # 2. 内存使用对比
    ax2 = axes[1]
    for system_name, df in data.items():
        ax2.plot(
            df["num_inserts"],
            df["memory_rss_mb"],
            color=COLORS[system_name],
            linestyle=LINESTYLES[system_name],
            linewidth=2,
            label=system_name,
        )

    ax2.set_xlabel("累计更新次数", fontsize=13)
    ax2.set_ylabel("内存使用 (MB)", fontsize=13)
    ax2.set_title("(b) 内存使用随更新次数变化", fontsize=14, fontweight="bold")
    ax2.legend(fontsize=12)
    ax2.grid(True, alpha=0.3)

    if dataset_label:
        fig.suptitle(f"{dataset_label}", fontsize=15, fontweight="bold")
        plt.tight_layout(rect=[0, 0, 1, 0.95])
    else:
        plt.tight_layout()

    suffix = f"_{dataset_tag}" if dataset_tag else ""
    output_file = os.path.join(
        output_dir, f"fig_update_throughput_comparison{suffix}.pdf"
    )
    plt.savefig(output_file, dpi=300, bbox_inches="tight")
    print(f"Saved: {output_file}")
    plt.close()


def plot_concurrent_performance_comparison(
    results_dir, output_dir, dataset_label=None, dataset_tag=None
):
    """
    绘制读写并发性能对比图（实验3）
    """
    print("Plotting concurrent performance comparison...")

    data = {}
    for system in ["dc-pdi", "ip-diskann", "fresh-diskann"]:
        csv_file = os.path.join(results_dir, f"exp3_concurrent_{system}.csv")
        if os.path.exists(csv_file):
            df = pd.read_csv(csv_file)
            df = filter_outliers(df, ["search_qps", "search_p99_us", "insert_tput"])
            system_name = system.replace("-", "-").upper()
            if system == "fresh-diskann":
                system_name = "FreshDiskANN"
            elif system == "ip-diskann":
                system_name = "IP-DiskANN"
            data[system_name] = df
        else:
            print(f"Warning: {csv_file} not found")

    if not data:
        print("No data found for concurrent performance comparison")
        return

    fig, axes = plt.subplots(2, 2, figsize=(16, 12))

    # 1. 搜索QPS随时间变化
    ax1 = axes[0, 0]
    for system_name, df in data.items():
        ax1.plot(
            df["time_sec"],
            df["search_qps"],
            color=COLORS[system_name],
            linestyle=LINESTYLES[system_name],
            linewidth=2,
            label=system_name,
        )
    ax1.set_xlabel("时间 (秒)", fontsize=13)
    ax1.set_ylabel("搜索QPS", fontsize=13)
    ax1.set_title("(a) 并发搜索吞吐量", fontsize=14, fontweight="bold")
    ax1.legend(fontsize=12)
    ax1.grid(True, alpha=0.3)

    # 2. P99延迟随时间变化
    ax2 = axes[0, 1]
    for system_name, df in data.items():
        ax2.plot(
            df["time_sec"],
            df["search_p99_us"] / 1000,
            color=COLORS[system_name],
            linestyle=LINESTYLES[system_name],
            linewidth=2,
            label=system_name,
        )
    ax2.set_xlabel("时间 (秒)", fontsize=13)
    ax2.set_ylabel("P99搜索延迟 (ms)", fontsize=13)
    ax2.set_title("(b) 并发搜索P99延迟", fontsize=14, fontweight="bold")
    ax2.legend(fontsize=12)
    ax2.grid(True, alpha=0.3)

    # 3. 插入吞吐量随时间变化
    ax3 = axes[1, 0]
    for system_name, df in data.items():
        ax3.plot(
            df["time_sec"],
            df["insert_tput"],
            color=COLORS[system_name],
            linestyle=LINESTYLES[system_name],
            linewidth=2,
            label=system_name,
        )
    ax3.set_xlabel("时间 (秒)", fontsize=13)
    ax3.set_ylabel("插入吞吐量 (ops/s)", fontsize=13)
    ax3.set_title("(c) 并发插入吞吐量", fontsize=14, fontweight="bold")
    ax3.legend(fontsize=12)
    ax3.grid(True, alpha=0.3)

    # 4. 稳定性分析（QPS的变异系数）
    ax4 = axes[1, 1]
    stability_data = []
    labels = []
    for system_name, df in data.items():
        # 计算QPS的变异系数 (CV = std / mean)
        qps_mean = df["search_qps"].mean()
        qps_std = df["search_qps"].std()
        cv = (qps_std / qps_mean) * 100 if qps_mean > 0 else 0

        stability_data.append(cv)
        labels.append(system_name)

    positions = np.arange(len(labels))
    bars = ax4.bar(positions, stability_data, width=0.6, alpha=0.8)

    # 为每个柱子设置对应的颜色
    for bar, system_name in zip(bars, labels):
        bar.set_color(COLORS[system_name])

    ax4.set_xticks(positions)
    ax4.set_xticklabels(labels, fontsize=12)
    ax4.set_ylabel("QPS变异系数 (%)", fontsize=13)
    ax4.set_title("(d) 搜索性能稳定性", fontsize=14, fontweight="bold")
    ax4.grid(True, alpha=0.3, axis="y")

    # 添加数值标签
    for i, (pos, val) in enumerate(zip(positions, stability_data)):
        ax4.text(pos, val + 0.5, f"{val:.1f}%", ha="center", va="bottom", fontsize=11)

    if dataset_label:
        fig.suptitle(f"{dataset_label}", fontsize=15, fontweight="bold")
        plt.tight_layout(rect=[0, 0, 1, 0.96])
    else:
        plt.tight_layout()

    suffix = f"_{dataset_tag}" if dataset_tag else ""
    output_file = os.path.join(
        output_dir, f"fig_concurrent_performance_comparison{suffix}.pdf"
    )
    plt.savefig(output_file, dpi=300, bbox_inches="tight")
    print(f"Saved: {output_file}")
    plt.close()


def generate_summary_table(
    results_dir, output_dir, dataset_label=None, dataset_tag=None
):
    """
    生成汇总对比表格
    """
    print("Generating summary table...")

    summary = []

    # 从各个实验中提取关键指标
    for system in ["dc-pdi", "ip-diskann", "fresh-diskann"]:
        system_name = system.replace("-", "-").upper()
        if system == "fresh-diskann":
            system_name = "FreshDiskANN"
        elif system == "ip-diskann":
            system_name = "IP-DiskANN"

        row: dict[str, object] = {"System": system_name}

        # 从实验1获取搜索性能（目标召回率）
        exp1_file = os.path.join(results_dir, f"exp1_search_latency_{system}.csv")
        if os.path.exists(exp1_file):
            df1 = pd.read_csv(exp1_file)
            if "recall_pct" in df1.columns:
                df1 = filter_outliers(
                    df1,
                    [
                        "search_qps",
                        "p50_lat_us",
                        "p90_lat_us",
                        "p99_lat_us",
                        "mean_ios",
                    ],
                )
                if df1.empty:
                    summary.append(row)
                    continue
                target_recall = df1.get("recall_target", pd.Series([90.0])).iloc[0]
                closest_idx = (df1["recall_pct"] - target_recall).abs().idxmin()
                row["QPS@90%"] = int(df1.loc[closest_idx, "search_qps"])
                row["P99(ms)@90%"] = f"{df1.loc[closest_idx, 'p99_lat_us'] / 1000:.2f}"
                row["MeanIOs@90%"] = f"{df1.loc[closest_idx, 'mean_ios']:.2f}"
            elif "recall" in df1.columns:
                df1 = filter_outliers(
                    df1,
                    [
                        "qps",
                        "avg_lat_us",
                        "p50_lat_us",
                        "p90_lat_us",
                        "p99_lat_us",
                        "io_amplification",
                    ],
                )
                if df1.empty:
                    summary.append(row)
                    continue
                target_recall = 0.95
                closest_idx = (df1["recall"] - target_recall).abs().idxmin()
                row["QPS@95%"] = int(df1.loc[closest_idx, "qps"])
                row["P99(ms)@95%"] = f"{df1.loc[closest_idx, 'p99_lat_us'] / 1000:.2f}"
                row["IO-Amp@95%"] = f"{df1.loc[closest_idx, 'io_amplification']:.2f}"

        # 从实验2获取更新性能
        exp2_file = os.path.join(results_dir, f"exp2_update_throughput_{system}.csv")
        if os.path.exists(exp2_file):
            df2 = pd.read_csv(exp2_file)
            df2 = filter_outliers(df2, ["throughput_ops", "memory_rss_mb"])
            if df2.empty:
                summary.append(row)
                continue
            # 取最后10%的平均吞吐量作为稳定值
            stable_df = df2.iloc[int(len(df2) * 0.9) :]
            row["Insert-TPS"] = int(stable_df["throughput_ops"].mean())
            row["Peak-Mem(MB)"] = int(df2["memory_rss_mb"].max())

        # 从实验3获取并发性能
        exp3_file = os.path.join(results_dir, f"exp3_concurrent_{system}.csv")
        if os.path.exists(exp3_file):
            df3 = pd.read_csv(exp3_file)
            df3 = filter_outliers(df3, ["search_qps", "search_p99_us", "insert_tput"])
            if df3.empty:
                summary.append(row)
                continue
            row["Concurrent-QPS"] = int(df3["search_qps"].mean())
            row["QPS-Stability(%)"] = (
                f"{(df3['search_qps'].std() / df3['search_qps'].mean() * 100):.1f}"
            )

        summary.append(row)

    # 创建DataFrame并保存
    summary_df = pd.DataFrame(summary)
    suffix = f"_{dataset_tag}" if dataset_tag else ""
    output_file = os.path.join(output_dir, f"system_comparison_summary{suffix}.csv")
    summary_df.to_csv(output_file, index=False)
    print(f"Saved: {output_file}")

    # 打印到控制台
    title = "系统对比汇总表"
    if dataset_label:
        title = f"{dataset_label} - {title}"
    print("\n" + "=" * 100)
    print(title)
    print("=" * 100)
    print(summary_df.to_string(index=False))
    print("=" * 100)


def has_results_files(results_path: Path) -> bool:
    return (
        any(results_path.glob("exp1_search_latency_*.csv"))
        or any(results_path.glob("exp2_update_throughput_*.csv"))
        or any(results_path.glob("exp3_concurrent_*.csv"))
    )


def resolve_dataset_label(dataset_name: str) -> str:
    name = dataset_name.lower()
    mapping = {
        "sift": "SIFT1B",
        "sift1b": "SIFT1B",
        "deep": "DEEP1B",
        "deep1b": "DEEP1B",
        "gist": "GIST",
    }
    return mapping.get(name, dataset_name.upper())


def collect_datasets(results_dir: str):
    results_path = Path(results_dir)
    if has_results_files(results_path):
        return [(results_path.name, results_path)]

    datasets = []
    for child in sorted(results_path.iterdir()):
        if child.is_dir() and has_results_files(child):
            datasets.append((child.name, child))
    return datasets


def main():
    if len(sys.argv) < 2:
        print(
            "Usage: python plot_system_comparison.py <results_dir> [--output <output_dir>]"
        )
        sys.exit(1)

    results_dir = sys.argv[1]

    # 解析输出目录
    output_dir = results_dir  # 默认与输入目录相同
    if "--output" in sys.argv:
        output_idx = sys.argv.index("--output")
        if output_idx + 1 < len(sys.argv):
            output_dir = sys.argv[output_idx + 1]

    datasets = collect_datasets(results_dir)
    if not datasets:
        print("No dataset results found. Ensure exp1/exp2/exp3 CSVs exist.")
        sys.exit(1)

    for dataset_name, dataset_path in datasets:
        dataset_label = resolve_dataset_label(dataset_name)
        dataset_tag = dataset_name

        dataset_output_dir = output_dir
        if len(datasets) > 1:
            dataset_output_dir = os.path.join(output_dir, dataset_name)
        figures_dir = os.path.join(dataset_output_dir, "figures")
        Path(figures_dir).mkdir(parents=True, exist_ok=True)

        print(f"Results directory: {dataset_path}")
        print(f"Output directory: {figures_dir}")
        print("")

        plot_search_latency_comparison(
            str(dataset_path),
            figures_dir,
            dataset_label=dataset_label,
            dataset_tag=dataset_tag,
        )
        plot_update_throughput_comparison(
            str(dataset_path),
            figures_dir,
            dataset_label=dataset_label,
            dataset_tag=dataset_tag,
        )
        plot_concurrent_performance_comparison(
            str(dataset_path),
            figures_dir,
            dataset_label=dataset_label,
            dataset_tag=dataset_tag,
        )
        generate_summary_table(
            str(dataset_path),
            dataset_output_dir,
            dataset_label=dataset_label,
            dataset_tag=dataset_tag,
        )

    print("\nAll plots generated successfully!")


if __name__ == "__main__":
    main()
