# Inferno on 9vm — build and boot harness

The purgatorio Inferno kernel running natively on the 9vm KVM hypervisor:

- 32-bit x86 kernel, no Linux under it
- root filesystem is **kfs64 on virtio-blk** (`disk/kfs64` serving 9P over a pipe)
- display and input through a shared-memory region the host maps at guest
  physical `0xF8000000` (no 9P in the pixel path)
- Dis JIT enabled at boot (`cflag=1`); a switch back to the interpreter is
  `echo 0 > /dev/jit` inside the guest

This directory holds the scripts that reproduce the build → provision → boot
loop.  Everything here was used to validate the state described at the bottom.

## Files

| file | purpose |
|---|---|
| `mkelf.py` | convert `os/pc/ipc` (Plan 9 a.out) to `ipc.elf` for 9vm, plus `ipc-qemu.elf` for QEMU |
| `aout2elf.py` | the a.out → ELF32 converter mkelf.py calls |
| `provision.sh` | create a 32 MiB image and populate it with `/dis`, `/icons`, `/fonts/misc`, `/lib/wmsetup` |
| `provision.txt` | the shell script fed to the guest (mount `kfs64 -r -b 4096`, copy, mkdir device dirs) |
| `boot.sh` | boot 9vm with `-disk` and optionally `-display-shm` |
| `draindisp.py` | consume committed frames from `/dev/shm/<name>` and write a PPM |
| `sendinput.py` | inject input records (`key 104 0`, `move X Y`, …) into the same region |
| `makesymelf.py` | build `ipc-sym.elf` (symbols) from the a.out, for gdb/symlookup |
| `symlookup.py` | address → kernel symbol, using `ipc-sym.elf` |
| `root.img.gz` | a known-good provisioned 32 MiB kfs64 image (optional; `provision.sh` rebuilds it) |

## Prerequisites

- 9vm built: `make -C util/9vm 9vm`
- a purgatorio checkout next to this one (`../purgatorio`; override with `$PURGATORIO`)
- the Inferno 386 toolchain in `$PURGATORIO/Linux/386/bin` and `mk` on `PATH`
- `$NINEVM_WORK` (default `/tmp/9vm-inferno`) is where build outputs and images go

## Build

```sh
cd ../purgatorio/os/pc
ROOT=../.. PATH=../../Linux/386/bin:/usr/local/plan9/bin:$PATH mk
cd -
./mkelf.py                # writes $NINEVM_WORK/ipc.elf
```

The kernel embeds `/osinit.dis` and the root tree from `os/pc/pc`, so rebuilding
the init means rebuilding the kernel:

```sh
cd ../purgatorio/os/init
ROOT=../.. PATH=../../Linux/386/bin:/usr/local/plan9/bin:$PATH mk shell.dis
# then rebuild os/pc again and re-run mkelf.py
```

## Provision a root image

```sh
./provision.sh            # ~1 minute; writes $NINEVM_WORK/root.img
```

or use the bundled baseline:

```sh
mkdir -p /tmp/9vm-inferno
gunzip -c root.img.gz > /tmp/9vm-inferno/root.img
```

`provision.sh` boots a blank disk, runs `disk/kfs64 -r -b 4096` on
`#S/sd00/data`, and copies from the embedded root.  With `-b 4096` the ream
covers the whole disk up front (16 dentries per directory block instead of 4).

## Boot

Headless (serial console on stdin/stdout):

```sh
./boot.sh
```

With the display:

```sh
./boot.sh -s inferno
```

Then, in another shell, grab frames and type at it:

```sh
python3 draindisp.py inferno 10 /tmp/9vm-inferno/frame.ppm   # 10 s of frames
python3 sendinput.py inferno 'key 104 0' 'key 105 0' 'key 13 0'   # h, i, Enter
```

Inside the guest:

```sh
/dis/wm/wm.dis &        # window manager; needs the wm assets on the root
/dis/acme/dis/acme.dis  # editor
```

A healthy `wm` on the kfs root renders a `0x777777` background with the
`0xdddddd` toolbar; `tk: font not found: /fonts/pelm/unicode.8.font` is
expected and harmless (it falls back to the built-in font).

## Baseline measured on this setup

- `cp -r /dis` (783 files, 7.6 MB): ~7.7 s
- local syscall on the embedded root: ~40 µs
- 9P RPC on the kfs mount: ~870 µs (8 µs write, ~860 µs waiting for the kfs)
- bare pipe round-trip between two Dis programs: ~40 µs
- filesystem provisioning of the full root: ~1 minute

`^T^T D` on the serial console dumps the Dis scheduler, every `Prog`, and the
kernel stacks; `^T^T K` dumps the console queues and the `kbddrops` counter.
`/dev/jit` reads and writes the Dis JIT flag.

## Known limits

- The kernel is 32-bit (there is no 64-bit `os/` port in Inferno; inferno64 /
  infernode are 64-bit `emu` builds only).  `uvlong` is 4-byte aligned under
  `8c`, so on-disk structures need byte-wise packing.
- kfs64 is a simple write-through 9P server: every block write is synchronous
  and every block allocation rewrites the superblock.  It is frozen at
  cheap fixes; host-side storage is the intended way around it.
- The host tree is not mounted yet.  The next step is the 9ring host transport
  (`#~/host`), so `/dis`, `/icons` and `/fonts` can be served from the host
  instead of provisioned into the image.
