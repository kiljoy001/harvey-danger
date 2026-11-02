#!/bin/sh
# Build objective-9c's o9c for the host with plan9port's 9c/9l.
#
# o9c is a Plan 9 program: the copy in objective-9c/o9c is a native a.out for
# Plan 9 amd64, which a Linux build host cannot run.  plan9port ships 9c, 9l,
# yacc and libbio as ordinary host tools, so the same sources compile into a
# host ELF.  build_o9_services.py looks for build/host/o9c first.
#
#	sh util/arm64/build_o9c.sh [output]
#
# O9_REPO overrides the objective-9c checkout (default ../objective-9c).
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
O9=${O9_REPO:-$ROOT/../objective-9c}
OUT=${1:-$ROOT/build/host/o9c}

: "${PLAN9:=/usr/local/plan9}"
export PLAN9
PATH="$PATH:$PLAN9/bin"
export PATH

for tool in 9c 9l yacc; do
	command -v "$tool" >/dev/null 2>&1 || {
		echo "build_o9c: $tool not found; install plan9port and set PLAN9" >&2
		exit 1
	}
done

SRC=$O9/o9c
test -f "$SRC/o9_type.c" || {
	echo "build_o9c: no o9c sources at $SRC (set O9_REPO)" >&2
	exit 1
}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cp "$SRC/o9_type.c" "$SRC/node.h" "$SRC/o9_type.h" "$WORK/"
if [ -f "$SRC/y.tab.c" ]; then
	cp "$SRC/y.tab.c" "$SRC/y.tab.h" "$WORK/"
else
	(cd "$WORK" && yacc -d "$SRC/grammar.y" >/dev/null)
fi

(
	cd "$WORK"
	9c -I. y.tab.c 2>/dev/null
	9c -I. o9_type.c 2>/dev/null
	9l -o o9c y.tab.o o9_type.o -lbio
)

mkdir -p "$(dirname "$OUT")"
cp "$WORK/o9c" "$OUT"
chmod +x "$OUT"
echo "build_o9c: $OUT"
"$OUT" 2>&1 | head -2 || true
