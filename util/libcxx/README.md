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
  shared memory provide mutexes and condition variables (Plan 9's futex).

  RFMEM shares the data, bss and heap segments but **not** the stack, so the
  main thread is moved onto a heap-allocated stack before `main` runs, and
  thread stacks come from the heap too.  The child switches to its stack
  inside the assembly `rfork` wrapper, before any C code can touch the
  shared stack.  APEX's allocator is serialised with a semaphore.

  Not yet per-thread: `errno` is still a single global, and stdio has no
  locking.

  The layer is complete enough that `LLVMSupport` builds with
  `LLVM_ENABLE_THREADS=ON`: `pthread_sigmask`, CPU affinity (a single-CPU
  stub) and `MINSIGSTKSZ` are provided for LLVM's threading and signal
  code.
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
  registers the destructor and runs it at process exit (there is one thread).
- **charconv** — this libc++ snapshot's `src/include/shared/fp_bits.h` is
  missing, so `charconv.cpp` is skipped. `std::to_string` and the integer
  paths used by `<string>` still work.

## Next rungs

1. Per-thread `errno` and locking around stdio.
2. Floating-point `charconv`.
3. `std::exception_ptr`/`std::nested_exception` round-trip tests, and
   `-fno-exceptions` builds of the library for size.


1. `pthread` / `std::thread` / `mutex` / `condition_variable`.
2. Real `libc++abi` + `libunwind`, then enable `-fexceptions` and `-frtti`.
3. `iostream`/locale, `charconv`, and `std::filesystem`.
4. Boost.Filesystem/Thread/ProgramOptions, then LevelDB.
5. Emercoin's configure/build, once the dependency ladder is real.

The compiler is still the **cross** path: `ucc` drives host `clang++` with the
APEX sysroot. A native C++ compiler on Harvey is a separate project.
