# Harvey ARM64 bring-up

The ARM64 Image boots directly on QEMU `virt`. It enters Harvey's ARM64
assembly and C, clears BSS, sets a stack, parses the device tree to discover
RAM, the chosen PL011 or DesignWare UART, GICv2 or GICv3, and the timer PPI. A monotonic early allocator excludes the
kernel image, DTB, firmware memory reservations and `/reserved-memory` `reg`
ranges. An identity map enables the MMU and caches with normal attributes for
RAM and device attributes for MMIO. The guest then receives three virtual timer
interrupts through an EL1 vector and reports synchronous exceptions over
serial. It enters EL0, resumes after a test syscall, and switches between two
page-table roots whose user mappings point to different physical pages. The
boot check confirms that EL0 cannot read kernel memory, write its code page,
or execute its stack. A timer interrupt can arrive from EL0 and return to the
interrupted instruction. The full kernel starts the shared Plan 9 scheduler,
creates a `Proc` with its own page-table root, and runs a small libc-backed C
program at EL0. Its AArch64 ELF is validated and loaded from its
`PT_LOAD` records into separate RX text, RW data, and NX stack mappings, with a
device-tree-selected UART page. The program enters through the ELF entry and ARM64 libc `_main`,
calls the generated `nsec` wrapper through the normal syscall table, and writes
its result marker through the UART mapping. A normally segmented bootstrap
then executes the filesystem-backed `/boot/init`. The ARM64 `Label` save/restore routines match the calling
convention used by Harvey's shared scheduler in `sys/src/9/port/proc.c`. QEMU
tests switch between two independent kernel stacks and check callee-saved
register preservation while the timer is active. Interrupt masking is also
checked across two timer periods.

`build_kernel.py` now links the shared core port sources, the local `#~` ring
transport, the `#9` virtio-MMIO 9P transport, ARM64 architecture sources, and
ARM64 builds of kernel libc, libip, and libsec. Hardware drivers and the kernel
IP stack are excluded. The output is an AArch64 ELF and direct-boot
Image. After the boot diagnostics, the full Image initializes process and image
allocation, resets the device tables, primes the page allocator, and starts
Harvey's shared scheduler with two kernel processes. It drives the shared
clock from the ARM generic timer, walks into the root device's `dev`
directory, and exchanges a tagged 9P request and response over a named `#~`
connection. With a virtio-9P device present, it also negotiates 9P over `#9/0`.
There is no `#t/eia0` UART device in the kernel. Early boot and
fault diagnostics write to the UART selected by `/chosen/stdout-path` before user space is
available; this temporary bootstrap console is not a serial service.
It does **not** yet offer an interactive shell.

### APEX on ARM64

`util/arm64/build_apex.py` cross-builds the APEX startup layer for ARM64:
crt0, `__libc_start_main`, the mmap arena and the raw syscall wrappers,
linked against the ARM64 Plan 9 libc.  `util/arm64/test_apex_boot.py`
substitutes the resulting binary for the init image in the generated
userland header and boots it in QEMU virt, so the first EL0 program the
kernel execs is the APEX one:

```text
apex-hello argc=1 tls=11
TLS-OK
```

That exercises the entry convention (argc/argv/Tos in registers), the heap
stack switch, the mapping arena, and variant-I TLS end to end: the startup
builds the block, publishes it through `#p/<pid>/tls`, the kernel loads
`TPIDR_EL0`, and the program reads its initialised `__thread` variable.

