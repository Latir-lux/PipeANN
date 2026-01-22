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
plt.rcParams['font.sans-serif'] = ['SimHei', 'DejaVu Sans']
plt.rcParams['axes.unicode_minus'] = False
plt.rcParams['figure.figsize'] = (12, 8)
plt.rcParams['font.size'] = 11

# 系统颜色配置（与plot_thesis_figures.py保持一致）
COLORS = {
    'DC-PDI': '#0072B2',       # 蓝色
    'IP-DiskANN': '#C73E1D',   # 红色
    'FreshDiskANN': '#F18F01'  # 橙色
}

MARKERS = {
    'DC-PDI': 'o',
    'IP-DiskANN': 'D',
    'FreshDiskANN': '^'
}

LINESTYLES = {
    'DC-PDI': '-',
    'IP-DiskANN': '--',
    'FreshDiskANN': '-.'
}


def plot_search_latency_comparison(results_dir, output_dir):
    """
    绘制搜索延迟对比图（实验1）
    包含: QPS-Recall曲线, P99延迟对比, IO放大率对比
    """
    print("Plotting search latency comparison...")
    
    # 读取所有系统的数据
    data = {}
    for system in ['dc-pdi', 'ip-diskann', 'fresh-diskann']:
        csv_file = os.path.join(results_dir, f'exp1_search_latency_{system}.csv')
        if os.path.exists(csv_file):
            df = pd.read_csv(csv_file)
            system_name = system.replace('-', '-').upper()
            if system == 'fresh-diskann':
                system_name = 'FreshDiskANN'
            elif system == 'ip-diskann':
                system_name = 'IP-DiskANN'
            data[system_name] = df
        else:
            print(f"Warning: {csv_file} not found, skipping {system}")
    
    if not data:
        print("No data found for search latency comparison")
        return
    
    # 创建子图
    fig, axes = plt.subplots(2, 2, figsize=(16, 12))
    
    # 1. QPS vs Recall
    ax1 = axes[0, 0]
    for system_name, df in data.items():
        ax1.plot(df['recall'], df['qps'], 
                marker=MARKERS[system_name],
                color=COLORS[system_name],
                linestyle=LINESTYLES[system_name],
                linewidth=2, markersize=8, label=system_name)
    ax1.set_xlabel('召回率 (Recall@10)', fontsize=13)
    ax1.set_ylabel('吞吐量 (QPS)', fontsize=13)
    ax1.set_title('(a) 吞吐量-召回率权衡', fontsize=14, fontweight='bold')
    ax1.legend(fontsize=12)
    ax1.grid(True, alpha=0.3)
    
    # 2. P99 Latency vs Recall
    ax2 = axes[0, 1]
    for system_name, df in data.items():
        ax2.plot(df['recall'], df['p99_lat_us'] / 1000,  # 转换为ms
                marker=MARKERS[system_name],
                color=COLORS[system_name],
                linestyle=LINESTYLES[system_name],
                linewidth=2, markersize=8, label=system_name)
    ax2.set_xlabel('召回率 (Recall@10)', fontsize=13)
    ax2.set_ylabel('P99延迟 (ms)', fontsize=13)
    ax2.set_title('(b) P99尾延迟对比', fontsize=14, fontweight='bold')
    ax2.legend(fontsize=12)
    ax2.grid(True, alpha=0.3)
    
    # 3. 平均延迟分布（箱线图）
    ax3 = axes[1, 0]
    target_recall = 0.95  # 选择95%召回率的数据
    latency_data = []
    labels = []
    for system_name, df in data.items():
        # 找到最接近target_recall的数据点
        closest_idx = (df['recall'] - target_recall).abs().idxmin()
        avg_lat = df.loc[closest_idx, 'avg_lat_us'] / 1000  # 转换为ms
        p50 = df.loc[closest_idx, 'p50_lat_us'] / 1000
        p90 = df.loc[closest_idx, 'p90_lat_us'] / 1000
        p99 = df.loc[closest_idx, 'p99_lat_us'] / 1000
        
        # 模拟分布数据用于箱线图
        latency_data.append([p50, avg_lat, p90, p99])
        labels.append(system_name)
    
    positions = np.arange(len(labels))
    for i, (system_name, data_point) in enumerate(zip(labels, latency_data)):
        ax3.bar(positions[i], data_point[3], width=0.6,
               color=COLORS[system_name], alpha=0.7, label=system_name)
        # 添加P50, AVG, P90的标记
        ax3.plot([positions[i]], [data_point[0]], 'ko', markersize=6)  # P50
        ax3.plot([positions[i]], [data_point[1]], 'k^', markersize=6)  # AVG
        ax3.plot([positions[i]], [data_point[2]], 'ks', markersize=6)  # P90
    
    ax3.set_xticks(positions)
    ax3.set_xticklabels(labels, fontsize=12)
    ax3.set_ylabel('延迟 (ms)', fontsize=13)
    ax3.set_title(f'(c) 延迟分布对比 (Recall≈{target_recall:.0%})', fontsize=14, fontweight='bold')
    ax3.legend(fontsize=12)
    ax3.grid(True, alpha=0.3, axis='y')
    
    # 添加图例说明
    from matplotlib.lines import Line2D
    custom_lines = [Line2D([0], [0], color='k', marker='o', linestyle='', markersize=6),
                   Line2D([0], [0], color='k', marker='^', linestyle='', markersize=6),
                   Line2D([0], [0], color='k', marker='s', linestyle='', markersize=6)]
    ax3.legend(custom_lines, ['P50', 'AVG', 'P90'], loc='upper left', fontsize=10)
    
    # 4. IO放大率对比
    ax4 = axes[1, 1]
    for system_name, df in data.items():
        ax4.plot(df['recall'], df['io_amplification'],
                marker=MARKERS[system_name],
                color=COLORS[system_name],
                linestyle=LINESTYLES[system_name],
                linewidth=2, markersize=8, label=system_name)
    ax4.set_xlabel('召回率 (Recall@10)', fontsize=13)
    ax4.set_ylabel('I/O放大率', fontsize=13)
    ax4.set_title('(d) I/O放大率对比', fontsize=14, fontweight='bold')
    ax4.legend(fontsize=12)
    ax4.grid(True, alpha=0.3)
    ax4.axhline(y=1.0, color='gray', linestyle=':', linewidth=1, label='理想值')
    
    plt.tight_layout()
    output_file = os.path.join(output_dir, 'fig_search_latency_comparison.pdf')
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"Saved: {output_file}")
    plt.close()


