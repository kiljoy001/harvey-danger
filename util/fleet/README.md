# Fleet registry

A tiny 9P app registry for the tablet home screen.  The directory is
served by `u9fs`; the tablet mounts it at `fleet/` and merges `apps`
with its local `tab/apps` registry.

```sh
./serve.sh /path/to/fleet 5655
```

The default bind is `127.0.0.1`.  For the tablet on the LAN, pass
`0.0.0.0` as the fourth argument; only do that on a trusted network.

`apps` is a libtab document:

```text
schema=app

name=cartoons
dis=files
mode=once
snap=fleet/cartoons.status
context=files:remote/babyfs/usr/scott/cartoons
```

`context=files:<path>` opens the tablet Files view at a virtual path
when the tile is tapped.  `snap=fleet/<name>` reads the tile subtitle
from this directory.

Keep `u9fs` on loopback or Ygg; `-a none` has no authentication.

The next step is to replace this static directory with a `9vmctl`
service that publishes the same `apps`/status files from the live
fleet.