The whole layer builds: stdio (including quad-precision `%Lf`, with the
soft-float routines compiled from compiler-rt's builtins), signal, locale,
math, regex and APEX's locked allocator, which replaces the Plan 9 libc
one.  `_envsetup` runs, so the environment is populated from `#e`.

### libc++ on ARM64

`build_libunwind.py` and `build_libcxx.py` build the C++ runtime for ARM64
- libunwind, libc++ and the libc++abi pieces - against the APEX layer, and
`build_libcxx.py --test` links a program that uses std::string,
std::vector, std::thread and a thread-local:

```text
cxx-libcxx harvey arm64 sum=6 tls=7 shared=42
LIBCXX-ARM64-OK
```

Two platform adaptations: `pthread.cpp`'s `thread_spawn` has an aarch64
body (rfork, switch to the new stack, branch), and `dl_iterate_phdr`
synthesises the phdrs libunwind needs from linker symbols, because the
kernel does not map the ELF header on ARM64.

The unwinder needed one build fix: `aarch64-none-elf` does not get
asynchronous unwind tables by default, so libunwind's own frames had no
FDEs and its first step reported end of stack.  libunwind and the APEX
layer are now built with `-funwind-tables`, and `throw`/`catch` works:

```text
cxx-libcxx harvey arm64 sum=6 tls=7 shared=42 int=1 what=boom
LIBCXX-ARM64-OK
```

`test-cxx.py` is the fast C++ runtime regression: it compiles
`testdata/cxxprobe.cpp` (C++23) and boots it as init.  It covers containers,
exceptions, `exception_ptr`, iostreams, `std::locale` and a custom
`numpunct` facet, regex, chrono and threads, `latch`/`barrier`/`jthread`/
`counting_semaphore`, random, filesystem paths plus writable
`fstream`/`filesystem` in `/initramfs`, `charconv`/`format`,
`ranges`/`span`/`variant`, `std::print`, `std::locale("")`, `std::put_time`,
directory iteration/canonical paths, the C++20 calendar, `std::expected`,
`std::numbers`/`bit_cast`, coroutines, `std::format` chrono formatting, and
smart pointers.  Three fixes
were needed to make that set pass:

- `apex.ld` now emits `.init_array` and defines
  `__init_array_start`/`__init_array_end`.  Without them no C++ static
  constructor ran, so `std::cout` and `std::locale` were left
  zero-initialized.
- `libbuiltins.a` includes the i64/binary128 compiler-rt routines
  (`__floatditf`, `__floatunditf`, `__fixtfdi`, `__fixunstfdi`,
  `__fixdfti`, `__fixunsdfti`).
- `apex/aarch64/include/float.h` described the x87 80-bit long double
  (`LDBL_MANT_DIG 64`); aarch64 long double is binary128
  (`LDBL_MANT_DIG 113`).  The wrong value made `frexpl` take its
  subnormal path forever and overflow the stack on the first
  floating-point `printf`.

```text
CXXPROBE containers               OK
...
CXXPROBE ranges_span_variant      OK
CXXPROBE-DONE
C++ runtime probe PASS
```

`test-cxx.py --guest` stages `aspera-tblgen-sdk.boot` and compiles the same
probe with the SDK's own `clang++` inside the guest; it also passes all 25
features.  This is the end-to-end check for the shipped C++ headers,
libraries and linker script, as opposed to the host cross driver.
`--guest --fmt` builds the header-only fmt 11 probe in
`testdata/fmtprobe.cpp` (C++17) and prints

```text
harvey 042 3.50 [1, 2, 3]
FMT-DONE
```

`std::filesystem::space` is reported as `SKIP`: APEX has no
`statvfs`/`statfs`, so libc++ throws `Function not implemented`.  That is a
known gap rather than a probe failure.
`--guest --lto` adds `-flto`; the SDK's clang++ and ld.lld are both LLVM 23,
so LTO works there even though the host cross driver's `ld.lld` may be an
older release.  PCH generation works as well (`-x c++-header`); the wrapper
passes `-static` in compile mode too so the `__STATIC__` macro matches the
link.

### SMP

`test_apex_boot.py --smp N` starts N QEMU vCPUs (the default is still one).
The kernel reads the `device_type = "cpu"` nodes and `/psci` from the device
tree, gives every core a `Mach` selected through `TPIDR_EL1`, and starts the
secondaries with PSCI `CPU_ON` (`hvc` on QEMU virt, `smc` on the board).  The
entry stub in `smp.S` installs the stack, `VBAR_EL1`, and the boot CPU's
`MAIR`/`TCR`/`TTBR0`/`SCTLR`, then `arm64cpustart()` wakes that core's GICv3
redistributor (or the banked GICv2 CPU interface), arms its virtual timer,
and enters the shared scheduler.  Each secondary calls `timersinit()` for its
own per-CPU HZ timer.  Pinned boot workers report:

```text
ARM64 smp: ncpu 4 psci 1/0xc4000003
ARM64 cpu1 online
...
HARVEY-ARM64 CPU1-SCHED PASS
HARVEY-ARM64 CPU1-TIMER PASS
```

SMP found two uniprocessor-only bugs in shared port code.  Clang hoisted the
plain loads in `while(l->key)` (`taslock.c`) and `while(p->mach != 0)`
(`sysproc.c`) out of their loops, turning a contended wait into an infinite
self-branch; both now use atomic loads.  `todinit()` is also serialised
because every core calls it through `timersinit()`.

Two more SMP-only failures came out of stress.  User TLS (`TPIDR_EL0`) is a
per-CPU register, not part of the trap frame, so `procrestore()` now restores
`p->tls` for the IRQ resume path as well; APEX also installs the register
directly when it builds a thread's TLS block.  A thread's stack was freed by
`pthread_join()` while the exiting thread was still running `_exits()` on it;
the join now leaves it to process teardown.  Periodic timers are re-armed
before their handler runs: the old post-handler `tadd()` could touch a
`Proc.Timer` after the handler had woken a process that exited and recycled
its Proc on another CPU.  The heavy build also exposed two scheduling races:
`schedinit` wrote `up->mach`/`updatecpu(up)` after `ready()`/`psrelease()` had
made the Proc visible to another CPU, and a per-CPU `mmuflush` flag left by a
migrated process made `hzclock` release the page tables of whatever process
was current now.  `schedinit` finishes with the Proc first; `psalloc`/
`psrelease` carry arena/alignment/reuse guards; `hzclock` releases only the
current Proc's page tables and `mmuswitch` consumes `newtlb` for Procs that
were not running.  Page-table release is deferred to a user-return boundary
so a timer interrupt inside a syscall cannot unmap the kernel's in-flight
user buffer.

The per-CPU HZ timers used to be heap allocations immediately below the
init process's kernel stack; a stack underflow corrupted `tmode`/`tf`/`tt`
and `timerintr()` paniced.  They now live in a static BSS array, and every
kernel stack has an unused 8 KiB gap below its base.  A 150 s GICv3 boot
reaches `RC START` with `IPI PASS` and no timer panic.  The last unpinned
SMP failure was neither a Segment nor a Page lifetime problem: `schedinit`
readied a preempted Proc (or released a dead one to the free list) while
`machp()->externup` still pointed at it, so another CPU could dispatch or
reuse the same Proc while a timer or IPI on the old CPU still treated it as
current.  Clearing `externup` before `ready()`/`psrelease()` fixes it; the
unpinned four-vCPU `--jobs 4 --mem 6144` build now completes all 77 TUs.

User FP/SIMD state is now part of the context switch.  The kernel is built
with `-mgeneral-regs-only`, so `procsave`/`procrestore` use `fpu.S` to save
and restore q0-q31, FPCR and FPSR; without that a process resuming on a
different CPU inherits the previous occupant's registers.  An eight-process
register stress test failed with another child's value before the change
and passes after it.

Status: 1, 2 and 4 vCPU boots pass.  The full 77-TU `llvm-tblgen`
self-build compiles, links and matches the host byte for byte
(`TGEN VT-MATCH 46644 bytes`) on one vCPU, on a 4-vCPU kernel with user work
pinned to CPU0, and on an unpinned 4-vCPU kernel at
`--smp 4 --jobs 4 --mem 6144`.  At 3 GiB the unpinned four-way build can
fail under memory pressure.  `--smp 4 --jobs 2 --mem 3072` also completes
the full 77 TUs, which is the configuration the board uses: the first RAM
bank is only ~3.8 GiB, so two concurrent compiles fit while four do not.
`ARM64_PIN_USER=1` still pins the init process (and the whole user tree) to
CPU0.  `ARM64_AFFINITY=1` also wires the init to CPU0 and fork passes that
Mach to every child, so it runs the tree on one CPU as well; use the
default build for parallel work.  Both remain available as single-CPU test
knobs.
The v1.1 board now brings all four cores online (`CPU1/2/3-TIMER PASS`).
`test-selfbuild.py --smp 4` compiles, links and runs the guest `llvm-as`
with byte-identical bitcode, and `--tool llvm-dis` / `--tool llvm-ar` build
those tools as well.  The C++ runtime probe passes all 25 features with the
SDK's own `clang++`; fmt 11, guest LTO and PCH also pass.  All 26 probe
features now pass, including `std::filesystem::space`, which reports the
guest's page pool (APEX gained `statvfs`/`statfs` and `sysconf`
`_SC_PHYS_PAGES`, and the kernel prints free/total physical memory in
`#c/swap`).

The SDK ships a complete LLVM/Clang tool set, not just a compiler:

- one `llvm` multicall driver providing `clang`, `clang++`, `clang-cl`,
  `ld.lld`, `lld-link`, `wasm-ld`, `dsymutil` and the binutils set
  (`llvm-nm`, `objcopy`, `strip`, `readobj`, `readelf`, `objdump`, `size`,
  `strings`, `symbolizer`, `addr2line`, `cxxfilt`, `ar`, `ranlib`, `dwp`,
  `gsymutil`, `lipo`, `rc`, `windres`, `ml`, `ifs`, ...);
- the standalone IR tools the driver does not carry: `llc`, `opt`,
  `llvm-as`, `llvm-dis`, `llvm-link`, `llvm-lto`, `llvm-mc`,
  `llvm-dwarfdump`, `llvm-profdata`, `llvm-cov`, `llvm-extract`,
  `llvm-diff`, `llvm-bcanalyzer`;
