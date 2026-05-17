#!/usr/bin/env python3
"""
AI-QoS Integration Test: Plot Generator

Parses JSONL log files from ai_qos_bench and generates comparison plots.

Usage:
  ./plot_results.py results/                   # Process experiment directory
  ./plot_results.py log.jsonl -o out/          # Single log file
"""

import json
import sys
import os
import argparse
from collections import defaultdict

try:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
except ImportError:
    print("ERROR: matplotlib required. pip3 install matplotlib")
    sys.exit(1)

EXPERIMENTS = [
    ("baseline_no_urgent_random", "Baseline · No Urgent · Random", "baseline", False, False),
    ("baseline_no_urgent_ai",     "Baseline · No Urgent · AI Workload", "baseline", False, True),
    ("baseline_urgent_random",    "Baseline · Urgent · Random", "baseline", True, False),
    ("baseline_urgent_ai",        "Baseline · Urgent · AI Workload", "baseline", True, True),
    ("modified_no_urgent_random", "Modified · No Urgent · Random", "modified", False, False),
    ("modified_no_urgent_ai",     "Modified · No Urgent · AI Workload", "modified", False, True),
    ("modified_urgent_random",    "Modified · Urgent · Random", "modified", True, False),
    ("modified_urgent_ai",        "Modified · Urgent · AI Workload", "modified", True, True),
]


def parse_log(path):
    """Parse JSONL log file into list of IO records."""
    records = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
                records.append(rec)
            except json.JSONDecodeError:
                pass
    return records


def rolling_avg(values, window):
    """Simple rolling average."""
    if len(values) < window:
        return values[:]
    result = []
    for i in range(len(values)):
        start = max(0, i - window + 1)
        result.append(sum(values[start:i+1]) / (i - start + 1))
    return result


def compute_throughput(records, window_ms=100):
    """IOPS over time using sliding window."""
    if not records:
        return [], []
    max_ts = records[-1]['ts']
    bin_us = window_ms * 1000
    nbins = max(1, int(max_ts / bin_us) + 1)
    bin_counts = [0] * nbins
    for r in records:
        bidx = int(r['ts'] / bin_us)
        if bidx < nbins:
            bin_counts[bidx] += 1
    time_pts = [(i * bin_us + bin_us / 2) / 1000000.0 for i in range(nbins)]
    iops = [c / (window_ms / 1000.0) for c in bin_counts]
    return time_pts, iops


def compute_avg_latency(records, window_ms=100):
    """Average latency over sliding windows."""
    if not records:
        return [], []
    max_ts = records[-1]['ts']
    bin_us = window_ms * 1000
    nbins = max(1, int(max_ts / bin_us) + 1)
    bin_sum = [0] * nbins
    bin_cnt = [0] * nbins
    for r in records:
        bidx = int(r['ts'] / bin_us)
        if bidx < nbins:
            bin_sum[bidx] += r['latency_us']
            bin_cnt[bidx] += 1
    time_pts = [(i * bin_us + bin_us / 2) / 1000000.0 for i in range(nbins)]
    avg_lat = [s / max(c, 1) for s, c in zip(bin_sum, bin_cnt)]
    return time_pts, avg_lat


def plot_ema_tracking(records, output_path, label):
    fig, ax = plt.subplots(figsize=(12, 5))

    reads = [r for r in records if r['type'] == 'read']
    writes = [r for r in records if r['type'] == 'write']

    if reads:
        ts_r = [r['ts'] / 1000000.0 for r in reads]
        blk_r = [r['blocks'] for r in reads]
        ax.scatter(ts_r, blk_r, s=1, alpha=0.15, color='#2196F3', label='Read raw')
        ema = rolling_avg(blk_r, 30)
        ax.plot(ts_r[:len(ema)], ema, color='#0D47A1', lw=1.2, label='Read EMA (n=30)')

    if writes:
        ts_w = [r['ts'] / 1000000.0 for r in writes]
        blk_w = [r['blocks'] for r in writes]
        ax.scatter(ts_w, blk_w, s=1, alpha=0.15, color='#FF5722', label='Write raw')
        ema = rolling_avg(blk_w, 30)
        ax.plot(ts_w[:len(ema)], ema, color='#BF360C', lw=1.2, label='Write EMA (n=30)')

    ax.set_xlabel('Time (s)')
    ax.set_ylabel('IO Size (blocks)')
    ax.set_title(f'IO Size Tracking (EMA-like)\n{label}')
    ax.legend(fontsize=8)
    ax.grid(True, alpha=0.25)
    fig.tight_layout()
    fig.savefig(output_path, dpi=150)
    plt.close(fig)
    print(f"  -> {output_path}")


