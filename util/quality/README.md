# Aspera native quality gate

Run the mandatory native C checks from the repository root:

```
./bin/aspera quality
```

The gate owns the `o9drvman` lifecycle core plus the shared kernel transport
primitives used by `dev9ring` and `devvirtio9p`. It runs six distinct test
layers:

* deterministic unit tests for lifecycle transitions and generation handles;
* deterministic generated event sequences that check state invariants;
* AddressSanitizer and UndefinedBehaviorSanitizer over both suites;
* coverage guided libFuzzer input over arbitrary lifecycle event streams;
* per-function CRAP measurement from LLVM decision complexity and branch
  outcome coverage;
* semantic source mutations for stale generations, restart limits, and exact
  timeout boundaries, plus ring full/empty behavior, 9P wire lengths, queue
  boundaries, and descriptor cycles. Every seeded mutant must be detected by
  the tests.

The kernel transport suite additionally runs a four-thread SPSC stress test
under ThreadSanitizer. VM verification then exercises `#~` through shared and
local rings, the amd64 VirtIO fallback, and ARM64 VirtIO-MMIO versions 1 and 2.

CRAP uses `complexity^2 * (1 - coverage)^3 + complexity`. The required gate
rejects scores above 50 and writes the complete machine-readable report to
`build/quality/o9drvman/coverage/crap.json`. The report covers the lifecycle
policy and the host-testable production cores shared with both kernel drivers.

`FUZZ_RUNS` changes the bounded fuzzing pass length. CI and local runs default
to 20,000 inputs so the gate remains quick. Longer campaigns can reuse the
same harness directly from `build/quality/o9drvman/fuzz_lifecycle`.

Every new portable C policy core must enter this gate with every layer above.
Kernel integration remains in `bin/aspera verify`: amd64 boots test shared
9ring and VirtIO fallback, while ARM64 QEMU tests VirtIO-MMIO versions 1 and
2. Physical-board tests remain separate. The current GEFS compatibility
bridge is explicitly outside this milestone and will be replaced by a proper
port later.