- clang-tools-extra: `clangd`, `clang-tidy`, `clang-check`, `clang-format`,
  `clang-apply-replacements`, `clang-query`, `clang-doc`,
  `clang-include-cleaner`, `clang-include-fixer`, `clang-move`,
  `clang-reorder-fields`, `modularize`, `pp-trace`.

Plan 9 has no symbolic links, so every name the driver dispatches on is a
~10 KiB shim that re-execs `…/bin/llvm <subcommand>` with the original
arguments (see `util/arm64/llvm-shim.c`); the standalone tools are real
binaries.  `util/arm64/test-tools.py` exercises the set inside the guest
(compile, archive, nm, objcopy, strip, readelf, objdump, size, strings,
cxxfilt, clang-format, clang-tidy, clang-check and `llvm --help`).

`clangd` is the one tool that needs a threaded LLVM build.  Configure the
ARM64 tree with `-DLLVM_ENABLE_THREADS=ON -DCLANG_ENABLE_CLANGD=ON`; the
affinity API its threading code uses (`cpu_set_t`, `CPU_ALLOC`,
`CPU_ALLOC_SIZE`, `sched_getaffinity`) is in `util/libcxx/sched.h`.
`install-sdk.py --with-tools` then ships `clangd`, and
`clangd --check=<file> --compile-commands-dir=<dir>` exercises parsing,
diagnostics and the index without an LSP client.  `test-clangd.py` builds a
small project, runs that check in QEMU, and writes a board Image with
`--board-image`; the v1.1 board reported `All checks completed, 0 errors`.

`install-sdk.py --with-sanitizers` (or `build_sanitizers.py`) builds the
UBSan and ASan runtimes into the clang resource directory.  The normal
`-fsanitize=undefined` links `libclang_rt.ubsan_standalone.a`, implemented
in `util/arm64/ubsan_standalone.c`: it reads the compiler's handler
descriptors directly and prints `file:line:col: runtime error: <check>`
without sanitizer_common's Linux syscall layer.  The minimal variant,
`-fsanitize=undefined -fsanitize-minimal-runtime`, links compiler-rt's
`libclang_rt.ubsan_minimal.a` and prints the check and caller PC.  Both
were verified in QEMU with signed-overflow and out-of-bounds programs.
A guest `clang++ -fsanitize=undefined` compile of the same test printed
`UBGUEST-COMPILE status 0` and
`/initramfs/t/overflow.c:3:31: runtime error: add_overflow ...`, so the
resource-directory auto-link path works in the guest as well.

AddressSanitizer is a minimal user-space runtime in `util/arm64/asan_rt.c`,
shipped as `libclang_rt.asan.a`.  It reserves the 64 GiB shadow at
`0x1000000000` (the M0 probe) and a 64 GiB heap at `0x2000000000`, and
poisons `malloc`/`free` redzones and registered globals.  It deliberately
does **not** install a `SIGSEGV` handler: the ARM64 kernel currently turns
an unhandled user fault into `pexit("user fault")`, so wild-pointer and
stack-guard reports are unavailable without a kernel change.  The
compiler-instrumented heap, global and use-after-free checks do not need a
signal.  `test-asan.py` runs the five-case matrix (C and C++), and
`test-cxx.py --asan` runs the 26-feature C++ probe under the runtime with
no reports.  There are no stack traces, no stack fake frames, no quarantine
and no signal handling.

The heap tail accounting in `qmalloc` was wrong: after installing a new
physical chunk, `morecore()` added only the requested units to `tailsize`
and kept the previous chunk's residue, so a later allocation could walk
past the new chunk.  `tailsize` now tracks the current chunk, `mallocinit`
seeds it from the static 32 MiB arena, and `qmallocalign` asks for the
alignment residue in the same request.  `physalloc`/`physfree` now take a
cross-CPU `Lock` (the old `splhi()` only masked the local CPU), and kernel
stacks are 32 KiB.

`test-selfbuild-tblgen.py --jobs N` compiles several TableGen TUs at once
through `TGEN_JOBS`.  `--mem` is explicit now; when omitted, `--jobs > 1`
defaults to 4 GiB and a lower value is honoured with a warning.  With the
exact-size allocator and the qmalloc tail fix, 8-TU `--jobs 2` and
`--jobs 4` runs both completed at 3 GiB on one vCPU, so four concurrent
clang++ processes no longer need 6 GiB for this workload.
Cross-CPU page-table shootdown uses SGI 0 (`GICD_SGIR` on GICv2,
`ICC_SGI1R_EL1` on GICv3): `procflushseg()` broadcasts it and the boot probe
reports `IPI PASS`.  The SGI handler only invalidates translations; page
table release stays at the next user tick, context switch or user return.
An EL0 code replacement still invalidates the I-cache only on the core that
performs it.

### Heap and large allocations

APEX `malloc` keeps allocations >= 1 MiB at their exact size on a large
free list instead of rounding to the next power of two.  The old
power-of-two buckets retained every old size class; one guest `clang`
reached 1.4-1.6 GB of segments for a TU whose host build peaks near
100 MB.  An in-place top-of-heap growth was tried and dropped: it moved
the break on every growing realloc, and `ibrk()`'s `mmuflush` only covers
the calling thread while `rfork(RFMEM)` threads share the BSS segment with
separate page tables.  The heap is not shrunk; parallel builds still need
RAM, and on the board that means the second memory bank first.

`Proc.Timer` is zeroed when a `Proc` is allocated and detached in `pexit()`,
so a recycled Proc cannot inherit a stale timer-list pointer.

### Running ARM tools with arguments

The kernel starts its first user process with no descriptors and no
arguments.  `util/arm64/mkrun.py` wraps an ARM64 program in a launcher that
opens the console, unions a ramfs over the read-only boot root, writes the
embedded program (and optionally a tree of data files) out, and execs it
with a command line:

```sh
python3 util/arm64/mkrun.py /tmp/opencode/llvm-arm64/bin/llvm-tblgen \
    --tree /home/scott/Repo/llvm-project/llvm/include:/td \
    -- -gen-vt -I /td /td/llvm/Target/ValueTypes.td
python3 util/arm64/test_apex_boot.py --elf build/arm64-run/runner.elf \
    --marker '#include'
```

LLVM builds for aarch64 with `harvey-aarch64-toolchain.cmake`, and
`util/arm64/test-tblgen.py` runs the result against the real .td tree:

```text
tblgen: 46644 bytes generated on Harvey/ARM64, identical to the host tool
```

That is `llvm-tblgen -gen-vt` producing `GenVT.inc` byte-for-byte the same
as the host build of the same revision.

```sh
python3 util/arm64/test_virt.py
python3 util/arm64/test_fdt.py
python3 util/arm64/check_port.py
python3 util/arm64/test_syscalls.py
python3 util/arm64/build_libc.py
python3 util/arm64/build_o9_services.py
python3 util/arm64/test_user_runtime.py
python3 util/arm64/test_full_kernel.py
python3 util/arm64/test_rc.py
python3 util/arm64/test_virtio9p.py
```

