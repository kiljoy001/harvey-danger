#!/bin/sh
# Create a 32 MiB kfs64 image and populate it from the guest's embedded root.
#
#   ./provision.sh [ipc.elf] [root.img]
#
# Boots 9vm headless, mounts the blank virtio disk with
#   disk/kfs64 -r -b 4096 '#S/sd00/data'
# and runs provision.txt (which copies /dis, /icons, /fonts/misc,
# /lib/wmsetup and /osinit.dis into it).  Takes roughly a minute.
#
# Override the work directory with $NINEVM_WORK (default /tmp/9vm-inferno).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
work=${NINEVM_WORK:-/tmp/9vm-inferno}
elf=${1:-$work/ipc.elf}
img=${2:-$work/root.img}
vm=$repo/util/9vm/9vm
log=$work/provision.log

mkdir -p "$work"
[ -x "$vm" ] || { echo "build $vm first (make -C $repo/util/9vm)" >&2; exit 1; }
[ -f "$elf" ] || { echo "missing $elf (run ./mkelf.py)" >&2; exit 1; }

rm -f "$img"
dd if=/dev/zero of="$img" bs=1M count=32 status=none

# The feeder exits as soon as the script is written; the 9vm keeps running
# until we kill it, so no long-lived child is left holding our descriptors.
( sleep 10; cat "$here/provision.txt" ) | "$vm" \
	-c "bootargs=local!#9/0 nobootprompt=local console=0" \
	-disk "$img" "$elf" >"$log" 2>&1 &
pid=$!

i=0
ok=
while [ "$i" -lt 180 ]; do
	n=0
	if [ -f "$log" ]; then
		n=$(grep -c 'PROVISION-OK' "$log" || true)
	fi
	if [ "${n:-0}" -ge 2 ]; then	# once echoed, once executed
		ok=1
		break
	fi
	sleep 1
	i=$((i + 1))
done
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

if [ -z "$ok" ]; then
	echo "provision failed; see $log" >&2
	tail -20 "$log" >&2
	exit 1
fi
echo "provisioned $img"
