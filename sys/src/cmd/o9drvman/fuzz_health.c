#include "health.h"

#include <stddef.h>
#include <stdint.h>

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	DrvHealthReport report;
	DrvHealthTracker tracker;
	DrvHealthDecision decision;

	if(drv_health_parse((const char *)data, size, &report) < 0)
		return 0;
	if(report.version != DrvHealthVersion || report.generation == 0 ||
	   report.sequence == 0 || report.state <= DrvHealthUnknown ||
	   report.state >= DrvHealthStateCount ||
	   report.recommendation >= DrvHealthRecommendationCount)
		__builtin_trap();
	drv_health_reset(&tracker, report.generation);
	decision = drv_health_accept(&tracker, &report, report.timestamp_ms);
	if(decision != DrvHealthRenew && decision != DrvHealthRestart)
		__builtin_trap();
	if(!tracker.valid || tracker.report.sequence != report.sequence)
		__builtin_trap();
	return 0;
}