For interactive inspection:

```sh
python3 util/arm64/build_virt.py
qemu-system-aarch64 -machine virt -cpu cortex-a57 -m 512M -smp 1 \
    -nographic -monitor none -kernel build/arm64/harvey-virt.img

python3 util/arm64/build_kernel.py
qemu-system-aarch64 -machine virt -cpu cortex-a57 -m 512M -smp 1 \
    -nographic -monitor none -kernel build/arm64-kernel/harvey.img
```

The **same** `build/arm64-kernel/harvey.img` is used with QEMU `virt`, the
Orange Pi 3B (RK3566), and the Orange Pi 3 (Allwinner H6). No board flag or
separate kernel build is needed. The firmware
passes a device tree in `x0`; the kernel chooses its console, timer controller,
MMIO ranges, and a RAM bank containing the loaded Image from that tree. The
current Image is linked at `0x40080000`, so firmware must place it there. A
board whose RAM does not cover that address still needs a relocatable Image
before it can use this kernel. The Image header allows firmware to retain the
load address, but the kernel itself has no relocation support yet.

For another board, the boot contract is: load this Image at `0x40080000`, pass
an FDT in `x0` with `/memory`, `/chosen/stdout-path`, a supported UART, an ARM
GICv2 or GICv3, and an ARMv8 timer. The parser accepts an alias in
`stdout-path` and chooses the RAM bank containing the Image. Hardware not
needed for early boot belongs in a user-space driver filesystem; adding that
driver should not alter the kernel image. A future relocation stage must remove
the fixed load address before this is a general ARM64 distribution image.

For the Orange Pi 3B RK3566 bring-up:

```sh
python3 util/arm64/build_kernel.py
```

Boot it with the full **OS** board DTB as `booti 0x40080000 - 0x48000000`
after loading the Image at `0x40080000` and the DTB at `0x48000000`. The
installed vendor U-Boot's control DTB at `0xeb9f5e30` is stripped of its GIC
and timer nodes and cannot be used for this boot. U-Boot adds valid `/memory`
nodes to the OS DTB during `booti`. The early parser
uses `/chosen/stdout-path` (including `/aliases`) to select UART2 on this board,
and chooses the RAM bank containing the Image. The NPU register/DMA probe is excluded from normal
boot; set `ARM64_RK3566_NPU_PROBE=1` only for a separate NPU experiment. The
QEMU image now provides an interactive `rc`. The same image was reflashed over
router-hosted TFTP and verified on the v1.1 board: `echo hardware-rc` executed
`/boot/echo`, `whatis cd` reported the builtin, and `exit` relaunched `rc`.
The vendor U-Boot reports `FB base = 0` and exposes only the serial console, so
it does not hand off a framebuffer. It also reports no SD/eMMC card and no USB
storage. The display path therefore still requires a VOP2/HDMI driver; the
RK3566 VOP compatible is now recognized by the device-tree inventory.
On an Orange Pi 3B v1.1, the common Image and the full OS DTB loaded over TFTP
passed the early memory, MMU, timer, EL0 isolation, scheduler, root walk, and
`#~` boot checks. A later router-hosted TFTP boot of the same Image and the
full OS DTB also passed every driver probe: `DRVMAN` enumerated the on-board
I2C, UART, GPIO, MMC, GMAC, USB, PCIe, HDMI, and codec nodes; `uartfs`,
`drvman`, and `hubfs` reached `PASS`, and the driver manager reported its `i2c`
and `gpio` backends `PASS`. The image now embeds Plan 9
`rc`, its `rcmain`, and a small `/boot/echo`; `init` forks `rc` on the console
after the driver services start and relaunches it after `exit`. UART input is
sampled by the ARM64 clock callback and fed through the console line editor.
`test_rc.py` verifies the prompt, an external `/boot/echo` execution, a
built-in, and exit/relaunch under QEMU. The scheduled libc-backed C process
entered through `_main`, called its generated `nsec` wrapper, wrote its
acceptance marker through a user-only mapping of the RK3566 DesignWare UART,
and returned to the kernel without an exception. A real root filesystem and
storage/network services are still missing. The board's 8 GiB appears as two memory banks in U-Boot;
the kernel currently manages the bank containing the Image (roughly 3.8 GiB)
and leaves the second bank for later multi-bank allocator work. Early code is
built with `-mstrict-align`: before the MMU
is enabled, the RK3566 traps on unaligned multi-byte stores even with
`SCTLR_EL1.A` clear. Replacing EL0 test code at an identity-mapped address
also requires invalidating the instruction-cache alias used by its user
virtual address. A local `ic iallu` handles that boot probe; with SMP, replacing
code that another core may execute still needs an I-cache shootdown. The board
has no virtio-9P device attached, so the optional `#9` exchange is skipped.

### Rockchip IOMMU

The RK3568/RK3566 IOMMU is implemented in `sys/src/9/aarch64/rkiommu.c`.
It is a two-level Rockchip v2 table: 1024 directory entries, 1024 page-table
entries per directory, and 4 KiB pages.  This gives a 4 GiB IOVA space, while
the RK3568 PTE format carries a 40-bit physical address so pages from the high
DRAM bank can be mapped into the VOP's 32-bit address space.

The implementation includes:

- page-table allocation and teardown
- v2 DTE/PTE encoding
- map/unmap with TLB shootdown
- reset, paging enable/disable, interrupt mask, and auto-gating setup
- page-fault status reporting
- a hardware-independent page-table self-test

The FDT parser now classifies `rockchip,rk3568-iommu` nodes and records the
VOP IOMMU's two register ranges.  The ARM64 MMU build also identity-maps the
SoC device ranges discovered from the device tree; user processes still reach
device MMIO only through `#P`/`segattach` grants.

A Linux 6.18 boot with the VOP2/DW-HDMI drivers initialized a 2560x1440,
32-bpp framebuffer.  Its register state, including VOP, HDMI, CRU, PMU, GRF,
and PMUGRF, is recorded in
`snapshots/rk3566-linux-2560x1440.txt`.  The snapshots show `FB base = 0`
because DRM exposes the display through the VOP IOMMU, not as a physical
address.

The IOMMU page-table self-test passes under QEMU.  Board-level enumeration and
the VOP2/HDMI scanout driver are the next steps.

### Orange Pi 3 (Allwinner H6)

The H6 path is earlier than the RK3566 path. The common early kernel now
recognizes the H6's `arm,gic-400` GICv2, its DesignWare UART0, the ARMv8
timer, and the H6 MMC/GPIO/I2C/EMAC device-tree classes. `test_fdt.py` builds
`testdata/h6-orangepi3.dts` and checks the expected memory, console, GIC,
timer, and class discovery.

Boot contract:

```text
load harvey.img at 0x40080000
load the full OS sun50i-h6-orangepi-3.dtb at 0x48000000
booti 0x40080000 - 0x48000000
```

