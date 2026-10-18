#include "lifecycle.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(c) check((c), #c, __LINE__)

static void
check(int condition, const char *expression, int line)
{
	if(condition)
		return;
	fprintf(stderr, "lifecycle:%d: check failed: %s\n", line, expression);
	failures++;
}

static DrvPolicy
policy(void)
{
	DrvPolicy p;

	p.startup_timeout_ms = 100;
	p.heartbeat_timeout_ms = 50;
	p.base_backoff_ms = 10;
	p.max_backoff_ms = 80;
	p.max_restarts = 2;
	return p;
}

static DrvResult
apply(DrvLifecycle *lifecycle, DrvEventType type, uint32_t generation,
	uint64_t now, uint64_t progress)
{
	DrvEvent event;

	event.type = type;
	event.generation = generation;
	event.now_ms = now;
	event.progress = progress;
	return drv_lifecycle_apply(lifecycle, &event);
}

static int
isgenerationevent(DrvEventType type)
{
	return type == DrvReady || type == DrvProgress || type == DrvExit ||
		type == DrvStoppedEvent;
}

static void
test_start_and_progress(void)
{
	DrvLifecycle lifecycle;
	DrvPolicy p;
	uint64_t handle;

	p = policy();
	CHECK(drv_lifecycle_init(&lifecycle, &p) == 0);
	CHECK(apply(&lifecycle, DrvStart, 0, 1000, 0) == DrvApplied);
	CHECK(lifecycle.state == DrvStarting);
	CHECK(lifecycle.generation == 1);
	CHECK(lifecycle.deadline_ms == 1100);
	CHECK(apply(&lifecycle, DrvReady, 1, 1020, 0) == DrvApplied);
	CHECK(lifecycle.state == DrvRunning);
	CHECK(lifecycle.deadline_ms == 1070);
	CHECK(apply(&lifecycle, DrvProgress, 1, 1040, 7) == DrvApplied);
	CHECK(lifecycle.last_progress == 7);
	CHECK(lifecycle.deadline_ms == 1090);
	CHECK(apply(&lifecycle, DrvProgress, 1, 1041, 7) == DrvIgnored);
	handle = drv_lifecycle_handle(&lifecycle, 9);
	CHECK(handle != 0);
	CHECK(drv_lifecycle_handle_valid(&lifecycle, handle));
	CHECK(!drv_lifecycle_handle_valid(&lifecycle, handle + (1ULL << 32)));
}

static void
test_stale_events(void)
{
	DrvLifecycle lifecycle;
	DrvLifecycle before;
	DrvPolicy p;
	DrvEventType types[] = { DrvReady, DrvProgress, DrvExit, DrvStoppedEvent };
	size_t i;

	p = policy();
	CHECK(drv_lifecycle_init(&lifecycle, &p) == 0);
	CHECK(apply(&lifecycle, DrvStart, 0, 0, 0) == DrvApplied);
	for(i = 0; i < sizeof(types) / sizeof(types[0]); i++){
		before = lifecycle;
		CHECK(apply(&lifecycle, types[i], lifecycle.generation + 1, 20, 3) ==
			DrvIgnored);
		CHECK(memcmp(&before, &lifecycle, sizeof(before)) == 0);
	}
	CHECK(apply(&lifecycle, DrvReady, lifecycle.generation, 20, 0) == DrvApplied);
}

static void
test_restart_budget(void)
{
	DrvLifecycle lifecycle;
	DrvPolicy p;
	uint32_t generation;

	p = policy();
	CHECK(drv_lifecycle_init(&lifecycle, &p) == 0);
	CHECK(apply(&lifecycle, DrvStart, 0, 0, 0) == DrvApplied);
	generation = lifecycle.generation;
	CHECK(apply(&lifecycle, DrvExit, generation, 1, 0) == DrvApplied);
	CHECK(lifecycle.state == DrvBackoff && lifecycle.retry_at_ms == 11);
	CHECK(apply(&lifecycle, DrvTick, 0, 10, 0) == DrvIgnored);
	CHECK(apply(&lifecycle, DrvTick, 0, 11, 0) == DrvApplied);
	CHECK(lifecycle.generation == generation + 1);
	generation = lifecycle.generation;
	CHECK(apply(&lifecycle, DrvExit, generation, 12, 0) == DrvApplied);
	CHECK(lifecycle.state == DrvBackoff && lifecycle.retry_at_ms == 32);
	CHECK(apply(&lifecycle, DrvTick, 0, 32, 0) == DrvApplied);
	generation = lifecycle.generation;
	CHECK(apply(&lifecycle, DrvExit, generation, 33, 0) == DrvApplied);
	CHECK(lifecycle.state == DrvFailed);
	CHECK(lifecycle.restarts == p.max_restarts);
	CHECK(!lifecycle.desired_running);
	CHECK(apply(&lifecycle, DrvStart, 0, 34, 0) == DrvIgnored);
	CHECK(apply(&lifecycle, DrvReset, 0, 35, 0) == DrvApplied);
	CHECK(lifecycle.state == DrvStopped && lifecycle.restarts == 0);
}

