#!/bin/sh
# mkgen.sh: regenerates the headers Inferno's limbo derives from its module
# definitions, by purgatorio's own rules (libinterp/mkfile), into this
# directory.  build_emu.py needs them and purgatorio does not commit them,
# so they are kept here: a build then needs no limbo.  Run it, with limbo
# on PATH, whenever build_emu.py moves to a purgatorio commit whose module
# definitions differ; PURGATORIO records the commit they came from.
set -eu
gen=$(cd "$(dirname "$0")" && pwd)
purg=${PURGATORIO:-$(cd "$gen/../../../../.." && pwd)/purgatorio}
cd "$purg/libinterp"
limbo -a -I../module ../module/runt.m > "$gen/runt.h"
limbo -t Sys -I../module ../module/runt.m > "$gen/sysmod.h"
limbo -t Keyring -I../module keyringif.m > "$gen/keyring.h"
limbo -t Math -I../module ../module/runt.m > "$gen/mathmod.h"
limbo -t Loader -I../module ../module/runt.m > "$gen/loadermod.h"
git -C "$purg" rev-parse HEAD > "$gen/PURGATORIO"
