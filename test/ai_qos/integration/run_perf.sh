#!/usr/bin/env bash
#
# AI-QoS Performance Benchmark Runner
#
# Usage:
#   ./run_perf.sh                        # Run all 8 experiments + generate plots
#   ./run_perf.sh --mode forced-off     # Force-disable AI-QoS decision
#   ./run_perf.sh --mode forced-on      # Force-enable AI-QoS decision
#   ./run_perf.sh --plot-only           # Only regenerate plots from existing data
#   ./run_perf.sh --single 0 4          # Run specific experiments by index (0-7)
#   ./run_perf.sh --clean               # Remove all cached JSONL data, re-run from scratch
#
# Experiment indices:
#   0 = baseline_no_urgent_random   4 = modified_no_urgent_random
#   1 = baseline_no_urgent_ai       5 = modified_no_urgent_ai
#   2 = baseline_urgent_random      6 = modified_urgent_random
#   3 = baseline_urgent_ai          7 = modified_urgent_ai
#

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SIMULATOR="${SCRIPT_DIR}/ai_qos_sim.py"
PLOT_SCRIPT="${SCRIPT_DIR}/generate_8_plots.py"

MODE="auto"
PLOT_ONLY=false
SINGLE_ARGS=""
CLEAN=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)
            MODE="$2"
            shift 2
            ;;
        --plot-only)
            PLOT_ONLY=true
            shift
            ;;
        --single)
            shift
            SINGLE_ARGS=()
            while [[ $# -gt 0 && "$1" != -* ]]; do
                SINGLE_ARGS+=("$1")
                shift
            done
            ;;
        --clean)
            CLEAN=true
            shift
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--mode forced-on|auto|forced-off] [--plot-only] [--single 0 4 ...] [--clean]"
            exit 1
            ;;
    esac
done

# Clean if requested
if $CLEAN; then
    echo "=== Cleaning all cached experiment data ==="
    find "${SCRIPT_DIR}/results" -name "ai_qos_bench.jsonl" -delete
    echo "Done."
fi

echo "=============================================="
echo " AI-QoS Performance Benchmark Runner"
echo " Mode: $MODE"
echo "=============================================="
echo ""

# Step 1: Run experiments (unless --plot-only)
if ! $PLOT_ONLY; then
    echo ">>> Step 1: Simulation Experiments"
    echo ""

    if [ ${#SINGLE_ARGS[@]} -gt 0 ]; then
        echo "Running experiments: ${SINGLE_ARGS[*]}"
        python3 "$SIMULATOR" --experiment "${SINGLE_ARGS[@]}" --mode "$MODE"
    else
        python3 "$SIMULATOR" --mode "$MODE"
    fi

    echo ""
    echo ">>> Step 1 Complete"
    echo ""
fi

# Step 2: Generate 8 individual plots (one per experiment)
echo ">>> Step 2: Generating 8 Individual Experiment Plots"
echo ""
python3 "$PLOT_SCRIPT"

# Step 3: Also generate comparison plots from ai_qos_sim
echo ""
echo ">>> Step 3: Generating Comparison Summary"
echo ""
python3 "$SIMULATOR" --plot-only --mode "$MODE"

echo ""
echo "=============================================="
echo " All done! Results:"
echo ""
ls -lh "${SCRIPT_DIR}/results/plots/exp_"*.png 2>/dev/null
echo ""
echo " Experiment logs: ${SCRIPT_DIR}/results/<tag>/ai_qos_bench.jsonl"
echo "=============================================="
