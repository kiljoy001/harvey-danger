# gpucap — record the job chain a real Mali driver submits

The missing piece in the G52 bring-up is a *reference* job chain: the
descriptor bytes a working driver puts on the wire.  This directory records
one without touching the board, by running Mesa's own panfrost Gallium driver
against Mesa's **panfrost DRM shim**.

The shim (`src/panfrost/drm-shim/panfrost_noop.c`) is the fake `/dev/dri`
device Mesa's CI uses to run the real driver with no GPU.  It keeps every
fake buffer object in host memory (`struct shim_bo { mem_addr, map, size }`)
and hands out `bo->mem_addr` as the BO offset, so a GPU virtual address in
`struct drm_panfrost_submit.jc` can be resolved straight back to the bytes
the driver wrote.  The patch here adds a `PANSHIM` dump to
`DRM_PANFROST_SUBMIT`.

## Build

Mesa 25.0.7 is the last release before panfrost's OpenCL runtime pulled in
`mesa_clc`, libclc, SPIRV-Tools and LLVM.  Mesa 26 will not configure with
`-Dllvm=disabled`.

```sh
curl -O https://archive.mesa3d.org/mesa-25.0.7.tar.xz
tar xf mesa-25.0.7.tar.xz
patch -p0 -d mesa-25.0.7 < mesa-shim-submit-dump.patch

meson setup build mesa-25.0.7 \
  -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dtools=drm-shim \
  -Dglx=disabled -Dgles1=disabled -Dgles2=enabled -Dopengl=false \
  -Dplatforms= -Dllvm=disabled -Dbuild-tests=false \
  -Dvideo-codecs= -Dgallium-rusticl=false
ninja -C build
```

## Run

```sh
sed -i "s|mesa25|build|g" run-capture.sh
DRM_SHIM_DEBUG=1 ./run-capture.sh
```

Each submit prints:

```
PANSHIM SUBMIT jc=0x... reqs=0x0 in=0 out=0
PANSHIM chain gpuva=0x... offset=0x... len=256
PANSHIM 0000: 1000f    <- JOB_HEADER word 4: is64b|type<<1|index<<16
PANSHIM 0004: 0
...
```

`jc` is the first job header of the chain, so word 4 is the one that matters
(`type`, `job_index`, `is_64b`).

## Status

The shim and the capture program build and the driver gets as far as the
loader.  Mesa 25's in-tree DRI module (`libgallium-25.0.7.so`) is not
installed as `…/dri/panfrost_dri.so` and does not export the driver
descriptor the 25.0 loader looks for, so `eglInitialize` fails with
`DRI2: failed to load driver`.  Next steps, in order of least effort:

1. `ninja -C build install` into a real prefix (not a destdir) and point
   `LIBGL_DRIVERS_PATH` at its `lib/x86_64-linux-gnu/dri`.
2. Or build Mesa 24.x, whose DRI module is the classic
   `__driDriverGetExtensions_panfrost` shared object.
3. Or skip the loader: add the same dump to Mesa's `panfrost` pool
   allocator and build a small program that calls the descriptor emitters
   directly.

The alternative, if the board is free, is to boot Linux with panfrost and
read the chain from a `devcoredump` (submit a job that hangs) or with
`PAN_CPU_TRACE=lib.desc` on Mesa 26, which dumps descriptors from userspace.
