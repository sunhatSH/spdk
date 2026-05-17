/*
 * Standalone unit test for AI-QoS workload pattern detection.
 *
 * Tests EMA-based IO size tracking + consecutive large-IO burst detection
 * for LLM checkpoint, data loading, and inference patterns.
 *
 * Compile: clang -o test_ai_qos_workload test_ai_qos_workload.c -lm
 * Run:     ./test_ai_qos_workload
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

/* ========== Minimal replica of the AI workload detection logic ========== */

struct spdk_bdev_qos_ai_workload {
	bool		enabled;
	uint8_t		ema_alpha;		/* alpha = ema_alpha / 256 */
	uint64_t	ema_write_blocks;
	uint64_t	ema_read_blocks;
	uint32_t	large_write_consecutive;
	uint32_t	large_read_consecutive;
	uint64_t	ckpt_size_threshold_blocks;
	uint32_t	ckpt_consecutive_threshold;
	uint64_t	dataload_size_threshold_blocks;
	uint32_t	dataload_consecutive_threshold;
	bool		checkpoint_active;
	bool		data_load_active;
	bool		inference_steady;
};

#define SPDK_BDEV_QOS_EMA_ALPHA_DEFAULT	26   /* ~0.1 in 1/256 units */

static void
ai_workload_sample_write(struct spdk_bdev_qos_ai_workload *wl, uint64_t blocks)
{
	int64_t diff;

	diff = (int64_t)blocks - (int64_t)wl->ema_write_blocks;
	wl->ema_write_blocks += (uint64_t)((diff * (int32_t)wl->ema_alpha) >> 8);

	if (blocks >= wl->ckpt_size_threshold_blocks) {
		wl->large_write_consecutive++;
	} else {
		wl->large_write_consecutive = 0;
	}
}

static void
ai_workload_sample_read(struct spdk_bdev_qos_ai_workload *wl, uint64_t blocks)
{
	int64_t diff;

	diff = (int64_t)blocks - (int64_t)wl->ema_read_blocks;
	wl->ema_read_blocks += (uint64_t)((diff * (int32_t)wl->ema_alpha) >> 8);

	if (blocks >= wl->dataload_size_threshold_blocks) {
		wl->large_read_consecutive++;
	} else {
		wl->large_read_consecutive = 0;
	}
}

static void
ai_workload_detect(struct spdk_bdev_qos_ai_workload *wl)
{
	if (!wl->enabled) {
		wl->checkpoint_active = false;
		wl->data_load_active = false;
		wl->inference_steady = false;
		return;
	}

	/* Checkpoint detection: sustained large writes */
	if (wl->large_write_consecutive >= wl->ckpt_consecutive_threshold &&
	    wl->ema_write_blocks >= wl->ckpt_size_threshold_blocks) {
		wl->checkpoint_active = true;
	} else {
		wl->checkpoint_active = false;
	}

	/* Data loading detection: sustained large reads */
	if (wl->large_read_consecutive >= wl->dataload_consecutive_threshold &&
	    wl->ema_read_blocks >= wl->dataload_size_threshold_blocks) {
		wl->data_load_active = true;
	} else {
		wl->data_load_active = false;
	}

	/* Inference steady state: small IOs, neither checkpoint nor data load */
	wl->inference_steady = (!wl->checkpoint_active && !wl->data_load_active);
}

/* ========== Test cases ========== */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
	printf("  TEST: %s ... ", name); \
	fflush(stdout);

#define CHECK(cond) \
	if (cond) { \
		printf("PASS\n"); \
		tests_passed++; \
	} else { \
		printf("FAIL\n"); \
		tests_failed++; \
	}

static void
test_ema_convergence(void)
{
	struct spdk_bdev_qos_ai_workload wl;
	int i;

	memset(&wl, 0, sizeof(wl));
	wl.enabled = true;
	wl.ema_alpha = SPDK_BDEV_QOS_EMA_ALPHA_DEFAULT;
	wl.ckpt_size_threshold_blocks = 2048;
	wl.dataload_size_threshold_blocks = 512;

	/* Feed 100 identical writes (100 blocks each) */
	for (i = 0; i < 100; i++) {
		ai_workload_sample_write(&wl, 100);
	}
	TEST("EMA converges towards 100 for 100-block writes");
	CHECK(wl.ema_write_blocks >= 90 && wl.ema_write_blocks <= 100);
}

