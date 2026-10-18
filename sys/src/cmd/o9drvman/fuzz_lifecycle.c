#include "lifecycle.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint32_t
get32(const uint8_t *data)
{
	return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
		(uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static uint64_t
get64(const uint8_t *data)
{
	return (uint64_t)get32(data) | (uint64_t)get32(data + 4) << 32;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	DrvLifecycle lifecycle;
	DrvLifecycle before;
	DrvPolicy policy;
	DrvEvent event;
	uint32_t oldgeneration;
	size_t offset;

	policy.startup_timeout_ms = 1 + (size > 0 ? data[0] : 10);
	policy.heartbeat_timeout_ms = 1 + (size > 1 ? data[1] : 10);
	policy.base_backoff_ms = 1 + (size > 2 ? data[2] : 1);
	policy.max_backoff_ms = policy.base_backoff_ms +
		(size > 3 ? data[3] : 10);
	policy.max_restarts = size > 4 ? data[4] % 16 : 3;
	if(drv_lifecycle_init(&lifecycle, &policy) < 0)
		__builtin_trap();
	for(offset = 5; offset + 24 <= size; offset += 24){
		before = lifecycle;
		oldgeneration = lifecycle.generation;
		event.type = (DrvEventType)(data[offset] % DrvEventCount);
		event.generation = get32(data + offset + 4);
		event.now_ms = get64(data + offset + 8);
		event.progress = get64(data + offset + 16);
		(void)drv_lifecycle_apply(&lifecycle, &event);
		if(!drv_lifecycle_valid(&lifecycle))
			__builtin_trap();
		if(lifecycle.generation < oldgeneration)
			__builtin_trap();
		if((event.type == DrvReady || event.type == DrvProgress ||
		    event.type == DrvExit || event.type == DrvStoppedEvent) &&
		   event.generation != before.generation &&
		   memcmp(&before, &lifecycle, sizeof(before)) != 0)
			__builtin_trap();
	}
	return 0;
}
