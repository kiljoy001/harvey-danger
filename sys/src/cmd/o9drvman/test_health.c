#include "health.h"

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
	fprintf(stderr, "health:%d: check failed: %s\n", line, expression);
	failures++;
}

static const char good[] =
	"o9health/1\t7\t12\t900\tdegraded\t81\t2\t79\t3\t1\t2\treset\tcrc-error\n";

static DrvHealthReport
report(uint32_t generation, uint64_t sequence, uint64_t timestamp,
	DrvHealthState state)
{
	DrvHealthReport value;

	memset(&value, 0, sizeof value);
	value.version = DrvHealthVersion;
	value.generation = generation;
	value.sequence = sequence;
	value.timestamp_ms = timestamp;
	value.state = state;
	value.progress = sequence;
	value.completed = sequence;
	value.recommendation = DrvHealthRecommendNone;
	strcpy(value.error_code, "none");
	return value;
}

static void
replacefield(char *output, size_t capacity, const char *input, int wanted,
	const char *replacement)
{
	const char *end, *start;
	size_t prefix, replacementlen, suffix;
	int field;

	start = input;
	for(field = 0; field < wanted; field++){
		start = strchr(start, '\t');
		CHECK(start != NULL);
		if(start == NULL)
			return;
		start++;
	}
	end = strchr(start, wanted == 12 ? '\n' : '\t');
	CHECK(end != NULL);
	if(end == NULL)
		return;
	prefix = (size_t)(start - input);
	replacementlen = strlen(replacement);
	suffix = strlen(end);
	CHECK(prefix + replacementlen + suffix + 1 <= capacity);
	if(prefix + replacementlen + suffix + 1 > capacity)
		return;
	memcpy(output, input, prefix);
	memcpy(output + prefix, replacement, replacementlen);
	memcpy(output + prefix + replacementlen, end, suffix + 1);
}

static void
rejectfield(int field, const char *replacement)
{
	DrvHealthReport value;
	char changed[640];

	replacefield(changed, sizeof changed, good, field, replacement);
	CHECK(drv_health_parse(changed, strlen(changed), &value) < 0);
}

static void
test_parse(void)
{
	DrvHealthReport value;
	char changed[640];
	int fields[] = {1, 2, 3, 5, 6, 7, 8, 9, 10};
	int i;

	CHECK(drv_health_parse(good, sizeof good - 1, &value) == 0);
	CHECK(value.version == 1 && value.generation == 7);
	CHECK(value.sequence == 12 && value.timestamp_ms == 900);
	CHECK(value.state == DrvHealthDegraded && value.progress == 81);
	CHECK(value.inflight == 2 && value.completed == 79);
	CHECK(value.errors == 3 && value.timeouts == 1 && value.resets == 2);
	CHECK(value.recommendation == DrvHealthRecommendReset);
	CHECK(strcmp(value.error_code, "crc-error") == 0);
	CHECK(drv_health_parse(good, sizeof good - 2, &value) < 0);
	CHECK(drv_health_parse(NULL, sizeof good - 1, &value) < 0);
	CHECK(drv_health_parse(good, sizeof good - 1, NULL) < 0);
	CHECK(drv_health_parse(good, 0, &value) < 0);
	memset(changed, 'x', 511);
	changed[511] = '\n';
	CHECK(drv_health_parse(changed, 512, &value) < 0);
	CHECK(drv_health_parse("o9health/1\n", 11, &value) < 0);
	snprintf(changed, sizeof changed, "%.*s\textra", (int)sizeof good - 2, good);
	strcat(changed, "\n");
	CHECK(drv_health_parse(changed, strlen(changed), &value) < 0);
	memcpy(changed, good, sizeof good);
	changed[0] = 'x';
	CHECK(drv_health_parse(changed, strlen(changed), &value) < 0);
	for(i = 0; i < (int)(sizeof fields / sizeof fields[0]); i++)
		rejectfield(fields[i], "x");
	rejectfield(1, "4294967296");
	rejectfield(2, "9223372036854775808");
	rejectfield(1, "");
	rejectfield(1, "0");
	rejectfield(2, "0");
	rejectfield(4, "bogus");
	rejectfield(11, "bogus");
	rejectfield(12, "");
	rejectfield(12, "bad/code");
	rejectfield(12, "123456789012345678901234567890123");
	replacefield(changed, sizeof changed, good, 12, "AZaz09._-");
	CHECK(drv_health_parse(changed, strlen(changed), &value) == 0);
}

