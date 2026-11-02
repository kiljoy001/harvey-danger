# Clang on Harvey

The goal is a `clang++` that *runs on Harvey*. That needs three things beyond
the C++ runtime:

1. **libc++** — the standard library, built for APEX. See
   `util/libcxx/README.md` and `util/9vm/build_libcxx.py`.
2. **LLVM/Clang itself, cross-built to the Harvey target.** The compiler
   executes on Harvey; only the build runs on Linux.
3. **A linker** — Harvey binaries are ELF and are linked with `lld` today.
   A native `ld.lld` is the last rung.

This directory holds the cross toolchain that makes step 2 possible, and the
portability shims the Harvey target needs.

## Status

| Piece | State |
| --- | --- |
| libc++ (containers, iostream, locale, RTTI, exceptions, filesystem, threads) | builds and runs in 9vm |
| `LLVMSupport` | builds for the Harvey target |
| all LLVM/Clang libraries | build for the Harvey target |
| native `clang` binary | **runs on Harvey and emits object files** |
| native `ld.lld` | **runs on Harvey and links executables** |
| self-configuring driver | **`clang++ hello.cpp -o hello` works with no flags** |

The end-to-end test is `util/clang-harvey/test-native-toolchain.py`. It
installs the SDK and then the guest runs exactly:

```sh
clang++ hello.cpp -o hello
```

No `-target`, no include paths, no linker invocation. The driver loads
`clang++.cfg` from its own directory, derives its resource directory from
that, runs `cc1`, and invokes the `ld.lld` beside it:

```text
  hello from harvey
native toolchain: clang++ -> cc1 -> ld.lld -> hello (877816 bytes) ran and printed 'hello from harvey'
```

`util/clang-harvey/test-native.py` is the smaller object-only check (the full
driver, including the fork/exec of `cc1`):

```text
native clang produced build/clang-harvey/test.o (712 bytes) with 'add'
```

## SDK layout

`util/clang-harvey/install-sdk.py` builds the guest-visible tree:

```text
build/clang-harvey/
  clang, clang++, ld.lld     the native tools
  clang.cfg, clang++.cfg     driver configuration (loaded automatically)
build/lib/clang/22/include   clang's own resource headers
tmp/                         the guest's /tmp (its root is read-only)
```

The configuration files in `util/clang-harvey/sdk/` carry the whole recipe:
target triple, `-nostdinc` plus the APEX and libc++ include paths, the
compatibility header, `-static -no-pie -nostdlib`, `-fuse-ld=lld`, and the
runtime objects as `$`-prefixed *tail* arguments — options Clang only passes
when linking.

Two details matter:

- The triple is `x86_64-unknown-linux-elf`, not `x86_64-unknown-elf`. Clang's
  generic ELF toolchain links *through gcc*; the Linux one invokes `ld`
  directly, which is what lets `-fuse-ld=lld` pick up `ld.lld`.
- `libap.a` must precede `libc.a`. Both define `malloc`; APEX's is the one
  that works with Harvey's crt0.

## Using the wrappers

```sh
# Compile and link a Harvey program with the host clang.
util/clang-harvey/harvey-clang++ hello.cpp -o build/hello.elf

# Run it under the 9vm microVM.
util/9vm/9vm -boot build/hello.elf build/harvey.32bit
```

`harvey-clang` is the C driver, `harvey-clang++` the C++ one. Both pin the
target triple, the APEX sysroot, libc++, and the Harvey link recipe
(`crt0.o`, `libc++.a`, `libcxxabi.a`, `libap.a`, `libc.a`, `crtn.o`).

Prerequisites: run `util/9vm/build_libcxx.py` once so
`build/libcxx/include/c++/v1` and `build/libcxx/lib/libc++.a` exist.

## Cross-building Clang

Host tools first (tablegen runs on the build machine):

```sh
cmake -G Ninja -S llvm-project/llvm -B /tmp/llvm-host \
    -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=X86 \
    -DLLVM_ENABLE_PROJECTS=clang -DLLVM_INCLUDE_TESTS=OFF
ninja -C /tmp/llvm-host llvm-tblgen clang-tblgen
```

Then configure LLVM for the Harvey target:

