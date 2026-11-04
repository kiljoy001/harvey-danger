# gpucc — host-side Bifrost shader compiler

`gpucc` is the first half of the precompiled-shader path for `gpufs`. It
uses Mesa's NIR and Bifrost backend directly, with no Gallium, DRM, LLVM or
EGL, and emits a Bifrost binary for the Mali-G52.

It compiles both shaders for a full-screen triangle:

- **fragment** (`frag.bin`, 320 bytes): writes opaque red to
  `FRAG_RESULT_COLOR`; the disassembly shows `ATEST`/`BLEND` writing
  `r0`/`r1`.
- **vertex** (`vert.bin`, 160 bytes): reads a `vec4` position from
  attribute 0 and stores it to `VARYING_SLOT_POS`.

The vertex shader uses **attribute 0**, not `gl_VertexID`. The vertex-ID
path needs the Gallium driver's IDVS/raw-vertex-offset preload; the
attribute path avoids that and is what the tiler job will feed.

## Build

Use Mesa 26.1.6 source. Configure it so the panfrost compiler and tools are
built without Gallium/Vulkan:

```sh
meson setup build mesa \
  -Dgallium-drivers= -Dvulkan-drivers= -Dtools=panfrost \
  -Dglx=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled \
  -Dopengl=false -Dplatforms= -Dllvm=disabled -Dbuild-tests=false \
  -Dvideo-codecs= -Dgallium-rusticl=false
```

Copy the files in:

```sh
cp gpucc.c mesa/src/panfrost/clc/
cat gpucc.meson >> mesa/src/panfrost/clc/meson.build
ninja -C build src/panfrost/clc/gpucc
```

## Run

```sh
cd /tmp/gpucc
/path/to/mesa/build/src/panfrost/clc/gpucc
# frag: binary 320 bytes stage 4 tls 0 outputs 0x4
# vert: binary 160 bytes stage 0 tls 0 outputs 0x1
# frag.bin vert.bin
```

The raw GPU ID passed to the compiler is `0x74021000` (G52, arch 7,
product 0x7002, r1p0). `pan_arch()` extracts 7 from it; the Bifrost backend
uses the v6 NIR options for arch < 9.

## What is still missing

`gpucc` is the compiler half only. The other half is the **fragment job
chain**:

- `FRAGMENT_JOB` header and payload (tile bounds, framebuffer pointer)
- `RENDERER_STATE` (shader pointer, early-Z, blend, output format)
- `FRAMEBUFFER`/render-target descriptors
- `SHADER_PROGRAM` descriptor built from the blob and `pan_shader_info`
- a `TILER_JOB` with the vertex shader, an attribute buffer holding the
  three `vec4` positions, the tiler context/heap, and the fragment RSD/blend
- a `FRAGMENT_JOB` with the FBD for the VOP scanout buffer
- the tiler -> fragment chain, with the shader/attribute/FBD/tiler GPU
  addresses patched at submit time

The vertex shader reads attribute 0, so the tiler job must provide a
three-vertex attribute buffer; there is no vertex-ID preload path yet.

`gpufs` can already map a buffer into the GPU's A64 page tables and submit a
job; it still rejects the synthetic `NULL`/`WRITE_VALUE` jobs. The next
step is to emit the real fragment chain around this blob, using panfrost's
`genxml`/`pan_desc` layout as the reference.

## SPIR-V input

`gpucc` also takes SPIR-V, which is how shaders are written from here on:

```sh
glslc --target-env=opengl -fshader-stage=vert blit.vert -o blit.vert.spv
glslc --target-env=opengl -fshader-stage=frag blit.frag -o blit.frag.spv
gpucc blit.frag.spv blit.vert.spv
```

The SPIR-V path runs `spirv_to_nir` and then the panfrost pipeline
(`nir_lower_system_values`, `nir_lower_io`, `pan_optimize_nir`,
`pan_preprocess_nir`, `pan_postprocess_nir`) before `pan_shader_compile`.
Three things had to be added on top:

- **viewport sysvals.** `gl_Position` is clip space, and the GLSL path
  leaves the viewport transform to `load_viewport_scale/offset`.  Language
  doesn't give gpufs per-draw uniforms yet, so those are inlined as
  constants (960/540 for the 1080p scanout) after postprocess, where they
  first appear.
- **sampler derefs.** Bifrost wants a constant `texture_index`/
  `sampler_index`, not `nir_tex_src_texture_deref`; they are stripped and
  replaced with index 0 (we bind one texture).
- **counts.** `spirv_to_nir` leaves `attribute_count`, `texture_count` and
  `sampler_count` at zero because there is no API binding table, so
  `inputs_read` is set for the vertex stage and the texture/sampler counts
  are restored after the compile.

`blit.vert`/`blit.frag` are the first pair: a full-screen triangle and a
sampling fragment shader driven by `gl_FragCoord`.
