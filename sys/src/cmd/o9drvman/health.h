#ifndef O9DRVMAN_HEALTH_H
#define O9DRVMAN_HEALTH_H

#ifdef __HARVEY__
#include <u.h>
typedef usize size_t;
typedef u8 uint8_t;
typedef u32 uint32_t;
typedef u64 uint64_t;
#ifndef NULL
#define NULL nil
#endif
#ifndef UINT32_MAX
#define UINT32_MAX ((uint32_t)~0U)
#endif
#define INT64_MAX ((uint64_t)0x7FFFFFFFFFFFFFFFULL)
#else
#include <stddef.h>
#include <stdint.h>
#endif

enum {
	DrvHealthVersion = 1,
	DrvHealthErrorSize = 33,
};

typedef enum DrvHealthState DrvHealthState;
typedef enum DrvHealthRecommendation DrvHealthRecommendation;
typedef enum DrvHealthDecision DrvHealthDecision;
typedef struct DrvHealthReport DrvHealthReport;
typedef struct DrvHealthTracker DrvHealthTracker;

enum DrvHealthState {
	DrvHealthUnknown,
	DrvHealthHealthy,
	DrvHealthDegraded,
	DrvHealthStalled,
	DrvHealthRecovering,
	DrvHealthFailed,
	DrvHealthOffline,
	DrvHealthStateCount,
};

enum DrvHealthRecommendation {
	DrvHealthRecommendNone,
	DrvHealthRecommendQuiesce,
	DrvHealthRecommendRestart,
	DrvHealthRecommendReset,
	DrvHealthRecommendOffline,
	DrvHealthRecommendationCount,
};

enum DrvHealthDecision {
	DrvHealthReject = -1,
	DrvHealthIgnore,
	DrvHealthRenew,
	DrvHealthRestart,
};

struct DrvHealthReport {
	uint32_t version;
	uint32_t generation;
	uint64_t sequence;
	uint64_t timestamp_ms;
	DrvHealthState state;
	uint64_t progress;
	uint64_t inflight;
	uint64_t completed;
	uint64_t errors;
	uint64_t timeouts;
	uint64_t resets;
	DrvHealthRecommendation recommendation;
	char error_code[DrvHealthErrorSize];
};

struct DrvHealthTracker {
	uint32_t generation;
	uint8_t valid;
	DrvHealthReport report;
};

int drv_health_parse(const char *, size_t, DrvHealthReport *);
void drv_health_reset(DrvHealthTracker *, uint32_t);
DrvHealthDecision drv_health_accept(DrvHealthTracker *,
	const DrvHealthReport *, uint64_t);
const char *drv_health_state_name(DrvHealthState);
const char *drv_health_recommendation_name(DrvHealthRecommendation);

#endif
