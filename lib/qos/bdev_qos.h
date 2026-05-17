/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_BDEV_QOS_INTERNAL_H
#define SPDK_BDEV_QOS_INTERNAL_H

#include "spdk/stdinc.h"

#include "spdk/bdev.h"

struct spdk_bdev_io;
struct spdk_bdev_channel;

/*
 * =================================
 * Shared QoS constants
 * =================================
 */
#define SPDK_BDEV_QOS_TIMESLICE_IN_USEC		1000
#define SPDK_BDEV_QOS_MIN_IO_PER_TIMESLICE		1
#define SPDK_BDEV_QOS_MIN_BYTE_PER_TIMESLICE		512
#define SPDK_BDEV_QOS_LIMIT_NOT_DEFINED		UINT64_MAX

/*
 * =================================
 * AI-QoS: Adaptive + Urgent structures
 * =================================
 */

/** AI-QoS: adaptive policy configuration */
struct spdk_bdev_qos_adaptive_cfg {
	bool		enabled;
	uint32_t	check_interval_us;	/* default 100ms */

	/* Thresholds */
	uint64_t	disk_lat_yellow_ticks;	/* 100us */
	uint64_t	disk_lat_red_ticks;	/* 500us */
	float		mem_pressure_yellow;	/* 0.70 */
	float		mem_pressure_red;	/* 0.90 */
	uint32_t	queue_depth_yellow;	/* 128 */
	uint32_t	queue_depth_red;	/* 512 */
	uint64_t	queue_wait_yellow_ticks; /* 200us */
	uint64_t	queue_wait_red_ticks;	/* 1ms */

	/* Multipliers (applied to base limits) */
	float		yellow_mult;		/* 0.75 */
	float		red_mult;		/* 0.30 */
};

/** AI-QoS: urgent I/O configuration */
struct spdk_bdev_qos_urgent_cfg {
	bool		enabled;
	uint64_t	token_hash;		/* stored token hash */
	uint64_t	token_expiry;		/* token expiry tsc ticks */
	uint32_t	max_urgent_per_timeslice; /* max urgent IOs per 1ms slice */
	uint64_t	max_urgent_bytes_per_ts; /* max urgent bytes per slice */
	uint32_t	max_urgent_per_sec;	/* max urgent IOs per second */
};

/** AI-QoS: auto-urgent configuration (self-detected queue congestion) */
struct spdk_bdev_qos_auto_urgent_cfg {
	bool		enabled;
	uint32_t	queue_depth_threshold;	/* queue depth that triggers auto-urgent */
	uint32_t	consecutive_polls;	/* how many consecutive 1ms polls at threshold */
	uint32_t	max_auto_urgent_per_ts;	/* max auto-urgent IOs per 1ms timeslice */
};

/** AI-QoS: condition snapshot */
struct spdk_bdev_qos_cond_snapshot {
	/* Disk */
	uint64_t	disk_latency_p99_ticks;
	uint32_t	disk_queue_depth;
	/* Memory */
	uint64_t	mem_pool_free_cnt;
	uint64_t	mem_pool_total_cnt;
	/* Queue */
	uint32_t	qos_queue_depth;
	uint64_t	qos_queue_oldest_wait_ticks;
	/* Aggregate levels */
	enum spdk_bdev_qos_cond_level disk_level;
	enum spdk_bdev_qos_cond_level mem_level;
	enum spdk_bdev_qos_cond_level queue_level;
};

/** Per-rate-limit tracking structure */
struct spdk_bdev_qos_limit {
	/** IOs or bytes allowed per second */
	uint64_t limit;
	/** Remaining IOs or bytes in current timeslice (may go negative) */
	int64_t remaining_this_timeslice;
	/** Minimum IOs or bytes per timeslice */
	uint32_t min_per_timeslice;
	/** Maximum IOs or bytes per timeslice */
	uint32_t max_per_timeslice;
	/** Queue check function */
	bool (*queue_io)(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io);
	/** Quota rewind function */
	void (*rewind_quota)(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io);
};