def plot_phases(records, output_path, label):
    if not records:
        return
    fig, ax = plt.subplots(figsize=(12, 2.5))
    max_ts = records[-1]['ts'] / 1000000.0
    colors = ['#2196F3', '#FF5722', '#4CAF50', '#FFC107', '#9C27B0']
    seen = set()
    for r in records:
        if r['phase'] not in seen:
            seen.add(r['phase'])
            start = r['ts'] / 1000000.0
            ax.axvspan(start, start + 0.3, alpha=0.2,
                       color=colors[r['phase'] % len(colors)],
                       label=f'Phase {r["phase"]}')
    ax.set_xlim(0, max_ts)
    ax.set_ylim(0, 1)
    ax.set_xlabel('Time (s)')
    ax.set_title(f'Workload Phase Timeline\n{label}')
    ax.legend(fontsize=7, ncol=3)
    fig.tight_layout()
    fig.savefig(output_path, dpi=150)
    plt.close(fig)
    print(f"  -> {output_path}")


def plot_throughput(records, output_path, label):
    if not records:
        return
    time_pts, iops = compute_throughput(records)

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 6), sharex=True)
    ax1.plot(time_pts, iops, color='#1565C0', lw=0.8)
    ax1.set_ylabel('IOPS')
    ax1.set_title(f'Throughput\n{label}')
    ax1.grid(True, alpha=0.25)

    # Bandwidth
    window_us = 100 * 1000
    nbins = max(1, int(records[-1]['ts'] / window_us) + 1)
    bw = [0.0] * nbins
    for r in records:
        bidx = int(r['ts'] / window_us)
        if bidx < nbins:
            bw[bidx] += r['blocks'] * 512.0 / (1024 * 1024)
    bw_sec = [b * (1000000.0 / window_us) for b in bw]
    bt = [(i * window_us) / 1000000.0 for i in range(nbins)]

    ax2.plot(bt, bw_sec, color='#E65100', lw=0.8)
    ax2.set_xlabel('Time (s)')
    ax2.set_ylabel('Throughput (MB/s)')
    ax2.grid(True, alpha=0.25)
    fig.tight_layout()
    fig.savefig(output_path, dpi=150)
    plt.close(fig)
    print(f"  -> {output_path}")


def plot_latency(records, output_path, label):
    if not records:
        return
    time_pts, avg_lat = compute_avg_latency(records)
    lat_ms = [l / 1000.0 for l in avg_lat]

    fig, ax = plt.subplots(figsize=(12, 4))
    ax.plot(time_pts, lat_ms, color='#666', lw=0.8, label='Avg Latency')

    urgent = [r for r in records if r.get('urgent', 0)]
    normal = [r for r in records if not r.get('urgent', 0)]

    if urgent:
        _, lu = compute_avg_latency(urgent)
        tu, _ = compute_throughput(urgent)
        ax.plot(tu, [l / 1000.0 for l in lu], color='#D32F2F', lw=1.2, label='Urgent', alpha=0.8)
    if normal:
        _, ln = compute_avg_latency(normal)
        tn, _ = compute_throughput(normal)
        ax.plot(tn, [l / 1000.0 for l in ln], color='#1976D2', lw=1.0, label='Normal', alpha=0.8)

    ax.set_xlabel('Time (s)')
    ax.set_ylabel('Latency (ms)')
    ax.set_title(f'IO Latency\n{label}')
    ax.legend(fontsize=8)
    ax.grid(True, alpha=0.25)
    fig.tight_layout()
    fig.savefig(output_path, dpi=150)
    plt.close(fig)
    print(f"  -> {output_path}")


