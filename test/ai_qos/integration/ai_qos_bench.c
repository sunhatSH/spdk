/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * AI-QoS Integration Benchmark
 *
 * A SPDK application that generates IO patterns to test AI-QoS features.
 *
 * Usage:
 *   ./ai_qos_bench -b Malloc0 -w workload.json -o /tmp/io_log.jsonl
 *   ./ai_qos_bench -b Malloc0 --ai-workload -o /tmp/io_log.jsonl --urgent
 *
 * Output JSONL format per line:
 * {"ts":<us>,"type":"read|write","blocks":<n>,"latency_us":<n>,"urgent":0|1,"seq":<n>,"phase":<n>}
 */

#include "spdk/stdinc.h"
#include "spdk/thread.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/json.h"
#include "spdk/bdev_module.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* Phase config: each phase specifies a type and IO parameters */
struct phase_config {
	char type[32];          /* "read", "write", "random", "random_rw" */
	uint64_t io_size_blocks;
	uint64_t io_size_range[2]; /* for random type */
	uint64_t ios;           /* number of IOs in this phase */
	uint64_t interval_us;   /* interval between IO submissions */
};

/* Top-level workload config */
struct workload_config {
	int num_phases;
	struct phase_config phases[32];
};

/* Global benchmark state */
struct bench_context {
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *bdev_desc;
	struct spdk_io_channel *bdev_io_channel;
	char *bdev_name;
	char *log_path;
	FILE *log_fp;

	struct workload_config workload;
	int current_phase;
	uint64_t phase_ios_done;
	uint64_t total_ios;
	uint64_t seq_num;
	int urgent_mode;
	int phase_transition;

	uint64_t start_tsc;
	uint64_t tsc_rate;
	uint64_t last_submit_tsc;

	struct spdk_poller *poller;
	int phase_done;
	int error;
};

static struct bench_context g_ctx;

/* ---- AI workload auto-generator: mimics LLM inference lifecycle ---- */
static void
build_ai_workload(struct workload_config *wl)
{
	int p = 0;

	/* Phase 0: Inference steady state (small random R/W) */
	snprintf(wl->phases[p].type, sizeof(wl->phases[p].type), "random_rw");
	wl->phases[p].io_size_range[0] = 4;      /* 4 blocks = 2KB */
	wl->phases[p].io_size_range[1] = 32;     /* 32 blocks = 16KB */
	wl->phases[p].ios = 2000;
	wl->phases[p].interval_us = 1000;         /* 1 IO per ms = 1000 IOPS */
	p++;

	/* Phase 1: Checkpoint (sustained large writes, 4096 blocks = 2MB) */
	snprintf(wl->phases[p].type, sizeof(wl->phases[p].type), "write");
	wl->phases[p].io_size_blocks = 4096;
	wl->phases[p].ios = 150;
	wl->phases[p].interval_us = 500;           /* high throughput */
	p++;

	/* Phase 2: Back to inference */
	snprintf(wl->phases[p].type, sizeof(wl->phases[p].type), "random_rw");
	wl->phases[p].io_size_range[0] = 4;
	wl->phases[p].io_size_range[1] = 32;
	wl->phases[p].ios = 2000;
	wl->phases[p].interval_us = 1000;
	p++;

	/* Phase 3: Data loading (sustained large reads, 1024 blocks = 512KB) */
	snprintf(wl->phases[p].type, sizeof(wl->phases[p].type), "read");
	wl->phases[p].io_size_blocks = 1024;
	wl->phases[p].ios = 200;
	wl->phases[p].interval_us = 300;
	p++;

	/* Phase 4: Inference again (steady) */
	snprintf(wl->phases[p].type, sizeof(wl->phases[p].type), "random_rw");
	wl->phases[p].io_size_range[0] = 4;
	wl->phases[p].io_size_range[1] = 32;
	wl->phases[p].ios = 2000;
	wl->phases[p].interval_us = 1000;
	p++;

	wl->num_phases = p;
}

/* ---- Random workload: always random small IOs ---- */
static void
build_random_workload(struct workload_config *wl)
{
	int p = 0;

	/* Same duration / IO count as AI workload but all random small IO */
	snprintf(wl->phases[p].type, sizeof(wl->phases[p].type), "random_rw");
	wl->phases[p].io_size_range[0] = 4;
	wl->phases[p].io_size_range[1] = 2048;
	wl->phases[p].ios = 8000;
	wl->phases[p].interval_us = 500;
	p++;

	wl->num_phases = p;
}

