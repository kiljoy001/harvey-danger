# What Mesa does, and what a Harvey GPU stack should be

This is the design note behind `gpufs`. It answers two questions:

1. What is Mesa actually responsible for?
2. If we do not port Mesa, what is the smallest stack that gives the display
   the GPU's power?

The short answer: Mesa is a **userspace compiler and state tracker**, not a
driver. The kernel driver owns memory, the MMU, job submission, power and
interrupts. That split is the useful part, and it is the split Harvey should
copy. The compiler is the expensive part, and Mesa already ships an offline
compiler we can run on the host.

## The panfrost stack, layer by layer

```text
application
   |  GL / GLES / EGL / Vulkan / OpenCL
   v
Mesa frontends            src/mesa, src/egl, src/panfrost/vulkan, clc
   |  API state, objects, validation
   v
Gallium state tracker     src/gallium/state_trackers/*
   |  pipe_context / pipe_resource / pipe_fence
   v
Panfrost Gallium driver   src/gallium/drivers/panfrost      21k lines
   |  resources, tiling, formats, clear/blit, command emission
   v
Panfrost compiler         src/panfrost/compiler             46k lines
   |  GLSL/SPIR-V -> NIR -> Midgard/Bifrost machine code
   v
Panfrost descriptor lib   src/panfrost/lib + genxml         14k lines
   |  job chain, renderer state, textures, framebuffer, MMU descriptors
   v
DRM UAPI                  DRM_IOCTL_PANFROST_*
   |  CREATE_BO, MMAP_BO, SUBMIT, WAIT_BO, GET_PARAM
   v
Panfrost kernel driver    drivers/gpu/drm/panfrost          3.5k lines
   |  GEM, GPU MMU (io-pgtable), job scheduler, IRQ, power
   v
Mali G52
```

The important boundaries:

| Concern | Mesa | Panfrost kernel |
|---|---|---|
| API and object state | yes | no |
| Shader source to machine code | yes (NIR + Bifrost backend) | no |
| Resource layout (tiling, stride, AFBC) | yes | no |
| Command/descriptor encoding | yes (job chain, renderer state) | no |
| Buffer objects and GPU virtual addresses | handles via ioctls | GEM + GPU MMU |
| Job queue and dependencies | builds the chain | submits it, schedules slots |
| Interrupts, fault handling, reset | no | yes |
| Power domains, clocks, DVFS | no | yes |
| Scanout / modesetting | no (KMS is elsewhere) | no |

Two details matter for us:

- **Mesa does not touch the GPU's MMIO registers.** It builds a **job chain** in a buffer
  object: a linked list of 64-byte job headers (`WRITE_VALUE`, `VERTEX`,
  `TILER`, `FRAGMENT`, `COMPUTE`, `FUSED`, `CACHE_FLUSH`) with scoreboard
  dependencies. The kernel receives the GPU address of the first header in
  `drm_panfrost_submit.jc` and writes `JS_HEAD_NEXT`, `JS_CONFIG` and
  `JS_COMMAND_START`. The kernel never interprets the job chain.
- **The Bifrost ISA has no public specification.** Mesa's
  `src/panfrost/compiler/bifrost` is the reverse-engineered implementation:
  105 files, about 30k lines, plus NIR (130k lines) and Mesa's utility
  libraries. It generates the instruction stream, register allocation,
  scheduling, packing and the shader metadata (`pan_shader_info`: registers,
  workgroup size, I/O locations, early-Z, TLS, etc.).

## Measured sizes (Mesa 26.1.6)

| Component | Scale | Portable to Harvey? |
|---|---|---|
| `src/panfrost/compiler` (Midgard+Bifrost+NIR lowerings) | 46k lines | compiler is C, but needs NIR/util; not a first step |
| `src/panfrost/compiler/bifrost` | 105 files, 30k lines | host-only is realistic |
| `src/compiler/nir` | 130k lines | host-only |
| `src/gallium/drivers/panfrost` | 41 files, 21k lines | no: Gallium + DRI + Linux winsys |
| `src/panfrost/lib` + `genxml` | 14k lines | a subset is useful for descriptors |
| Linux panfrost MMU + job + GEM | 928 + 997 + 490 lines | yes, simplified, no DRM scheduler |

The compiler itself builds standalone. With Mesa's build system:

```sh
meson setup build mesa -Dgallium-drivers= -Dvulkan-drivers= -Dtools=panfrost \
  -Dglx=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled \
  -Dopengl=false -Dplatforms= -Dllvm=disabled -Dbuild-tests=false
ninja -C build src/panfrost/clc/panfrost_compile
```