The early UART assumes firmware/U-Boot has already initialized UART0
(`serial0`, 115200). The next hardware step is a serial boot on the board to
confirm the H6 GIC-400, timer, MMU, and EL0 probes before adding storage or
network drivers.

Still missing for an interactive H6 system:

- sunxi MMC/SD and eMMC user-space driver, for a real root filesystem
- H6 pinctrl/GPIO backend instead of the RK356x register layout
- H6 I2C and AXP805 PMIC/regulator support
- H6 EMAC (`dwmac-sun8i`) plus the board PHY/MDIO setup
- USB, HDMI/display, and thermal/RTC support
- a durable U-Boot/ATF/SD-card boot image rather than manual TFTP/`booti`

### SoC family and board drivers

The RK3566 shares its SoC family with the RK3568 ("rk356x"). SoC-specific
register layouts live in `sys/include/rockchip/rk356x.h`: GPIO bank offsets,
the GRF/PMUGRF/CRU base addresses, and the pinmux (iomux) formula. The kernel
already classifies the board's peripherals from the device tree through `#P`,
so each driver discovers its live base addresses from `#P`, maps the MMIO with
`segattach` (the resource grant), and serves 9P.

That discovery, mapping, 9P-tree and health boilerplate is the driver kit:
`sys/include/drv.h` and `sys/src/cmd/drvkit/drv.c`. A new driver calls
`drvfind`/`drvmap`, keeps its register code, describes its files as `DrvFile`
entries and posts them with `drvsrv`; `drvhealthrun` keeps it in `drvman`'s
table. `uartfs` is the worked example, and
[`sys/src/cmd/drvkit/README.md`](../../sys/src/cmd/drvkit/README.md) has the
API, the 9front-to-drvkit mapping and the limits (no IOMMU isolation, one wait
per interrupt, no PCI config space on amd64).

The first driver is `gpiofs` (`sys/src/cmd/gpiofs/gpiofs.c`), served at
`/srv/gpio`:

- `/status` lists the discovered banks and pinmux.
- `/gpioN/data` reads the actual pin state, or writes the output data.
- `/gpioN/dir` reads/writes the 32-bit direction mask (1 = output).
- `/gpioN/ctl` takes `"N in"` or `"N out [0|1]"` and muxes that pin to GPIO.

The five RK3566 banks (GPIO0..GPIO4) are discovered and sorted by base address;
GPIO0's pinmux is in PMUGRF, GPIO1-4's in GRF. The mux offsets and the V2 GPIO
register layout are encoded in the family header and need on-board verification
against the RK3566 TRM. `init` launches gpiofs non-fatally; the QEMU `virt`
machine has no GPIO banks, so it reports `GPIOFS PENDING` there and `PASS` on
the real board.

For the tested v1.1 board, the installed vendor U-Boot needed a 100 Mbps PHY
workaround before the TFTP transfer. With a TFTP server at `192.168.2.1`
serving `aspera-arm64.img` and the full `orangepi-3b-v1.1.dtb`, the serial
console sequence was:

```text
setenv ipaddr 192.168.2.250; setenv netmask 255.255.255.0; setenv serverip 192.168.2.1
ping 192.168.2.1
<interrupt the first failed ping with Ctrl-C>
mii write 0 9 0; mii write 0 0 0x1200; mii write 0 1e 0xa003; mii write 0 1f 0x40f1
ping 192.168.2.1
tftpboot 0x40080000 aspera-arm64.img
tftpboot 0x48000000 orangepi-3b-v1.1.dtb
booti 0x40080000 - 0x48000000
```

The first ping initializes the PHY and may need Ctrl-C to return; the MDIO
changes must follow that initialization. The board then reached the 9ring
test at roughly 3.8 MiB/s TFTP throughput. U-Boot's automatic `bootm` attempt
after each TFTP transfer reports an image error; the explicit `booti` command
is required. The repeatable development path is a serial boot script; the
durable fix is in the board's U-Boot PHY setup and boot script, keeping these
MDIO commands out of the common kernel.

The APEX SDK self-build is delivered as two TFTP files.  The first is an
Image whose `/boot/init` is the tblgen runner; the second is an `O9BOOT01`
archive holding `/boot/sdk.tar` (the base SDK plus the `sdk/l` TableGen
overlay, about 395 MiB).  Build the overlay with
`test-selfbuild-tblgen.py --stage-only --boot <archive>`; the plain
`install-sdk.py --boot-archive` output has no `tg.list` and the runner will
report `TGEN no manifest`.  The kernel reads the archive at
the fixed address `0x50000000` (up to 768 MiB), reserves it, and exposes its
files under `/boot`; the runner unpacks `sdk.tar` into `/initramfs` before
compiling.  Load the archive before `booti`:

```text
tftpboot 0x40080000 aspera-tblgen.img
tftpboot 0x50000000 aspera-tblgen-sdk.boot
tftpboot 0x48000000 orangepi-3b-v1.1.dtb
booti 0x40080000 - 0x48000000
```

The kernel currently manages only the contiguous RAM bank containing the
Image.  On the 8 GiB board that is the ~3.8 GiB lower bank, which holds the
256 MiB kernel/archive window and is enough for a two-way (`TGEN_JOBS=2`)
SDK self-build; the second bank is not yet used.

The two-file layout was validated in QEMU virt exactly as U-Boot will load
it: `aspera-tblgen.img` was built with a `TGEN_JOBS=2` runner as
`/boot/init`, the 395 MiB overlay archive was loaded at `0x50000000`, and
all 77 TUs compiled on four vCPUs with `TGEN VT-MATCH 46644 bytes`.  The
image build is `test_apex_boot.py --elf <runner.elf> --image-out <Image>`
with a `--jobs 2` stage; the archive comes from
`test-selfbuild-tblgen.py --stage-only --boot <archive>`.  Build the board
image **without** `ARM64_AFFINITY`: a wired init passes its Mach to every
child, so an affinity build pins the whole user tree to CPU0 and runs at
the single-CPU rate.  The same image and the 404 MiB board archive
completed 77/77 on the v1.1 board with `TGEN VT-MATCH 46644 bytes` in
1,874,559 ms on four vCPUs with `TGEN_JOBS=2` (the earlier 1,884,316 ms
run used the same default scheduler).  The first concurrent board run
failed at TU 42 with `clang++: error: argument to '-o' is missing`: exec
had kept the argument count in a global, so two simultaneous execs
overwrote each other's value and one program lost its last argv entry.
The count now lives on the Proc.

On the board, the vendor/router TFTP root (`/tmp/aspera-tftp`) is too small
for the archive.  Point U-Boot at the host instead and split the archive
into parts below the 16-bit TFTP block limit (65,535 blocks of 1468 bytes,
about 96 MiB):

```text
setenv serverip 192.168.2.2; setenv tftpdstp 6969
tftpboot 0x40080000 aspera-tblgen.img
tftpboot 0x50000000 sdk.part.00
tftpboot 0x55000000 sdk.part.01
...
tftpboot 0x48000000 orangepi-3b-v1.1.dtb
booti 0x40080000 - 0x48000000
```

