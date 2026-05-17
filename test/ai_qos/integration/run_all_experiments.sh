#!/usr/bin/env bash
#
# Run all 8 AI-QoS experiments and generate plots.
#
# Usage: ./run_all_experiments.sh
# Prerequisites: Docker Desktop running
#
# Strategy:
#   1. Create tar archives of baseline and modified SPDK
#   2. Build two Docker images from those archives
#   3. Run ai_qos_bench in each config
#   4. Generate plots
#

export LC_ALL=C
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SPDK_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
RESULTS_DIR="${SCRIPT_DIR}/results"

set -euo pipefail

if ! docker info &>/dev/null; then
    echo "ERROR: Docker is not running."
    exit 1
fi

# ---- Step 1: Prepare tarballs & .dockerignore ----
echo "========================================"
echo " Step 1: Prepare Build Artifacts"
echo "========================================"
echo ""

cd "${SPDK_ROOT}"

echo "Creating baseline tarball from f786c6d75..."
git archive --format=tar.gz f786c6d75 -o /tmp/spdk-baseline.tar.gz

echo "Creating modified tarball from HEAD..."
git archive --format=tar.gz HEAD -o /tmp/spdk-modified.tar.gz

# Copy tarballs to SPDK root for Docker build context
cp /tmp/spdk-baseline.tar.gz "${SPDK_ROOT}/spdk-baseline.tar.gz"
cp /tmp/spdk-modified.tar.gz "${SPDK_ROOT}/spdk-modified.tar.gz"

# Create .dockerignore at root (will be removed after build)
if [ ! -f "${SPDK_ROOT}/.dockerignore" ]; then
    echo ".git/" > "${SPDK_ROOT}/.dockerignore"
    echo ".gitignore" >> "${SPDK_ROOT}/.dockerignore"
    CLEANUP_DOCKERIGNORE=1
else
    CLEANUP_DOCKERIGNORE=0
fi

# ---- Step 2: Build both Docker images ----
echo ""
echo "========================================"
echo " Step 2: Build Docker Images"
echo "========================================"
echo ""

# Remove old images
docker rmi spdk-aiqos-baseline:latest 2>/dev/null || true
docker rmi spdk-aiqos-modified:latest 2>/dev/null || true

echo "Building spdk-aiqos-baseline (this takes 5-10 min)..."
cd "${SPDK_ROOT}"
docker build -t spdk-aiqos-baseline:latest \
    -f test/ai_qos/integration/Dockerfile.baseline \
    . 2>&1 | tail -8
echo "  -> spdk-aiqos-baseline built"

echo ""
echo "Building spdk-aiqos-modified (this takes 5-10 min)..."
docker build -t spdk-aiqos-modified:latest \
    -f test/ai_qos/integration/Dockerfile.modified \
    . 2>&1 | tail -8
echo "  -> spdk-aiqos-modified built"

# Clean up
rm -f "${SPDK_ROOT}/spdk-baseline.tar.gz" "${SPDK_ROOT}/spdk-modified.tar.gz"
if [ "${CLEANUP_DOCKERIGNORE}" = "1" ]; then
    rm -f "${SPDK_ROOT}/.dockerignore"
fi

# ---- Step 3: Build test programs ----
echo ""
echo "========================================"
echo " Step 3: Verify Test Programs in Images"
echo "========================================"
echo ""

for img in spdk-aiqos-baseline spdk-aiqos-modified; do
    if docker run --rm "${img}:latest" test -f /spdk/ai_qos_bench; then
        echo "[OK] ${img}: ai_qos_bench present"
    else
        echo "[FAIL] ${img}: ai_qos_bench missing"
        exit 1
    fi
done

# ---- Step 4: Run 8 experiments ----
echo ""
echo "========================================"
echo " Step 4: Run 8 Experiments"
echo "========================================"
echo ""

mkdir -p "${RESULTS_DIR}"