def plot_update_throughput_comparison(results_dir, output_dir):
    """
    绘制更新吞吐量对比图（实验2）
    """
    print("Plotting update throughput comparison...")
    
    data = {}
    for system in ['dc-pdi', 'ip-diskann', 'fresh-diskann']:
        csv_file = os.path.join(results_dir, f'exp2_update_throughput_{system}.csv')
        if os.path.exists(csv_file):
            df = pd.read_csv(csv_file)
            system_name = system.replace('-', '-').upper()
            if system == 'fresh-diskann':
                system_name = 'FreshDiskANN'
            elif system == 'ip-diskann':
                system_name = 'IP-DiskANN'
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
        ax1.plot(df['time_sec'], df['throughput_ops'],
                color=COLORS[system_name],
                linestyle=LINESTYLES[system_name],
                linewidth=2, label=system_name)
        
        # 标记merge点
        merge_points = df[df['merge_triggered'] == 1]
        if not merge_points.empty:
            ax1.scatter(merge_points['time_sec'], merge_points['throughput_ops'],
                       color=COLORS[system_name], marker='x', s=100, zorder=5)
    
    ax1.set_xlabel('时间 (秒)', fontsize=13)
    ax1.set_ylabel('更新吞吐量 (ops/s)', fontsize=13)
    ax1.set_title('(a) 更新吞吐量随时间变化', fontsize=14, fontweight='bold')
    ax1.legend(fontsize=12)
    ax1.grid(True, alpha=0.3)
    
    # 2. 内存使用对比
    ax2 = axes[1]
    for system_name, df in data.items():
        ax2.plot(df['num_inserts'], df['memory_rss_mb'],
                color=COLORS[system_name],
                linestyle=LINESTYLES[system_name],
                linewidth=2, label=system_name)
    
    ax2.set_xlabel('累计更新次数', fontsize=13)
    ax2.set_ylabel('内存使用 (MB)', fontsize=13)
    ax2.set_title('(b) 内存使用随更新次数变化', fontsize=14, fontweight='bold')
    ax2.legend(fontsize=12)
    ax2.grid(True, alpha=0.3)
    
    plt.tight_layout()
    output_file = os.path.join(output_dir, 'fig_update_throughput_comparison.pdf')
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"Saved: {output_file}")
    plt.close()