/** Per-bdev QoS state */
struct spdk_bdev_qos {
	/* Standard rate limits */
	struct spdk_bdev_qos_limit rate_limits[SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES];

	struct spdk_bdev_channel *ch;
	struct spdk_thread *thread;
	uint64_t timeslice_size;
	uint64_t last_timeslice;
	struct spdk_poller *poller;

	/* === AI-QoS fields === */

	/** AI-QoS master enable flag */
	bool				ai_qos_enabled;

	/** Adaptive policy configuration */
	struct spdk_bdev_qos_adaptive_cfg	adaptive_cfg;

	/** Latest condition snapshot */
	struct spdk_bdev_qos_cond_snapshot	cond_snap;

	/** Condition monitoring poller */
	struct spdk_poller			*cond_poller;

	/** Base rate limits (before adaptive adjustment) */
	uint64_t				base_limits[SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES];

	/** Urgent I/O configuration */
	struct spdk_bdev_qos_urgent_cfg		urgent_cfg;

	/** Urgent counters */
	int32_t					urgent_count_this_ts;
	uint64_t				urgent_bytes_this_ts;
	uint64_t				urgent_sec_start_ticks;
	int32_t					urgent_count_this_sec;

	/** Total IOs queued across all channels (approximate, atomics) */
	int64_t					total_queued_io_count;

	/** Auto-urgent: self-detected queue congestion bypass */
	struct spdk_bdev_qos_auto_urgent_cfg	auto_urgent_cfg;
	bool					auto_urgent_active;
	uint32_t				auto_urgent_consecutive;
	uint32_t				auto_urgent_count_this_ts;

	/** Urgent IO priority queue (drained before normal qos queue) */
	TAILQ_HEAD(, spdk_bdev_io)		urgent_queued_io;
};

/*
 * =================================
 * Function declarations
 * =================================
 */

/* QoS algorithm helpers */
bool bdev_qos_is_iops_rate_limit(enum spdk_bdev_qos_rate_limit_type limit);
bool bdev_qos_io_to_limit(struct spdk_bdev_io *bdev_io);
bool bdev_is_read_io(struct spdk_bdev_io *bdev_io);
uint64_t bdev_get_io_size_in_byte(struct spdk_bdev_io *bdev_io);

/* Per-rate-limit queue/rewind functions */
bool bdev_qos_rw_iops_queue(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io);
void bdev_qos_rw_iops_rewind_quota(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io);
bool bdev_qos_rw_bps_queue(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io);
void bdev_qos_rw_bps_rewind_quota(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io);
bool bdev_qos_r_bps_queue(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io);
void bdev_qos_r_bps_rewind_quota(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io);
bool bdev_qos_w_bps_queue(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io);
void bdev_qos_w_bps_rewind_quota(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io);

/* Core QoS logic */
void bdev_qos_set_ops(struct spdk_bdev_qos *qos);
bool bdev_qos_queue_io(struct spdk_bdev_qos *qos, struct spdk_bdev_io *bdev_io);
void bdev_qos_update_max_quota_per_timeslice(struct spdk_bdev_qos *qos);

/* AI-QoS condition monitoring */
void bdev_qos_cond_snapshot_init(struct spdk_bdev_qos *qos);
void bdev_qos_cond_snapshot_update(struct spdk_bdev_qos *qos,
				   uint64_t disk_lat_ticks,
				   uint32_t disk_qd,
				   uint64_t mem_free, uint64_t mem_total,
				   uint32_t qos_qd, uint64_t qos_wait_ticks);

/* AI-QoS adaptive adjustment */
void bdev_qos_adaptive_adjust(struct spdk_bdev_qos *qos);

/* AI-QoS urgent IO */
bool bdev_qos_urgent_token_check(struct spdk_bdev_qos *qos, struct spdk_bdev_io *bdev_io);

/* Poller dispatch */
int bdev_qos_cond_poller(void *arg);

#endif /* SPDK_BDEV_QOS_INTERNAL_H */