run_experiment() {
    local code="$1"
    local img="$2"
    shift 2
    local bench_args=("$@")

    local exp_dir="${RESULTS_DIR}/${code}"
    mkdir -p "${exp_dir}"

    # Skip if already done
    if [ -f "${exp_dir}/ai_qos_bench.jsonl" ] && [ -s "${exp_dir}/ai_qos_bench.jsonl" ]; then
        local line_count=$(wc -l < "${exp_dir}/ai_qos_bench.jsonl")
        if [ "$line_count" -gt 100 ]; then
            echo "[SKIP] ${code}: already done (${line_count} records)"
            return
        fi
    fi

    echo "--- ${code} ---"
    echo "  image=${img} args='${bench_args[*]}'"

    # Start SPDK target in background container
    CID=$(docker run -d --privileged "${img}:latest" /spdk/build/bin/spdk_tgt -m 0x1 2>/dev/null)
    echo "  Container: ${CID:0:12}"

    sleep 3

    # Retry RPC startup
    local rpc_ok=0
    for attempt in 1 2 3; do
        if docker exec "${CID}" /spdk/scripts/rpc.py bdev_malloc_create 128 4096 2>/dev/null; then
            rpc_ok=1
            break
        fi
        sleep 3
    done

    if [ "$rpc_ok" -eq 0 ]; then
        echo "  FATAL: Could not create Malloc bdev"
        docker kill "${CID}" 2>/dev/null || true
        docker rm "${CID}" 2>/dev/null || true
        return
    fi

    echo "  Malloc bdev created"
    echo "  Starting ai_qos_bench..."

    # Run benchmark
    set +e
    timeout 120 docker exec "${CID}" /spdk/ai_qos_bench \
        -b Malloc0 \
        -o /tmp/ai_qos_bench.jsonl \
        "${bench_args[@]}"
    local bench_rc=$?
    set -e

    # Wait for IO completions
    sleep 2

    # Copy log
    docker cp "${CID}:/tmp/ai_qos_bench.jsonl" "${exp_dir}/ai_qos_bench.jsonl" 2>/dev/null || \
        echo "  WARN: log not found"

    if [ -f "${exp_dir}/ai_qos_bench.jsonl" ]; then
        echo "  Records: $(wc -l < "${exp_dir}/ai_qos_bench.jsonl")"
    fi

    docker kill "${CID}" 2>/dev/null || true
    docker rm "${CID}" 2>/dev/null || true
    echo "  -> ${code} done"
    echo ""
}

# Run sequentially
run_experiment "baseline_no_urgent_random" "spdk-aiqos-baseline" "--random-workload"
run_experiment "baseline_no_urgent_ai" "spdk-aiqos-baseline" "--ai-workload"
run_experiment "baseline_urgent_random" "spdk-aiqos-baseline" "--urgent" "--random-workload"
run_experiment "baseline_urgent_ai" "spdk-aiqos-baseline" "--urgent" "--ai-workload"
run_experiment "modified_no_urgent_random" "spdk-aiqos-modified" "--random-workload"
run_experiment "modified_no_urgent_ai" "spdk-aiqos-modified" "--ai-workload"
run_experiment "modified_urgent_random" "spdk-aiqos-modified" "--urgent" "--random-workload"
run_experiment "modified_urgent_ai" "spdk-aiqos-modified" "--urgent" "--ai-workload"

# ---- Step 5: Generate plots ----
echo ""
echo "========================================"
echo " Step 5: Generate Plots"
echo "========================================"
echo ""

cd "${SCRIPT_DIR}"
python3 plot_results.py "${RESULTS_DIR}"

echo ""
echo "========================================"
echo " ALL DONE"
echo "========================================"
echo ""
echo "Result logs: ${RESULTS_DIR}/"
ls -la "${RESULTS_DIR}/"
echo ""
echo "Plots:       ${RESULTS_DIR}/plots/"
ls -la "${RESULTS_DIR}/plots/" 2>/dev/null || echo "  (no plots yet)"
