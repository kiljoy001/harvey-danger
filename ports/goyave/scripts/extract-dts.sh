#!/bin/sh
# Flatten the goyavewifi DTS with the C preprocessor (kernel #includes) and
# then dtc (kernel /include/). Usage: extract-dts.sh <kernel-dir> [output]
set -eu

k=${1:-upstream/linux}
out=${2:-goyave.flat.dts}
dts="$k/arch/arm/boot/dts"
tmp=$(mktemp)

cpp -P -nostdinc -I "$k/include" -I "$dts" -undef -x assembler-with-cpp \
	"$dts/sprd-scx35_goyavewifi_rev04.dts" -o "$tmp"
dtc -q -i "$dts" -I dts -O dts -o "$out" "$tmp"
rm -f "$tmp"
echo "$out"
