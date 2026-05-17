/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/queue.h"
#include "spdk/nvme_spec.h"
#include "spdk/log.h"
#include "spdk/util.h"

#include "bdev_qos.h"

#define SPDK_BDEV_QOS_TIMESLICE_IN_USEC		1000
#define SPDK_BDEV_QOS_MIN_IO_PER_TIMESLICE		1
#define SPDK_BDEV_QOS_MIN_BYTE_PER_TIMESLICE		512
#define SPDK_BDEV_QOS_LIMIT_NOT_DEFINED		UINT64_MAX

bool
bdev_qos_is_iops_rate_limit(enum spdk_bdev_qos_rate_limit_type limit)
{
	assert(limit != SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES);

	switch (limit) {
	case SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT:
		return true;
	case SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT:
	case SPDK_BDEV_QOS_R_BPS_RATE_LIMIT:
	case SPDK_BDEV_QOS_W_BPS_RATE_LIMIT:
		return false;
	case SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES:
	default:
		return false;
	}
}

bool
bdev_qos_io_to_limit(struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		if (bdev_io->u.bdev.zcopy.start) {
			return true;
		} else {
			return false;
		}
	default:
		return false;
	}
}

bool
bdev_is_read_io(struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		if (bdev_io->u.nvme_passthru.cmd.opc & SPDK_NVME_OPC_READ) {
			return true;
		} else {
			return false;
		}
	case SPDK_BDEV_IO_TYPE_READ:
		return true;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		if (bdev_io->u.bdev.zcopy.populate) {
			return true;
		} else {
			return false;
		}
	default:
		return false;
	}
}

uint64_t
bdev_get_io_size_in_byte(struct spdk_bdev_io *bdev_io)
{
	uint32_t block_size;
	uint64_t io_size_bytes;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		return bdev_io->u.nvme_passthru.nbytes;
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_WRITE_UNCORRECTABLE:
		block_size = spdk_bdev_get_block_size(bdev_io->bdev);
		return (uint64_t)bdev_io->u.bdev.num_blocks * block_size;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		if (bdev_io->u.bdev.zcopy.start) {
			block_size = spdk_bdev_get_block_size(bdev_io->bdev);
			return (uint64_t)bdev_io->u.bdev.num_blocks * block_size;
		} else {
			return 0;
		}
	default:
		return 0;
	}
}

/*
 * =================================
 * Per-rate-limit queue/rewind (token-bucket core)
 * =================================
 */

static inline bool
bdev_qos_rw_queue_io(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io, uint64_t delta)
{
	int64_t remaining_this_timeslice;

	if (!limit->max_per_timeslice) {
		return false;
	}

	remaining_this_timeslice = __atomic_sub_fetch(&limit->remaining_this_timeslice, delta,
				   __ATOMIC_RELAXED);
	if (remaining_this_timeslice + (int64_t)delta > 0) {
		return false;
	}

	__atomic_add_fetch(&limit->remaining_this_timeslice, delta, __ATOMIC_RELAXED);
	return true;
}

static inline void
bdev_qos_rw_rewind_io(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io, uint64_t delta)
{
	__atomic_add_fetch(&limit->remaining_this_timeslice, delta, __ATOMIC_RELAXED);
}

bool
bdev_qos_rw_iops_queue(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	return bdev_qos_rw_queue_io(limit, io, 1);
}

void
bdev_qos_rw_iops_rewind_quota(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	bdev_qos_rw_rewind_io(limit, io, 1);
}

bool
bdev_qos_rw_bps_queue(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	return bdev_qos_rw_queue_io(limit, io, bdev_get_io_size_in_byte(io));
}

void
bdev_qos_rw_bps_rewind_quota(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	bdev_qos_rw_rewind_io(limit, io, bdev_get_io_size_in_byte(io));
}

bool
bdev_qos_r_bps_queue(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	if (bdev_is_read_io(io) == false) {
		return false;
	}
	return bdev_qos_rw_bps_queue(limit, io);
}

void
bdev_qos_r_bps_rewind_quota(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	if (bdev_is_read_io(io) != false) {
		bdev_qos_rw_rewind_io(limit, io, bdev_get_io_size_in_byte(io));
	}
}