static void
test_accept(void)
{
	DrvHealthTracker tracker;
	DrvHealthReport value;
	int field;

	drv_health_reset(NULL, 1);
	drv_health_reset(&tracker, 7);
	value = report(7, 1, 10, DrvHealthHealthy);
	CHECK(drv_health_accept(NULL, &value, 10) == DrvHealthReject);
	CHECK(drv_health_accept(&tracker, NULL, 10) == DrvHealthReject);
	value.version = 2;
	CHECK(drv_health_accept(&tracker, &value, 10) == DrvHealthReject);
	value = report(0, 1, 10, DrvHealthHealthy);
	CHECK(drv_health_accept(&tracker, &value, 10) == DrvHealthReject);
	value = report(7, 0, 10, DrvHealthHealthy);
	CHECK(drv_health_accept(&tracker, &value, 10) == DrvHealthReject);
	value = report(7, 1, 10, DrvHealthUnknown);
	CHECK(drv_health_accept(&tracker, &value, 10) == DrvHealthReject);
	value.state = DrvHealthStateCount;
	CHECK(drv_health_accept(&tracker, &value, 10) == DrvHealthReject);
	value = report(7, 1, 10, DrvHealthHealthy);
	value.recommendation = (DrvHealthRecommendation)-1;
	CHECK(drv_health_accept(&tracker, &value, 10) == DrvHealthReject);
	value.recommendation = DrvHealthRecommendationCount;
	CHECK(drv_health_accept(&tracker, &value, 10) == DrvHealthReject);

	drv_health_reset(&tracker, 7);
	value = report(8, 1, 10, DrvHealthHealthy);
	CHECK(drv_health_accept(&tracker, &value, 10) == DrvHealthIgnore);
	CHECK(!tracker.valid);
	value.generation = 7;
	CHECK(drv_health_accept(&tracker, &value, 9) == DrvHealthRenew);
	CHECK(tracker.valid && tracker.report.sequence == 1);
	CHECK(drv_health_accept(&tracker, &value, 11) == DrvHealthIgnore);
	value.sequence = 2;
	value.timestamp_ms = 9;
	CHECK(drv_health_accept(&tracker, &value, 11) == DrvHealthIgnore);
	value.timestamp_ms = 11;
	value.progress = 0;
	CHECK(drv_health_accept(&tracker, &value, 11) == DrvHealthIgnore);
	value.progress = 2;
	value.completed = 2;
	value.errors = 2;
	value.timeouts = 2;
	value.resets = 2;
	value.state = DrvHealthRecovering;
	CHECK(drv_health_accept(&tracker, &value, 11) == DrvHealthRenew);
	for(field = 0; field < 4; field++){
		DrvHealthReport backwards = value;
		backwards.sequence++;
		backwards.timestamp_ms++;
		if(field == 0)
			backwards.completed--;
		else if(field == 1)
			backwards.errors--;
		else if(field == 2)
			backwards.timeouts--;
		else
			backwards.resets--;
		CHECK(drv_health_accept(&tracker, &backwards, 12) == DrvHealthIgnore);
	}
	value.sequence = 3;
	value.timestamp_ms = 12;
	value.progress = 3;
	value.completed = 3;
	value.state = DrvHealthStalled;
	CHECK(drv_health_accept(&tracker, &value, 12) == DrvHealthRestart);
	value.sequence++;
	value.timestamp_ms++;
	value.progress++;
	value.completed++;
	value.state = DrvHealthFailed;
	CHECK(drv_health_accept(&tracker, &value, 13) == DrvHealthRestart);
	value.sequence++;
	value.timestamp_ms++;
	value.progress++;
	value.completed++;
	value.state = DrvHealthOffline;
	CHECK(drv_health_accept(&tracker, &value, 14) == DrvHealthRestart);
	CHECK(strcmp(drv_health_state_name((DrvHealthState)-1), "invalid") == 0);
	CHECK(strcmp(drv_health_state_name(DrvHealthStateCount), "invalid") == 0);
	CHECK(strcmp(drv_health_recommendation_name((DrvHealthRecommendation)-1),
	             "invalid") == 0);
	CHECK(strcmp(drv_health_recommendation_name(DrvHealthRecommendationCount),
	             "invalid") == 0);
}

