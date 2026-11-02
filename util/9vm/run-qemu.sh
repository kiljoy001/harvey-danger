#!/bin/sh
# Boot the amd64 microkernel under plain QEMU, without 9vm or v86.
#
#	sh util/9vm/run-qemu.sh                 # SDL/GTK window
#	sh util/9vm/run-qemu.sh -display none -vnc :1
#	KERNEL=... IMAGE=... sh util/9vm/run-qemu.sh
#
# The kernel is a 64-bit Harvey in a 32-bit Multiboot wrapper (build_kernel.py
# makes it with objcopy); QEMU loads it and the GEFS image as a module.  The
# GEFS bootstrap opens /dev/initrd.  Kernels built since the multiboot name
# fix register that name whatever the loader called the file; older ones named
# the module after the path, so the image is also handed over through a
# symlink called "initrd", which costs nothing and keeps both working.
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KERNEL=${KERNEL:-$ROOT/build/harvey-vps.32bit}
IMAGE=${IMAGE:-$ROOT/build/aspera-vps.gefs}

test -f "$KERNEL" || {
	echo "run-qemu: no kernel at $KERNEL (util/9vm/build_kernel.py)" >&2
	exit 1
}
test -f "$IMAGE" || {
	echo "run-qemu: no GEFS image at $IMAGE (util/9vm/install_gefs.py)" >&2
	exit 1
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
ln -s "$IMAGE" "$TMP/initrd"

# ne2k_pci is the driver the amd64 kernel has; user-mode networking gives it
# a DHCP address the way the browser demo's wisp relay does.
exec qemu-system-x86_64 \
	-m 1G -smp 1 \
	-kernel "$KERNEL" \
	-initrd "$TMP/initrd" \
	-append "bootargs=local!#9/0 nobootprompt=local gefsroot=1" \
	-netdev user,id=n0 -device ne2k_pci,netdev=n0 \
	-vga std \
	"$@"
