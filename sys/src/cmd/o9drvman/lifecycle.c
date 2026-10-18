#include "lifecycle.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static uint64_t
addsat(uint64_t a, uint64_t b)
{
	if(UINT64_MAX - a < b)
		return UINT64_MAX;
	return a + b;
}

static uint64_t
backoff(const DrvLifecycle *lifecycle)
{
	uint64_t delay;
	uint32_t i;

	delay = lifecycle->policy.base_backoff_ms;
	for(i = 1; i < lifecycle->restarts; i++){
		if(delay >= lifecycle->policy.max_backoff_ms / 2){
			delay = lifecycle->policy.max_backoff_ms;
			break;
		}
		delay *= 2;
	}
	if(delay > lifecycle->policy.max_backoff_ms)
		delay = lifecycle->policy.max_backoff_ms;
	return delay;
}

static DrvResult
begin(DrvLifecycle *lifecycle, uint64_t now)
{
	if(lifecycle->generation == UINT32_MAX){
		lifecycle->state = DrvFailed;
		lifecycle->desired_running = 0;
		lifecycle->deadline_ms = 0;
		lifecycle->retry_at_ms = 0;
		return DrvRejected;
	}
	lifecycle->generation++;
	lifecycle->state = DrvStarting;
	lifecycle->deadline_ms = addsat(now, lifecycle->policy.startup_timeout_ms);
	lifecycle->retry_at_ms = 0;
	lifecycle->last_progress = 0;
	return DrvApplied;
}

static DrvResult
failed(DrvLifecycle *lifecycle, uint64_t now)
{
	lifecycle->deadline_ms = 0;
	lifecycle->last_progress = 0;
	if(!lifecycle->desired_running){
		lifecycle->state = DrvStopped;
		lifecycle->retry_at_ms = 0;
		return DrvApplied;
	}
	if(lifecycle->restarts >= lifecycle->policy.max_restarts){
		lifecycle->state = DrvFailed;
		lifecycle->desired_running = 0;
		lifecycle->retry_at_ms = 0;
		return DrvApplied;
	}
	lifecycle->restarts++;
	lifecycle->state = DrvBackoff;
	lifecycle->retry_at_ms = addsat(now, backoff(lifecycle));
	return DrvApplied;
}

static int
generationevent(DrvEventType type)
{
	return type == DrvReady || type == DrvProgress || type == DrvExit ||
		type == DrvStoppedEvent;
}

int
drv_lifecycle_init(DrvLifecycle *lifecycle, const DrvPolicy *policy)
{
	if(lifecycle == NULL || policy == NULL)
		return -1;
	if(policy->startup_timeout_ms == 0 || policy->heartbeat_timeout_ms == 0 ||
	   policy->base_backoff_ms == 0 || policy->max_backoff_ms == 0 ||
	   policy->base_backoff_ms > policy->max_backoff_ms)
		return -1;
	memset(lifecycle, 0, sizeof(*lifecycle));
	lifecycle->policy = *policy;
	lifecycle->state = DrvStopped;
	return 0;
}