```sh
cmake -G Ninja -S llvm-project/llvm -B /tmp/llvm-harvey \
    -DCMAKE_TOOLCHAIN_FILE=$PWD/util/clang-harvey/harvey-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_TARGETS_TO_BUILD=X86 \
    -DLLVM_ENABLE_PROJECTS=clang \
    -DLLVM_ENABLE_THREADS=OFF \
    -DLLVM_ENABLE_EH=OFF -DLLVM_ENABLE_RTTI=OFF \
    -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF \
    -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_CURL=OFF \
    -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
    -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_TABLEGEN=/tmp/llvm-host/bin/llvm-tblgen \
    -DCLANG_TABLEGEN=/tmp/llvm-host/bin/clang-tblgen \
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON

ninja -C /tmp/llvm-harvey clang
```

`LLVM_ENABLE_THREADS=OFF` matters: Harvey has no real pthreads yet (see
`util/libcxx/pthread.h`). `LLVM_ENABLE_EH=OFF` and `LLVM_ENABLE_RTTI=OFF`
match the libc++ build.

Apply `apply-llvm-patches.py` to a fresh LLVM checkout first:

```sh
util/clang-harvey/apply-llvm-patches.py ~/Repo/llvm-project
```

It patches LLVM Support (`stat`/`fstat`, `getMainExecutable`) and libc++'s
filesystem (`fs_stat`/`fs_fstat`) — the latter because `using ::stat;` would
name APEX's macro and `detail::stat(...)` would expand inside the namespace.

## What the target needs that APEX does not have

`target-include/` shadows or supplies headers for the POSIX surface LLVM
expects. Each file documents its own reason:

- `signal.h` — POSIX `sa_handler`/`sa_sigaction`, `siginfo_t`, `stack_t`,
  `SA_*`, `kill()`; APEX models Plan 9 note handlers.
- `sys/resource.h` — `rlimit`, `RLIMIT_*`, `getrlimit`; APEX only has
  `rusage`.
- `poll.h`, `sysexits.h`, `dlfcn.h`, `nl_types.h`, `sys/vfs.h` — types and
  constants for interfaces Harvey does not implement; the calls fail at
  runtime.
- `sys/ioctl.h`, `sys/socket.h` — `#include_next` wrappers that define
  `_BSD_EXTENSION` for just that header.

Runtime pieces live in `util/libcxx/compat.{h,cpp}` and
`util/libcxx/locale_stub.cpp`.

## Known limits

- No dynamic loading (`dlopen` always fails), no sockets in the file system,
  no filesystem statistics, no resource limits, no message catalogues.
- `getrlimit(RLIMIT_STACK)` reports 8 MiB; nothing enforces it.
- The pthread layer is a single-threaded shim.
- `realpath()` does not resolve symbolic links.

## Target support added for the toolchain

Running a compiler and linker on Harvey needed three runtime pieces that APEX
was missing.  They live in APEX now, not in a shim:

- **Environment across `exec`.** `execve()` starts with `rfork(RFCENVG)` and
  then rebuilds `#e` *from `environ`* — but `putenv()` only wrote `/env/NAME`,
  never `environ`, so any variable set with it vanished at the next `exec`.
  `putenv`/`setenv`/`unsetenv` now maintain `environ` (and still mirror into
  `#e`), which is what lets the harness hand `TMPDIR` to the driver.
- **TLS.** APEX never set the FS base, so any `thread_local` access faulted at
  address 0 (lld links with a PT_TLS segment). `__libc_start_main` now reads
  PT_TLS back through `#p/<pid>/text`, copies the `.tdata` initialiser image,
  builds the TCB and publishes the thread pointer through `#p/<pid>/tls`; the
  kernel applies it in `kexit`.
- **Raw mmap syscalls.** `__sys_mmap`/`__sys_munmap`/`__sys_mprotect` called
  the identically named libc functions and recursed until the stack ran out.
  They now issue syscalls 55/56/57 directly.
- **File mappings.** Harvey's demand pager cannot fault pages in from a 9P
  channel, so file mappings are materialised into anonymous memory on `mmap`
  and flushed back on `msync`/`munmap`.

## Next rungs

1. Threads are real: `util/libcxx/pthread.cpp` uses `rfork(RFPROC|RFMEM)`,
   with the main thread moved onto a heap stack and `semacquire`/`semrelease`
   for synchronisation.
2. Make `getMainExecutable` more robust. The patch resolves `argv[0]`, so the
   driver must be started with a path. A real `/proc/self/exe` equivalent, or
   passing the image path through the environment, would fix bare `clang++`.
3. Add a Harvey toolchain to Clang instead of a config file, so the paths are
   baked in and `x86_64-unknown-harvey` is a real target.
4. A native `as`/`objdump` and the rest of the binutils story.
5. `std::filesystem` needs its library built; the headers alone are enabled.
