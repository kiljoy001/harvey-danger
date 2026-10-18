# Aspera native quality gate

Run the mandatory native C checks from the repository root:

```
./bin/aspera quality
```

The gate currently owns the `o9drvman` lifecycle core and runs five distinct
test layers:

* deterministic unit tests for lifecycle transitions and generation handles;
* deterministic generated event sequences that check state invariants;
* AddressSanitizer and UndefinedBehaviorSanitizer over both suites;
* coverage guided libFuzzer input over arbitrary lifecycle event streams;
* semantic source mutations for stale generations, restart limits, and exact
  timeout boundaries. Every seeded mutant must be detected by the tests.

`FUZZ_RUNS` changes the bounded fuzzing pass length. CI and local runs default
to 20,000 inputs so the gate remains quick. Longer campaigns can reuse the
same harness directly from `build/quality/o9drvman/fuzz_lifecycle`.

Every new portable C policy core must enter this gate with all five layers.
Hardware integration tests remain separate because they require KVM or a
physical board. The current GEFS compatibility bridge is explicitly outside
this milestone and will be replaced by a proper port later.