/* ---- Random IO size within range ---- */
static uint64_t
pick_blocks(const struct phase_config *ph)
{
	if (strcmp(ph->type, "random") == 0 || strcmp(ph->type, "random_rw") == 0) {
		uint64_t range = ph->io_size_range[1] - ph->io_size_range[0] + 1;
		return ph->io_size_range[0] + ((uint64_t)rand() % range);
	}
	return ph->io_size_blocks;
}

/* Decide read or write for random_rw */
static int
pick_is_write(const struct phase_config *ph)
{
	if (strcmp(ph->type, "random_rw") == 0) {
		return (rand() % 2);
	}
	return (strcmp(ph->type, "write") == 0);
}

static int
should_be_urgent(struct bench_context *ctx)
{
	if (!ctx->urgent_mode) return 0;
	/* Mark 30% of IOs as urgent */
	return (rand() % 10) < 3;
}

struct io_cb_arg {
	struct bench_context *ctx;
	uint64_t submit_tsc;
	uint64_t blocks;
	int is_write;
	int urgent;
	uint64_t seq;
};

static void
io_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct io_cb_arg *arg = cb_arg;
	struct bench_context *ctx = arg->ctx;
	uint64_t now = spdk_get_ticks();
	uint64_t latency_ticks = now - arg->submit_tsc;
	uint64_t latency_us = (latency_ticks * 1000000ULL) / ctx->tsc_rate;
	uint64_t ts = (now - ctx->start_tsc) * 1000000ULL / ctx->tsc_rate;

	if (!success) {
		SPDK_ERRLOG("IO %lu failed\n", arg->seq);
		ctx->error = 1;
	}

	fprintf(ctx->log_fp,
		"{\"ts\":%lu,\"type\":\"%s\",\"blocks\":%lu,\"latency_us\":%lu,"
		"\"urgent\":%d,\"seq\":%lu,\"phase\":%d}\n",
		ts, arg->is_write ? "write" : "read", arg->blocks,
		latency_us, arg->urgent, arg->seq, ctx->current_phase);

	spdk_bdev_free_io(bdev_io);
	free(arg);

	ctx->phase_ios_done++;
	ctx->total_ios++;
}

static int
submit_io(struct bench_context *ctx, const struct phase_config *ph)
{
	uint64_t blocks = pick_blocks(ph);
	int is_write = pick_is_write(ph);
	int urgent = should_be_urgent(ctx);

	char *buf = spdk_dma_malloc(blocks * 512, 0x200, NULL);
	if (!buf) {
		SPDK_ERRLOG("Failed to allocate %lu bytes\n", blocks * 512);
		return -ENOMEM;
	}

	struct io_cb_arg *cb = calloc(1, sizeof(*cb));
	if (!cb) { spdk_dma_free(buf); return -ENOMEM; }
	cb->ctx = ctx;
	cb->submit_tsc = spdk_get_ticks();
	cb->blocks = blocks;
	cb->is_write = is_write;
	cb->urgent = urgent;
	cb->seq = ctx->seq_num;
	ctx->seq_num++;

	int rc;
	if (is_write) {
		rc = spdk_bdev_write(ctx->bdev_desc, ctx->bdev_io_channel,
				     buf, 0, blocks, io_complete, cb);
	} else {
		rc = spdk_bdev_read(ctx->bdev_desc, ctx->bdev_io_channel,
				    buf, 0, blocks, io_complete, cb);
	}

	if (rc != 0) {
		spdk_dma_free(buf);
		free(cb);
	}
	return rc;
}

static int
bench_poller(void *arg)
{
	struct bench_context *ctx = arg;
	uint64_t now = spdk_get_ticks();

	/* Phase transition */
	if (ctx->phase_done || ctx->error) {
		if (ctx->error) {
			SPDK_ERRLOG("IO error during phase %d\n", ctx->current_phase);
			goto finish;
		}

		SPDK_NOTICELOG("Phase %d (%s) done: %lu IOs\n",
			       ctx->current_phase,
			       ctx->workload.phases[ctx->current_phase].type,
			       ctx->phase_ios_done);

		if (ctx->current_phase + 1 < ctx->workload.num_phases) {
			ctx->current_phase++;
			ctx->phase_ios_done = 0;
			ctx->phase_done = 0;
			ctx->last_submit_tsc = 0;
			SPDK_NOTICELOG("Starting phase %d (%s): %lu IOs\n",
				       ctx->current_phase,
				       ctx->workload.phases[ctx->current_phase].type,
				       ctx->workload.phases[ctx->current_phase].ios);

			uint64_t wait_ticks = 1000000ULL * ctx->tsc_rate / 1000000ULL; /* 1s gap between phases */
			if (now - ctx->last_submit_tsc < wait_ticks) {
				return SPDK_POLLER_BUSY;
			}
		} else {
			goto finish;
		}
	}

	const struct phase_config *ph = &ctx->workload.phases[ctx->current_phase];
	uint64_t interval_ticks = ph->interval_us * ctx->tsc_rate / 1000000ULL;

	if (now - ctx->last_submit_tsc >= interval_ticks &&
	    ctx->phase_ios_done < ph->ios) {
		if (submit_io(ctx, ph) == 0) {
			ctx->last_submit_tsc = now;
		}
		if (ctx->phase_ios_done >= ph->ios) {
			ctx->phase_done = 1;
		}
	}

	return SPDK_POLLER_BUSY;

finish:
	spdk_poller_unregister(&ctx->poller);
	fclose(ctx->log_fp);
	if (ctx->bdev_io_channel) {
		spdk_put_io_channel(ctx->bdev_io_channel);
	}
	if (ctx->bdev_desc) {
		spdk_bdev_close(ctx->bdev_desc);
	}
	SPDK_NOTICELOG("Benchmark complete. Log: %s\n", ctx->log_path);
	spdk_app_stop(ctx->error);
	return SPDK_POLLER_BUSY;
}

