#!/usr/bin/env python3
"""
生成示例数据用于测试绘图功能
"""

import os
import numpy as np
import pandas as pd
from pathlib import Path

def generate_example_data():
    """生成示例实验数据"""
    script_dir = Path(__file__).parent
    output_dir = script_dir.parent / 'data' / 'thesis_results'
    output_dir.mkdir(parents=True, exist_ok=True)
    
    print(f"Generating example data to {output_dir}")
    
    # 实验1: 延迟分布数据
    L_values = [10, 20, 30, 40, 50, 60, 80, 100, 150, 200]
    recalls = [0.80, 0.85, 0.88, 0.91, 0.93, 0.945, 0.96, 0.97, 0.98, 0.985]
    
    for dataset in ['sift100m', 'deep100m', 'gist1m']:
        data = {
            'L': L_values,
            'recall': recalls,
            'qps': [28000, 25000, 23000, 21000, 19000, 17500, 15000, 12000, 8500, 6000],
            'avg_lat_us': [350, 400, 435, 480, 520, 570, 680, 850, 1200, 1700],
            'p50_lat_us': [280, 320, 350, 380, 420, 460, 550, 680, 950, 1350],
            'p90_lat_us': [480, 540, 590, 650, 720, 790, 950, 1200, 1700, 2400],
            'p95_lat_us': [580, 650, 720, 800, 880, 970, 1150, 1450, 2100, 2900],
            'p99_lat_us': [780, 880, 970, 1080, 1200, 1350, 1600, 2000, 2800, 3900],
            'mean_ios': [35, 42, 48, 55, 62, 68, 80, 95, 125, 160],
            'avg_compute_us': [120, 145, 170, 195, 220, 245, 300, 380, 540, 780],
            'avg_prefetch_us': [80, 95, 110, 125, 140, 155, 190, 240, 340, 490],
            'io_amplification': [3.8, 3.7, 3.6, 3.5, 3.4, 3.4, 3.3, 3.2, 3.1, 3.0],
            'overlap_ratio': [1.4, 1.45, 1.48, 1.52, 1.55, 1.58, 1.62, 1.68, 1.75, 1.82],
        }
        df = pd.DataFrame(data)
        df.to_csv(output_dir / f'exp1_latency_{dataset}.csv', index=False)
    
    # 实验6: 流水线宽度数据
    pipeline_widths = [1, 2, 4, 8, 16, 32, 64]
    data = {
        'pipeline_width': pipeline_widths,
        'qps': [5000, 9500, 16000, 21000, 24000, 24500, 23500],
        'recall': [0.952, 0.950, 0.948, 0.945, 0.942, 0.938, 0.930],
        'avg_lat_us': [2000, 1050, 620, 480, 420, 410, 430],
        'p99_lat_us': [3500, 1800, 1100, 850, 750, 740, 780],
        'mean_ios': [45, 52, 62, 75, 90, 110, 140],
        'io_efficiency': [42, 40, 38, 35, 32, 28, 22],
    }
    df = pd.DataFrame(data)
    df.to_csv(output_dir / 'exp6_pipeline_width_sift.csv', index=False)
    
    print("Example data generated successfully!")
    print(f"Files created in: {output_dir}")
    for f in output_dir.glob('*.csv'):
        print(f"  - {f.name}")


if __name__ == '__main__':
    generate_example_data()
