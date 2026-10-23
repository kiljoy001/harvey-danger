#!/bin/sh
# Boot the Inferno guest on 9vm.
#
#   ./boot.sh [-s SHMNAME] [-c CMDLINE] [image] [ipc.elf]
#
# Without -s the guest runs headless on the serial console (stdin/stdout of
# this script).  With -s the 9vm display region is created as /dev/shm/SHMNAME
# and mapped into the guest at 0xF8000000; drive it with draindisp.py and
# sendinput.py using the same name.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
work=${NINEVM_WORK:-/tmp/9vm-inferno}
shm=
cmd="bootargs=local!#9/0 nobootprompt=local console=0"
while [ $# -gt 0 ]; do
	case "$1" in
	-s) shm=$2; shift 2 ;;
	-c) cmd=$2; shift 2 ;;
	-h|--help) sed -n '2,10p' "$0"; exit 0 ;;
	*) break ;;
	esac
done
img=${1:-$work/root.img}
elf=${2:-$work/ipc.elf}

[ -x "$repo/util/9vm/9vm" ] || {
	echo "build 9vm first: make -C $repo/util/9vm 9vm" >&2; exit 1; }
[ -f "$img" ] || {
	echo "missing $img (run ./provision.sh, or gunzip root.img.gz)" >&2; exit 1; }
[ -f "$elf" ] || { echo "missing $elf (run ./mkelf.py)" >&2; exit 1; }

set -- -c "$cmd" -disk "$img"
if [ -n "$shm" ]; then
	# shm_open(3) needs the leading slash; 9vm creates with O_EXCL, so a
	# stale region from a previous run would make it fail.
	shm=/${shm#/}
	rm -f "/dev/shm/${shm#/}"
	set -- "$@" -display-shm "$shm"
fi
exec "$repo/util/9vm/9vm" "$@" "$elf"