That took **36 seconds** on this host, with no LLVM and no Gallium. The
resulting `panfrost_compile` takes SPIR-V, a GPU arch (7 for G52), and emits a
C header/blob. The blob starts with:

```c
struct bifrost_precompiled_kernel_info {
    struct pan_shader_info info;   /* registers, workgroup, I/O, ... */
    unsigned local_size_x, y, z;
    unsigned binary_size;
};
```

followed by the Bifrost instruction stream. That is exactly the shape of an
offline shader cache. (The CLI expects the OpenCL library form `mesa_clc` produces; vanilla
Clang OpenCL SPIR-V reached `nir_precomp` with a null variant parameter. A
graphics frontend should call `bifrost_compile_shader_nir()` directly. The
compiler library is the reusable part, not the CLI's input assumptions.)

## What "something similar" should be

Do **not** port GL, EGL, Gallium or the DRI winsys. They assume Linux file
descriptors, `mmap`, dma-buf, syncobj and a DRM device node. A compatibility
shim for that is larger and less useful than a native seam.

Build the same *seams* with Harvey pieces:

```text
libdraw / o9display                  frontend: 2D ops, surfaces, views
        |
libgpu                               winsys/state: BOs, VA, command list,
        |                            fences, pipeline objects
        |
/srv/gpu (gpufs)                      kernel: GPU MMU, BOs, job queue,
        |                            IRQ/fence, power/clock/reset
        |
Mali G52  --->  VOP2 scanout buffer
```

And one **host-side** tool:

```text
gpucc (Linux host, Mesa libs)         SPIR-V/NIR -> Bifrost blob + metadata
        |                            (fixed shader library, or dynamic later)
        v
/lib/gpu/*.bin                        shader cache consumed by libgpu
```

The split is deliberately asymmetric:

- **Reuse Mesa where it is strong**: the Bifrost compiler, NIR, the
  precompiled-blob format, `genxml` descriptor layouts, the reverse-engineered
  ISA and errata knowledge.
- **Write the small native runtime**: GPU MMU, BOs, job submission, fences,
  a command encoder for the fixed pipelines, display sharing.
- **Skip the Linux-shaped parts**: Gallium, GL/EGL/Vulkan frontends, DRI,
  GEM/shmem, dma-buf, syncobj.

## The first four milestones

1. **GPU MMU and a job with no shader.** Mali LPAE page tables, one address
   space, map a BO, submit a `WRITE_VALUE` or `NULL` job, wait for completion,
   read the BO back. This proves memory, submission and fences without the
   compiler.
2. **First pixels on HDMI.** A precompiled full-screen clear/quad pipeline
   writes the VOP framebuffer (`vopfb.pa`, linear XRGB8888). `devvop` exposes
   the framebuffer physical address and geometry; `gpufs` maps those pages
   into the GPU address space. The display already scans that buffer, so the
   result is visible with no compositor changes.
3. **A textured blit.** One surface BO, one sampler, a precompiled blit
   pipeline; GPU copies/composites a surface into the scanout buffer. This is
   the first useful "GPU into the display" operation.
4. **`o9display` on GPU buffers.** Surfaces become GPU BOs (or are uploaded to
   them), `commit` submits a composition job and waits on its fence. Keep the
   CPU path as the fallback for text, paths, and unsupported formats.

Only after those should we consider a `libdraw` backend, a general pipeline
compiler on-device, or a 3D API. The full Mesa/GL stack is not a prerequisite
for any of them.

## Risks and non-goals

- **Linear render targets.** VOP2 scans linear XRGB8888. Bifrost prefers tiled
  or AFBC targets; the first pipelines must render linear, or render tiled and
  resolve with a blit. Verify this early.
- **Coherence.** `COHERENCY = 0` (no ACE/ACE-lite). CPU writes to textures and
  command buffers need cache clean; GPU writes need invalidate before CPU
  reads.
- **One VOP buffer.** The current display has a single scanout buffer. A GPU
  job that takes longer than a frame will tear or block; double buffering (two
  VOP-mapped buffers, flip on fence) is a display-driver change, not a GPU one.
- **No dynamic shaders initially.** The fixed pipeline library is enough for
  fills, blits, gradients and simple compositing. Arbitrary GLSL needs `gpucc`
  on a host or a NIR/Bifrost port.
- **Non-goals for now**: GL/EGL/Vulkan compatibility, Mesa's Gallium, DRI,
  dma-buf, Steam, and compute frameworks. Those are separate projects built on
  the same four milestones.

The point of the GPU work is not to run Mesa. It is to give Harvey the same
division of labour Mesa relies on: compiled shaders and encoded jobs in
userspace, memory/MMU/queues in the GPU service, and a display that scans out
what the GPU wrote.
