#!/bin/sh
# interop9p.sh - drive an external Plan 9 client against the esp9 device.
#
# Everything tested so far is our own client talking to our own server,
# which is self-confirming. This script uses plan9port's 9p, which we did
# not write, so agreement means something.
#
# usage: interop9p.sh 'tcp!192.168.4.1!564' [path-to-read]
#        interop9p.sh 192.168.4.1 564 [path-to-read]
#
# Exit 0 only if version, attach, walk, readdir and stat all succeed.
set -u

if [ $# -eq 0 ]; then
	echo "usage: $0 'tcp!HOST!PORT' [path]" >&2
	echo "       $0 HOST PORT [path]" >&2
	exit 2
fi

if [ $# -ge 2 ] && [ "$2" -eq "$2" ] 2>/dev/null; then
	ADDR="tcp!$1!$2"
	shift 2
else
	ADDR=$1
	shift
fi
REMOTE=${1:-}

NINE=${NINE:-/usr/local/plan9/bin/9p}
if [ ! -x "$NINE" ]; then
	echo "FAIL: plan9port 9p not found at $NINE (set NINE=...)" >&2
	exit 2
fi

# 9p blocks forever on an unreachable server, so every step is bounded.
TMO=${TMO:-15}
if ! command -v timeout >/dev/null 2>&1; then
	echo "FAIL: no timeout(1); refusing to run an unbounded client" >&2
	exit 2
fi

echo "address: $ADDR"
echo "client:  $NINE"
echo

fail=0
step() {
	desc=$1
	shift
	printf '%-26s ' "$desc"
	out=$(timeout "$TMO" "$@" 2>&1)
	st=$?
	if [ $st -eq 0 ]; then
		printf 'ok\n'
		[ -n "${VERBOSE:-}" ] && printf '%s\n' "$out" | sed 's/^/    /'
		return 0
	fi
	if [ $st -eq 124 ]; then
		printf 'FAIL: no response within %ss\n' "$TMO"
	else
		printf 'FAIL (exit %s): %s\n' "$st" "$(printf '%s\n' "$out" | head -1)"
	fi
	fail=1
	return 1
}

step "version + attach + stat /" "$NINE" -a "$ADDR" stat /
step "readdir /"                 "$NINE" -a "$ADDR" ls -l /
step "walk /wifi"                "$NINE" -a "$ADDR" ls /wifi
step "walk /dev"                 "$NINE" -a "$ADDR" ls /dev

if [ -n "$REMOTE" ]; then
	step "read $REMOTE"          "$NINE" -a "$ADDR" read "$REMOTE"
fi

echo
if [ $fail -eq 0 ]; then
	echo "INTEROP: PASS - an external Plan 9 client mounted and read this device."
	exit 0
fi
echo "INTEROP: FAIL - see the step above. Distinguish:"
echo "  connection refused/timeout -> transport or listener, not 9P"
echo "  'bad version'              -> Rversion/msize negotiation"
echo "  'file does not exist'      -> walk or the flash file table"
echo "  hangs                      -> msize larger than the device buffers"
exit 1