`host:/home/scott/tftp` is served on port 6969 by `tftpd.py`; the parts are
80 MiB so each load address is a clean 0x05000000 step.  The full tool set
makes the overlay 766 MiB, so there are ten parts:

```text
split -b 83886080 aspera-tblgen-sdk.boot sdk.part.
```

The kernel accepts boot archives up to 1280 MiB.  A ten-part load places
`sdk.part.00`..`08` at 0x50000000 through 0x78000000 (0x05000000 steps) and
the 11 MiB tail at 0x7d000000.

The installed vendor U-Boot ignores `tftpdstp` and always talks to UDP port
69; the `serverip=192.168.2.2; tftpdstp=6969` path above is an extension of
the Aspera U-Boot.  With the vendor U-Boot, serve the files from a port-69
server.  On this LAN the OpenWrt router at 192.168.2.1 runs dnsmasq's TFTP
server rooted at `/overlay/aspera-serve`; put `aspera-tblgen.img`,
`orangepi-3b-v1.1.dtb` and the 80 MiB parts there.  The vendor U-Boot still
needs the 100 Mb/s PHY workaround above before the first transfer.  Note
that the router's `/tmp` tmpfs is only 242 MiB and remains full after files
are removed, so keep the TFTP root on `/overlay`.  `util/arm64/push-tftp.py`
publishes the Image, DTB and an 80 MiB-split SDK archive to
`root@192.168.2.1:/overlay/aspera-serve` and prints the matching U-Boot
load sequence.

The SD card also carries the bootloader on this unit: the Rockchip SPL
reads the FIT at sector 0x4000 (byte 8 MiB) and the idbloader area starts
at sector 0x40.  Never write an image at LBA 0 that puts a partition over
those sectors — the board then falls back to the eMMC's vendor U-Boot,
which has `bootdelay=0` and no boot target.  `mk-sdimg.py --uboot` places
`u-boot-rockchip.bin` at 0x40, keeps the FAT partition at 16 MiB and
verifies that nothing overlaps.

The FIT must contain the TF-A loadables (`atf-1` through `atf-6`).  A
`u-boot-rockchip.bin` built without BL31 still starts, then aborts in
`__arm_smccc_smc` called from `flush_shm_dcache` and resets the board in a
loop.  `mk-sdimg.py --uboot` now rejects a FIT without `atf-1`, and the
validated image is `u-boot-rockchip-final.bin` (the `u-boot` image hash is
`69265f82...`, matching the October 18 boot log).  To repair only a card's
bootloader without touching its filesystem, load `u-boot-final.itb`
(1,373,184 bytes) and write the FIT at sector 0x4000:

```text
tftp 0x50000000 u-boot-final.itb
mmc write 0x50000000 0x4000 0xA7A
reset
```

With the eMMC removed, the SPI SPL tries the SD FIT first; if it is
missing or invalid, the vendor U-Boot in SPI runs.  That SPI fallback is
the recovery path when the card itself has no working bootloader.

The board also boots from the SD card, which removes TFTP from the boot
path.  U-Boot sees the card as `mmc 1` (the v1.1 unit has a 58.2 GiB
SD 3.0 card, high speed 50 MHz, 4-bit).  That is the Aspera U-Boot's
numbering, which follows the Linux aliases (`mmc 0` = eMMC,
`mmc 1` = SD).  The installed vendor U-Boot numbers the same slots the
other way round: `mmc 0` = SD and `mmc 1` = eMMC.  Run `mmc list` and
check the capacity before every `mmc write`; a recovery image written to
`mmc 1` under the vendor U-Boot lands on the eMMC instead of the card.
`util/arm64/mk-sdimg.py` builds a
457 MiB image with an MBR, one FAT32 partition and `Image`, `board.dtb` and
`sdk.tar`.  Write it once from U-Boot (load the 80 MiB parts at
0x50000000 + i*0x05000000 first):

```text
mmc dev 1
mmc write 0x50000000 0 0xDA000
fatload mmc 1:1 0x40080000 Image
fatload mmc 1:1 0x48000000 board.dtb
fatload mmc 1:1 0x50000000 sdk.tar
booti 0x40080000 - 0x48000000
```

The card reads at about 11.5 MiB/s, so the 404 MiB board SDK loads in
34 s; the kernel then prints `BOOT-ARCHIVE PASS` and runs the same
self-build.  U-Boot reports `Loading Environment from nowhere`, so a
`setenv bootcmd ...; saveenv` may not persist until an environment
location is configured.

If the card's bootloader area is damaged, it can be restored from the
running vendor U-Boot without a card reader: the recovery image is the
same 457 MiB image with `u-boot-rockchip.bin` at sector 0x40 and the FAT
partition at 16 MiB.  Under the vendor numbering the card is `mmc 0`:

```text
mmc dev 0; mmc info          # confirm the 58.2 GiB card, not the eMMC
mw.l 0xfe60000c 0x76; mw.l 0xfe600000 0
tftp 0x50000000 sd-recovery.img
mmc write 0x50000000 0 0xDA000
```

The user-space `sdcard` driver does not re-initialise the card; like the
eMMC driver it takes over the controller U-Boot already configured.  Run
`mmc dev 0; mmc info` at the vendor prompt before `booti`, then
`/boot/sdcard probe` (or `sdcard read <lba> [n]`) reads the card through
the DesignWare FIFO path.

The RK3566 eMMC works from user space.  `#P` grants the
`mmc@fe310000` node, `/boot/emmcfs` serves it as a 9P tree (`ctl`, `data`,
`partN`, `flush`), and the card comes up as HS200 200 MHz 8-bit:
488554496 blocks (250 GB) with a 128 MiB system partition and a 200 GiB
data partition.  `util/arm64/test-emmc.py` starts the server, mounts it and
reads the MBR (`--setup` provisions the data partition as ext2 and writes a
test file; `--install` is the SDK install path).  Note the libc mount
convention: a 5-argument `mount()` expands to `mount_()` through
`<libc.h>`; code that declares its own `mount` must call `mount_`.

The full 766 MiB SDK does not fit on the board: the kernel manages only the
~3.8 GiB bank that holds the archive, and the archive plus its unpacked copy
(another 766 MiB) leave too little for two concurrent compiles — the board
panics with `kickpager(): no physical memory`.  Build the board archive with
`install-sdk.py --board-sdk` (driver, all standalone IR tools,
clang-format and clang-apply-replacements; 355 MiB, 404 MiB with the
TableGen overlay) and keep the full SDK for QEMU and host distribution.

Do not use a round 80,000,000-byte split: the 0x003b4c00 gap between parts
shows up as a `bad tar header` in the runner.

U-Boot leaves the RK3566 watchdog running, and it resets the board in the
middle of the multi-part TFTP load.  At the U-Boot prompt, clear it before
loading:

```text
mw.l 0xfe60000c 0x76; mw.l 0xfe600000 0
```

