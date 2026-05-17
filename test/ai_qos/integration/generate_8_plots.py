#!/usr/bin/env python3
"""
Generate 8 individual experiment plots (one per experiment configuration).

Overwrites results/plots/ with 8 self-contained PNGs, one per experiment.
Each plot shows: IO scatter (read/write), EMA tracking, condition level,
IOPS throughput, latency time series, and latency distribution.

Usage:
  cd test/ai_qos/integration
  python3 generate_8_plots.py
"""

import json
import os
import sys
import math
import random
from collections import defaultdict

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
RESULTS_DIR = os.path.join(SCRIPT_DIR, "results")
PLOTS_DIR = os.path.join(RESULTS_DIR, "plots")

EMA_ALPHA = 26

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

EXP_LABELS = {
    "baseline_no_urgent_random": "Exp 1: Baseline, No Urgent, Random IO",
    "baseline_no_urgent_ai":     "Exp 2: Baseline, No Urgent, AI Workload",
    "baseline_urgent_random":    "Exp 3: Baseline, With Urgent, Random IO",
    "baseline_urgent_ai":        "Exp 4: Baseline, With Urgent, AI Workload",
    "modified_no_urgent_random": "Exp 5: AI-QoS, No Urgent, Random IO",
    "modified_no_urgent_ai":     "Exp 6: AI-QoS, No Urgent, AI Workload",
    "modified_urgent_random":    "Exp 7: AI-QoS, With Urgent, Random IO",
    "modified_urgent_ai":        "Exp 8: AI-QoS, With Urgent, AI Workload",
}

EXP_NUM = {
    "baseline_no_urgent_random": "01",
    "baseline_no_urgent_ai":     "02",
    "baseline_urgent_random":    "03",
    "baseline_urgent_ai":        "04",
    "modified_no_urgent_random": "05",
    "modified_no_urgent_ai":     "06",
    "modified_urgent_random":    "07",
    "modified_urgent_ai":        "08",
}


def compute_ema(data):
    ema_r, ema_w = 0.0, 0.0
    ema_r_vals, ema_w_vals, cond_vals, phase_bounds = [], [], [], []
    last_phase = data[0]["phase"]
    for i, r in enumerate(data):
        s = float(r["blocks"])
        if r["type"] == "write":
            ema_w += (s - ema_w) * EMA_ALPHA / 256.0
        else:
            ema_r += (s - ema_r) * EMA_ALPHA / 256.0
        ema_r_vals.append(ema_r)
        ema_w_vals.append(ema_w)
        emax = max(ema_r, ema_w)
        if emax > 256:
            cond_vals.append(2)
        elif emax > 64:
            cond_vals.append(1)
        else:
            cond_vals.append(0)
        if r["phase"] != last_phase:
            phase_bounds.append(r["ts"] / 1e6)
            last_phase = r["phase"]
    return ema_r_vals, ema_w_vals, cond_vals, phase_bounds


def compute_iops_timeseries(data):
    bucket_us = 100000
    num_b = int(data[-1]["ts"] / bucket_us) + 1
    bc = [0] * num_b
    for r in data:
        idx = int(r["ts"] / bucket_us)
        if idx < num_b:
            bc[idx] += 1
    bt = [i * 0.1 for i in range(num_b)]
    biops = [c * 10 for c in bc]
    return bt, biops