static void
test_ema_large_write_follow(void)
{
	struct spdk_bdev_qos_ai_workload wl;
	int i;

	memset(&wl, 0, sizeof(wl));
	wl.enabled = true;
	wl.ema_alpha = SPDK_BDEV_QOS_EMA_ALPHA_DEFAULT;
	wl.ckpt_size_threshold_blocks = 2048;
	wl.dataload_size_threshold_blocks = 512;

	/* Feed large writes (4096 blocks = 2MB) and verify EMA tracks */
	for (i = 0; i < 200; i++) {
		ai_workload_sample_write(&wl, 4096);
	}
	TEST("EMA follows large writes");
	CHECK(wl.ema_write_blocks >= wl.ckpt_size_threshold_blocks);
}

static void
test_ema_small_io_resets_consecutive(void)
{
	struct spdk_bdev_qos_ai_workload wl;

	memset(&wl, 0, sizeof(wl));
	wl.enabled = true;
	wl.ema_alpha = SPDK_BDEV_QOS_EMA_ALPHA_DEFAULT;
	wl.ckpt_size_threshold_blocks = 2048;
	wl.dataload_size_threshold_blocks = 512;

	/* 10 large writes */
	for (int i = 0; i < 10; i++) {
		ai_workload_sample_write(&wl, 4096);
	}
	/* 1 small write should reset consecutive counter */
	ai_workload_sample_write(&wl, 4);
	TEST("Small write resets consecutive counter");
	CHECK(wl.large_write_consecutive == 0);
}

static void
test_checkpoint_detection(void)
{
	struct spdk_bdev_qos_ai_workload wl;
	int i;

	memset(&wl, 0, sizeof(wl));
	wl.enabled = true;
	wl.ema_alpha = SPDK_BDEV_QOS_EMA_ALPHA_DEFAULT;
	wl.ckpt_size_threshold_blocks = 2048;
	wl.dataload_size_threshold_blocks = 512;
	wl.ckpt_consecutive_threshold = 64;
	wl.dataload_consecutive_threshold = 128;

	/* Not enough large writes → no checkpoint */
	for (i = 0; i < 60; i++) {
		ai_workload_sample_write(&wl, 4096);
	}
	ai_workload_detect(&wl);
	TEST("Checkpoint not triggered before threshold (60 < 64)");
	CHECK(wl.checkpoint_active == false && wl.data_load_active == false);

	/* Exceed threshold → checkpoint detected */
	for (i = 0; i < 10; i++) {
		ai_workload_sample_write(&wl, 4096);
	}
	ai_workload_detect(&wl);
	TEST("Checkpoint detected after 70 consecutive large writes");
	CHECK(wl.checkpoint_active == true && wl.data_load_active == false && wl.inference_steady == false);
}

static void
test_dataload_detection(void)
{
	struct spdk_bdev_qos_ai_workload wl;
	int i;

	memset(&wl, 0, sizeof(wl));
	wl.enabled = true;
	wl.ema_alpha = SPDK_BDEV_QOS_EMA_ALPHA_DEFAULT;
	wl.ckpt_size_threshold_blocks = 2048;
	wl.dataload_size_threshold_blocks = 512;
	wl.ckpt_consecutive_threshold = 64;
	wl.dataload_consecutive_threshold = 128;

	/* Not enough large reads → no data load */
	for (i = 0; i < 120; i++) {
		ai_workload_sample_read(&wl, 1024);
	}
	ai_workload_detect(&wl);
	TEST("Data load not triggered before threshold (120 < 128)");
	CHECK(wl.data_load_active == false);

	/* Exceed threshold */
	for (i = 0; i < 20; i++) {
		ai_workload_sample_read(&wl, 1024);
	}
	ai_workload_detect(&wl);
	TEST("Data load detected after 140 consecutive large reads");
	CHECK(wl.data_load_active == true && wl.checkpoint_active == false && wl.inference_steady == false);
}

static void
test_inference_steady_state(void)
{
	struct spdk_bdev_qos_ai_workload wl;
	int i;

	memset(&wl, 0, sizeof(wl));
	wl.enabled = true;
	wl.ema_alpha = SPDK_BDEV_QOS_EMA_ALPHA_DEFAULT;
	wl.ckpt_size_threshold_blocks = 2048;
	wl.dataload_size_threshold_blocks = 512;
	wl.ckpt_consecutive_threshold = 64;
	wl.dataload_consecutive_threshold = 128;

	/* Feed small IOs (8 blocks = 4KB), mix read/write */
	for (i = 0; i < 200; i++) {
		ai_workload_sample_read(&wl, 8);
		ai_workload_sample_write(&wl, 16);
	}
	ai_workload_detect(&wl);
	TEST("Inference steady state for small mixed IOs");
	CHECK(wl.inference_steady == true && wl.checkpoint_active == false && wl.data_load_active == false);
}

