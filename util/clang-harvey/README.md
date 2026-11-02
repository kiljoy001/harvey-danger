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
| libc++ (containers, iostream, locale, threads shim) | builds and runs in 9vm |
| `LLVMSupport` | builds for the Harvey target |
| all LLVM/Clang libraries | build for the Harvey target |
| native `clang` binary | **runs on Harvey and emits object files** |
| native `ld.lld` | not yet |

The milestone test is `util/clang-harvey/test-native.py`: it boots the
cross-built `clang` under 9vm, compiles `test-native-clang.c` with `-cc1
-emit-obj`, and checks the resulting ELF object on the host:

```text
native clang produced build/clang-harvey/test.o (712 bytes) with 'add'
```

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

## Next rungs

1. Get the Clang libraries and the `clang` binary linking.
2. Run `clang -cc1` on Harvey and produce an object file.
3. Port `ld.lld` (or drive a Harvey-side linker) so `clang++ foo.cpp` can
   produce an executable without a Linux host.
