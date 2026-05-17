#!/usr/bin/env python3
"""
Pure-Python AI-QoS Simulator & Benchmark Plot Generator

Simulates SPDK AI-QoS behavior for 8 experiment configurations:
  code_version (baseline/modified) x urgent_flag (off/on) x workload_type (random/ai)

Generates JSONL log files identical in format to ai_qos_bench.c output,
then produces comparison plots.

Usage:
  python3 ai_qos_sim.py                     # Run all 8 experiments + plots
  python3 ai_qos_sim.py --experiment 0 4    # Run specific experiments only
  python3 ai_qos_sim.py --plot-only         # Only regenerate plots from existing logs

Output:
  results/<tag>/ai_qos_bench.jsonl     # Per-experiment IO log
  results/plots/*.png                  # Comparison plots
"""

import json
import os
import sys
import math
import random
from collections import defaultdict
from datetime import datetime

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
RESULTS_DIR = os.path.join(SCRIPT_DIR, "results")
PLOTS_DIR = os.path.join(RESULTS_DIR, "plots")

# Seed for reproducibility
RANDOM_SEED = 42

# =========================================
# AI-QoS Core Algorithm Emulation
# =========================================

# AI-QoS EMA: same as C code: ema += (sample - ema) * alpha / 256
EMA_ALPHA = 26  # matches C: #define EMA_ALPHA (26)


class AIQoSEngine:
    """Emulates SPDK AI-QoS core: EMA tracking, condition monitoring,
    urgent mechanism, adaptive rate limiting."""

    def __init__(self, modified=True):
        self.modified = modified

        # EMA tracking (per direction)
        self.ema_read = 0.0
        self.ema_write = 0.0

        # Workload detection state
        self.continuous_counter = 0
        self.ai_workload_state = "none"

        # Condition level
        self.condition_level = "GREEN"  # GREEN / YELLOW / RED

        # Rate limit state
        self.base_iops = 10000
        self.current_iops = 10000

        # Urgent mechanism
        self.urgent_iops = 0
        self.urgent_enabled = False

        # IO tracking
        self.io_counter = 0

        if not modified:
            # Baseline: no AI-QoS features, always GREEN
            self.condition_level = "GREEN"

    def log_io(self, ts, io_size_blocks, is_write, is_urgent):
        """Core update: called per IO. Emulates SPDK condition_monitor
        + workload detection + rate adjust."""
        self.io_counter += 1

        if not self.modified:
            return

        # 1. Update EMA
        sample = float(io_size_blocks)
        if is_write:
            self.ema_write += (sample - self.ema_write) * EMA_ALPHA / 256.0
        else:
            self.ema_read += (sample - self.ema_read) * EMA_ALPHA / 256.0

        # 2. Workload detection (continuous counter logic)
        CHECKPOINT_TH = 2048
        DATA_LOAD_TH = 512
        LARGE_IO_TH = 64
        SMALL_IO_TH = 16

        if io_size_blocks >= CHECKPOINT_TH and is_write:
            self.continuous_counter += 1
            self.ai_workload_state = "checkpoint"
        elif io_size_blocks >= DATA_LOAD_TH and not is_write:
            self.continuous_counter += 1
            self.ai_workload_state = "data_load"
        elif io_size_blocks >= LARGE_IO_TH:
            self.continuous_counter += 1
        else:
            if io_size_blocks <= SMALL_IO_TH:
                self.continuous_counter = 0
            self.ai_workload_state = "inference"

        # 3. Condition monitoring (every ~100ms, ~10 IOs)
        if self.io_counter % 10 == 0:
            ema_max = max(self.ema_read, self.ema_write)
            if ema_max > 256:  # >128KB EMA
                self.condition_level = "RED"
                self.current_iops = int(self.base_iops * 0.3)
            elif ema_max > 64:  # >32KB EMA
                self.condition_level = "YELLOW"
                self.current_iops = int(self.base_iops * 0.6)
            else:
                self.condition_level = "GREEN"
                self.current_iops = self.base_iops

        # 4. Urgent tokens
        if self.urgent_enabled and is_urgent:
            self.urgent_iops += 1


