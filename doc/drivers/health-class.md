# DriverHealth class

`DriverHealth` is the shared driver health/readiness/stop implementation. It
is not a filesystem and not a driver. It is linked into every managed driver
and used directly by that driver.

## Why a class

The health protocol was previously implemented inside `o9driverfs`, which
meant only that forwarding service could participate. Every real driver needs
the same record format:

```text
o9health/1 GENERATION SEQUENCE TIMESTAMP_MS STATE PROGRESS INFLIGHT
           COMPLETED ERRORS TIMEOUTS RESETS RECOMMENDATION ERROR_CODE
```

Duplicating that string in every driver is how drift starts. One class owns:

- the record format;
- ready and stop markers;
- generation and sequence rules;
- heartbeat cadence;
- stop waiting.

## API

C core:

```c
typedef struct DrvHealth DrvHealth;

DrvHealth *drvhealth_open(const char *name, uint32_t generation,
                          uint32_t slot, const char *statusdir);
DrvHealth *drvhealth_openenv(void);
int  drvhealth_ready(DrvHealth *);
int  drvhealth_format(char *out, size_t capacity, uint32_t generation,
                      uint64_t sequence, uint64_t timestamp,
                      DrvHealthState state, uint64_t progress,
                      uint64_t inflight, uint64_t completed, uint64_t errors,
                      uint64_t timeouts, uint64_t resets,
                      DrvHealthRecommendation recommendation,
                      const char *error_code);
int  drvhealth_report(DrvHealth *, DrvHealthState state,
                      uint64_t progress, uint64_t inflight,
                      uint64_t completed, uint64_t errors,
                      uint64_t timeouts, uint64_t resets,
                      DrvHealthRecommendation recommendation,
                      const char *error_code);
int  drvhealth_stopwait(DrvHealth *, int milliseconds);
void drvhealth_close(DrvHealth *);
```

`drvhealth_openenv` is the normal entry point. It reads the four
`O9_DRIVER_*` variables and calls `drvhealth_open`. If the variables are
absent, it returns `nil`, so the same driver binary also runs unmanaged.

Objective-9C services bind these functions with a small inline `c { ... }`
block. The record format, markers, and sequence rules live only in the C
core; there is no second implementation in the service.

The C core is the source of truth.

## Manager contract

`o9drvman` launches a driver with:

```text
argv[1] = driver name
argv[2] = generation
argv[3] = slot
argv[4] = status directory
```

Environment:

```text
O9_DRIVER_NAME
O9_DRIVER_GENERATION
O9_DRIVER_SLOT
O9_DRIVER_STATUS
```

The driver calls `drvhealth_openenv()` during startup. If the environment is
absent, it returns `nil` and the driver must not enter the health loop; it
still serves normally.

## Files

```text
<status>/<name>.<generation>          ready marker
<status>/<name>.<generation>.health   o9health/1 records
<status>/<name>.<generation>.stop     shutdown marker
```

The driver writes the ready marker once. It writes health on the first tick
and at the configured cadence. It polls the stop marker on its event loop.

## Rules

- One writer per driver process.
- Generation is fixed for the process lifetime.
- Sequence and cumulative counters never decrease.
- A failed write is not fatal to the driver; the next tick retries.
- `o9drvman` is the only reader.
- The driver’s 9P service is separate and remains the data plane.
- No `o9driverfs`, no forwarding namespace.

## Integration

Every managed driver should:

1. call `drvhealth_openenv` after parsing its own arguments;
2. call `drvhealth_ready` after its `/srv/<name>` endpoint exists;
3. call `drvhealth_report` on its normal event loop;
4. call `drvhealth_stopwait` when idle;
5. call `drvhealth_close` on exit.

The C drivers (`uartfs`, `gpiofs`, `i2cfs`) write readiness and health from
the managed process. They currently publish their 9P endpoint with
`postmountsrv`, so the server itself is a detached child; stop removes the
managed process but not that child yet. Running the server in-process is a
follow-up once the lib9p thread path is validated on the board. `o9xhci` uses
the same writer from its Objective-9C service.

## Non-goals

- No central driver database.
- No health filesystem.
- No MMIO or IRQ handling.
- No class-specific policy; the class reports facts and recommendations, the
  manager decides restart/quiesce/offline.