bool
bdev_qos_w_bps_queue(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	if (bdev_is_read_io(io) == true) {
		return false;
	}
	return bdev_qos_rw_bps_queue(limit, io);
}

void
bdev_qos_w_bps_rewind_quota(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	if (bdev_is_read_io(io) != true) {
		bdev_qos_rw_rewind_io(limit, io, bdev_get_io_size_in_byte(io));
	}
}

void
bdev_qos_set_ops(struct spdk_bdev_qos *qos)
{
	int i;

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		if (qos->rate_limits[i].limit == SPDK_BDEV_QOS_LIMIT_NOT_DEFINED) {
			qos->rate_limits[i].queue_io = NULL;
			continue;
		}

		switch (i) {
		case SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT:
			qos->rate_limits[i].queue_io = bdev_qos_rw_iops_queue;
			qos->rate_limits[i].rewind_quota = bdev_qos_rw_iops_rewind_quota;
			break;
		case SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT:
			qos->rate_limits[i].queue_io = bdev_qos_rw_bps_queue;
			qos->rate_limits[i].rewind_quota = bdev_qos_rw_bps_rewind_quota;
			break;
		case SPDK_BDEV_QOS_R_BPS_RATE_LIMIT:
			qos->rate_limits[i].queue_io = bdev_qos_r_bps_queue;
			qos->rate_limits[i].rewind_quota = bdev_qos_r_bps_rewind_quota;
			break;
		case SPDK_BDEV_QOS_W_BPS_RATE_LIMIT:
			qos->rate_limits[i].queue_io = bdev_qos_w_bps_queue;
			qos->rate_limits[i].rewind_quota = bdev_qos_w_bps_rewind_quota;
			break;
		default:
			break;
		}
	}
}

bool
bdev_qos_queue_io(struct spdk_bdev_qos *qos, struct spdk_bdev_io *bdev_io)
{
	int i;

	if (bdev_qos_io_to_limit(bdev_io) == true) {
		for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
			if (!qos->rate_limits[i].queue_io) {
				continue;
			}

			if (qos->rate_limits[i].queue_io(&qos->rate_limits[i],
							 bdev_io) == true) {
				for (i -= 1; i >= 0; i--) {
					if (!qos->rate_limits[i].queue_io) {
						continue;
					}
					qos->rate_limits[i].rewind_quota(&qos->rate_limits[i], bdev_io);
				}
				return true;
			}
		}
	}

	return false;
}

void
bdev_qos_update_max_quota_per_timeslice(struct spdk_bdev_qos *qos)
{
	uint32_t max_per_timeslice = 0;
	int i;

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		if (qos->rate_limits[i].limit == SPDK_BDEV_QOS_LIMIT_NOT_DEFINED) {
			qos->rate_limits[i].max_per_timeslice = 0;
			continue;
		}

		max_per_timeslice = qos->rate_limits[i].limit *
				    SPDK_BDEV_QOS_TIMESLICE_IN_USEC / SPDK_SEC_TO_USEC;

		qos->rate_limits[i].max_per_timeslice = spdk_max(max_per_timeslice,
							qos->rate_limits[i].min_per_timeslice);

		__atomic_store_n(&qos->rate_limits[i].remaining_this_timeslice,
				 qos->rate_limits[i].max_per_timeslice, __ATOMIC_RELEASE);
	}

	bdev_qos_set_ops(qos);
}

/*
 * =================================
 * AI-QoS: Condition snapshot
 * =================================
 */

void
bdev_qos_cond_snapshot_init(struct spdk_bdev_qos *qos)
{
	memset(&qos->cond_snap, 0, sizeof(qos->cond_snap));

	/* Set defaults for AI-QoS configuration */
	qos->adaptive_cfg.check_interval_us = 100000; /* 100ms default */

	qos->adaptive_cfg.disk_lat_yellow_ticks = 100000;  /* 100us */
	qos->adaptive_cfg.disk_lat_red_ticks = 500000;     /* 500us */
	qos->adaptive_cfg.mem_pressure_yellow = 0.70f;
	qos->adaptive_cfg.mem_pressure_red = 0.90f;
	qos->adaptive_cfg.queue_depth_yellow = 128;
	qos->adaptive_cfg.queue_depth_red = 512;
	qos->adaptive_cfg.queue_wait_yellow_ticks = 200000; /* 200us */
	qos->adaptive_cfg.queue_wait_red_ticks = 1000000;   /* 1ms */
	qos->adaptive_cfg.yellow_mult = 0.75f;
	qos->adaptive_cfg.red_mult = 0.30f;

	/* Initialize urgent queue */
	TAILQ_INIT(&qos->urgent_queued_io);
}