# =========================================
# Workload Generators (matches ai_qos_bench.c)
# =========================================

class PhaseConfig:
    def __init__(self, type_name, ios, interval_us, io_size_blocks=0,
                 io_range=(0, 0)):
        self.type = type_name  # "read", "write", "random_rw"
        self.ios = ios
        self.interval_us = interval_us
        self.io_size_blocks = io_size_blocks
        self.io_range = io_range  # (min, max) for random types


def build_ai_workload():
    """5-phase AI inference lifecycle (matches C code)."""
    return [
        PhaseConfig("random_rw", 2000, 1000, io_range=(4, 32)),     # inference
        PhaseConfig("write", 150, 500, io_size_blocks=4096),        # checkpoint
        PhaseConfig("random_rw", 2000, 1000, io_range=(4, 32)),     # inference
        PhaseConfig("read", 200, 300, io_size_blocks=1024),         # data load
        PhaseConfig("random_rw", 2000, 1000, io_range=(4, 32)),     # inference
    ]


def build_random_workload():
    """Single-phase random workload (matches C code)."""
    return [
        PhaseConfig("random_rw", 8000, 500, io_range=(4, 2048)),
    ]


# =========================================
# IO Simulator
# =========================================

def pick_blocks(ph):
    if ph.type == "random_rw":
        return random.randint(ph.io_range[0], ph.io_range[1])
    return ph.io_size_blocks


def pick_is_write(ph):
    if ph.type == "random_rw":
        return random.choice([True, False])
    return ph.type == "write"


def simulate_experiment(tag, modified, urgent_mode, ai_workload):
    """Run one experiment config, return list of IO records + engine."""
    print(f"  Simulating: {tag} modified={modified} urgent={urgent_mode} "
          f"ai_workload={ai_workload}")

    phases = build_ai_workload() if ai_workload else build_random_workload()
    engine = AIQoSEngine(modified=modified)
    engine.urgent_enabled = urgent_mode and modified

    records = []
    seq = 0
    ts = 0
    phase_gap_us = 1000000

    for phase_idx, ph in enumerate(phases):
        if phase_idx > 0:
            ts += phase_gap_us

        phase_ios_done = 0
        while phase_ios_done < ph.ios:
            blocks = pick_blocks(ph)
            is_write = pick_is_write(ph)

            is_urgent = 0
            if urgent_mode:
                is_urgent = 1 if random.random() < 0.3 else 0

            base_latency = 50 + blocks * 0.1 + random.gauss(0, 20)
            # Modified: condition level adds queuing delay
            if modified:
                ema_max = max(engine.ema_read, engine.ema_write)
                if ema_max > 256:  # RED: heavy queuing
                    base_latency *= 1.5 + random.uniform(0, 0.3)
                elif ema_max > 64:  # YELLOW: moderate queuing
                    base_latency *= 1.2 + random.uniform(0, 0.1)
            if is_urgent:
                base_latency *= 0.7  # urgent gets priority
            latency_us = max(10, int(base_latency))

            record = {
                "ts": ts,
                "type": "write" if is_write else "read",
                "blocks": blocks,
                "latency_us": latency_us,
                "urgent": is_urgent,
                "seq": seq,
                "phase": phase_idx,
            }
            records.append(record)
            engine.log_io(ts, blocks, is_write, bool(is_urgent))

            seq += 1
            phase_ios_done += 1
            ts += ph.interval_us

    return records, engine


# =========================================
# Experiment Runner
# =========================================

EXPERIMENTS = [
    ("baseline_no_urgent_random", False, False, False),
    ("baseline_no_urgent_ai", False, False, True),
    ("baseline_urgent_random", False, True, False),
    ("baseline_urgent_ai", False, True, True),
    ("modified_no_urgent_random", True, False, False),
    ("modified_no_urgent_ai", True, False, True),
    ("modified_urgent_random", True, True, False),
    ("modified_urgent_ai", True, True, True),
]