def plot_one_experiment(tag, data, all_data):
    """Plot a single 2-up figure per experiment."""
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import numpy as np

    label = EXP_LABELS[tag]
    exp_num = EXP_NUM[tag]
    itemp = tag.replace("_", "-")

    ts_sec = np.array([r["ts"] for r in data]) / 1e6
    sizes = np.array([r["blocks"] for r in data])
    is_write = np.array([r["type"] == "write" for r in data])
    latencies = np.array([r["latency_us"] for r in data])
    urg_flags = np.array([r.get("urgent", 0) for r in data])

    is_modified = "modified" in tag
    has_urgent = "urgent" in tag

    ema_r_vals, ema_w_vals, cond_vals, phase_bounds = compute_ema(data)
    bt, biops = compute_iops_timeseries(data)

    fig = plt.figure(figsize=(16, 14))
    fig.suptitle(label, fontsize=14, fontweight='bold')

    # Grid: 3 rows x 2 cols
    gs = fig.add_gridspec(3, 2, hspace=0.35, wspace=0.3)

    # ----- Row 1, Col 0: EMA Tracking -----
    ax1 = fig.add_subplot(gs[0, 0])
    step = 5
    rd_mask = (~is_write[::step])
    wr_mask = is_write[::step]
    if rd_mask.any():
        ax1.scatter(ts_sec[::step][rd_mask], sizes[::step][rd_mask],
                    s=1.5, alpha=0.2, color='blue', label='Reads')
    if wr_mask.any():
        ax1.scatter(ts_sec[::step][wr_mask], sizes[::step][wr_mask],
                    s=1.5, alpha=0.2, color='red', label='Writes')
    ax1.plot(ts_sec, ema_r_vals, linewidth=1.5, color='cyan', label='EMA read')
    ax1.plot(ts_sec, ema_w_vals, linewidth=2.0, color='magenta', label='EMA write')
    ax1.axhline(y=64, color='orange', ls='--', alpha=0.5, label='YELLOW (64)')
    ax1.axhline(y=256, color='red', ls='--', alpha=0.5, label='RED (256)')
    for pb in phase_bounds:
        ax1.axvline(x=pb, color='green', ls=':', alpha=0.4)
    ax1.set_ylabel("IO Size (blocks)")
    ax1.set_yscale('symlog', linthresh=32)
    ax1.legend(fontsize=6, ncol=2)
    ax1.grid(True, alpha=0.3)
    ax1.set_title("IO Size EMA Tracking", fontsize=10)

    # ----- Row 1, Col 1: Condition Level -----
    ax2 = fig.add_subplot(gs[0, 1])
    cmap_colors = ['green', 'orange', 'red']
    cbar = [cmap_colors[c] for c in cond_vals]
    ax2.scatter(ts_sec[::4], np.array(cond_vals[::4]), c=cbar[::4],
                s=2, alpha=0.4)
    ax2.set_yticks([0, 1, 2])
    ax2.set_yticklabels(["GREEN", "YELLOW", "RED"])
    ax2.set_xlabel("Time (s)")
    ax2.grid(True, alpha=0.3)
    ax2.set_title("Condition Level", fontsize=10)

    # ----- Row 2, Col 0: IOPS Throughput -----
    ax3 = fig.add_subplot(gs[1, 0])
    ax3.fill_between(bt, biops, alpha=0.3, color='steelblue')
    ax3.plot(bt, biops, linewidth=1, color='navy')
    for pb in phase_bounds:
        ax3.axvline(x=pb, color='green', ls=':', alpha=0.4)
    ax3.set_xlabel("Time (s)")
    ax3.set_ylabel("IOPS")
    ax3.grid(True, alpha=0.3)
    ax3.set_title("IOPS Throughput (100ms buckets)", fontsize=10)

    # ----- Row 2, Col 1: Latency Time Series -----
    ax4 = fig.add_subplot(gs[1, 1])
    step_lat = 5 if len(data) > 1000 else 1
    if has_urgent and is_modified:
        normal_ts = ts_sec[::step_lat][urg_flags[::step_lat] == 0]
        normal_lat = latencies[::step_lat][urg_flags[::step_lat] == 0]
        urg_ts_plt = ts_sec[::step_lat][urg_flags[::step_lat] == 1]
        urg_lat_plt = latencies[::step_lat][urg_flags[::step_lat] == 1]
        if len(normal_ts) > 0:
            ax4.scatter(normal_ts, normal_lat, s=1, alpha=0.3, color='blue',
                        label=f'Normal (n={len(normal_ts)})')
        if len(urg_ts_plt) > 0:
            ax4.scatter(urg_ts_plt, urg_lat_plt, s=2, alpha=0.6, color='red',
                        label=f'Urgent (n={len(urg_ts_plt)})')
    else:
        ax4.scatter(ts_sec[::step_lat], latencies[::step_lat],
                    s=1, alpha=0.3, color='gray')
    for pb in phase_bounds:
        ax4.axvline(x=pb, color='green', ls=':', alpha=0.4)
    ax4.set_xlabel("Time (s)")
    ax4.set_ylabel("Latency (us)")
    ax4.set_yscale('log')
    ax4.legend(fontsize=7, markerscale=3)
    ax4.grid(True, alpha=0.3)
    ax4.set_title("Latency Time Series", fontsize=10)

    # ----- Row 3, Col 0: Latency Distribution -----
    ax5 = fig.add_subplot(gs[2, 0])
    ax5.hist(latencies, bins=50, alpha=0.6, color='steelblue',
             label=f'All IO (n={len(latencies)})', density=True)
    if has_urgent and is_modified:
        urg_lats = latencies[urg_flags == 1]
        if len(urg_lats) > 0:
            ax5.hist(urg_lats, bins=30, alpha=0.7, color='red',
                     label=f'Urgent (n={len(urg_lats)})',
                     density=True, histtype='step', linewidth=2)
    ax5.set_xlabel("Latency (us)")
    ax5.set_ylabel("Density")
    ax5.set_xscale('log')
    ax5.legend(fontsize=7)
    ax5.grid(True, alpha=0.3)
    ax5.set_title("Latency Distribution", fontsize=10)

    # ----- Row 3, Col 1: Urgent Impact Boxplot (if applicable) -----
    ax6 = fig.add_subplot(gs[2, 1])
    if has_urgent and is_modified:
        # Compare: non-urgent vs urgent within this experiment
        nonurg_lat = latencies[urg_flags == 0]
        urg_lat_dist = latencies[urg_flags == 1]
        # Also get no-urgent counterpart for comparison
        no_urg_tag = tag.replace("urgent_", "no_urgent_")
        no_urg_data_all = all_data.get(no_urg_tag, [])
        no_urg_lats = np.array([r["latency_us"] for r in no_urg_data_all]) if no_urg_data_all else []

        groups = []
        labels_bp = []
        if len(no_urg_lats) > 0:
            groups.append(no_urg_lats[:min(len(no_urg_lats), 6000)])
            labels_bp.append("No Urgent mode\n(modified)")
        if len(nonurg_lat) > 0:
            groups.append(nonurg_lat[:min(len(nonurg_lat), 6000)])
            labels_bp.append("Non-Urgent\n(urgent mode)")
        if len(urg_lat_dist) > 0:
            groups.append(urg_lat_dist)
            labels_bp.append("Urgent IO")

        if groups:
            bp = ax6.boxplot(groups, labels=labels_bp, patch_artist=True)
            box_colors = ['lightgray', 'lightblue', 'lightcoral']
            for patch, c in zip(bp['boxes'], box_colors):
                patch.set_facecolor(c)
            ax6.set_yscale('log')
            ax6.grid(True, alpha=0.3, axis='y')
    else:
        ax6.text(0.5, 0.5, "Urgent not applicable\nfor this experiment",
                 ha='center', va='center', transform=ax6.transAxes, fontsize=10)
    ax6.set_title("Urgent IO Impact", fontsize=10)

    # Save
    out_path = os.path.join(PLOTS_DIR, f"exp_{exp_num}_{itemp}.png")
    plt.savefig(out_path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  [OK] {os.path.basename(out_path)}")


def main():
    """Load all experiment data and generate 8 individual plots."""
    print("\n=== Generating 8 Individual Experiment Plots ===")
    os.makedirs(PLOTS_DIR, exist_ok=True)

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
        print("  ERROR: matplotlib not installed. Run: pip3 install matplotlib numpy")
        return

    experiment_order = [
        "baseline_no_urgent_random", "baseline_no_urgent_ai",
        "baseline_urgent_random", "baseline_urgent_ai",
        "modified_no_urgent_random", "modified_no_urgent_ai",
        "modified_urgent_random", "modified_urgent_ai",
    ]

    for tag in experiment_order:
        data = all_data.get(tag, [])
        if not data:
            print(f"  SKIP {tag}: no data")
            continue
        plot_one_experiment(tag, data, all_data)

    # Also regenerate the 6 comparison plots for backward compatibility
    print("\n  [Note] 6 comparison plots remain at results/plots/ (from ai_qos_sim.py)")
    print(f"\n  8 individual plots saved to: {PLOTS_DIR}/")


if __name__ == "__main__":
    main()