static void
ai_qos_bench_start(void *arg1, void *arg2)
{
	struct bench_context *ctx = arg1;
	int rc;

	ctx->start_tsc = spdk_get_ticks();
	ctx->tsc_rate = spdk_get_ticks_hz();

	rc = spdk_bdev_open_ext(ctx->bdev_name, false, NULL, NULL, &ctx->bdev_desc);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to open bdev '%s': %d\n", ctx->bdev_name, rc);
		spdk_app_stop(-1);
		return;
	}

	ctx->bdev = spdk_bdev_desc_get_bdev(ctx->bdev_desc);
	ctx->bdev_io_channel = spdk_bdev_get_io_channel(ctx->bdev_desc);
	if (!ctx->bdev_io_channel) {
		SPDK_ERRLOG("Failed to get IO channel\n");
		spdk_bdev_close(ctx->bdev_desc);
		spdk_app_stop(-1);
		return;
	}

	ctx->log_fp = fopen(ctx->log_path, "w");
	if (!ctx->log_fp) {
		SPDK_ERRLOG("Failed to open log: %s\n", ctx->log_path);
		spdk_bdev_close(ctx->bdev_desc);
		spdk_app_stop(-1);
		return;
	}

	SPDK_NOTICELOG("Benchmark started: bdev=%s phases=%d urgent=%d log=%s\n",
		       ctx->bdev_name, ctx->workload.num_phases,
		       ctx->urgent_mode, ctx->log_path);

	ctx->current_phase = 0;
	ctx->phase_ios_done = 0;
	ctx->phase_done = 0;
	ctx->last_submit_tsc = 0;

	ctx->poller = spdk_poller_register(bench_poller, ctx, 100);
}

/* ---- Usage ---- */
static void
usage(void)
{
	printf("  -b <bdev>          Bdev name (default: Malloc0)\n");
	printf("  -o <path>          Output log path (default: /tmp/ai_qos_bench.log)\n");
	printf("  -w <file>          Workload JSON file (overrides --ai-workload/--random)\n");
	printf("  --ai-workload      Use built-in AI mock workload (5 phases)\n");
	printf("  --random-workload  Use built-in random-only workload\n");
	printf("  --urgent           Enable urgent IO flag (30%% of IOs marked urgent)\n");
}

/* ---- Main ---- */
int main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "ai_qos_bench";
	opts.reactor_mask = "0x1";

	g_ctx.bdev_name = "Malloc0";
	g_ctx.log_path = "/tmp/ai_qos_bench.jsonl";
	g_ctx.urgent_mode = 0;

	/* Parse args */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
			g_ctx.bdev_name = argv[++i];
		} else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			g_ctx.log_path = argv[++i];
		} else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
			rc = parse_workload_file(argv[++i], &g_ctx.workload);
			if (rc != 0) {
				fprintf(stderr, "Failed to parse workload file\n");
				return 1;
			}
		} else if (strcmp(argv[i], "--ai-workload") == 0) {
			build_ai_workload(&g_ctx.workload);
		} else if (strcmp(argv[i], "--random-workload") == 0) {
			build_random_workload(&g_ctx.workload);
		} else if (strcmp(argv[i], "--urgent") == 0) {
			g_ctx.urgent_mode = 1;
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage();
			return 0;
		}
	}

	/* Default workload if none specified */
	if (g_ctx.workload.num_phases == 0) {
		build_ai_workload(&g_ctx.workload);
	}

	return spdk_app_start(&opts, ai_qos_bench_start, &g_ctx, NULL);
}
