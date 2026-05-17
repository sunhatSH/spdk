#!/usr/bin/env bash
#
# Docker-based test script for AI-QoS workload pattern detection.
#
# Prerequisites: Docker Desktop must be running.
#
# Usage:
#   ./test/ai_qos/docker_test.sh          # Standalone unit test (fast)
#   ./test/ai_qos/docker_test.sh full     # Full SPDK build + test
#

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SPDK_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
IMAGE="spdk-aiqos-test:latest"

if ! docker info &>/dev/null; then
	echo "Error: Docker is not running. Please start Docker Desktop first."
	exit 1
fi

standalone_test() {
	echo "=== [Phase 1] Standalone AI-QoS Unit Test ==="

	cat > /tmp/Dockerfile.aiqos <<- 'DOCKER_EOF'
		FROM ubuntu:22.04
		RUN apt-get update -qq && \
		    apt-get install -y -qq gcc libc6-dev make 2>&1 | tail -3 && \
		    apt-get clean && rm -rf /var/lib/apt/lists/*
		COPY test/ai_qos/test_ai_qos_workload.c .
		RUN gcc -O2 -o /tmp/test_ai_qos_workload test_ai_qos_workload.c -lm
	DOCKER_EOF

	docker build -t ${IMAGE} -f /tmp/Dockerfile.aiqos "${SPDK_ROOT}" 2>&1 | grep -E "^(#|ERROR|Step)"

	echo ""
	docker run --rm ${IMAGE} /tmp/test_ai_qos_workload
}

full_build() {
	echo "=== [Phase 2] Full SPDK Build ==="

	cat > /tmp/Dockerfile.aiqos.full <<- 'DOCKER_EOF'
		FROM ubuntu:22.04 AS builder

		ENV DEBIAN_FRONTEND=noninteractive

		RUN apt-get update -qq && \
		    apt-get install -y -qq \
		        gcc g++ make meson ninja-build pkg-config \
		        libssl-dev libaio-dev libudev-dev \
		        libjson-c-dev libkeyutils-dev \
		        libnuma-dev python3 python3-pip \
		        uuid-dev nasm git curl \
		    && apt-get clean && rm -rf /var/lib/apt/lists/*

		WORKDIR /spdk
		COPY . .

		# Configure with minimal options (no hardware-dependent features)
		RUN ./configure --without-rdma --without-vfio-user --without-vhost \
		        --without-virtio --without-iscsi --without-rbd \
		        --without-fuse --without-ocf --without-xnvme \
		        --without-idxd --without-ublk --without-nbd \
		        --without-daos --without-nvme-cuse \
		    && make -j$(nproc) 2>&1 | tail -5

		# Run standalone test
		RUN gcc -O2 -o /tmp/test_ai_qos_workload test/ai_qos/test_ai_qos_workload.c -lm
	DOCKER_EOF

	echo "Building full SPDK (this takes 5-10 minutes)..."
	docker build -t ${IMAGE} -f /tmp/Dockerfile.aiqos.full "${SPDK_ROOT}" 2>&1 | tail -10

	echo ""
	echo "=== Full SPDK build complete ==="
	docker run --rm ${IMAGE} /tmp/test_ai_qos_workload

	echo ""
	echo "You can now run integration tests:"
	echo "  docker run --rm -it ${IMAGE} /bin/bash"
	echo "  # Inside the container:"
	echo "  #   ./build/bin/spdk_tgt &"
	echo "  #   sleep 3"
	echo "  #   ./scripts/rpc.py bdev_malloc_create 128 4096"
	echo "  #   ./scripts/rpc.py bdev_set_qos_limit --rw_ios_per_sec 1000 Malloc0"
	echo "  #   ./test/bdevperf/bdevperf.py ..."
}

# --- Main ---
echo "==========================================="
echo " AI-QoS Docker Test Suite"
	echo " SPDK root: ${SPDK_ROOT}"
echo "==========================================="
echo ""

case "${1:-}" in
	full)
		standalone_test
		echo ""
		full_build
		;;
	*)
		standalone_test
		;;
esac
