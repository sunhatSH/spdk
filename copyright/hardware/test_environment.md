# Hardware & Test Environment Specification
# SPDK AI-QoS Project - 软著材料配套

## Test Machine (Host)

### Machine
| Field | Value |
|-------|-------|
| Model | MacBook Pro (Mac16,5) |
| Model Number | MX303CH/A |
| Chip | Apple M4 Max |
| Total Cores | 14 (10 Performance + 4 Efficiency) |
| Memory | 36 GB Unified |
| Firmware | 18000.101.7 |
| OS | macOS 26.4.1 (Darwin 25.4.0, arm64) |

### Storage
| Field | Value |
|-------|-------|
| Device | APPLE SSD AP1024Z |
| Capacity | 1.0 TB (1,000,555,581,440 bytes) |
| Protocol | Apple Fabric (NVMe-compatible) |
| Interface | Internal, Fixed, Solid State |
| TRIM | Yes |
| Firmware Rev | 2,973.100 |
| S.M.A.R.T. | Verified |
| Partition Map | GPT |
| Bus | Built-in Apple NVMe Controller |

### Filesystem Layout
| Partition | Type | Size | Mount |
|-----------|------|------|-------|
| disk0s1 | APFS ISC | 524 MB | iSCPreboot |
| disk0s2 | APFS Container | 994.7 GB | / (Macintosh HD + Data) |
| disk0s3 | APFS Recovery | 5.4 GB | Recovery |

Available disk on /System/Volumes/Data: ~432 GB free of 926 GB

---

## Docker Test Environment

### Docker Desktop
| Field | Value |
|-------|-------|
| Docker Host | Docker Desktop (macOS backend) |
| Runtime | containerd / desktop-linux |
| CPU allocation | 14 cores |
| Memory allocation | 7.65 GiB |
| Build Kit | v0.22.0 |
| Platforms | linux/amd64, linux/arm64 |

### Docker Images Created
- `spdk-aiqos-baseline` : Ubuntu 22.04 + SPDK @ f786c6d75 (upstream)
- `spdk-aiqos-modified` : Ubuntu 22.04 + SPDK @ ai-qos-v1 HEAD (ee48e7ea9)

### Container Runtime
- Base image: `ubuntu:22.04`
- Privileged mode (for hugepages + DPDK)
- Memory: unlimited
- CPU: pinned to core 0 (`-m 0x1`)

---

## SPDK Build Configuration

### Baseline (f786c6d75)
| Option | Value |
|--------|-------|
| DPDK | Submodule (in-tree) |
| RDMA | Disabled |
| VFIO-User | Disabled |
| Vhost | Disabled |
| Compiler | GCC (Ubuntu 22.04 default) |

### Modified (ai-qos-v1, ee48e7ea9)
Same configure options as baseline, with AI-QoS module additions:
- `lib/qos/` — standalone bdev_qos module
- `include/spdk/bdev.h` — new public API declarations
- `lib/bdev/bdev.c` — urgent token integration, condition poller
- `lib/bdev/bdev_rpc.c` — 3 new RPC handlers
- `test/ai_qos/` — unit test + integration framework

---

## Performance Baseline (Measured)

### Host System IO (macOS)
Sustained sequential throughput (theoretical): ~3,500 MB/s (NVMe over Apple Fabric)
Note: Direct block device access blocked by macOS SIP.

### Python Simulator (ai_qos_sim.py)
| Metric | Baseline Random | Modified Random | Baseline AI | Modified AI |
|--------|----------------|----------------|------------|------------|
| Avg Lat (us) | 151.4 | 252.0 (+66%) | 64.7 | 75.1 (+16%) |
| P99 Lat (us) | 272.0 | 460.0 (+69%) | 466.0 | 772.0 (+66%) |
| Condition | GREEN (always) | RED (automatic) | GREEN (always) | GREEN |

### Summary of Differences
- **Random workload**: Modified code detects condition RED (EMA > 256 blocks threshold), throttles to 30% of base IOPS, resulting in ~66% higher queuing latency.
- **AI workload**: Modified code stays GREEN throughout inference phases; P99 latency increases during checkpoint/data-load phases due to large IO queuing.
- **Urgent IO**: Receives priority treatment (~30% lower latency than non-urgent IOs) when enabled on modified code.

---

## File Structure
```
copyright/
├── hardware/
│   └── test_environment.md        ← this file
├── source_pdfs/                   → (placeholder for source code PDFs)
├── documentation/                  → (placeholder for design doc PDFs)
└── comparison/
    └── ai_qos_benchmark_results/  → (simulation output)
        ├── results/plots/         → 6 comparison charts
        └── summary_statistics.txt → (in reports above)
```
