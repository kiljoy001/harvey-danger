# Minimal libc++ for Harvey/APEX

This is the first rung of the Harvey C++ ladder: a real libc++ header tree and
a small library that is enough for `std::string`, `std::vector`, and
`std::map`. It builds a Harvey ELF and runs under 9vm.

```sh
util/9vm/build_libcxx.py --run
```

The smoke test prints:

```text
hello 2 2 42 42
[init] PID 1 exited cleanly (), shutting down.
libc++ smoke test passed
```

It is deliberately not a full libc++ port yet.

## What the build does

- copies the libc++ headers into `build/libcxx/include/c++/v1` (API version 1,
  namespace `std::__1`),
- generates `__config_site` with threads, exceptions, RTTI, localization,
  wide characters, filesystem, random-device and time-zone support off,
- compiles the subset of libc++ sources needed by the containers:
  `string`, `stdexcept`, `exception`, `typeinfo`, `functional`, `memory`,
  `hash`, `algorithm`, `any`, `optional`, `variant`, `new_handler`,
  `new_helpers`, `verbose_abort`,
- builds `build/libcxx/lib/libc++.a`, including `compat.o`,
- links `util/libcxx/test_stl.cpp` against APEX (`crt0.o`, `libap.a`,
  `libc.a`, `libcxxabi.a`) and runs it with 9vm.

`ucc`'s C++ mode picks the tree up automatically when it exists:

```sh
HARVEY=$HOME/Repo/harvey $HOME/Repo/ucc/c++ util/libcxx/test_stl.cpp \
    -o build/libcxx/test_ucxx.elf
util/9vm/9vm -boot build/libcxx/test_ucxx.elf build/harvey.32bit
```

The `-boot` path must be inside the Harvey checkout because 9vm's hostfs
resolves it relative to the host root.

## APEX compatibility shim

APEX's C headers predate C11 and carry a few Plan 9-isms. `compat.h` and
`compat.cpp` fill the gaps until they move into APEX proper:

- `lldiv_t` / `lldiv`
- `max_align_t`
- `int_fast16_t`, `int_fast32_t`, `uint_fast16_t`, `uint_fast32_t`
- `strtof` / `strtold` (currently via `strtod`, so float precision is
  approximate)
- `#define remove __remove` is undone so `std::ranges::remove` is not a macro
  casualty; `remove()` is re-declared and forwarded to `__remove`
- C++17 aligned `operator new`/`delete` (the APEX `libcxxabi` stub only has
  the unaligned forms)

## Deliberately off for now

Exceptions and RTTI **are** available.

Exception support is libunwind (DWARF, built by `util/9vm/build_libunwind.py`)
plus libc++abi's `__cxa_*` runtime and `__gxx_personality_v0`, compiled into
`libc++.a`.  Unwinding runs destructors, so RAII works:

```text
  unwound through main-guard
caught: boom
```

Harvey has no dynamic loader, so libunwind finds `.eh_frame` through a
`dl_iterate_phdr()` shim (compat.cpp) that reports the single static image
using the ELF header lld exposes as `__ehdr_start`.

RTTI **is** available: libc++ is built with `-frtti`, and
`libcxxabi/src/private_typeinfo.cpp` (compiled into `libc++.a`) provides the
`__class_type_info` hierarchy and `__dynamic_cast`.  `dynamic_cast<Ptr>` and
`typeid` work; the reference form of `dynamic_cast` would need exceptions.
- **Threads** — real.  A pthread is an `rfork(RFPROC|RFMEM)` process, which
  the kernel schedules independently.  `semacquire`/`semrelease` on a word of
  shared memory are Plan 9's equivalent of a futex.

  Three things make it work:

  * RFMEM shares data, bss and heap but **not** the stack, so the main
    thread is moved onto a heap-allocated stack before `main` runs, and
    thread stacks are malloc'd.  Anything a thread can name is then shared.
  * The child leaves the parent's stack inside the assembly `rfork` wrapper,
    before any C code can touch it.
  * Each thread gets its own TLS block: `.tdata` is copied, `.tbss` is
    zeroed, and the thread pointer is published through `#p/<pid>/tls`.

  `mmap` comes from a single arena reserved at startup.  `rfork` copies the
  segment table, so a mapping created *after* a thread starts would not exist
  in that thread -- lld hits this, because its thread pool is created before
  the output file buffer is mapped.  One pre-shared arena keeps every thread
  looking at the same mappings.  It sits below the kernel's exec area
  (`NIXCALL`), because `sysexec` builds the argument vector on a temporary
  stack there.

  LLVM, Clang and lld all build with `LLVM_ENABLE_THREADS=ON`; the resulting
  toolchain compiles and links in the guest, and lld links correctly with
  `--threads=2/4/8`.  `std::thread`, `std::mutex`, `std::condition_variable`,
  `std::async`/`std::future` and `thread_local` all work.

  Not yet per-thread: `errno` is still a single global, and stdio has no
  locking.
- **std::filesystem** — supported.  libc++'s own sources are patched to call
  `__stat_ap`/`__fstat` directly (`apply-llvm-patches.py`), because defining
  real `stat`/`fstat` symbols would capture Plan 9's three-argument `stat`
  and recurse.  The `*at()` family is implemented over `fd2path` plus a small
  fd-to-path table, and compiler-rt's 128-bit division builtins are compiled
  in for the filesystem clock.
- **locale data** — Harvey has one C locale; `locale_stub.cpp` implements the
  POSIX `*_l` entry points libc++ calls, plus `newlocale`/`uselocale`.
- **libm** — APEX ships almost no math library, so `libm_stub.cpp` supplies
  the C99 functions LLVM needs, using x87 instructions.
- **`__cxa_thread_atexit`** — missing from APEX's libcxxabi stub; compat.cpp
  registers the destructor per thread and runs it at process exit.

- **`std::exception_ptr`** — exception.cpp is compiled in libc++abi mode
  (`LIBCXX_BUILDING_LIBCXXABI`) so the `__cxa_*` implementation is used
  rather than libc++'s "not yet implemented" fallback.  That pulls in
  libc++abi's `stdlib_exception`, `cxa_handlers` and `cxa_default_handlers`;
  `bad_cast`/`bad_typeid` are defined in compat.cpp to avoid a second
  `std::type_info` destructor.

- **`__cxa_demangle`** — stubbed to fail; diagnostics print mangled names.
- **charconv** — this libc++ snapshot's `src/include/shared/fp_bits.h` is
  missing, so `charconv.cpp` is skipped. `std::to_string` and the integer
  paths used by `<string>` still work.

## Next rungs

1. Per-thread `errno` and locking around stdio.  Both change the ABI of every
   object that touches them, so they need a full rebuild of the library and
   of anything compiled against it.
2. Floating-point `charconv`.
3. `std::exception_ptr`/`std::nested_exception` round-trip tests, and
   `-fno-exceptions` builds of the library for size.