DrvResult
drv_lifecycle_apply(DrvLifecycle *lifecycle, const DrvEvent *event)
{
	if(lifecycle == NULL || event == NULL || !drv_lifecycle_valid(lifecycle) ||
	   event->type < 0 || event->type >= DrvEventCount)
		return DrvRejected;
	if(generationevent(event->type) &&
	   event->generation != lifecycle->generation)
		return DrvIgnored;

	switch(event->type){
	case DrvStart:
		if(lifecycle->state == DrvFailed)
			return DrvIgnored;
		lifecycle->desired_running = 1;
		if(lifecycle->state == DrvStopped)
			return begin(lifecycle, event->now_ms);
		if(lifecycle->state == DrvBackoff &&
		   event->now_ms >= lifecycle->retry_at_ms)
			return begin(lifecycle, event->now_ms);
		return DrvIgnored;
	case DrvReady:
		if(lifecycle->state != DrvStarting)
			return DrvIgnored;
		lifecycle->state = DrvRunning;
		lifecycle->deadline_ms = addsat(event->now_ms,
			lifecycle->policy.heartbeat_timeout_ms);
		return DrvApplied;
	case DrvProgress:
		if(lifecycle->state != DrvRunning ||
		   event->progress <= lifecycle->last_progress)
			return DrvIgnored;
		lifecycle->last_progress = event->progress;
		lifecycle->deadline_ms = addsat(event->now_ms,
			lifecycle->policy.heartbeat_timeout_ms);
		return DrvApplied;
	case DrvExit:
		if(lifecycle->state != DrvStarting && lifecycle->state != DrvRunning &&
		   lifecycle->state != DrvStopping)
			return DrvIgnored;
		return failed(lifecycle, event->now_ms);
	case DrvStop:
		lifecycle->desired_running = 0;
		if(lifecycle->state == DrvStarting || lifecycle->state == DrvRunning){
			lifecycle->state = DrvStopping;
			lifecycle->deadline_ms = addsat(event->now_ms,
				lifecycle->policy.startup_timeout_ms);
			return DrvApplied;
		}
		if(lifecycle->state == DrvBackoff || lifecycle->state == DrvFailed){
			lifecycle->state = DrvStopped;
			lifecycle->deadline_ms = 0;
			lifecycle->retry_at_ms = 0;
			lifecycle->restarts = 0;
			return DrvApplied;
		}
		return DrvIgnored;
	case DrvStoppedEvent:
		if(lifecycle->state != DrvStopping)
			return DrvIgnored;
		lifecycle->state = DrvStopped;
		lifecycle->deadline_ms = 0;
		lifecycle->retry_at_ms = 0;
		lifecycle->restarts = 0;
		if(lifecycle->desired_running)
			return begin(lifecycle, event->now_ms);
		return DrvApplied;
	case DrvTick:
		if((lifecycle->state == DrvStarting ||
		    lifecycle->state == DrvRunning ||
		    lifecycle->state == DrvStopping) &&
		   event->now_ms >= lifecycle->deadline_ms)
			return failed(lifecycle, event->now_ms);
		if(lifecycle->state == DrvBackoff && lifecycle->desired_running &&
		   event->now_ms >= lifecycle->retry_at_ms)
			return begin(lifecycle, event->now_ms);
		return DrvIgnored;
	case DrvReset:
		if(lifecycle->state != DrvFailed)
			return DrvIgnored;
		lifecycle->state = DrvStopped;
		lifecycle->restarts = 0;
		lifecycle->desired_running = 0;
		lifecycle->deadline_ms = 0;
		lifecycle->retry_at_ms = 0;
		lifecycle->last_progress = 0;
		return DrvApplied;
	case DrvEventCount:
		break;
	}
	return DrvRejected;
}

int
drv_lifecycle_valid(const DrvLifecycle *lifecycle)
{
	if(lifecycle == NULL || lifecycle->state < 0 ||
	   lifecycle->state >= DrvStateCount)
		return 0;
	if(lifecycle->policy.startup_timeout_ms == 0 ||
	   lifecycle->policy.heartbeat_timeout_ms == 0 ||
	   lifecycle->policy.base_backoff_ms == 0 ||
	   lifecycle->policy.max_backoff_ms == 0 ||
	   lifecycle->policy.base_backoff_ms > lifecycle->policy.max_backoff_ms)
		return 0;
	if(lifecycle->restarts > lifecycle->policy.max_restarts)
		return 0;
	if(lifecycle->state == DrvFailed && lifecycle->desired_running)
		return 0;
	if(lifecycle->state == DrvBackoff && lifecycle->retry_at_ms == 0)
		return 0;
	if((lifecycle->state == DrvStarting || lifecycle->state == DrvRunning ||
	    lifecycle->state == DrvStopping) && lifecycle->generation == 0)
		return 0;
	return 1;
}

uint64_t
drv_lifecycle_handle(const DrvLifecycle *lifecycle, uint32_t slot)
{
	if(lifecycle == NULL || lifecycle->generation == 0 || slot == 0)
		return 0;
	return ((uint64_t)lifecycle->generation << 32) | slot;
}

int
drv_lifecycle_handle_valid(const DrvLifecycle *lifecycle, uint64_t handle)
{
	uint32_t generation;
	uint32_t slot;

	if(lifecycle == NULL)
		return 0;
	generation = (uint32_t)(handle >> 32);
	slot = (uint32_t)handle;
	return generation == lifecycle->generation && generation != 0 && slot != 0;
}

const char *
drv_state_name(DrvState state)
{
	static const char *names[] = {
		"stopped", "starting", "running", "stopping", "backoff", "failed",
	};

	if(state < 0 || state >= DrvStateCount)
		return "invalid";
	return names[state];
}
