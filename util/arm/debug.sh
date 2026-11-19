#!/bin/sh
# Boot the ARMv7 kernel paused and attach an ARM-capable gdb.
#
#   sh util/arm/debug.sh              # break on sleep, interactive
#   BREAK=tsleep sh util/arm/debug.sh
#   sh util/arm/debug.sh -batch -ex bt -ex detach
#
# Extra arguments go to gdb after it attaches and continues.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
image=${IMAGE:-$root/build/arm32-kernel/harvey-arm32}
port=${GDBPORT:-1235}
serial=${SERIAL:-/tmp/opencode/arm32-serial.log}
brk=${BREAK:-sleep}

if [ ! -f "$image" ]; then
	echo "no image at $image; run util/arm/build_kernel.py first" >&2
	exit 1
fi
command -v gdb-multiarch >/dev/null 2>&1 || {
	echo "gdb-multiarch not found" >&2
	exit 1
}

# A stale QEMU from an earlier session would own the port and gdb would
# attach to a running, already-booted guest.
if ss -ltn 2>/dev/null | grep -q ":$port "; then
	echo "port $port is already in use (stale QEMU?); kill it or set GDBPORT" >&2
	exit 1
fi

qemu-system-arm -M virt -cpu "${CPU:-cortex-a7}" -m "${MEM:-512}" \
	-kernel "$image" -display none -serial "file:$serial" \
	-gdb "tcp::$port" -S >"${QEMULOG:-/tmp/opencode/arm32-qemu.log}" 2>&1 &
qpid=$!
trap 'kill $qpid 2>/dev/null' EXIT INT TERM

# Wait for the stub to listen.  Do not probe with connect(): QEMU's
# gdbstub accepts one client and an aborted connection wedges it.
i=0
while [ $i -lt 100 ]; do
	if ss -ltn 2>/dev/null | grep -q ":$port "; then
		break
	fi
	i=$((i + 1))
	sleep 0.1
done

gdb-multiarch -q \
	-ex "set architecture arm" \
	-ex "set pagination off" \
	-ex "file $image" \
	-ex "target remote :$port" \
	-ex "break $brk" \
	-ex "continue" \
	"$@"

kill $qpid 2>/dev/null || true
wait $qpid 2>/dev/null || true