static uint64_t
random64(uint64_t *state)
{
	uint64_t value;

	value = *state;
	value ^= value << 13;
	value ^= value >> 7;
	value ^= value << 17;
	*state = value;
	return value;
}

static void
property_sequences(void)
{
	DrvHealthTracker tracker;
	DrvHealthReport value;
	DrvHealthDecision decision;
	uint64_t random, sequence, timestamp;
	int seed, step;

	for(seed = 1; seed <= 128; seed++){
		random = (uint64_t)seed * 0x9e3779b97f4a7c15ULL;
		sequence = 0;
		timestamp = 0;
		drv_health_reset(&tracker, 3);
		for(step = 0; step < 2048; step++){
			sequence++;
			timestamp += random64(&random) % 5;
			value = report(3, sequence, timestamp,
				(DrvHealthState)(DrvHealthHealthy +
				(random64(&random) % (DrvHealthStateCount - 1))));
			decision = drv_health_accept(&tracker, &value, timestamp);
			CHECK(decision == DrvHealthRenew || decision == DrvHealthRestart);
			CHECK(tracker.valid && tracker.report.sequence == sequence);
			CHECK(tracker.report.generation == tracker.generation);
			CHECK(drv_health_accept(&tracker, &value, timestamp) == DrvHealthIgnore);
		}
	}
}

static void
test_format(void)
{
	char record[256];
	const char *expected =
		"o9health/1\t7\t3\t1234\thealthy\t10\t2\t9\t1\t2\t3\trestart\tio-timeout\n";
	DrvHealthReport parsed;

	CHECK(drvhealth_format(record, sizeof record, 7, 3, 1234,
	                       DrvHealthHealthy, 10, 2, 9, 1, 2, 3,
	                       DrvHealthRecommendRestart, "io-timeout") ==
	      (int)strlen(expected));
	CHECK(strcmp(record, expected) == 0);
	CHECK(drv_health_parse(record, strlen(record), &parsed) == 0);
	CHECK(parsed.generation == 7 && parsed.sequence == 3 &&
	      parsed.timestamp_ms == 1234 && parsed.state == DrvHealthHealthy &&
	      parsed.progress == 10 && parsed.inflight == 2 &&
	      parsed.completed == 9 && parsed.errors == 1 &&
	      parsed.timeouts == 2 && parsed.resets == 3 &&
	      parsed.recommendation == DrvHealthRecommendRestart &&
	      strcmp(parsed.error_code, "io-timeout") == 0);
	CHECK(drvhealth_format(record, 16, 7, 3, 1234, DrvHealthHealthy,
	                       10, 2, 9, 1, 2, 3,
	                       DrvHealthRecommendRestart, "io-timeout") < 0);
	CHECK(drvhealth_format(record, sizeof record, 0, 3, 1234,
	                       DrvHealthHealthy, 0, 0, 0, 0, 0, 0,
	                       DrvHealthRecommendNone, "none") < 0);
	CHECK(drvhealth_format(record, sizeof record, 7, 3, 1234,
	                       DrvHealthUnknown, 0, 0, 0, 0, 0, 0,
	                       DrvHealthRecommendNone, "none") < 0);
	CHECK(drvhealth_format(record, sizeof record, 7, 3, 1234,
	                       DrvHealthHealthy, 0, 0, 0, 0, 0, 0,
	                       DrvHealthRecommendNone, "bad code") < 0);
	CHECK(drvhealth_format(record, sizeof record, 7, 3, 1234,
	                       DrvHealthHealthy, 0, 0, 0, 0, 0, 0,
	                       DrvHealthRecommendRestart, "none") > 0);
}

int
main(int argc, char **argv)
{
	int rununit, runproperty;

	rununit = argc == 1 || strcmp(argv[1], "--unit") == 0;
	runproperty = argc == 1 || strcmp(argv[1], "--property") == 0;
	if(!rununit && !runproperty){
		fprintf(stderr, "usage: %s [--unit|--property]\n", argv[0]);
		return 2;
	}
	if(rununit){
		test_parse();
		test_accept();
		test_format();
	}
	if(runproperty)
		property_sequences();
	if(failures != 0)
		return 1;
	printf("O9DRVHEALTH-%s PASS\n", rununit && runproperty ? "ALL" :
		rununit ? "UNIT" : "PROPERTY");
	return 0;
}