With the watchdog stopped, the parts load on the v1.1 board and the
kernel reaches the runner: it prints `HARVEY-ARM64 BOOT-ARCHIVE PASS`,
untars the SDK from `0x50000000` and starts `/initramfs/bin/tblgen`.  The
kernel now brings all four cores online (`ARM64 cpu1/2/3 online`, then
`CPU1/2/3-TIMER PASS`), and a `TGEN_JOBS=2` run completes all 77 TUs in
1,884,316 ms with `TGEN VT-MATCH 46644 bytes`.  A uniprocessor build of the
same tree takes 3,111,312 ms.

This uses QEMU's direct ARM64 Image loading. No UEFI or Limine is involved.
The Image header and x0 device tree handoff also match U-Boot's `booti`
interface. The early parser supports root-level 1- or 2-cell
`reg` addresses, multiple RAM ranges, PL011 or DesignWare UART, and GICv2 or
GICv3. It currently manages only the contiguous RAM bank containing the Image
and requires device registers to have direct physical `reg` addresses; bus
`ranges` translation and runtime kernel relocation are future work. The GICv3 timer path has passed its boot probes on the RK3566; broader
device interrupt routing still needs work. The early allocator hands out
zeroed 4 KiB pages; the full build also
has a free-list allocator for process pages and page tables.
The identity map remains writable and executable in EL1, while its entries are
inaccessible to EL0. Because user and kernel share TTBR0, user text starts
above the identity-mapped window (`UTZERO` is 16 GiB, matching `Maxidentity`
in `mmu.c`) and must stay there until the kernel moves to TTBR1. Separate user
roots and a complete general-register trap frame are in place. The lower-EL synchronous trap path can now dispatch a
scheduled process's register-based syscall through the generated syscall
table. Error unwinding and return-value propagation are exercised by the boot
probe, but notes and the complete production syscall-exit path remain. The
linked kernel has `Proc`/`Mach`, page mapping, the shared scheduler and core
devices. The runtime handoff runs `procinit0()`, `psinit()`, `devtabreset()`,
`pageinit()`, `timersinit()`, and `schedinit()`. A scheduled EL0 probe runs after
the root walk and local transport checks. Before launching general untrusted
programs, resource grants and the remaining process lifecycle paths still need
hardening. ARM64 libc and all 42 generated syscall wrappers now cross-build,
and a small `_main` process links against that libc. The embedded probe is
loaded as ELF. A second, normally segmented bootstrap process calls
`exec("/boot/init")`; the shared ELF loader replaces its address space, and the
filesystem-backed ARM64 init image is demand-paged through tracked per-process
page tables. The same build cross-compiles the Objective-9C runtime and Harvey
support libraries, then packages the ARM64 `o9drvman`, `o9xhci`, and
`usbpower` ELF images as `/boot/o9drvman`, `/boot/o9xhci`, and
`/boot/usbpower`. Init launches the C `drvman` on the board; the driver manager
starts `usbpower` after `gpiofs` and before any USB controller service.
The shared `#9` driver uses PCI on amd64 and device-tree discovered virtio-MMIO
on ARM64. Both legacy MMIO v1 and modern v2 work in QEMU `virt`. The ARM64
transport currently polls for replies; device interrupts are not routed yet.
`#~` provides named local IPC on ARM64 and has a tagged request/response boot
check. Device services belong in user-space 9P servers. Each driver uses the
shared `DriverHealth` writer for readiness, health, and stop markers and
receives hardware access through `#P` resource grants. The ARM64 user-mode UART
probe proves the privilege boundary needed by a driver server; a restricted
resource-grant interface is still required.

The build uses `clang-23` if present because this host's default `clang` only
contains the x86 backend.

