#!/bin/sh
# Serve the fleet directory over 9P for the tablet's Wada view.
# u9fs speaks the protocol on stdin/stdout; socat supplies the listener.
#
#   ./serve.sh [root] [port] [user] [bind]
#
# bind defaults to loopback.  Use 0.0.0.0 or a Ygg address only on a
# trusted network: u9fs -a none has no authentication.
set -eu
root=${1:-$(cd "$(dirname "$0")" && pwd)}
port=${2:-5655}
user=${3:-$(id -un)}
bind=${4:-127.0.0.1}
exec socat "TCP4-LISTEN:${port},bind=${bind},reuseaddr,fork" \
	"EXEC:/usr/local/bin/u9fs -a none -u ${user} ${root}"