def plot_concurrent_performance_comparison(results_dir, output_dir):
    """
    绘制读写并发性能对比图（实验3）
    """
    print("Plotting concurrent performance comparison...")
    
    data = {}
    for system in ['dc-pdi', 'ip-diskann', 'fresh-diskann']:
        csv_file = os.path.join(results_dir, f'exp3_concurrent_{system}.csv')
        if os.path.exists(csv_file):
            df = pd.read_csv(csv_file)
            system_name = system.replace('-', '-').upper()
            if system == 'fresh-diskann':
                system_name = 'FreshDiskANN'
            elif system == 'ip-diskann':
                system_name = 'IP-DiskANN'
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
        ax1.plot(df['time_sec'], df['search_qps'],
                color=COLORS[system_name],
                linestyle=LINESTYLES[system_name],
                linewidth=2, label=system_name)
    ax1.set_xlabel('时间 (秒)', fontsize=13)
    ax1.set_ylabel('搜索QPS', fontsize=13)
    ax1.set_title('(a) 并发搜索吞吐量', fontsize=14, fontweight='bold')
    ax1.legend(fontsize=12)
    ax1.grid(True, alpha=0.3)
    
    # 2. P99延迟随时间变化
    ax2 = axes[0, 1]
    for system_name, df in data.items():
        ax2.plot(df['time_sec'], df['search_p99_us'] / 1000,
                color=COLORS[system_name],
                linestyle=LINESTYLES[system_name],
                linewidth=2, label=system_name)
    ax2.set_xlabel('时间 (秒)', fontsize=13)
    ax2.set_ylabel('P99搜索延迟 (ms)', fontsize=13)
    ax2.set_title('(b) 并发搜索P99延迟', fontsize=14, fontweight='bold')
    ax2.legend(fontsize=12)
    ax2.grid(True, alpha=0.3)
    
    # 3. 插入吞吐量随时间变化
    ax3 = axes[1, 0]
    for system_name, df in data.items():
        ax3.plot(df['time_sec'], df['insert_tput'],
                color=COLORS[system_name],
                linestyle=LINESTYLES[system_name],
                linewidth=2, label=system_name)
    ax3.set_xlabel('时间 (秒)', fontsize=13)
    ax3.set_ylabel('插入吞吐量 (ops/s)', fontsize=13)
    ax3.set_title('(c) 并发插入吞吐量', fontsize=14, fontweight='bold')
    ax3.legend(fontsize=12)
    ax3.grid(True, alpha=0.3)
    
    # 4. 稳定性分析（QPS的变异系数）
    ax4 = axes[1, 1]
    stability_data = []
    labels = []
    for system_name, df in data.items():
        # 计算QPS的变异系数 (CV = std / mean)
        qps_mean = df['search_qps'].mean()
        qps_std = df['search_qps'].std()
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
    ax4.set_ylabel('QPS变异系数 (%)', fontsize=13)
    ax4.set_title('(d) 搜索性能稳定性', fontsize=14, fontweight='bold')
    ax4.grid(True, alpha=0.3, axis='y')
    
    # 添加数值标签
    for i, (pos, val) in enumerate(zip(positions, stability_data)):
        ax4.text(pos, val + 0.5, f'{val:.1f}%', ha='center', va='bottom', fontsize=11)
    
    plt.tight_layout()
    output_file = os.path.join(output_dir, 'fig_concurrent_performance_comparison.pdf')
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"Saved: {output_file}")
    plt.close()