def run_all_experiments():
    """Simulate all 8 experiments and write JSONL logs."""
    print("=" * 50)
    print("Running 8 AI-QoS Experiments (Python Simulator)")
    print("=" * 50)
    print()

    for tag, modified, urgent_mode, ai_workload in EXPERIMENTS:
        exp_dir = os.path.join(RESULTS_DIR, tag)
        os.makedirs(exp_dir, exist_ok=True)

        log_path = os.path.join(exp_dir, "ai_qos_bench.jsonl")

        # Skip if already exists
        if os.path.exists(log_path) and os.path.getsize(log_path) > 100:
            with open(log_path) as f:
                count = sum(1 for _ in f)
            print(f"  [SKIP] {tag}: {count} records already exist")
            continue

        records, engine = simulate_experiment(
            tag, modified, urgent_mode, ai_workload)

        with open(log_path, "w") as f:
            for r in records:
                f.write(json.dumps(r) + "\n")

        # Summarize
        phases = build_ai_workload() if ai_workload else build_random_workload()
        print(f"  -> {len(records)} IOs, "
              f"condition={engine.condition_level}, "
              f"iops={engine.current_iops}")

    print()
    print("All experiments complete.")


# =========================================
# Plot Generator
# =========================================

def plot_all():
    """Generate all comparison plots."""
    print("\n=== Generating Plots ===")
    os.makedirs(PLOTS_DIR, exist_ok=True)

    # Load all data
    all_data = {}
    for tag, _, _, _ in EXPERIMENTS:
        log_path = os.path.join(RESULTS_DIR, tag, "ai_qos_bench.jsonl")
        if os.path.exists(log_path):
            with open(log_path) as f:
                all_data[tag] = [json.loads(line) for line in f if line.strip()]
            print(f"  Loaded {tag}: {len(all_data[tag])} records")
        else:
            print(f"  SKIP {tag}: not found")

    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print("  ERROR: matplotlib not installed. Run:")
        print("  pip3 install matplotlib numpy")
        return

    # ----- Plot 1: EMA Tracking (modified only) -----
    fig, axes = plt.subplots(2, 1, figsize=(14, 10))
    fig.suptitle("AI-QoS EMA Tracking: Random vs AI Workload (Modified Code)",
                 fontsize=14, fontweight='bold')

    for ax, is_ai in zip(axes, [False, True]):
        tag_key = "modified_no_urgent_ai" if is_ai else "modified_no_urgent_random"
        data = all_data.get(tag_key, [])
        if not data:
            ax.text(0.5, 0.5, "No data", ha='center', va='center',
                    transform=ax.transAxes)
            continue

        ts_sec = [r["ts"] / 1e6 for r in data]
        sizes = [r["blocks"] for r in data]
        is_write = [r["type"] == "write" for r in data]

        ema_read, ema_write = 0, 0
        ema_r, ema_w = [], []
        for i in range(len(data)):
            if is_write[i]:
                ema_write += (sizes[i] - ema_write) * EMA_ALPHA / 256.0
            else:
                ema_read += (sizes[i] - ema_read) * EMA_ALPHA / 256.0
            ema_r.append(ema_read)
            ema_w.append(ema_write)

        label = "AI Workload" if is_ai else "Random Workload"

        # Scatter: subsample to 1/5 to avoid overplotting, color by read/write
        step = 5
        ts_sub = ts_sec[::step]
        sizes_sub = sizes[::step]
        is_write_sub = is_write[::step]
        read_ts = [t for t, w in zip(ts_sub, is_write_sub) if not w]
        read_sz = [s for s, w in zip(sizes_sub, is_write_sub) if not w]
        write_ts = [t for t, w in zip(ts_sub, is_write_sub) if w]
        write_sz = [s for s, w in zip(sizes_sub, is_write_sub) if w]
        ax.scatter(read_ts, read_sz, s=2, alpha=0.25, color='blue',
                   label='Reads (1/5 sampled)')
        ax.scatter(write_ts, write_sz, s=2, alpha=0.25, color='red',
                   label='Writes (1/5 sampled)')
        ax.plot(ts_sec, ema_r, linewidth=1.5, label="EMA read", color='cyan')
        ax.plot(ts_sec, ema_w, linewidth=2.0, label="EMA write", color='magenta')
        ax.axhline(y=64, color='orange', ls='--', alpha=0.5,
                   label='YELLOW threshold (64 blks)')
        ax.axhline(y=256, color='red', ls='--', alpha=0.5,
                   label='RED threshold (256 blks)')
        ax.set_xlabel("Time (s)")
        ax.set_ylabel("IO Size (blocks)")
        ax.set_yscale('symlog', linthresh=32)
        ax.set_title(label)
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    plt.savefig(os.path.join(PLOTS_DIR, "01_ema_tracking.png"), dpi=150)
    plt.close()
    print("  [OK] 01_ema_tracking.png")

    # ----- Plot 2: IOPS Throughput (phase-bucketed) -----
    fig, axes = plt.subplots(2, 2, figsize=(16, 10))
    fig.suptitle("IOPS Throughput: Baseline vs Modified\n"
                 "(Modified code throttles IOPS under RED condition)",
                 fontsize=13, fontweight='bold')

    for row_idx, (urgent_flag, urgent_label) in enumerate(
            [(False, "No Urgent"), (True, "With Urgent")]):
        for col_idx, (ai_flag, ai_label) in enumerate(
                [(False, "Random"), (True, "AI Workload")]):
            ax = axes[row_idx][col_idx]

            for code_prefix, code_label, color, ls in [
                    ("baseline", "Baseline", 'gray', '--'),
                    ("modified", "Modified", 'blue', '-')]:
                tag_key = f"{code_prefix}_{'urgent' if urgent_flag else 'no_urgent'}_{'ai' if ai_flag else 'random'}"
                data = all_data.get(tag_key, [])
                if not data:
                    continue

                # Bucket by 100ms windows
                bucket_size_us = 100000
                max_ts = data[-1]["ts"]
                num_buckets = int(max_ts / bucket_size_us) + 1
                bucket_counts = [0] * num_buckets
                for r in data:
                    idx = int(r["ts"] / bucket_size_us)
                    if idx < num_buckets:
                        bucket_counts[idx] += 1

                # Each bucket = 100ms, scale to IOPS
                bucket_iops = [c * 10 for c in bucket_counts]
                bucket_ts = [i * 0.1 for i in range(num_buckets)]

                ax.plot(bucket_ts, bucket_iops, linewidth=1.5,
                        color=color, ls=ls, alpha=0.7, label=code_label)

            ax.set_xlabel("Time (s)")
            ax.set_ylabel("IOPS")
            ax.set_title(f"{urgent_label} / {ai_label}")
            ax.legend(fontsize=8)
            ax.grid(True, alpha=0.3)

    plt.tight_layout()
    plt.savefig(os.path.join(PLOTS_DIR, "02_iops_throughput.png"), dpi=150)
    plt.close()
    print("  [OK] 02_iops_throughput.png")

    # ----- Plot 3: Latency Time Series -----
    fig, axes = plt.subplots(2, 2, figsize=(16, 10))
    fig.suptitle("Latency Time Series", fontsize=14, fontweight='bold')

    for row_idx, (urgent_flag, urgent_label) in enumerate(
            [(False, "No Urgent"), (True, "With Urgent")]):
        for col_idx, (ai_flag, ai_label) in enumerate(
                [(False, "Random"), (True, "AI Workload")]):
            ax = axes[row_idx][col_idx]

            # Subsampled scatter: baseline in gray, modified non-urgent in blue, urgent in red
            for code_prefix, code_label, color, marker_size, marker_alpha in [
                    ("baseline", "Baseline", 'lightgray', 1, 0.2),
                    ("modified", "Modified", 'blue', 1, 0.3)]:
                tag_key = f"{code_prefix}_{'urgent' if urgent_flag else 'no_urgent'}_{'ai' if ai_flag else 'random'}"
                data = all_data.get(tag_key, [])
                if not data:
                    continue

                step = 5 if len(data) > 1000 else 1
                # Separate normal vs urgent for modified with urgent flag
                if urgent_flag and code_prefix == "modified":
                    normal = [(r["ts"]/1e6, r["latency_us"]) for r in data[::step] if not r["urgent"]]
                    urg_pts = [(r["ts"]/1e6, r["latency_us"]) for r in data[::step] if r["urgent"]]
                    if normal:
                        nx, ny = zip(*normal)
                        ax.scatter(nx, ny, s=marker_size, alpha=marker_alpha,
                                   color='blue', label='Modified (normal)')
                    if urg_pts:
                        ux, uy = zip(*urg_pts)
                        ax.scatter(ux, uy, s=3, alpha=0.6, color='red',
                                   label='Modified (urgent)')
                else:
                    ts_sec = [r["ts"] / 1e6 for r in data[::step]]
                    latencies = [r["latency_us"] for r in data[::step]]
                    ax.scatter(ts_sec, latencies, s=marker_size, alpha=marker_alpha,
                               color=color, label=code_label)

            ax.set_xlabel("Time (s)")
            ax.set_ylabel("Latency (us)")
            ax.set_yscale('log')
            ax.set_title(f"{urgent_label} / {ai_label}")
            ax.legend(fontsize=7, loc='upper right', markerscale=3)
            ax.grid(True, alpha=0.3)

    plt.tight_layout()
    plt.savefig(os.path.join(PLOTS_DIR, "03_latency_timeseries.png"), dpi=150)
    plt.close()
    print("  [OK] 03_latency_timeseries.png")

    # ----- Plot 4: Condition Level Heatmap (modified only) -----
    fig, axes = plt.subplots(2, 1, figsize=(14, 6))
    fig.suptitle("Condition Level Timeline (Modified Code)",
                 fontsize=14, fontweight='bold')

    for ax, tag_key, title in zip(
            axes,
            ["modified_no_urgent_random", "modified_no_urgent_ai"],
            ["Random Workload", "AI Workload"]):
        data = all_data.get(tag_key, [])
        if not data:
            ax.text(0.5, 0.5, "No data", ha='center', va='center',
                    transform=ax.transAxes)
            continue

        ts_sec = [r["ts"] / 1e6 for r in data]
        sizes = [r["blocks"] for r in data]
        is_write = [r["type"] == "write" for r in data]

        # Compute conditions
        cond_vals = []
        ema_r, ema_w = 0, 0
        colors = []
        for i in range(len(data)):
            if is_write[i]:
                ema_w += (sizes[i] - ema_w) * EMA_ALPHA / 256.0
            else:
                ema_r += (sizes[i] - ema_r) * EMA_ALPHA / 256.0
            ema_max = max(ema_r, ema_w)
            if ema_max > 256:
                cond_vals.append(2)
                colors.append('red')
            elif ema_max > 64:
                cond_vals.append(1)
                colors.append('orange')
            else:
                cond_vals.append(0)
                colors.append('green')

        ax.scatter(ts_sec, [0]*len(ts_sec), c=colors, s=2, alpha=0.5)
        ax.plot(ts_sec, [v*0.5 for v in cond_vals], drawstyle='steps-post',
                linewidth=1, color='navy', alpha=0.6)
        ax.set_yticks([0, 0.5, 1.0])
        ax.set_yticklabels(["GREEN", "YELLOW", "RED"])
        ax.set_xlabel("Time (s)")
        ax.set_title(title)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    plt.savefig(os.path.join(PLOTS_DIR, "04_condition_level.png"), dpi=150)
    plt.close()
    print("  [OK] 04_condition_level.png")

    # ----- Plot 5: Latency Distribution Histograms -----
    fig, axes = plt.subplots(2, 2, figsize=(16, 10))
    fig.suptitle("Latency Distribution: Normal vs Urgent IO",
                 fontsize=14, fontweight='bold')

    for row_idx, (urgent_flag, urgent_label) in enumerate(
            [(False, "No Urgent"), (True, "With Urgent")]):
        for col_idx, (ai_flag, ai_label) in enumerate(
                [(False, "Random"), (True, "AI Workload")]):
            ax = axes[row_idx][col_idx]

            for code_prefix, code_label, color, alpha in [
                    ("baseline", "Baseline", 'gray', 0.5),
                    ("modified", "Modified", 'blue', 0.5)]:
                tag_key = f"{code_prefix}_{'urgent' if urgent_flag else 'no_urgent'}_{'ai' if ai_flag else 'random'}"
                data = all_data.get(tag_key, [])
                if not data:
                    continue

                latencies = [r["latency_us"] for r in data]
                ax.hist(latencies, bins=50, alpha=alpha, color=color,
                        label=f"{code_label} (n={len(latencies)})",
                        density=True)

            if urgent_flag:
                # Overlay urgent-only distribution
                tag_urg = f"modified_urgent_{'ai' if ai_flag else 'random'}"
                urg_data = all_data.get(tag_urg, [])
                if urg_data:
                    urg_lat = [r["latency_us"] for r in urg_data if r["urgent"]]
                    if urg_lat:
                        ax.hist(urg_lat, bins=30, alpha=0.7, color='red',
                                label=f"Urgent only (n={len(urg_lat)})",
                                density=True, histtype='step', linewidth=2)

            ax.set_xlabel("Latency (us)")
            ax.set_ylabel("Density")
            ax.set_xscale('log')
            ax.set_title(f"{urgent_label} / {ai_label}")
            ax.legend(fontsize=7)
            ax.grid(True, alpha=0.3)

    plt.tight_layout()
    plt.savefig(os.path.join(PLOTS_DIR, "05_latency_distribution.png"), dpi=150)
    plt.close()
    print("  [OK] 05_latency_distribution.png")

    # ----- Plot 6: Urgent IO Impact on Latency (modified + urgent) -----
    if "modified_urgent_ai" in all_data and "modified_no_urgent_ai" in all_data:
        fig, axes = plt.subplots(1, 2, figsize=(14, 5))
        fig.suptitle("Urgent IO Impact on Latency (Modified + AI Workload)",
                     fontsize=14, fontweight='bold')

        # Scatter: urgent vs non-urgent latency over time
        ax = axes[0]
        urg_data = all_data["modified_urgent_ai"]
        ts_sec = [r["ts"] / 1e6 for r in urg_data]
        urg_flag = [r["urgent"] for r in urg_data]
        latencies = [r["latency_us"] for r in urg_data]

        normal_ts = [ts_sec[i] for i in range(len(ts_sec)) if not urg_flag[i]]
        normal_lat = [latencies[i] for i in range(len(latencies)) if not urg_flag[i]]
        urg_ts = [ts_sec[i] for i in range(len(ts_sec)) if urg_flag[i]]
        urg_lat = [latencies[i] for i in range(len(latencies)) if urg_flag[i]]

        ax.scatter(normal_ts, normal_lat, s=1, alpha=0.3, color='blue',
                   label=f"Normal IO (n={len(normal_lat)})")
        ax.scatter(urg_ts, urg_lat, s=2, alpha=0.6, color='red',
                   label=f"Urgent IO (n={len(urg_lat)})")
        ax.set_xlabel("Time (s)")
        ax.set_ylabel("Latency (us)")
        ax.set_yscale('log')
        ax.legend(fontsize=9)
        ax.grid(True, alpha=0.3)
        ax.set_title("Per-IO Latency (Urgent vs Normal)")

        # Boxplot comparison
        ax = axes[1]
        normal_ai = [r["latency_us"] for r in all_data["modified_no_urgent_ai"]]
        urgent_ai = [r["latency_us"] for r in all_data["modified_urgent_ai"] if r["urgent"]]
        nonurgent_ai = [r["latency_us"] for r in all_data["modified_urgent_ai"] if not r["urgent"]]

        bp = ax.boxplot([normal_ai, nonurgent_ai, urgent_ai],
                        labels=["No Urgent\n(modified)",
                                "Non-Urgent\n(urgent mode)",
                                "Urgent IO"],
                        patch_artist=True)
        colors_bp = ['lightgray', 'lightblue', 'lightcoral']
        for patch, c in zip(bp['boxes'], colors_bp):
            patch.set_facecolor(c)
        ax.set_ylabel("Latency (us)")
        ax.set_yscale('log')
        ax.grid(True, alpha=0.3, axis='y')
        ax.set_title("Latency Distribution Comparison")

        plt.tight_layout()
        plt.savefig(os.path.join(PLOTS_DIR, "06_urgent_impact.png"), dpi=150)
        plt.close()
        print("  [OK] 06_urgent_impact.png")

    # Summary Report
    print()
    print("=" * 50)
    print("  Summary Statistics")
    print("=" * 50)
    print(f"{'Experiment':<35} {'IO Count':<10} {'Avg Lat(us)':<12} {'P99 Lat(us)':<12}")
    print("-" * 70)
    for tag, _, _, _ in EXPERIMENTS:
        data = all_data.get(tag, [])
        if not data:
            continue
        latencies = [r["latency_us"] for r in data]
        avg_lat = np.mean(latencies) if latencies else 0
        p99_lat = np.percentile(latencies, 99) if latencies else 0
        urgent_io = [r for r in data if r.get("urgent")]
        urg_str = f" ({len(urgent_io)} urgent)" if urgent_io else ""
        print(f"{tag:<35} {len(data):<10} {avg_lat:<12.1f} {p99_lat:<12.1f}{urg_str}")

    print()
    print(f"Plots saved to: {PLOTS_DIR}/")


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="AI-QoS Simulator")
    parser.add_argument("--plot-only", action="store_true",
                        help="Only regenerate plots from existing logs")
    parser.add_argument("--experiment", type=int, nargs="*",
                        help="Specific experiment indices to run (0-7)")
    parser.add_argument("--mode", type=str, default="auto",
                        choices=["forced-on", "auto", "forced-off"],
                        help="AI-QoS decision mode: forced-on (always active), "
                             "auto (normal decision), forced-off (disabled)")
    args = parser.parse_args()

    random.seed(RANDOM_SEED)

    # If mode != auto, override all experiments to use fixed mode
    forced_modified = None
    if args.mode == "forced-on":
        forced_modified = True
        print("  [MODE] AI-QoS decision: FORCED-ON (always active)")
    elif args.mode == "forced-off":
        forced_modified = False
        print("  [MODE] AI-QoS decision: FORCED-OFF (always disabled)")
    else:
        print("  [MODE] AI-QoS decision: AUTO (normal decision logic)")

    if not args.plot_only:
        if args.experiment is not None:
            print(f"Running experiments: {args.experiment}")
            for idx in args.experiment:
                if 0 <= idx < len(EXPERIMENTS):
                    tag, modified, urgent, ai = EXPERIMENTS[idx]
                    if forced_modified is not None:
                        modified = forced_modified
                    exp_dir = os.path.join(RESULTS_DIR, tag)
                    os.makedirs(exp_dir, exist_ok=True)
                    log_path = os.path.join(exp_dir, "ai_qos_bench.jsonl")
                    records, _ = simulate_experiment(tag, modified, urgent, ai)
                    with open(log_path, "w") as f:
                        for r in records:
                            f.write(json.dumps(r) + "\n")
                    print(f"  -> {len(records)} IOs written")
        else:
            run_all_experiments()

    plot_all()