static void
test_deadlines_and_stop(void)
{
	DrvLifecycle lifecycle;
	DrvPolicy p;
	uint32_t generation;

	p = policy();
	CHECK(drv_lifecycle_init(&lifecycle, &p) == 0);
	CHECK(apply(&lifecycle, DrvStart, 0, 100, 0) == DrvApplied);
	generation = lifecycle.generation;
	CHECK(apply(&lifecycle, DrvTick, 0, 199, 0) == DrvIgnored);
	CHECK(apply(&lifecycle, DrvTick, 0, 200, 0) == DrvApplied);
	CHECK(lifecycle.state == DrvBackoff);
	CHECK(apply(&lifecycle, DrvStop, 0, 201, 0) == DrvApplied);
	CHECK(lifecycle.state == DrvStopped);
	CHECK(apply(&lifecycle, DrvStart, 0, 202, 0) == DrvApplied);
	generation = lifecycle.generation;
	CHECK(apply(&lifecycle, DrvReady, generation, 203, 0) == DrvApplied);
	CHECK(apply(&lifecycle, DrvStop, 0, 204, 0) == DrvApplied);
	CHECK(lifecycle.state == DrvStopping);
	CHECK(apply(&lifecycle, DrvStoppedEvent, generation, 205, 0) == DrvApplied);
	CHECK(lifecycle.state == DrvStopped);
}

static void
test_validation_and_saturation(void)
{
	DrvLifecycle lifecycle;
	DrvPolicy p;

	p = policy();
	p.base_backoff_ms = 81;
	CHECK(drv_lifecycle_init(&lifecycle, &p) < 0);
	p = policy();
	CHECK(drv_lifecycle_init(&lifecycle, &p) == 0);
	CHECK(apply(&lifecycle, DrvStart, 0, UINT64_MAX - 10, 0) == DrvApplied);
	CHECK(lifecycle.deadline_ms == UINT64_MAX);
	lifecycle.generation = UINT32_MAX;
	lifecycle.state = DrvStopped;
	CHECK(apply(&lifecycle, DrvStart, 0, 0, 0) == DrvRejected);
	CHECK(lifecycle.state == DrvFailed);
}

static uint64_t
random64(uint64_t *state)
{
	uint64_t x;

	x = *state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	*state = x;
	return x;
}

static void
property_sequences(void)
{
	DrvLifecycle lifecycle;
	DrvLifecycle before;
	DrvPolicy p;
	DrvEvent event;
	uint64_t random;
	uint64_t now;
	uint32_t oldgeneration;
	int seed;
	int step;

	p = policy();
	for(seed = 1; seed <= 128; seed++){
		random = (uint64_t)seed * 0x9e3779b97f4a7c15ULL;
		now = 0;
		CHECK(drv_lifecycle_init(&lifecycle, &p) == 0);
		for(step = 0; step < 4096; step++){
			before = lifecycle;
			oldgeneration = lifecycle.generation;
			event.type = (DrvEventType)(random64(&random) % DrvEventCount);
			now += random64(&random) % 7;
			event.now_ms = now;
			event.progress = random64(&random);
			if(isgenerationevent(event.type)){
				if((random64(&random) & 3) == 0)
					event.generation = lifecycle.generation ^ 0x80000000U;
				else
					event.generation = lifecycle.generation;
			}else
				event.generation = 0;
			(void)drv_lifecycle_apply(&lifecycle, &event);
			CHECK(drv_lifecycle_valid(&lifecycle));
			CHECK(lifecycle.generation >= oldgeneration);
			CHECK(lifecycle.restarts <= lifecycle.policy.max_restarts);
			if(isgenerationevent(event.type) &&
			   event.generation != before.generation)
				CHECK(memcmp(&before, &lifecycle, sizeof(before)) == 0);
		}
	}
}

static void
run_unit(void)
{
	test_start_and_progress();
	test_stale_events();
	test_restart_budget();
	test_deadlines_and_stop();
	test_validation_and_saturation();
}

int
main(int argc, char **argv)
{
	int rununit;
	int runproperty;

	rununit = argc == 1 || strcmp(argv[1], "--unit") == 0;
	runproperty = argc == 1 || strcmp(argv[1], "--property") == 0;
	if(!rununit && !runproperty){
		fprintf(stderr, "usage: %s [--unit|--property]\n", argv[0]);
		return 2;
	}
	if(rununit)
		run_unit();
	if(runproperty)
		property_sequences();
	if(failures != 0)
		return 1;
	printf("O9DRVMAN-%s PASS\n", rununit && runproperty ? "ALL" :
		rununit ? "UNIT" : "PROPERTY");
	return 0;
}