static void
test_disabled_no_detection(void)
{
	struct spdk_bdev_qos_ai_workload wl;
	int i;

	memset(&wl, 0, sizeof(wl));
	wl.enabled = false;
	wl.ema_alpha = SPDK_BDEV_QOS_EMA_ALPHA_DEFAULT;
	wl.ckpt_size_threshold_blocks = 2048;
	wl.dataload_size_threshold_blocks = 512;
	wl.ckpt_consecutive_threshold = 64;
	wl.dataload_consecutive_threshold = 128;

	/* Load up write counter as if checkpointing */
	for (i = 0; i < 100; i++) {
		ai_workload_sample_write(&wl, 4096);
	}
	ai_workload_detect(&wl);
	TEST("All flags false when disabled");
	CHECK(wl.checkpoint_active == false && wl.data_load_active == false && wl.inference_steady == false);
}

static void
test_switch_from_checkpoint_to_inference(void)
{
	struct spdk_bdev_qos_ai_workload wl;
	int i;

	memset(&wl, 0, sizeof(wl));
	wl.enabled = true;
	wl.ema_alpha = SPDK_BDEV_QOS_EMA_ALPHA_DEFAULT;
	wl.ckpt_size_threshold_blocks = 2048;
	wl.dataload_size_threshold_blocks = 512;
	wl.ckpt_consecutive_threshold = 64;
	wl.dataload_consecutive_threshold = 128;

	/* Phase 1: checkpoint */
	for (i = 0; i < 100; i++) {
		ai_workload_sample_write(&wl, 4096);
	}
	ai_workload_detect(&wl);
	TEST("Phase 1: checkpoint active");
	CHECK(wl.checkpoint_active == true);

	/* Phase 2: switch to inference (small IOs) */
	for (i = 0; i < 50; i++) {
		ai_workload_sample_write(&wl, 8);
	}
	ai_workload_detect(&wl);

	/*
	 * After 50 small writes, consecutive counter is reset
	 * Write EMA is still decaying from the large value,
	 * so we need enough small IOs to bring it down.
	 * Check: checkpoint_active should be false,
	 *        consecutive counter should be 0.
	 */
	TEST("Phase 2: consecutive counter reset by small writes");
	CHECK(wl.large_write_consecutive == 0 && wl.checkpoint_active == false);
}

static void
test_ema_robustness_to_noise(void)
{
	struct spdk_bdev_qos_ai_workload wl;
	int i;

	memset(&wl, 0, sizeof(wl));
	wl.enabled = true;
	wl.ema_alpha = SPDK_BDEV_QOS_EMA_ALPHA_DEFAULT;
	wl.ckpt_size_threshold_blocks = 2048;
	wl.dataload_size_threshold_blocks = 512;
	wl.ckpt_consecutive_threshold = 64;
	wl.dataload_consecutive_threshold = 128;

	/* Mostly small writes, with 5 large ones mixed in */
	for (i = 0; i < 200; i++) {
		ai_workload_sample_write(&wl, 8);
		if (i % 15 == 0) {
			ai_workload_sample_write(&wl, 4096); /* occasional spike */
		}
	}
	ai_workload_detect(&wl);

	/*
	 * Spikes reset consecutive counter (because they're separated by
	 * small IOs). EMA should still be near 8 because small writes dominate.
	 */
	TEST("No false positive: noise spikes don't trigger checkpoint");
	CHECK(wl.checkpoint_active == false && wl.large_write_consecutive < 64);
}

int main(void)
{
	printf("AI-QoS Workload Pattern Detection Tests\n");
	printf("========================================\n\n");

	test_ema_convergence();
	test_ema_large_write_follow();
	test_ema_small_io_resets_consecutive();
	test_checkpoint_detection();
	test_dataload_detection();
	test_inference_steady_state();
	test_disabled_no_detection();
	test_switch_from_checkpoint_to_inference();
	test_ema_robustness_to_noise();

	printf("\nResults: %d passed, %d failed out of %d tests\n",
	       tests_passed, tests_failed, tests_passed + tests_failed);

	return tests_failed > 0 ? 1 : 0;
}
