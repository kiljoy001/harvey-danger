# Starting and packaging Aspera

## Start the current system

From this checkout, after building the VM, kernel, Objective-9C libraries,
and guest GEFS server:

```sh
./bin/aspera check
./bin/aspera start
```

`start` boots the GEFS-backed Aspera system and opens the same Objective-9C
desktop as the development preview. The desktop and shelf service are real
Plan 9 executables installed in the image's `/bin`; the host SDL process only
shows committed frames and forwards input. Notes save under `/lib/aspera` in
GEFS. The image also contains native `rc`, `ls`, `cat`, `test`, and `echo`, so
the guest can inspect its installed applications. The desktop starts with a
terminal; closing it stops that `rc` session. Use **Open Terminal** in the shelf
menu or Alt+Enter to start another. It can be invoked from
another working directory by its absolute path.
Supply a different image with `--image /path/to/image` and guest memory with
`--ram`.

To provision an existing GEFS image, using an installed 9front VM image as the
native GEFS writer:

```sh
python3 util/9vm/install_aspera.py --base /path/to/9front.img --image /path/to/aspera.img
```

The script compiles the desktop and shelf from Objective-9C source, builds the
native shell commands, copies them through a temporary FAT stage disk, checks
the copies, and syncs GEFS.
Keep a backup of an image before provisioning it. `bin/aspera start --smoke`
checks the installed desktop's frame, input, Notes, and terminal command path.
The source VM regression for persistent `rc` state and close/reopen is
`python3 util/9vm/test_aspera_terminal.py`.

The graphical development preview has a separate command:

```sh
./bin/aspera preview
SDL_VIDEODRIVER=dummy ./bin/aspera preview --smoke
```

`preview` compiles `live.o9` and the shelf service to real Plan 9 guest
executables, boots them with 9vm's `-boot` override, and exits when the SDL
window closes. It uses the source checkout's host 9P tree and saves Notes in
`build/aspera-live/note.txt`. It is not the image-backed system boot. If
Objective-9C is not in the sibling `objective-9c` checkout, set `O9_REPO` to
its source root.

For a new source checkout with 9front source in a sibling `plan9front`
directory, the current build sequence is:

```sh
for lib in libc libbio libndb libsec libauth libauthsrv libthread lib9p libdraw libo9; do
    python3 "util/9vm/build_${lib}.py" || exit
done
python3 util/9vm/build_gefs_guest.py
python3 util/9vm/build_kernel.py
make -C util/9vm 9vm
python3 util/9vm/build_netd.py
./bin/aspera check
```

Set `O9_REPO` for Objective-9C and pass `--source /path/to/plan9front` to
`build_gefs_guest.py` if the checkouts are elsewhere. This is a source build;
the runtime bundle below is not assembled yet.

The built guest kernel is `build/harvey.32bit`, the host runner is
`util/9vm/9vm`, and Objective-9C archives are under `build/`. `check` names
missing inputs. The build instructions and transport tests are in
[README.md](README.md).

`install_aspera.py` also installs `/bin/o9drvman` and `/bin/o9driverfs`.
The GEFS bootstrap maps the console through the device filesystem and starts
the manager after the root pivot. Lifecycle markers live under the RAM-backed
`/mnt/o9` tree; trusted device configuration remains under the persistent
system image.

Use an image of at least 512 MiB. The installer rejects smaller images before
starting 9front because GEFS is append only and cannot reliably hold the
desktop, command tools, and managed driver services in 256 MiB.

Run both required development gates before packaging:

```sh
./bin/aspera quality
./bin/aspera verify
```

The quality command runs unit, property, sanitizer, coverage-guided fuzzing,
and mutation tests for user-space policy and kernel transports. `verify` adds
namespace/persistence, Objective-9C driver-manager 9P integration, amd64
9ring/VirtIO fallback, ARM64 VirtIO-MMIO v1/v2 tests, and a managed driver
boot that spans a complete heartbeat interval.

## Runtime bundle target

The installable package should contain only runtime artifacts:

```text
bin/aspera                    installed launcher
libexec/aspera/9vm           KVM runner
libexec/aspera/netd          guest /net service (installed in image)
libexec/aspera/display-sdl   host window and input adapter
libexec/aspera/harvey.32bit  guest kernel
share/aspera/root.img        temporary compatibility image with desktop and shell
```

At first launch, copy the image template to a user-owned data directory and
boot that copy. Updates replace executables and the template; they must not
overwrite a user's image. Runtime state (including display exchange files,
logs, and locks) belongs in a user-owned runtime directory, with a separate
directory per VM instance. A package build should generate a manifest with
hashes and toolchain/source revisions, then verify a boot and Notes save/reopen
from the assembled bundle itself.

Before producing that bundle, remove remaining source-checkout paths from the
host runner, assemble the runtime artifacts and pristine image into a
relocatable bundle, and verify boot and Notes persistence from that bundle.
`bin/aspera start` now runs the installed graphical desktop from GEFS; it is
still a source-checkout launcher.