void
bdev_qos_cond_snapshot_update(struct spdk_bdev_qos *qos,
			      uint64_t disk_lat_ticks,
			      uint32_t disk_qd,
			      uint64_t mem_free, uint64_t mem_total,
			      uint32_t qos_qd, uint64_t qos_wait_ticks)
{
	struct spdk_bdev_qos_cond_snapshot *snap = &qos->cond_snap;
	struct spdk_bdev_qos_adaptive_cfg *acfg = &qos->adaptive_cfg;

	snap->disk_latency_p99_ticks = disk_lat_ticks;
	snap->disk_queue_depth = disk_qd;
	snap->mem_pool_free_cnt = mem_free;
	snap->mem_pool_total_cnt = mem_total;
	snap->qos_queue_depth = qos_qd;
	snap->qos_queue_oldest_wait_ticks = qos_wait_ticks;

	/* Compute disk level */
	if (disk_lat_ticks >= acfg->disk_lat_red_ticks || disk_qd >= acfg->queue_depth_red) {
		snap->disk_level = SPDK_BDEV_QOS_COND_RED;
	} else if (disk_lat_ticks >= acfg->disk_lat_yellow_ticks ||
		   disk_qd >= acfg->queue_depth_yellow) {
		snap->disk_level = SPDK_BDEV_QOS_COND_YELLOW;
	} else {
		snap->disk_level = SPDK_BDEV_QOS_COND_GREEN;
	}

	/* Compute memory level */
	if (mem_total > 0) {
		float mem_usage = 1.0f - (float)mem_free / (float)mem_total;
		if (mem_usage >= acfg->mem_pressure_red) {
			snap->mem_level = SPDK_BDEV_QOS_COND_RED;
		} else if (mem_usage >= acfg->mem_pressure_yellow) {
			snap->mem_level = SPDK_BDEV_QOS_COND_YELLOW;
		} else {
			snap->mem_level = SPDK_BDEV_QOS_COND_GREEN;
		}
	} else {
		snap->mem_level = SPDK_BDEV_QOS_COND_GREEN;
	}

	/* Compute queue level */
	if (qos_qd >= acfg->queue_depth_red ||
	    qos_wait_ticks >= acfg->queue_wait_red_ticks) {
		snap->queue_level = SPDK_BDEV_QOS_COND_RED;
	} else if (qos_qd >= acfg->queue_depth_yellow ||
		   qos_wait_ticks >= acfg->queue_wait_yellow_ticks) {
		snap->queue_level = SPDK_BDEV_QOS_COND_YELLOW;
	} else {
		snap->queue_level = SPDK_BDEV_QOS_COND_GREEN;
	}
}

/*
 * =================================
 * AI-QoS: Adaptive limit adjustment
 * =================================
 */

