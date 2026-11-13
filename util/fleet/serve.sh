#!/bin/sh
# Serve the fleet directory over 9P for the tablet's Wada view.
# u9fs speaks the protocol on stdin/stdout; socat supplies the listener.
set -eu
root=${1:-$(cd "$(dirname "$0")" && pwd)}
port=${2:-5640}
user=${3:-$(id -un)}
exec socat "TCP-LISTEN:${port},reuseaddr,fork" \
	"EXEC:/usr/local/bin/u9fs -a none -u ${user} ${root}"
