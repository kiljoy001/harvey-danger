# Starting and packaging Aspera

## Start the current desktop

From this checkout, after building the VM, kernel, Objective-9C libraries,
and guest GEFS server:

```sh
./bin/aspera check
./bin/aspera start
```

`start` runs in the foreground and exits when the SDL window closes. It can
be invoked from another working directory by its absolute path. The current
desktop is a development boot: it recompiles `live.o9` and the shelf service,
then runs the VM against this checkout's host 9P tree. `--smoke` drives a
short, headless input and Notes save/reopen check. If Objective-9C is not in
the sibling `objective-9c` checkout, set `O9_REPO` to its source root.

For a new source checkout with 9front source in a sibling `plan9front`
directory, the current build sequence is:

```sh
for lib in libc libbio libndb libsec libauth libauthsrv libthread lib9p libdraw libo9; do
    python3 "util/9vm/build_${lib}.py" || exit
done
python3 util/9vm/build_gefs_guest.py
python3 util/9vm/build_kernel.py
make -C util/9vm 9vm
./bin/aspera check
```

Set `O9_REPO` for Objective-9C and pass `--source /path/to/plan9front` to
`build_gefs_guest.py` if the checkouts are elsewhere. This is a source build;
the runtime bundle below is not assembled yet.

The built guest kernel is `build/harvey.32bit`, the host runner is
`util/9vm/9vm`, and Objective-9C archives are under `build/`. `check` names
missing inputs. The build instructions and transport tests are in
[README.md](README.md).

## Runtime bundle target

The installable package should contain only runtime artifacts:

```text
bin/aspera                    installed launcher
libexec/aspera/9vm           KVM runner
libexec/aspera/display-sdl   host window and input adapter
libexec/aspera/harvey.32bit  guest kernel
libexec/aspera/desktop       compiled Objective-9C desktop
libexec/aspera/shelf-service compiled Objective-9C shelf service
share/aspera/root.img        pristine GEFS image template
```

At first launch, copy the image template to a user-owned data directory and
boot that copy. Updates replace executables and the template; they must not
overwrite a user's image. Runtime state (including display exchange files,
logs, and locks) belongs in a user-owned runtime directory, with a separate
directory per VM instance. A package build should generate a manifest with
hashes and toolchain/source revisions, then verify a boot and Notes save/reopen
from the assembled bundle itself.

Before producing that bundle, separate guest compilation from VM startup,
remove source-checkout paths from the host runner, install the guest desktop
and services into GEFS, and make the isolated GEFS boot start the graphical
session. The current image boot mounts persistent storage and starts the IRC
service, but the desktop window is still launched through the development
host-tree path. These are packaging gates, not properties of the current
`bin/aspera start` command.