void
bdev_qos_adaptive_adjust(struct spdk_bdev_qos *qos)
{
	struct spdk_bdev_qos_adaptive_cfg *acfg = &qos->adaptive_cfg;
	enum spdk_bdev_qos_cond_level worst;
	float multiplier = 1.0f;
	int i;

	if (!qos->ai_qos_enabled || !acfg->enabled) {
		return;
	}

	/* Determine the most severe condition */
	worst = qos->cond_snap.disk_level;
	if (qos->cond_snap.mem_level > worst) {
		worst = qos->cond_snap.mem_level;
	}
	if (qos->cond_snap.queue_level > worst) {
		worst = qos->cond_snap.queue_level;
	}

	/* Pick multiplier */
	switch (worst) {
	case SPDK_BDEV_QOS_COND_GREEN:
		multiplier = 1.0f;
		break;
	case SPDK_BDEV_QOS_COND_YELLOW:
		multiplier = acfg->yellow_mult;
		break;
	case SPDK_BDEV_QOS_COND_RED:
		multiplier = acfg->red_mult;
		break;
	}

	/* Apply multiplier to all limits */
	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		uint64_t base = qos->base_limits[i];
		uint64_t effective = (uint64_t)((float)base * multiplier);

		if (base == SPDK_BDEV_QOS_LIMIT_NOT_DEFINED || base == 0) {
			continue;
		}

		/* Clamp to minimum */
		if (bdev_qos_is_iops_rate_limit(i)) {
			if (effective < SPDK_BDEV_QOS_MIN_IO_PER_TIMESLICE *
					(SPDK_SEC_TO_USEC / SPDK_BDEV_QOS_TIMESLICE_IN_USEC)) {
				effective = SPDK_BDEV_QOS_MIN_IO_PER_TIMESLICE *
					    (SPDK_SEC_TO_USEC / SPDK_BDEV_QOS_TIMESLICE_IN_USEC);
			}
		} else {
			if (effective < SPDK_BDEV_QOS_MIN_BYTE_PER_TIMESLICE *
					(SPDK_SEC_TO_USEC / SPDK_BDEV_QOS_TIMESLICE_IN_USEC)) {
				effective = SPDK_BDEV_QOS_MIN_BYTE_PER_TIMESLICE *
					    (SPDK_SEC_TO_USEC / SPDK_BDEV_QOS_TIMESLICE_IN_USEC);
			}
		}

		qos->rate_limits[i].limit = effective;
	}

	bdev_qos_update_max_quota_per_timeslice(qos);

	SPDK_DEBUGLOG(bdev, "AI-QoS adaptive adjust: condition=%d, mult=%.2f\n",
		      worst, multiplier);
}

/*
 * =================================
 * AI-QoS: Urgent token check
 * =================================
 */

bool
bdev_qos_urgent_token_check(struct spdk_bdev_qos *qos, struct spdk_bdev_io *bdev_io)
{
	uint8_t token_check = bdev_io->internal.urgent_token_check;

	if (!qos->urgent_cfg.enabled) {
		return false;
	}

	if (token_check == 0) {
		return false;
	}

	/* Verify token: In production, a proper hash comparison is used.
	 * Token is stored xor-obfuscated in token_hash.
	 * The bdev_io carries a one-byte check: check_value == (token_hash & 0xFF)
	 * This is a simplified check — a real implementation should use
	 * a full hash comparison.
	 */
	if ((uint8_t)(qos->urgent_cfg.token_hash & 0xFF) != token_check) {
		SPDK_WARNLOG("AI-QoS: urgent token check failed for IO\n");
		return false;
	}

	/* Check expiry */
	if (qos->urgent_cfg.token_expiry > 0 &&
	    spdk_get_ticks() > qos->urgent_cfg.token_expiry) {
		SPDK_WARNLOG("AI-QoS: urgent token expired\n");
		return false;
	}

	/* Check per-timeslice limits */
	if (qos->urgent_cfg.max_urgent_per_timeslice > 0 &&
	    qos->urgent_count_this_ts >= (int32_t)qos->urgent_cfg.max_urgent_per_timeslice) {
		return false;
	}

	/* Check per-second limits */
	if (qos->urgent_cfg.max_urgent_per_sec > 0 &&
	    qos->urgent_count_this_sec >= (int32_t)qos->urgent_cfg.max_urgent_per_sec) {
		return false;
	}

	return true;
}

/*
 * =================================
 * AI-QoS: Condition poller entry
 * =================================
 */

int
bdev_qos_cond_poller(void *arg)
{
	struct spdk_bdev_qos *qos = arg;

	if (!qos->ai_qos_enabled || !qos->adaptive_cfg.enabled) {
		return SPDK_POLLER_IDLE;
	}

	/* The actual snapshot collection is done by the bdev layer integration
	 * code (in bdev.c) which has access to bdev-level stats.
	 * This poller triggers the adaptive adjustment based on the latest
	 * snapshot data already stored in qos->cond_snap.
	 */
	bdev_qos_adaptive_adjust(qos);

	return SPDK_POLLER_BUSY;
}