def generate_summary_table(results_dir, output_dir):
    """
    生成汇总对比表格
    """
    print("Generating summary table...")
    
    summary = []
    
    # 从各个实验中提取关键指标
    for system in ['dc-pdi', 'ip-diskann', 'fresh-diskann']:
        system_name = system.replace('-', '-').upper()
        if system == 'fresh-diskann':
            system_name = 'FreshDiskANN'
        elif system == 'ip-diskann':
            system_name = 'IP-DiskANN'
        
        row = {'System': system_name}
        
        # 从实验1获取搜索性能（95%召回率）
        exp1_file = os.path.join(results_dir, f'exp1_search_latency_{system}.csv')
        if os.path.exists(exp1_file):
            df1 = pd.read_csv(exp1_file)
            target_recall = 0.95
            closest_idx = (df1['recall'] - target_recall).abs().idxmin()
            row['QPS@95%'] = int(df1.loc[closest_idx, 'qps'])
            row['P99(ms)@95%'] = f"{df1.loc[closest_idx, 'p99_lat_us'] / 1000:.2f}"
            row['IO-Amp@95%'] = f"{df1.loc[closest_idx, 'io_amplification']:.2f}"
        
        # 从实验2获取更新性能
        exp2_file = os.path.join(results_dir, f'exp2_update_throughput_{system}.csv')
        if os.path.exists(exp2_file):
            df2 = pd.read_csv(exp2_file)
            # 取最后10%的平均吞吐量作为稳定值
            stable_df = df2.iloc[int(len(df2) * 0.9):]
            row['Insert-TPS'] = int(stable_df['throughput_ops'].mean())
            row['Peak-Mem(MB)'] = int(df2['memory_rss_mb'].max())
        
        # 从实验3获取并发性能
        exp3_file = os.path.join(results_dir, f'exp3_concurrent_{system}.csv')
        if os.path.exists(exp3_file):
            df3 = pd.read_csv(exp3_file)
            row['Concurrent-QPS'] = int(df3['search_qps'].mean())
            row['QPS-Stability(%)'] = f"{(df3['search_qps'].std() / df3['search_qps'].mean() * 100):.1f}"
        
        summary.append(row)
    
    # 创建DataFrame并保存
    summary_df = pd.DataFrame(summary)
    output_file = os.path.join(output_dir, 'system_comparison_summary.csv')
    summary_df.to_csv(output_file, index=False)
    print(f"Saved: {output_file}")
    
    # 打印到控制台
    print("\n" + "=" * 100)
    print("系统对比汇总表")
    print("=" * 100)
    print(summary_df.to_string(index=False))
    print("=" * 100)


def main():
    if len(sys.argv) < 2:
        print("Usage: python plot_system_comparison.py <results_dir> [--output <output_dir>]")
        sys.exit(1)
    
    results_dir = sys.argv[1]
    
    # 解析输出目录
    output_dir = results_dir  # 默认与输入目录相同
    if '--output' in sys.argv:
        output_idx = sys.argv.index('--output')
        if output_idx + 1 < len(sys.argv):
            output_dir = sys.argv[output_idx + 1]
    
    # 创建输出目录
    figures_dir = os.path.join(output_dir, 'figures')
    Path(figures_dir).mkdir(parents=True, exist_ok=True)
    
    print(f"Results directory: {results_dir}")
    print(f"Output directory: {figures_dir}")
    print("")
    
    # 生成所有图表
    plot_search_latency_comparison(results_dir, figures_dir)
    plot_update_throughput_comparison(results_dir, figures_dir)
    plot_concurrent_performance_comparison(results_dir, figures_dir)
    generate_summary_table(results_dir, output_dir)
    
    print("\nAll plots generated successfully!")
    print(f"Figures saved in: {figures_dir}")


if __name__ == '__main__':
    main()
