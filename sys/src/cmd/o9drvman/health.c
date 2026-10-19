#include "health.h"

#ifdef __HARVEY__
#include <libc.h>
#else
#include <limits.h>
#include <string.h>
#endif

enum {
	FieldCount = 13,
	ReportSize = 512,
};

static int
number(const char *text, uint64_t maximum, uint64_t *value)
{
	uint64_t n;

	if(text == NULL || *text == 0)
		return -1;
	n = 0;
	for(; *text != 0; text++){
		if(*text < '0' || *text > '9')
			return -1;
		if(n > (maximum - (uint64_t)(*text - '0')) / 10)
			return -1;
		n = n * 10 + (uint64_t)(*text - '0');
	}
	*value = n;
	return 0;
}

static int
token(const char *text)
{
	size_t n;

	if(text == NULL || *text == 0)
		return 0;
	for(n = 0; text[n] != 0; n++){
		if(n >= DrvHealthErrorSize - 1)
			return 0;
		if(!((text[n] >= 'a' && text[n] <= 'z') ||
		   (text[n] >= 'A' && text[n] <= 'Z') ||
		   (text[n] >= '0' && text[n] <= '9') ||
		   text[n] == '.' || text[n] == '_' || text[n] == '-'))
			return 0;
	}
	return 1;
}

static DrvHealthState
parsestate(const char *text)
{
	DrvHealthState state;

	for(state = DrvHealthHealthy; state < DrvHealthStateCount; state++)
		if(strcmp(text, drv_health_state_name(state)) == 0)
			return state;
	return DrvHealthUnknown;
}

static DrvHealthRecommendation
parserecommendation(const char *text)
{
	DrvHealthRecommendation recommendation;

	for(recommendation = DrvHealthRecommendNone;
	    recommendation < DrvHealthRecommendationCount; recommendation++)
		if(strcmp(text, drv_health_recommendation_name(recommendation)) == 0)
			return recommendation;
	return DrvHealthRecommendationCount;
}

int
drv_health_parse(const char *text, size_t length, DrvHealthReport *report)
{
	char copy[ReportSize];
	char *fields[FieldCount];
	uint64_t values[9];
	size_t i, field;

	if(text == NULL || report == NULL || length == 0 || length >= sizeof copy ||
	   text[length - 1] != '\n')
		return -1;
	memcpy(copy, text, length);
	copy[length - 1] = 0;
	fields[0] = copy;
	field = 1;
	for(i = 0; i + 1 < length && field < FieldCount; i++)
		if(copy[i] == '\t'){
			copy[i] = 0;
			fields[field++] = copy + i + 1;
		}
	if(field != FieldCount || strchr(fields[FieldCount - 1], '\t') != NULL ||
	   strcmp(fields[0], "o9health/1") != 0)
		return -1;
	if(number(fields[1], UINT32_MAX, &values[0]) < 0 ||
	   number(fields[2], INT64_MAX, &values[1]) < 0 ||
	   number(fields[3], INT64_MAX, &values[2]) < 0 ||
	   number(fields[5], INT64_MAX, &values[3]) < 0 ||
	   number(fields[6], INT64_MAX, &values[4]) < 0 ||
	   number(fields[7], INT64_MAX, &values[5]) < 0 ||
	   number(fields[8], INT64_MAX, &values[6]) < 0 ||
	   number(fields[9], INT64_MAX, &values[7]) < 0 ||
	   number(fields[10], INT64_MAX, &values[8]) < 0 ||
	   !token(fields[12]))
		return -1;
	memset(report, 0, sizeof *report);
	report->version = DrvHealthVersion;
	report->generation = (uint32_t)values[0];
	report->sequence = values[1];
	report->timestamp_ms = values[2];
	report->state = parsestate(fields[4]);
	report->progress = values[3];
	report->inflight = values[4];
	report->completed = values[5];
	report->errors = values[6];
	report->timeouts = values[7];
	report->resets = values[8];
	report->recommendation = parserecommendation(fields[11]);
	if(report->generation == 0 || report->sequence == 0 ||
	   report->state == DrvHealthUnknown ||
	   report->recommendation == DrvHealthRecommendationCount)
		return -1;
	memcpy(report->error_code, fields[12], strlen(fields[12]) + 1);
	return 0;
}

void
drv_health_reset(DrvHealthTracker *tracker, uint32_t generation)
{
	if(tracker == NULL)
		return;
	memset(tracker, 0, sizeof *tracker);
	tracker->generation = generation;
}

DrvHealthDecision
drv_health_accept(DrvHealthTracker *tracker, const DrvHealthReport *report,
	uint64_t now_ms)
{
	(void)now_ms;
	if(tracker == NULL || report == NULL || report->version != DrvHealthVersion ||
	   report->generation == 0 || report->sequence == 0 ||
	   report->state <= DrvHealthUnknown || report->state >= DrvHealthStateCount ||
	   report->recommendation < 0 ||
	   report->recommendation >= DrvHealthRecommendationCount)
		return DrvHealthReject;
	if(report->generation != tracker->generation)
		return DrvHealthIgnore;
	if(tracker->valid && (report->sequence <= tracker->report.sequence ||
	   report->timestamp_ms < tracker->report.timestamp_ms ||
	   report->progress < tracker->report.progress ||
	   report->completed < tracker->report.completed ||
	   report->errors < tracker->report.errors ||
	   report->timeouts < tracker->report.timeouts ||
	   report->resets < tracker->report.resets))
		return DrvHealthIgnore;
	tracker->report = *report;
	tracker->valid = 1;
	if(report->state == DrvHealthStalled || report->state == DrvHealthFailed ||
	   report->state == DrvHealthOffline)
		return DrvHealthRestart;
	return DrvHealthRenew;
}

const char *
drv_health_state_name(DrvHealthState state)
{
	static const char *names[] = {
		"unknown", "healthy", "degraded", "stalled", "recovering",
		"failed", "offline",
	};

	if(state < 0 || state >= DrvHealthStateCount)
		return "invalid";
	return names[state];
}

const char *
drv_health_recommendation_name(DrvHealthRecommendation recommendation)
{
	static const char *names[] = {
		"none", "quiesce", "restart", "reset", "offline",
	};

	if(recommendation < 0 || recommendation >= DrvHealthRecommendationCount)
		return "invalid";
	return names[recommendation];
}
