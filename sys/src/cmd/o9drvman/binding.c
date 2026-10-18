#include <u.h>
#include <libc.h>

#include "lifecycle.h"

enum {
	Maxdrivers = 64,
};

static Lock driverlock;
static DrvLifecycle drivers[Maxdrivers];
static uchar initialized[Maxdrivers];

int
o9drvmaninit(int slot)
{
	DrvPolicy policy;
	int result;

	if(slot < 0 || slot >= Maxdrivers)
		return -1;
	policy.startup_timeout_ms = 5000;
	policy.heartbeat_timeout_ms = 30000;
	policy.base_backoff_ms = 250;
	policy.max_backoff_ms = 30000;
	policy.max_restarts = 5;
	lock(&driverlock);
	result = drv_lifecycle_init(&drivers[slot], &policy);
	if(result == 0)
		initialized[slot] = 1;
	unlock(&driverlock);
	return result;
}

int
o9drvmanapply(int slot, int type, uvlong generation, uvlong now, uvlong progress)
{
	DrvEvent event;
	int result;

	if(slot < 0 || slot >= Maxdrivers || !initialized[slot])
		return DrvRejected;
	event.type = (DrvEventType)type;
	event.generation = (uint32_t)generation;
	event.now_ms = now;
	event.progress = progress;
	lock(&driverlock);
	result = drv_lifecycle_apply(&drivers[slot], &event);
	unlock(&driverlock);
	return result;
}

vlong
o9drvmanfield(int slot, int field)
{
	DrvLifecycle *driver;
	vlong result;

	if(slot < 0 || slot >= Maxdrivers || !initialized[slot])
		return -1;
	lock(&driverlock);
	driver = &drivers[slot];
	switch(field){
	case 0:
		result = driver->state;
		break;
	case 1:
		result = driver->generation;
		break;
	case 2:
		result = driver->restarts;
		break;
	case 3:
		result = driver->desired_running;
		break;
	case 4:
		result = driver->deadline_ms;
		break;
	case 5:
		result = driver->retry_at_ms;
		break;
	case 6:
		result = driver->last_progress;
		break;
	default:
		result = -1;
		break;
	}
	unlock(&driverlock);
	return result;
}

char *
o9drvmanstate(int slot)
{
	char *result;

	if(slot < 0 || slot >= Maxdrivers || !initialized[slot])
		return "invalid";
	lock(&driverlock);
	result = (char *)drv_state_name(drivers[slot].state);
	unlock(&driverlock);
	return result;
}
