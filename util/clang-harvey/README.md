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
| LLVM cross-built with `LLVM_ENABLE_THREADS=ON` | **builds and runs: threaded clang + lld link in the guest** |
| building LLVM on Harvey | **`test-selfhost.py` compiles `CommandLine.cpp`; `test-selfbuild.py` builds `llvm-tblgen`; `test-selfcore.py` builds `LLVMCore`; `test-selfas.py` builds `llvm-as` and `llvm-dis`** |
| correctness against a reference compiler | **`test-diff.py`: 12 `-O3` Support/IR TUs compile byte-identically to a host clang from the same checkout** |

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

## Building LLVM on Harvey

`util/clang-harvey/test-selfhost.py` is the first self-hosting probe.  The
guest compiles a real LLVM translation unit with its own compiler:

```text
  == compile llvm/lib/Support/CommandLine.cpp on Harvey
  == CommandLine.cpp status 0
  == compile cl.cpp on Harvey
  == link with the prebuilt LLVMSupport
  llvm-on-harvey: count=7 name=harvey
```

It derives the compile flags from the cross build's own ninja graph, rewrites
the host paths to the guest's `/n/host`, and lets the guest compile the file.
The test then links the object and a driver against the prebuilt
`libLLVMSupport.a` and runs it.  The result parses options with
`llvm::cl::opt`, so the compiled object really is LLVM code, not a stub.

### Building TableGen in the guest

`util/clang-harvey/test-selfbuild.py` builds a real LLVM tool inside
Harvey.  It takes the cross build's own command graph for `llvm-tblgen` (270
translation units, four archives, one link), rewrites the tool and every
path for the guest, and runs it there with `hbuild`.

`hbuild` is a small native runner: a manifest of commands with `---`
between stages, up to `-j` children per stage, no shell and no dependency
graph.  LLVM's static-library + tool shape is exactly compiles → archives →
link.

```text
  == hbuild llvm-tblgen.build
  hbuild: stage done: 270/270 (0 failed)
  hbuild: stage done: 4/4 (0 failed)
  hbuild: stage done: 1/1 (0 failed)
  hbuild: stage done: 1/1 (0 failed)
  == hbuild status 0
  LLVM (http://llvm.org/):
    LLVM version 22.0.0git
```

The last stage uses the tool it just built: it runs `llvm-tblgen -gen-vt`
on `ValueTypes.td`, and the resulting `GenVT.inc` is byte-identical to the
file the host tablegen produced from the same source.

### Building a real library: LLVMCore

`util/clang-harvey/test-selfcore.py` goes one rung further, using the guest
`llvm-tblgen` from the previous step.  It mirrors the build's generated
headers into `build/llvm-guest`, runs all 26 tablegen commands there with
the guest tool, compiles 121 IR/BinaryFormat/Bitstream/TargetParser/Remarks
translation units against those headers, archives seven libraries, and then
links a program against `libLLVMCore.a`:

```text
  hbuild: stage done: 26/26 (0 failed)
  hbuild: stage done: 121/121 (0 failed)
  hbuild: stage done: 7/7 (0 failed)
  == LLVMCore status 0
  ; ModuleID = 'harvey'
  source_filename = "harvey"
  define i32 @answer() {
  entry:
    ret i32 42
  }
```

The program builds that module with `LLVMContext`, `Module`, `Function` and
`IRBuilder`, so the library is doing real work, not just linking.  One
generated file (`RuntimeLibcalls.inc`) differs from the host build only in
the order of two independent tables; the rest of the generated tree is
byte-identical.

Run `test-selfbuild.py` first: it produces the guest `bin/llvm-tblgen`.
`test-selfcore.py --clean` rebuilds the objects from scratch.

### A real tool: llvm-as

`util/clang-harvey/test-selfas.py` builds the bitcode assembler on top of
Core.  It is the largest guest build so far: 26 header-generation commands,
490 compilation units (AsmParser, BitWriter, MC, Symbolize, Core, Support),
26 archives and the link.  Then it assembles a module:

```text
  hbuild: batch done: 490/490 (0 failed)
  == llvm-as build status 0
  == llvm-as run status 0
  hbuild: batch done: 1/1 (0 failed)      # llvm-dis; the rest was built
  hbuild: batch done: 8/8 (0 failed)
  == llvm-dis run status 0
selfas: llvm-as (20808096 bytes) and llvm-dis (9354704 bytes) built on
        Harvey; test.ll -> test.bc (1536 bytes) -> roundtrip.ll
```

The disassembled module comes back intact:

```text
; ModuleID = 'test.bc'
source_filename = "test.ll"

define i32 @answer() {
entry:
  ret i32 42
}
```

`hbuild` retries a failed command once.  A parallel burst produced a single
transient 9P output-open failure among those 490 objects, and the retry
picked it up without hiding real compile errors.

The four BLAKE3 assembly objects are reused from the cross build: the
guest driver's config force-includes `compat.h`, which the assembler cannot
parse, and they were already compiled for the same target.

The build runs with `-j 4`.  The first parallel attempts failed because the
host 9P server did not record the new fid's path on a *partial* walk: a
build leaked about four fids per translation unit until the 1024-entry
table filled, after which every walk failed and the guest saw `EINVAL`.
With that fixed, fid use stays flat and the whole target builds clean at
`-j 4`.

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
- Threads are real (`rfork(RFPROC|RFMEM)`), `errno` is per thread, and stdio
  is locked per `FILE`.
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

## Is it correct?

The smoke tests prove the pieces work.  Two tests go further and compare the
guest against a known-good copy of the same compiler:

`util/clang-harvey/test-diff.py` builds a reference clang from the same
checkout, for the host, and compiles the same translation units with both.
The flags are not guessed: they come from the real ninja graph of the
Harvey-target build.  Output paths and dependency files are removed, the path
roots are normalised with `-ffile-prefix-map` (because the guest sees the
tree as `/n/host/...`), and `-frandom-seed` is pinned because its default is
derived from the output file name.  The first translation unit is built twice
on each side as a determinism check.

```text
+ compiling 13 objects with the host reference
+ compiling the same objects in the guest
  hbuild: batch done: 13/13 (0 failed)
differential test: 13 objects compile identically on the host reference and
on Harvey (12 translation units, one built twice on each side)
```

That covers APInt, APFloat, StringRef, SmallVector, Twine, StringMap, Error,
CommandLine, MemoryBuffer, Attributes, Module and Instructions at `-O3`: the
object files are byte-for-byte the same, so the guest compiler's codegen, its
preprocessor and its target ABI agree with the reference.

The system `clang-22` is a *different revision* and is not a valid oracle for
this comparison.

## Next rungs

1. More vCPUs.  `9vm` runs one KVM vCPU, so a parallel build is still one
   stream of work; adding vCPUs is a 9vm change.
2. More of LLVM: `llvm-config`, then the target libraries and code
   generator, on top of Core and AsmParser.
3. Differential *execution*: run the same programs built by both compilers
   and compare their output, which tests the runtime (libc++, `libm`,
   unwinding, stdio) rather than codegen.
4. A native `as`/`objdump` and the rest of the binutils story.

`-m` above about 4078 MB fails with `KVM_CREATE_VCPU: EEXIST`: the RAM
memslot then covers the in-kernel local APIC page at `0xFEE00000`.  3 GiB
is the reliable setting for builds.
