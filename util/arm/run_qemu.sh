#!/bin/sh
# Run the ARMv7 "virt" kernel.  Cortex-A7 matches the goyave tablet's
# cores; override with CPU=, MEM= or by passing extra QEMU arguments.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
image=${IMAGE:-$root/build/arm32-kernel/harvey-arm32}
exec qemu-system-arm -M virt -cpu "${CPU:-cortex-a7}" -m "${MEM:-512}" \
	-kernel "$image" -nographic "$@"