def plot_latency_dist(records, output_path, label):
    if not records:
        return
    fig, ax = plt.subplots(figsize=(10, 5))
    urgent = [r['latency_us'] / 1000.0 for r in records if r.get('urgent', 0)]
    normal = [r['latency_us'] / 1000.0 for r in records if not r.get('urgent', 0)]

    bins = 60
    if normal:
        ax.hist(normal, bins=bins, alpha=0.5, color='#1976D2',
                label=f'Normal (n={len(normal)})', density=True)
    if urgent:
        ax.hist(urgent, bins=bins, alpha=0.5, color='#D32F2F',
                label=f'Urgent (n={len(urgent)})', density=True)
    ax.set_xlabel('Latency (ms)')
    ax.set_ylabel('Density')
    ax.set_title(f'IO Latency Distribution\n{label}')
    ax.legend()
    ax.grid(True, alpha=0.25)
    fig.tight_layout()
    fig.savefig(output_path, dpi=150)
    plt.close(fig)
    print(f"  -> {output_path}")


def process_one(log_path, out_dir, label):
    os.makedirs(out_dir, exist_ok=True)
    records = parse_log(log_path)
    if not records:
        print(f"  WARN: no records in {log_path}")
        return False
    print(f"\n--- {label} ---")
    print(f"  IOs={len(records)} duration={records[-1]['ts']/1e6:.1f}s")
    plot_ema_tracking(records, os.path.join(out_dir, '01_io_size_tracking.png'), label)
    plot_phases(records, os.path.join(out_dir, '02_workload_phases.png'), label)
    plot_throughput(records, os.path.join(out_dir, '03_throughput.png'), label)
    plot_latency(records, os.path.join(out_dir, '04_latency.png'), label)
    plot_latency_dist(records, os.path.join(out_dir, '05_latency_distribution.png'), label)
    return True


def process_results_dir(results_dir):
    out_root = os.path.join(results_dir, 'plots')
    for code, label, _, _, _ in EXPERIMENTS:
        log_path = os.path.join(results_dir, code, 'ai_qos_bench.jsonl')
        if os.path.exists(log_path):
            process_one(log_path, os.path.join(out_root, code), label)

    # Comparison plot
    fig, axes = plt.subplots(4, 2, figsize=(18, 16))
    axes = axes.flatten()
    for idx, (code, label, _, _, _) in enumerate(EXPERIMENTS):
        log_path = os.path.join(results_dir, code, 'ai_qos_bench.jsonl')
        records = parse_log(log_path)
        ax = axes[idx]
        if not records:
            ax.text(0.5, 0.5, 'No data', ha='center', va='center')
            ax.set_title(label, fontsize=8)
            continue
        time_pts, avg_lat = compute_avg_latency(records)
        ax.plot(time_pts, [l / 1000.0 for l in avg_lat], lw=0.7)
        ax.set_title(label, fontsize=8)
        ax.set_xlabel('Time (s)', fontsize=7)
        ax.set_ylabel('Latency (ms)', fontsize=7)
        ax.grid(True, alpha=0.25)
    fig.suptitle('Latency Comparison: All 8 Experiments', fontsize=13)
    fig.tight_layout()
    fig.savefig(os.path.join(out_root, 'all_comparison_latency.png'), dpi=150)
    plt.close(fig)
    print(f"  -> {os.path.join(out_root, 'all_comparison_latency.png')}")


def main():
    parser = argparse.ArgumentParser(description='AI-QoS Plot Generator')
    parser.add_argument('input', help='Log file or results directory')
    parser.add_argument('-o', '--output', help='Output dir (for single log)')
    args = parser.parse_args()

    if os.path.isdir(args.input):
        process_results_dir(args.input)
    else:
        label = os.path.basename(args.input).replace('.jsonl', '')
        out_dir = args.output or 'plots_' + label
        process_one(args.input, out_dir, label)


if __name__ == '__main__':
    main()
