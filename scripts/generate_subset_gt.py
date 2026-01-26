#!/usr/bin/env python3
"""
生成子集专用的Ground Truth文件

问题背景：
  当索引只包含部分数据（如50%）时，原始GT文件基于100%数据构建，
  导致GT中的最近邻ID大多不在当前索引中，recall计算结果极低。

解决方案：
  对每个查询，只保留GT中存在于当前索引中的最近邻ID。

GT文件格式：
  [npts: 4 bytes] [dim: 4 bytes] [ids: npts*dim*4 bytes] [dists: npts*dim*4 bytes (可选)]

用法：
  python3 generate_subset_gt.py <original_gt_file> <output_gt_file> <base_pts_count> [--has-dists]
  
  参数：
    original_gt_file: 原始GT文件路径
    output_gt_file: 输出的子集GT文件路径  
    base_pts_count: 基础索引包含的点数（ID范围 [0, base_pts_count)）
    --has-dists: 如果原始GT包含距离信息，需要指定此选项
"""

import struct
import argparse
import os
import sys


def detect_gt_format(file_path: str, npts: int, dim: int) -> int:
    """检测GT文件格式：1=有距离，2=仅ID，3=有距离和标签"""
    actual_size = os.path.getsize(file_path)
    header_size = 8  # 2 * sizeof(int)
    
    # 格式1：ids + dists
    expected_with_dists = header_size + npts * dim * 4 * 2
    # 格式2：仅ids
    expected_just_ids = header_size + npts * dim * 4
    # 格式3：ids + dists + tags
    expected_with_tags = header_size + npts * dim * 4 * 3
    
    if actual_size == expected_with_dists:
        return 1
    elif actual_size == expected_just_ids:
        return 2
    elif actual_size == expected_with_tags:
        return 3
    else:
        # 尝试检测ivecs格式
        return -1


def detect_ivecs_format(file_path: str) -> bool:
    """检测是否为ivecs格式"""
    try:
        with open(file_path, 'rb') as f:
            k = struct.unpack('<i', f.read(4))[0]
            if k <= 0 or k > 10000:  # 合理的k值范围
                return False
            
            file_size = os.path.getsize(file_path)
            record_size = (k + 1) * 4
            if file_size % record_size == 0:
                return True
    except:
        pass
    return False


def load_ivecs_gt(file_path: str):
    """加载ivecs格式的GT文件"""
    with open(file_path, 'rb') as f:
        k = struct.unpack('<i', f.read(4))[0]
        file_size = os.path.getsize(file_path)
        record_size = (k + 1) * 4
        npts = file_size // record_size
        
        f.seek(0)
        ids = []
        for i in range(npts):
            row_dim = struct.unpack('<i', f.read(4))[0]
            if row_dim != k:
                raise ValueError(f"ivecs row dimension mismatch at row {i}")
            row_ids = struct.unpack(f'<{k}I', f.read(k * 4))
            ids.append(list(row_ids))
        
        return npts, k, ids, None


def load_bin_gt(file_path: str):
    """加载bin格式的GT文件"""
    with open(file_path, 'rb') as f:
        npts, dim = struct.unpack('<ii', f.read(8))
        
        gt_format = detect_gt_format(file_path, npts, dim)
        
        if gt_format == -1:
            raise ValueError(f"Unknown GT format: {file_path}")
        
        # 读取IDs
        ids = []
        for i in range(npts):
            row_ids = struct.unpack(f'<{dim}I', f.read(dim * 4))
            ids.append(list(row_ids))
        
        # 读取距离（如果有）
        dists = None
        if gt_format in [1, 3]:
            dists = []
            for i in range(npts):
                row_dists = struct.unpack(f'<{dim}f', f.read(dim * 4))
                dists.append(list(row_dists))
        
        return npts, dim, ids, dists