`o9c` (Objective-9C's compiler) is a Plan 9 program: the copy built inside
the 9front VM is a Plan 9 a.out and cannot run on a Linux build host.  Build
a host copy first with plan9port's `9c`/`9l`:

```sh
sh util/arm64/build_o9c.sh          # writes build/host/o9c
python3 util/arm64/build_userland.py --output build/arm64-o9/userland.h \
        --archive build/arm64-o9/userland.img
```

`build_o9_services.py` prefers `build/host/o9c` and falls back to the repo
copy, so a host without plan9port still builds when the sources were
transpiled elsewhere.  The host build was verified end to end: all eight
services transpile with it, compile and link, and the userland and kernel
built from a clean tree boot to `rc` under QEMU `virt`. Kernel sources and integer/text kernel libc use
`-mgeneral-regs-only`. Floating-point libc routines retain the standard ARM64
ABI; they must stay unused until per-process FP state is managed.
Set `ARM64_CC`, `ARM64_LD` and `ARM64_OBJCOPY` to override the tools.

`check_port.py` remains a fast compile-only check. `test_full_kernel.py` checks
the complete link for undefined symbols and boots that linked Image under QEMU
with 256 and 512 MiB of RAM. It exercises physical allocation and free after
the early MMU and trap probes, then checks device reset, page allocation,
process scheduling, clock ticks, the root walk, a scheduled EL0 process, its
restricted UART mapping, and generated syscall-table dispatch.
`test_virtio9p.py` attaches a QEMU host directory and checks a real 9P version
exchange through both MMIO transport versions. The boot check also exercises
the local `#~` request and reply path without a host ring.
The 32 MiB static kernel heap is a bring-up
limit; it will need growth and accounting before general use.

## APEX and a native clang

The POSIX surface on ARM64 is APEX (`apex/`) over the Plan 9 libc, plus
`util/libcxx` and libc++/libunwind/libc++abi.  It is built with
`util/arm64/build_apex.py`, `build_libcxx.py` and `build_libunwind.py`; the
result is what a guest program links:

    libc++  libunwind  libc++abi   C++ runtime
    libap.a                        POSIX/ANSI: stdio, errno, TLS, startup, mmap
    libc.a                         Plan 9 libc: syscalls, fmt, allocator

`util/arm64/install-sdk.py` lays that stack out as a self-contained tree
(clang, clang++, ld.lld, resource headers, machine/apEX/libc++ includes,
archives, `apex.ld`) whose driver configs use the guest root, and can emit it
as a ustar archive wrapped in an `O9BOOT01` boot archive.  `mkrun.py` loads
that archive at 0x50000000, unpacks it into a ramfs and runs a program from
it, so a 360M toolchain reaches the guest without being embedded in the
kernel image.

Building clang for the guest found a series of platform bugs, all fixed at
the source rather than worked around:

* `writev()` returned `i32`, so a failed stdout write became +4294967295 and
  `__stdio_write` retried with a wild pointer.
* APEX's `setenv()` built `"#e/\0NAME"` and silently failed, so variables
  never reached `#e`/`/env` and `execve` dropped them.
* `stat()` decided a file was a character device from `Dir.type`, which is
  the *device letter* for namespace files, so every ramfs file looked like a
  device and LLVM rejected its own config files.
* `ino_t` was 16 bits.  LLVM's FileManager keys files by (st_dev, st_ino),
  and `dirent.h` collided with `stdint.h`; clang returned the cached
  `stdint.h` and silently skipped `dirent.h`.
* ramfs capped a file at 32M and read a directory as if it were a byte
  stream, so a directory whose entries did not fit one 8K block could not be
  listed at all (clang saw `__type_traits` as empty).
* the kernel heap was a static 32M array; it now grows from `physalloc()`.
* libc had a 32-bit `labs`, no `llabs`, and only part of the libm surface.
* user text started at 2 MiB in the same TTBR0 address space as the kernel
  identity map.  Clang is about 120 MB, so its data segment crossed
  0x08000000 and replaced the kernel's GIC mapping; every timer tick then
  read a spurious interrupt ID, the timer was never acknowledged, and the
  CPU stayed in the exception vector forever.  User text now starts at
  16 GiB, above every identity mapping.
* the ARM64 kernel never called `fmtinit()`, so the `%q` format verb was
  never installed.  `sysawait()` wrote the literal `%q%` where the exit
  message belonged, APEX's `waitstatus()` could not parse it, and every
  child was reported as exiting with status 1.  Clang consequently thought
  its successful `ld.lld` child had failed and never kept the output
  binary.  `arm64kernelstart()` now installs the format verbs, as amd64
  and riscv already did.
* `jmp_buf` was ten words, the amd64 layout, while the aarch64
  `setjmp`/`longjmp` save and read up to offset 0xb8 (twenty-four words).
  LLVM's `CrashRecoveryContext` keeps a `jmp_buf` in a heap object, so
  every `setjmp` overwrote about a hundred bytes past it; `longjmp` could
  then restore a heap address as the stack pointer.  Clang and lld crashed
  at random points.  The header now sizes the buffer per architecture.
* with 3 GiB of guest RAM the fixed RK3566 device ranges (0xfdd20000 and
  friends) fell inside the RAM bank and the early MMU build rejected the
  whole identity map.  Fixed ranges that collide with the RAM firmware
  reported are now ignored; device-tree ranges still fail hard.

With those fixed, `clang++ hello.cpp -o hello` compiles, links against the
guest's libc++ and APEX archives, and the resulting static binary runs on the
ARM64 guest and prints its marker.

`util/arm64/test-selfbuild.py` then goes one step further: it stages the
LLVM headers, `llvm/tools/llvm-as/llvm-as.cpp` and the prebuilt ARM64 LLVM
archives into the SDK archive and boots a driver that compiles the TU with
the guest's own clang, links it with the guest's `ld.lld`, runs the result
and compares the bitcode byte for byte with a host build of the same
revision.  On the one-vCPU TCG harness the compile takes about 31 s and the
link about 1.3 s; the same TU cross-compiles on the host in 1.9 s, so TCG
costs roughly 16x for this workload.  A Cortex-A55 is much slower than the
host core and the board has four of them, so board builds should be far
closer to the host figure.

`--tool llvm-dis` builds `llvm/tools/llvm-dis/llvm-dis.cpp` and compares the
disassembly output (the `; ModuleID =` path line is ignored because the host
and guest input paths differ).  `--tool llvm-ar` builds both
`llvm-ar.cpp` and the generated `llvm-ar-driver.cpp`, links the
DlltoolDriver/LibDriver/AArch64 libraries, and then creates and lists an
archive.  All three pass:

```text
SB compile EXIT 0
SB link EXIT 0
SB MATCH 1852 bytes        # llvm-as
SB MATCH 261 bytes         # llvm-dis
SB ar-create EXIT 0        # llvm-ar
SB ar-list EXIT 0
SELFBUILD-DONE
```

`install-sdk.py --with-ar` also ships the cross-built `llvm-ar` and
`llvm-ranlib` as ready-to-run guest tools.  `test-tools.py` exercises the
shipped set: it compiles `testdata/x.c` with the SDK `clang`, creates and
lists `libx.a` with `llvm-ar`, and runs `llvm-ranlib`:

```text
ART cc OK
ART ar-create OK
ART ar-list OK
ART ranlib OK
ART-DONE
toolchain tools PASS
```

`test-fmtlib.py` goes one step further: it compiles `fmt/src/format.cc` and
a probe as separate objects with the guest `clang++`, archives `format.o`
with the shipped `llvm-ar`, links the probe against `libfmt.a` and the SDK
runtime, and runs it:

```text
FMTLIB compile-format OK
FMTLIB compile-probe OK
FMTLIB archive OK
FMTLIB link OK
harvey 042 3.50 [1, 2, 3]
FMTLIB-DONE
fmt library build PASS
```

The 32 MiB heap limit noted above is gone: `qmalloc.c` draws new chunks from
the physical allocator, doubling up to 64 MiB, and ramfs block maps grow on
demand.

## CMake

`harvey-aarch64-toolchain.cmake` lets the host's CMake cross-build a Harvey
project:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=util/arm64/harvey-aarch64-toolchain.cmake
ninja -C build
```

`test-cmake.py` builds a C library plus a C++20 executable (`<ranges>`),
checks the generated `compile_commands.json` (so clangd can consume it), and
boots the result in QEMU:

```text
CMAKE-PROJECT CMake-Harvey 12 3
CMAKE-PROJECT-DONE
CMake cross-build PASS
```

The wrapper defines `_PLAN9_SOURCE`, `_BSD_EXTENSION`, `_REENTRANT_SOURCE`
and `HAVE_SOCK_OPTS` and suppresses `-Wunused-command-line-argument`, because
CMake treats any warning in a `try_compile` feature probe as "unsupported".

A *native* CMake is built in "lean" mode: `lean-cmake.py` applies
`util/arm64/cmake/lean.diff` to a CMake 3.28 source tree and replaces the
bundled cURL, libarchive and libuv with the stubs in `util/arm64/cmake/`.
The result is a 9 MB static `cmake` that runs on Harvey:

```text
cmake version 3.28.3
CMake suite maintained and supported by Kitware (kitware.com/cmake).
```

`test-cmake-native.py` installs it as `/initramfs/cmake/bin/cmake` with a
minimal `share/cmake-3.28/Modules` (including `Platform/Harvey.cmake`), then
runs `cmake --version`, a `cmake -P` script (`CMSCRIPT-OK 3.28.3`) and a
`Unix Makefiles` configure/generate of `project(native NONE)` in QEMU:

```text
-- NPROJ-OK
-- Configuring done (0.1s)
-- Generating done (0.0s)
-- Build files have been written to: /initramfs/cmake/proj/build
```

Downloads, archive extraction and `cmake --workflow` are not available in
lean mode and report a clean error.

The Ninja generator is not exercised: it runs `ninja --version` through
KWSys, whose non-blocking pipe polling needs descriptor readiness that APEX
does not provide yet.  Unix Makefiles generate without spawning the make
program.  Building the generated files still needs a guest make or ninja;
that pairs with the same readiness work (and with `execute_process`).
