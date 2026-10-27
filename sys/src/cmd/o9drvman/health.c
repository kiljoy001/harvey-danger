#include "health.h"

#ifdef __HARVEY__
#include <libc.h>
#else
#include <limits.h>
#include <stdio.h>
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

int
drvhealth_format(char *out, size_t capacity, uint32_t generation,
                 uint64_t sequence, uint64_t timestamp, DrvHealthState state,
                 uint64_t progress, uint64_t inflight, uint64_t completed,
                 uint64_t errors, uint64_t timeouts, uint64_t resets,
                 DrvHealthRecommendation recommendation, const char *error_code)
{
	const char *state_name, *recommendation_name, *code;
	int n;

	if(out == NULL || capacity == 0 || generation == 0 || sequence == 0 ||
	   state <= DrvHealthUnknown || state >= DrvHealthStateCount ||
	   recommendation < 0 || recommendation >= DrvHealthRecommendationCount)
		return -1;
	code = error_code != NULL && *error_code != 0 ? error_code : "none";
	if(!token(code))
		return -1;
	state_name = drv_health_state_name(state);
	recommendation_name = drv_health_recommendation_name(recommendation);
#ifdef __HARVEY__
	n = snprint(out, (int)capacity,
	            "o9health/1\t%u\t%llu\t%llu\t%s\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%s\t%s\n",
	            (unsigned)generation, (unsigned long long)sequence,
	            (unsigned long long)timestamp, state_name,
	            (unsigned long long)progress, (unsigned long long)inflight,
	            (unsigned long long)completed, (unsigned long long)errors,
	            (unsigned long long)timeouts, (unsigned long long)resets,
	            recommendation_name, code);
#else
	n = snprintf(out, capacity,
	             "o9health/1\t%u\t%llu\t%llu\t%s\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%s\t%s\n",
	             (unsigned)generation, (unsigned long long)sequence,
	             (unsigned long long)timestamp, state_name,
	             (unsigned long long)progress, (unsigned long long)inflight,
	             (unsigned long long)completed, (unsigned long long)errors,
	             (unsigned long long)timeouts, (unsigned long long)resets,
	             recommendation_name, code);
	if(n < 0 || (size_t)n >= capacity)
		return -1;
#endif
	return n > 0 && (size_t)n < capacity ? n : -1;
}

#ifdef __HARVEY__
struct DrvHealth {
	char *ready;
	char *health;
	char *stop;
	uint32_t generation;
	uint64_t sequence;
	int active;
};

DrvHealth*
drvhealth_open(const char *name, uint32_t generation, uint32_t slot,
               const char *statusdir)
{
	DrvHealth *h;

	USED(slot);
	h = mallocz(sizeof *h, 1);
	if(h == nil)
		return nil;
	h->generation = generation;
	if(name == nil || *name == 0 || statusdir == nil || *statusdir == 0)
		return h;
	h->ready = smprint("%s/%s.%u", statusdir, name, generation);
	h->health = smprint("%s/%s.%u.health", statusdir, name, generation);
	h->stop = smprint("%s/%s.%u.stop", statusdir, name, generation);
	if(h->ready == nil || h->health == nil || h->stop == nil)
		return h;
	h->active = 1;
	return h;
}

DrvHealth*
drvhealth_openenv(void)
{
	char *name, *gen, *status, *p;
	uint32_t generation;

	name = getenv("O9_DRIVER_NAME");
	gen = getenv("O9_DRIVER_GENERATION");
	status = getenv("O9_DRIVER_STATUS");
	if(name == nil || gen == nil || status == nil)
		return nil;
	generation = (uint32_t)strtoul(gen, &p, 10);
	if(*p != 0 || generation == 0)
		return nil;
	return drvhealth_open(name, generation, 0, status);
}

int
drvhealth_ready(DrvHealth *h)
{
	int fd;
	int n;

	if(h == nil || !h->active || h->ready == nil)
		return 0;
	fd = create(h->ready, OWRITE, 0600);
	if(fd < 0)
		return -1;
	n = write(fd, "ready\n", 6);
	close(fd);
	return n == 6 ? 0 : -1;
}

int
drvhealth_report(DrvHealth *h, DrvHealthState state,
                 uint64_t progress, uint64_t inflight, uint64_t completed,
                 uint64_t errors, uint64_t timeouts, uint64_t resets,
                 DrvHealthRecommendation recommendation, const char *error_code)
{
	char buf[512];
	int fd, n;

	if(h == nil || !h->active || h->health == nil)
		return 0;
	if(h->sequence == 0)
		h->sequence = 1;
	else
		h->sequence++;
	n = drvhealth_format(buf, sizeof buf, h->generation, h->sequence,
	                     nsec()/1000000ULL, state, progress, inflight,
	                     completed, errors, timeouts, resets,
	                     recommendation, error_code);
	if(n <= 0 || n >= (int)sizeof buf)
		return -1;
	fd = create(h->health, OWRITE, 0600);
	if(fd < 0)
		return -1;
	if(write(fd, buf, n) != n){
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

int
drvhealth_stopwait(DrvHealth *h, int milliseconds)
{
	char buf[32];
	int fd, n;

	if(h == nil || !h->active || h->stop == nil)
		return 0;
	fd = open(h->stop, OREAD);
	if(fd >= 0){
		n = read(fd, buf, sizeof buf - 1);
		close(fd);
		if(n > 0){
			buf[n] = 0;
			if(strncmp(buf, "shutdown", 8) == 0)
				return 1;
		}
	}
	if(milliseconds > 0)
		sleep(milliseconds);
	return 0;
}

void
drvhealth_close(DrvHealth *h)
{
	if(h == nil)
		return;
	free(h->ready);
	free(h->health);
	free(h->stop);
	free(h);
}

#else

struct DrvHealth { int unused; };
DrvHealth *drvhealth_open(const char *n, uint32_t g, uint32_t s, const char *d)
{ (void)n; (void)g; (void)s; (void)d; return 0; }
DrvHealth *drvhealth_openenv(void) { return 0; }
int drvhealth_ready(DrvHealth *h) { (void)h; return 0; }
int drvhealth_report(DrvHealth *h, DrvHealthState s, uint64_t a, uint64_t b,
                     uint64_t c, uint64_t d, uint64_t e, uint64_t f,
                     DrvHealthRecommendation r, const char *x)
{ (void)h; (void)s; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)r; (void)x; return 0; }
int drvhealth_stopwait(DrvHealth *h, int m) { (void)h; (void)m; return 0; }
void drvhealth_close(DrvHealth *h) { (void)h; }

#endif
