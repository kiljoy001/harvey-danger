#ifndef O9DRVMAN_LIFECYCLE_H
#define O9DRVMAN_LIFECYCLE_H

#include <stdint.h>

typedef enum DrvState DrvState;
typedef enum DrvEventType DrvEventType;
typedef enum DrvResult DrvResult;
typedef struct DrvPolicy DrvPolicy;
typedef struct DrvEvent DrvEvent;
typedef struct DrvLifecycle DrvLifecycle;

enum DrvState {
	DrvStopped,
	DrvStarting,
	DrvRunning,
	DrvStopping,
	DrvBackoff,
	DrvFailed,
	DrvStateCount,
};

enum DrvEventType {
	DrvStart,
	DrvReady,
	DrvProgress,
	DrvExit,
	DrvStop,
	DrvStoppedEvent,
	DrvTick,
	DrvReset,
	DrvEventCount,
};

enum DrvResult {
	DrvRejected = -1,
	DrvApplied,
	DrvIgnored,
};

struct DrvPolicy {
	uint64_t startup_timeout_ms;
	uint64_t heartbeat_timeout_ms;
	uint64_t base_backoff_ms;
	uint64_t max_backoff_ms;
	uint32_t max_restarts;
};

struct DrvEvent {
	DrvEventType type;
	uint32_t generation;
	uint64_t now_ms;
	uint64_t progress;
};

struct DrvLifecycle {
	DrvPolicy policy;
	DrvState state;
	uint32_t generation;
	uint32_t restarts;
	uint8_t desired_running;
	uint64_t deadline_ms;
	uint64_t retry_at_ms;
	uint64_t last_progress;
};

int drv_lifecycle_init(DrvLifecycle *, const DrvPolicy *);
DrvResult drv_lifecycle_apply(DrvLifecycle *, const DrvEvent *);
int drv_lifecycle_valid(const DrvLifecycle *);
uint64_t drv_lifecycle_handle(const DrvLifecycle *, uint32_t);
int drv_lifecycle_handle_valid(const DrvLifecycle *, uint64_t);
const char *drv_state_name(DrvState);

#endif