def filter_gt_by_valid_ids(ids: list, dists: list, max_valid_id: int, target_k: int):
    """
    过滤GT，只保留有效ID范围内的最近邻
    
    参数：
        ids: 原始GT的ID列表 (npts x dim)
        dists: 原始GT的距离列表 (npts x dim)，可为None
        max_valid_id: 有效ID的最大值（不含），即索引中的点数
        target_k: 目标返回的最近邻数量
    
    返回：
        filtered_ids: 过滤后的ID列表
        filtered_dists: 过滤后的距离列表（如果原始有距离）
        valid_counts: 每个查询的有效邻居数量
    """
    filtered_ids = []
    filtered_dists = [] if dists is not None else None
    valid_counts = []
    
    for q_idx in range(len(ids)):
        row_ids = ids[q_idx]
        row_dists = dists[q_idx] if dists is not None else None
        
        valid_ids = []
        valid_d = []
        
        for i, neighbor_id in enumerate(row_ids):
            if neighbor_id < max_valid_id:
                valid_ids.append(neighbor_id)
                if row_dists is not None:
                    valid_d.append(row_dists[i])
                    
                if len(valid_ids) >= target_k:
                    break
        
        # 如果有效邻居不足，用-1（或最后一个有效ID）填充
        while len(valid_ids) < target_k:
            if valid_ids:
                valid_ids.append(valid_ids[-1])  # 复制最后一个有效ID
                if valid_d:
                    valid_d.append(valid_d[-1])
            else:
                valid_ids.append(0)  # 如果完全没有有效ID，用0填充
                if row_dists is not None:
                    valid_d.append(float('inf'))
        
        filtered_ids.append(valid_ids[:target_k])
        if filtered_dists is not None:
            filtered_dists.append(valid_d[:target_k])
        valid_counts.append(min(len([x for x in row_ids if x < max_valid_id]), target_k))
    
    return filtered_ids, filtered_dists, valid_counts


def save_bin_gt(output_path: str, npts: int, dim: int, ids: list, dists: list):
    """保存bin格式的GT文件"""
    with open(output_path, 'wb') as f:
        f.write(struct.pack('<ii', npts, dim))
        
        for row_ids in ids:
            f.write(struct.pack(f'<{dim}I', *row_ids))
        
        if dists is not None:
            for row_dists in dists:
                f.write(struct.pack(f'<{dim}f', *row_dists))


def main():
    parser = argparse.ArgumentParser(description='生成子集专用的Ground Truth文件')
    parser.add_argument('original_gt', help='原始GT文件路径')
    parser.add_argument('output_gt', help='输出的子集GT文件路径')
    parser.add_argument('base_pts', type=int, help='基础索引包含的点数')
    parser.add_argument('--target-k', type=int, default=0, 
                        help='目标K值（默认与原始GT相同）')
    parser.add_argument('--verbose', '-v', action='store_true', help='详细输出')
    
    args = parser.parse_args()
    
    if not os.path.exists(args.original_gt):
        print(f"Error: GT file not found: {args.original_gt}", file=sys.stderr)
        sys.exit(1)
    
    if args.base_pts <= 0:
        print(f"Error: base_pts must be positive", file=sys.stderr)
        sys.exit(1)
    
    # 检测格式并加载
    if detect_ivecs_format(args.original_gt):
        if args.verbose:
            print(f"Detected ivecs format: {args.original_gt}")
        npts, dim, ids, dists = load_ivecs_gt(args.original_gt)
    else:
        if args.verbose:
            print(f"Loading bin format: {args.original_gt}")
        npts, dim, ids, dists = load_bin_gt(args.original_gt)
    
    if args.verbose:
        print(f"Loaded GT: {npts} queries, k={dim}")
        print(f"Valid ID range: [0, {args.base_pts})")
    
    target_k = args.target_k if args.target_k > 0 else dim
    
    # 过滤GT
    filtered_ids, filtered_dists, valid_counts = filter_gt_by_valid_ids(
        ids, dists, args.base_pts, target_k
    )
    
    # 统计信息
    avg_valid = sum(valid_counts) / len(valid_counts)
    min_valid = min(valid_counts)
    full_valid = sum(1 for c in valid_counts if c >= target_k)
    
    if args.verbose:
        print(f"Filtering results:")
        print(f"  Average valid neighbors per query: {avg_valid:.2f}/{target_k}")
        print(f"  Min valid neighbors: {min_valid}")
        print(f"  Queries with full valid neighbors: {full_valid}/{npts} ({100*full_valid/npts:.1f}%)")
    
    # 保存
    save_bin_gt(args.output_gt, npts, target_k, filtered_ids, filtered_dists)
    
    if args.verbose:
        print(f"Saved filtered GT to: {args.output_gt}")
    
    # 输出摘要到stdout（供shell脚本解析）
    print(f"{avg_valid:.2f}")


if __name__ == '__main__':
    main()
