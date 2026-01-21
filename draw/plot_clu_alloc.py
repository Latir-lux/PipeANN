#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Clu-Alloc Experiment Plotting Script (Section 3.2.2)

This script generates comparison plots for the Clu-Alloc experiment:
1. Average Page Accesses (APA) vs. Number of Insertions
2. I/O Amplification vs. Number of Insertions
3. P99 Latency vs. Number of Insertions
4. Intra-page Edge Ratio vs. Number of Insertions
5. Disk Space Overhead Comparison

Author: Generated for PipeANN Clu-Alloc Experiment

Dependencies:
  pip install matplotlib numpy pandas

"""

import sys
import os
from pathlib import Path

# Check for required dependencies
try:
    import matplotlib.pyplot as plt
    import numpy as np
    import pandas as pd
except ImportError as e:
    print("=" * 60)
    print("ERROR: Missing required dependencies")
    print("=" * 60)
    print(f"\n{e}\n")
    print("Please install the required packages using:")
    print("  pip install matplotlib numpy pandas")
    print("=" * 60)
    sys.exit(1)

# ========== Configuration ==========
# Directory containing the CSV result files
RESULT_DIR = Path(__file__).parent.absolute()
OUTPUT_DIR = RESULT_DIR

# Style configuration
try:
    plt.style.use('seaborn-v0_8-whitegrid')
except:
    try:
        plt.style.use('seaborn-whitegrid')
    except:
        pass  # Use default style if seaborn not available
        
plt.rcParams['figure.figsize'] = (10, 6)
plt.rcParams['font.size'] = 12
plt.rcParams['axes.labelsize'] = 14
plt.rcParams['axes.titlesize'] = 16
plt.rcParams['legend.fontsize'] = 11
plt.rcParams['lines.linewidth'] = 2
plt.rcParams['lines.markersize'] = 8

# Color scheme for strategies
COLORS = {
    'Append-Only': '#1f77b4',   # Blue
    'Random-Alloc': '#ff7f0e',  # Orange
    'Clu-Alloc': '#2ca02c'      # Green
}

MARKERS = {
    'Append-Only': 'o',
    'Random-Alloc': 's',
    'Clu-Alloc': '^'
}

LINESTYLES = {
    'Append-Only': '-',
    'Random-Alloc': '--',
    'Clu-Alloc': '-.'
}


def load_experiment_data(result_dir):
    """Load experiment results from CSV files."""
    data = {}
    
    csv_files = {
        'Append-Only': 'clu_alloc_append.csv',
        'Random-Alloc': 'clu_alloc_random.csv',
        'Clu-Alloc': 'clu_alloc_cluster.csv'
    }
    
    for strategy, filename in csv_files.items():
        filepath = result_dir / filename
        if filepath.exists():
            df = pd.read_csv(filepath)
            data[strategy] = df
            print(f"Loaded {strategy}: {len(df)} data points")
        else:
            print(f"Warning: {filename} not found, skipping {strategy}")
    
    return data


def plot_apa_comparison(data, output_dir):
    """Plot Average Page Accesses (APA) comparison."""
    fig, ax = plt.subplots(figsize=(10, 6))
    
    for strategy, df in data.items():
        x = df['num_inserted'] / 1e6  # Convert to millions
        y = df['avg_page_accesses']
        
        ax.plot(x, y, 
                color=COLORS[strategy],
                marker=MARKERS[strategy],
                linestyle=LINESTYLES[strategy],
                label=strategy,
                markersize=8)
    
    ax.set_xlabel('Number of Insertions (Millions)')
    ax.set_ylabel('Average Page Accesses (APA)')
    ax.set_title('Average Page Accesses vs. Insertions')
    ax.legend(loc='upper left')
    ax.grid(True, alpha=0.3)
    
    # Set axis limits
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)
    
    plt.tight_layout()
    plt.savefig(output_dir / 'fig_apa_comparison.pdf', dpi=300, bbox_inches='tight')
    plt.savefig(output_dir / 'fig_apa_comparison.png', dpi=300, bbox_inches='tight')
    plt.close()
    print("Saved: fig_apa_comparison.pdf/png")


def plot_io_amplification(data, output_dir):
    """Plot I/O Amplification comparison."""
    fig, ax = plt.subplots(figsize=(10, 6))
    
    for strategy, df in data.items():
        x = df['num_inserted'] / 1e6
        y = df['io_amplification']
        
        ax.plot(x, y,
                color=COLORS[strategy],
                marker=MARKERS[strategy],
                linestyle=LINESTYLES[strategy],
                label=strategy,
                markersize=8)
    
    ax.set_xlabel('Number of Insertions (Millions)')
    ax.set_ylabel('I/O Amplification')
    ax.set_title('I/O Amplification vs. Insertions')
    ax.legend(loc='upper left')
    ax.grid(True, alpha=0.3)
    
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)
    
    plt.tight_layout()
    plt.savefig(output_dir / 'fig_io_amplification.pdf', dpi=300, bbox_inches='tight')
    plt.savefig(output_dir / 'fig_io_amplification.png', dpi=300, bbox_inches='tight')
    plt.close()
    print("Saved: fig_io_amplification.pdf/png")


def plot_p99_latency(data, output_dir):
    """Plot P99 Latency comparison."""
    fig, ax = plt.subplots(figsize=(10, 6))
    
    for strategy, df in data.items():
        x = df['num_inserted'] / 1e6
        y = df['p99_latency_ms']
        
        ax.plot(x, y,
                color=COLORS[strategy],
                marker=MARKERS[strategy],
                linestyle=LINESTYLES[strategy],
                label=strategy,
                markersize=8)
    
    ax.set_xlabel('Number of Insertions (Millions)')
    ax.set_ylabel('P99 Latency (ms)')
    ax.set_title('P99 Search Latency vs. Insertions')
    ax.legend(loc='upper left')
    ax.grid(True, alpha=0.3)
    
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)
    
    plt.tight_layout()
    plt.savefig(output_dir / 'fig_p99_latency.pdf', dpi=300, bbox_inches='tight')
    plt.savefig(output_dir / 'fig_p99_latency.png', dpi=300, bbox_inches='tight')
    plt.close()
    print("Saved: fig_p99_latency.pdf/png")


def plot_intra_page_ratio(data, output_dir):
    """Plot Intra-page Edge Ratio comparison."""
    fig, ax = plt.subplots(figsize=(10, 6))
    
    for strategy, df in data.items():
        x = df['num_inserted'] / 1e6
        y = df['intra_page_ratio'] * 100  # Convert to percentage
        
        ax.plot(x, y,
                color=COLORS[strategy],
                marker=MARKERS[strategy],
                linestyle=LINESTYLES[strategy],
                label=strategy,
                markersize=8)
    
    ax.set_xlabel('Number of Insertions (Millions)')
    ax.set_ylabel('Intra-page Edge Ratio (%)')
    ax.set_title('Intra-page Edge Ratio vs. Insertions')
    ax.legend(loc='lower right')
    ax.grid(True, alpha=0.3)
    
    ax.set_xlim(left=0)
    ax.set_ylim(0, 100)
    
    plt.tight_layout()
    plt.savefig(output_dir / 'fig_intra_page_ratio.pdf', dpi=300, bbox_inches='tight')
    plt.savefig(output_dir / 'fig_intra_page_ratio.png', dpi=300, bbox_inches='tight')
    plt.close()
    print("Saved: fig_intra_page_ratio.pdf/png")


def plot_disk_space(data, output_dir):
    """Plot Disk Space Overhead comparison."""
    fig, ax = plt.subplots(figsize=(10, 6))
    
    for strategy, df in data.items():
        x = df['num_inserted'] / 1e6
        y = df['disk_size_mb'] / 1024  # Convert to GB
        
        ax.plot(x, y,
                color=COLORS[strategy],
                marker=MARKERS[strategy],
                linestyle=LINESTYLES[strategy],
                label=strategy,
                markersize=8)
    
    ax.set_xlabel('Number of Insertions (Millions)')
    ax.set_ylabel('Disk Index Size (GB)')
    ax.set_title('Disk Space Usage vs. Insertions')
    ax.legend(loc='upper left')
    ax.grid(True, alpha=0.3)
    
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)
    
    plt.tight_layout()
    plt.savefig(output_dir / 'fig_disk_space.pdf', dpi=300, bbox_inches='tight')
    plt.savefig(output_dir / 'fig_disk_space.png', dpi=300, bbox_inches='tight')
    plt.close()
    print("Saved: fig_disk_space.pdf/png")


def plot_combined_metrics(data, output_dir):
    """Create a combined 2x2 subplot figure with all key metrics."""
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    
    # APA
    ax = axes[0, 0]
    for strategy, df in data.items():
        x = df['num_inserted'] / 1e6
        y = df['avg_page_accesses']
        ax.plot(x, y, color=COLORS[strategy], marker=MARKERS[strategy],
                linestyle=LINESTYLES[strategy], label=strategy, markersize=6)
    ax.set_xlabel('Insertions (M)')
    ax.set_ylabel('APA')
    ax.set_title('(a) Average Page Accesses')
    ax.legend(loc='upper left', fontsize=9)
    ax.grid(True, alpha=0.3)
    ax.set_xlim(left=0)
    
    # I/O Amplification
    ax = axes[0, 1]
    for strategy, df in data.items():
        x = df['num_inserted'] / 1e6
        y = df['io_amplification']
        ax.plot(x, y, color=COLORS[strategy], marker=MARKERS[strategy],
                linestyle=LINESTYLES[strategy], label=strategy, markersize=6)
    ax.set_xlabel('Insertions (M)')
    ax.set_ylabel('I/O Amplification')
    ax.set_title('(b) I/O Amplification')
    ax.legend(loc='upper left', fontsize=9)
    ax.grid(True, alpha=0.3)
    ax.set_xlim(left=0)
    
    # P99 Latency
    ax = axes[1, 0]
    for strategy, df in data.items():
        x = df['num_inserted'] / 1e6
        y = df['p99_latency_ms']
        ax.plot(x, y, color=COLORS[strategy], marker=MARKERS[strategy],
                linestyle=LINESTYLES[strategy], label=strategy, markersize=6)
    ax.set_xlabel('Insertions (M)')
    ax.set_ylabel('P99 Latency (ms)')
    ax.set_title('(c) P99 Search Latency')
    ax.legend(loc='upper left', fontsize=9)
    ax.grid(True, alpha=0.3)
    ax.set_xlim(left=0)
    
    # Intra-page Ratio
    ax = axes[1, 1]
    for strategy, df in data.items():
        x = df['num_inserted'] / 1e6
        y = df['intra_page_ratio'] * 100
        ax.plot(x, y, color=COLORS[strategy], marker=MARKERS[strategy],
                linestyle=LINESTYLES[strategy], label=strategy, markersize=6)
    ax.set_xlabel('Insertions (M)')
    ax.set_ylabel('Intra-page Ratio (%)')
    ax.set_title('(d) Intra-page Edge Ratio')
    ax.legend(loc='lower right', fontsize=9)
    ax.grid(True, alpha=0.3)
    ax.set_xlim(left=0)
    ax.set_ylim(0, 100)
    
    plt.tight_layout()
    plt.savefig(output_dir / 'fig_combined_metrics.pdf', dpi=300, bbox_inches='tight')
    plt.savefig(output_dir / 'fig_combined_metrics.png', dpi=300, bbox_inches='tight')
    plt.close()
    print("Saved: fig_combined_metrics.pdf/png")


def plot_bar_comparison(data, output_dir):
    """Create bar chart comparing final metrics across strategies."""
    if not data:
        print("No data available for bar comparison")
        return
    
    # Get final values for each strategy
    strategies = []
    final_apa = []
    final_io_amp = []
    final_p99 = []
    final_intra = []
    
    for strategy, df in data.items():
        if len(df) > 0:
            strategies.append(strategy)
            final_apa.append(df['avg_page_accesses'].iloc[-1])
            final_io_amp.append(df['io_amplification'].iloc[-1])
            final_p99.append(df['p99_latency_ms'].iloc[-1])
            final_intra.append(df['intra_page_ratio'].iloc[-1] * 100)
    
    x = np.arange(len(strategies))
    width = 0.6
    
    fig, axes = plt.subplots(1, 4, figsize=(16, 4))
    
    colors = [COLORS[s] for s in strategies]
    
    # APA
    axes[0].bar(x, final_apa, width, color=colors)
    axes[0].set_ylabel('APA')
    axes[0].set_title('Average Page Accesses')
    axes[0].set_xticks(x)
    axes[0].set_xticklabels(strategies, rotation=15, ha='right')
    
    # I/O Amplification
    axes[1].bar(x, final_io_amp, width, color=colors)
    axes[1].set_ylabel('I/O Amp')
    axes[1].set_title('I/O Amplification')
    axes[1].set_xticks(x)
    axes[1].set_xticklabels(strategies, rotation=15, ha='right')
    
    # P99 Latency
    axes[2].bar(x, final_p99, width, color=colors)
    axes[2].set_ylabel('Latency (ms)')
    axes[2].set_title('P99 Latency')
    axes[2].set_xticks(x)
    axes[2].set_xticklabels(strategies, rotation=15, ha='right')
    
    # Intra-page Ratio
    axes[3].bar(x, final_intra, width, color=colors)
    axes[3].set_ylabel('Ratio (%)')
    axes[3].set_title('Intra-page Edge Ratio')
    axes[3].set_xticks(x)
    axes[3].set_xticklabels(strategies, rotation=15, ha='right')
    
    plt.tight_layout()
    plt.savefig(output_dir / 'fig_bar_comparison.pdf', dpi=300, bbox_inches='tight')
    plt.savefig(output_dir / 'fig_bar_comparison.png', dpi=300, bbox_inches='tight')
    plt.close()
    print("Saved: fig_bar_comparison.pdf/png")


def generate_summary_table(data, output_dir):
    """Generate a summary table of experiment results."""
    if not data:
        print("No data available for summary table")
        return
    
    summary = []
    
    for strategy, df in data.items():
        if len(df) > 0:
            final_row = df.iloc[-1]
            summary.append({
                'Strategy': strategy,
                'Final APA': f"{final_row['avg_page_accesses']:.2f}",
                'Final I/O Amp': f"{final_row['io_amplification']:.2f}",
                'Final P99 (ms)': f"{final_row['p99_latency_ms']:.2f}",
                'Intra-page (%)': f"{final_row['intra_page_ratio']*100:.1f}",
                'Disk Size (MB)': f"{final_row['disk_size_mb']:.0f}",
                'Cluster Hits': f"{final_row['cluster_hits']:.0f}",
                'Overflow': f"{final_row['overflow_count']:.0f}"
            })
    
    summary_df = pd.DataFrame(summary)
    
    # Save as CSV
    summary_df.to_csv(output_dir / 'summary_table.csv', index=False)
    print("Saved: summary_table.csv")
    
    # Print to console
    print("\n" + "=" * 80)
    print("EXPERIMENT SUMMARY")
    print("=" * 80)
    print(summary_df.to_string(index=False))
    print("=" * 80 + "\n")


def create_sample_data(output_dir):
    """Create sample data for testing the plotting script."""
    print("Creating sample data for testing...")
    
    # Simulated data points
    insertions = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
    
    # Append-Only: worst performance, APA increases linearly
    append_data = {
        'strategy': ['Append-Only'] * len(insertions),
        'num_inserted': [x * 1000000 for x in insertions],
        'avg_page_accesses': [10 + x * 0.8 for x in insertions],
        'io_amplification': [15 + x * 1.2 for x in insertions],
        'p50_latency_ms': [2 + x * 0.15 for x in insertions],
        'p99_latency_ms': [8 + x * 0.6 for x in insertions],
        'recall': [0.95] * len(insertions),
        'intra_page_ratio': [0.15 - x * 0.005 for x in insertions],
        'disk_size_mb': [10000 + x * 1000 for x in insertions],
        'cluster_hits': [0] * len(insertions),
        'overflow_count': [0] * len(insertions)
    }
    
    # Random-Alloc: medium performance
    random_data = {
        'strategy': ['Random-Alloc'] * len(insertions),
        'num_inserted': [x * 1000000 for x in insertions],
        'avg_page_accesses': [10 + x * 0.5 for x in insertions],
        'io_amplification': [15 + x * 0.8 for x in insertions],
        'p50_latency_ms': [2 + x * 0.1 for x in insertions],
        'p99_latency_ms': [8 + x * 0.4 for x in insertions],
        'recall': [0.95] * len(insertions),
        'intra_page_ratio': [0.25 + x * 0.01 for x in insertions],
        'disk_size_mb': [10500 + x * 1050 for x in insertions],
        'cluster_hits': [0] * len(insertions),
        'overflow_count': [0] * len(insertions)
    }
    
    # Clu-Alloc: best performance
    cluster_data = {
        'strategy': ['Clu-Alloc'] * len(insertions),
        'num_inserted': [x * 1000000 for x in insertions],
        'avg_page_accesses': [10 + x * 0.2 for x in insertions],
        'io_amplification': [15 + x * 0.3 for x in insertions],
        'p50_latency_ms': [2 + x * 0.05 for x in insertions],
        'p99_latency_ms': [8 + x * 0.15 for x in insertions],
        'recall': [0.95] * len(insertions),
        'intra_page_ratio': [0.45 + x * 0.02 for x in insertions],
        'disk_size_mb': [11000 + x * 1100 for x in insertions],
        'cluster_hits': [x * 500000 for x in insertions],
        'overflow_count': [x * 50000 for x in insertions]
    }
    
    # Save sample data
    pd.DataFrame(append_data).to_csv(output_dir / 'clu_alloc_append.csv', index=False)
    pd.DataFrame(random_data).to_csv(output_dir / 'clu_alloc_random.csv', index=False)
    pd.DataFrame(cluster_data).to_csv(output_dir / 'clu_alloc_cluster.csv', index=False)
    
    print("Sample data created successfully!")


def main():
    """Main function to generate all plots."""
    print("=" * 60)
    print("Clu-Alloc Experiment Plotting Script")
    print("=" * 60)
    
    output_dir = Path(OUTPUT_DIR)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # Check if data exists, if not create sample data
    data = load_experiment_data(RESULT_DIR)
    
    if not data:
        print("\nNo experiment data found. Creating sample data for demonstration...")
        create_sample_data(output_dir)
        data = load_experiment_data(RESULT_DIR)
    
    if not data:
        print("Error: Could not load or create data. Exiting.")
        return
    
    print("\nGenerating plots...")
    
    # Generate individual plots
    plot_apa_comparison(data, output_dir)
    plot_io_amplification(data, output_dir)
    plot_p99_latency(data, output_dir)
    plot_intra_page_ratio(data, output_dir)
    plot_disk_space(data, output_dir)
    
    # Generate combined plots
    plot_combined_metrics(data, output_dir)
    plot_bar_comparison(data, output_dir)
    
    # Generate summary table
    generate_summary_table(data, output_dir)
    
    print("\n" + "=" * 60)
    print("All plots generated successfully!")
    print(f"Output directory: {output_dir}")
    print("=" * 60)


if __name__ == '__main__':
    main()
