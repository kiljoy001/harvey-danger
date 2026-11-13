# V8 on Harvey/ARM64

This directory holds the Harvey-side build glue for V8.  The V8 source itself
is not vendored here.

## Checkout

Use depot_tools and a V8 16.x checkout:

```sh
git clone https://chromium.googlesource.com/chromium/tools/depot_tools
export PATH=$PWD/depot_tools:$PATH
fetch --nohistory v8
```

The current work was done against V8 `16.0.0` (`54cd00b9`).

## Build

```sh
python3 util/v8/build-v8.py --v8 ~/Repo/v8
```

The script installs `harvey/BUILD.gn` and the patches in `patches/`, writes
`out/harvey/args.gn`, and runs `gn gen` plus `ninja d8`.

V8 is configured as:

- `v8_enable_lite_mode=true` (`V8_JITLESS`; no Sparkplug/Maglev/Turbofan)
- no WebAssembly
- no i18n/ICU
- no sandbox, no pointer compression and no cppgc 4 GiB caged heap
- static, non-component build
- host tools use V8's bundled libc++; the target uses APEX + the Harvey libc++

## Harvey APEX additions

The first cross build needed these POSIX/Linux compatibility pieces, all in
APEX:

- `pthread_getattr_np`, `pthread_attr_getstack`, `pthread_getschedparam`,
  `pthread_kill`, and a real main-thread stack registration.
- A futex implementation on top of Plan 9 semaphores (`semacquire` /
  `semrelease` / `tsemacquire`), used by abseil synchronization.
- `syscall()` for `read`, `write`, `getpid`, `gettid`, `mmap`, `munmap`,
  `getrandom` and `futex`.
- `getrandom`, `getauxval` (synthetic FP/ASIMD), `prctl`, `madvise`,
  `mincore`, `mremap`, `sysinfo`, `getpriority`/`setpriority`, `backtrace`
  stubs, and `malloc_usable_size`.
- Minimal `linux/auxvec.h`, `linux/futex.h`, `linux/random.h`,
  `asm/hwcap.h`, `sys/auxv.h`, `sys/prctl.h`, `sys/syscall.h`,
  `sys/sysmacros.h`, `sys/sysinfo.h`, `sys/ucontext.h`, `ucontext.h`,
  `netinet/ip.h`, `fenv.h`.
- `struct tm.tm_gmtoff`/`tm_zone`, `suseconds_t`/`useconds_t`, LP64
  `uint64_t`/`int64_t` (to match glibc and clang's ACLE headers), and
  guarded `nil`/`PTHREAD_*` macros.
- POSIX `send`/`recv`/`sendto`/`recvfrom` signatures.

The host compiler driver now clears `CPATH`/`C_INCLUDE_PATH`/
`CPLUS_INCLUDE_PATH`/`OBJC_INCLUDE_PATH`, so host headers cannot shadow the
APEX target headers.

## Port patches

`patches/absl-harvey.patch` selects abseil's POSIX backends for futex,
ELF symbolization and address-readability checks when `__harvey__` is
defined.  `patches/v8-simd-harvey.patch` suppresses V8's SVE search path,
which clang 23 cannot compile (the NEON path is used instead).

## Status

Host V8 builds and runs (`out/host/d8 -e 'print(1+2)'` → `3`).  The Harvey
cross build compiles `libv8_libbase.a`, the full target object set and
links a 22 MiB static `d8`.  V8's `-fstack-protector` is overridden in the
cross driver, since Harvey has no libssp runtime.

Verified on QEMU virt (2048 MiB, SMP 2):

```text
$ python3 util/v8/test-v8.py --v8 ~/Repo/v8
D8-OK 3
V8 d8 PASS
```

`d8` is running the snapshot, parsing and executing JavaScript in jitless
mode.

## VM limitations still visible to V8

Harvey's kernel manages mappings at segment granularity, while V8's page
allocator uses sub-range operations:

- partial `munmap` of a reservation,
- partial `mprotect` (V8 decommits individual pages of the read-only space),
- `MAP_FIXED` replacement (POSIX allows it; Harvey rejects overlaps).

The bring-up patches in `patches/v8-readonly-harvey.patch` make the
read-only flag/snapshot protections and no-access decommits advisory, and
the kernel currently accepts partial `munmap` without splitting the
segment.  The proper fix is to split segments for sub-range protections;
the workaround is recorded rather than hidden in APEX.

The ARM64 kernel enables `CNTKCTL_EL1.EL0PCTEN|EL0VCTEN`, so V8's
`CNTVCT_EL0` reads do not trap.  `rk`/image paging may print
`racing on demand load` when several V8 threads fault the same page; it is
a diagnostic, not a failure.
