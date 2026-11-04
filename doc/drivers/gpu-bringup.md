# Mali-G52 (Bifrost) bring-up

Board: Orange Pi 3B v1.1 (RK3566). GPU: Mali-G52, one shader core, one
tiler, one L2 slice, one core stack, three job slots, eight address spaces.

Status: **identification and feature block work on hardware**. `gpufs`
powers PD_GPU, un-gates the GPU clocks, releases the CRU resets, soft-resets
the GPU, reads the feature registers, and serves them at `/srv/gpu`. There is
no GPU MMU, no job submission, no shader/tiler power management, and no
userspace graphics stack yet.

```text
HARVEY-ARM64 GPU PROBE g52 id 0x7002 rev 0x1000
HARVEY-ARM64 DRVMAN gpu PASS
```

## The register evidence

Read through `/srv/gpu/regs` on the board:

```text
0000 GPU_ID               0x74021000
0004 L2_FEATURES          0x07110206
0008 CORE_FEATURES        0x00000002
000c TILER_FEATURES       0x00000209
0010 MEM_FEATURES         0x00000001
0014 MMU_FEATURES         0x00002823     va 35, pa 40
0018 AS_PRESENT           0x000000ff     8 address spaces
001c JS_PRESENT           0x00000007     3 job slots
0020 INT_RAWSTAT          0x00000000
0028 INT_MASK             0x00030781     fault sources masked
0034 STATUS               0x00000000
003c FAULT_STATUS         0x00000000
00a0 THREAD_MAX           0x00000300     768 threads
0100 SHADER_PRESENT_LO    0x00000001
0120 L2_PRESENT_LO        0x00000001
0300 COHERENCY            0x00000000     no ACE/ACE-lite
```

`GPU_ID` is in the GPU_ID2 format: arch major 7, arch minor 4, product
major 2, version 1.0. Panfrost masks the arch minor off (`& 0xf00f`) before
matching a model, which turns `0x7402` into product `0x7002` — a G52. The
driver does the same, and prints both the model id and the raw register.

The 35-bit VA / 40-bit PA of the GPU MMU are independent of the RK IOMMU;
the GPU's own MMU translates for the shader cores.

## Power, clock, reset

The RK3568 PMU controller is at `0xfdd90000`; PD_GPU is power bit 0, and
its idle request/ack is bit 1:

```c
wm(pmu, 0xa0, 1<<0, 0);        /* clear power-down */
/* wait until PMU_STATUS (0x98) bit 0 clears */
wm(pmu, 0x50, 1<<1, 0);        /* clear idle request */
/* wait until PMU_ACK (0x60) and PMU_IDLE (0x68) bit 1 clear */
```

The GPU core rate is owned by the firmware through SCMI, so the CRU mux and
divider for `clk_gpu_src` are read-only. The driver only touches the gates
and the resets:

```c
wm(cru, 0x300 + 2*4, (1<<0)|(1<<3), 0);  /* CLK_GPU_SRC, CLK_GPU */
wm(cru, 0x400 + 2*4, (1<<0)|(1<<1), 0);  /* SRST_GPU (32), SRST_A_GPU_NIU (33) */
```

The regulator is already up: `vdd_gpu` is DCDC2 of the RK809 at 900 mV
(`pmic-bringup.md`). After the clocks, the driver writes `GPU_CMD = 1`
(soft reset), waits for `INT_RAWSTAT` bit 8, clears the pending interrupts
and masks every source. The GPU has no interrupt handler yet, so the mask
stays on; the mask reads back `0x30781` (the GPU does not implement every
bit of the requested `0x30f87`).

## Using it

On the board, after `drvman` starts it:

```text
mkdir /gpu
mount -c /srv/gpu /gpu
cat /gpu/status
cat /gpu/regs
echo power off > /gpu/ctl
echo power on  > /gpu/ctl
echo reset     > /gpu/ctl
```

`gpufs -i` is the standalone probe: power on, reset, identify, print, exit.
It is what the board boot runs before posting the service.

Files:

```text
sys/src/cmd/gpufs/gpufs.c   the driver
sys/include/drv.h           drvfindreg, the rest of the kit
sys/src/cmd/drvman/drvman.c "gpu" -> /boot/gpufs -> /srv/gpu
```

## GPU MMU and the first job

### Which page-table format

Linux panfrost only uses AArch64 page tables when a GPU carries
`GPU_QUIRK_FORCE_AARCH64_PGTABLE`. **G52 does not**, so on this board Linux
runs the **Mali LPAE** format:

```c
if (pfdev->comp->gpu_quirks & BIT(GPU_QUIRK_FORCE_AARCH64_PGTABLE))
        fmt = ARM_64_LPAE_S1;
else
        fmt = ARM_MALI_LPAE;        /* what G52 gets */
```

`gpufs` now uses that path. It is a four-level table (`pgd_bits = 0`),
`AS_TRANSTAB = pgd | READ_INNER | ADRMODE_TABLE` (`pgd | 7`),
`AS_TRANSCFG = ADRMODE_LEGACY` (0), `AS_MEMATTR = 0x008d0088`, and leaves are
`pa | 0x2c5`: `TYPE_BLOCK`, `AttrIndx 1` (write-allocate), `SH_OS`, and both
permission bits. Mali uses "block" descriptors at every level and never sets
`AF`.

Bits [7:6] of a Mali LPAE leaf are **stage-2-like (HAP) permissions**, not
ARM's `AP[2:1]`. `echo 'leaf <bits>' > /srv/gpu/ctl` sweeps them on hardware
and the result is unambiguous:

| leaf | HAP | result |
| --- | --- | --- |
| `0x205` | 00 | `PERMISSION_FAULT`, READ -- no access |
| `0x245` | 01 | `PERMISSION_FAULT`, WRITE -- read only |
| `0x285` | 10 | `PERMISSION_FAULT`, READ -- write only |
| `0x2c5` | 11 | both allowed |

`0x2c5` is the only value that lets the job manager fetch the descriptor
*and* write the exception status back. (Linux's `arm_lpae_prot_to_pte`
produces `0x245` for a writable mapping, i.e. read-only under Mali's
interpretation; that is why the HAP sweep is worth recording.)

The earlier AArch64 experiment is instructive but wrong for this part: the
vendor's `AS_TRANSTAB_BASE_SHIFT` is 4, so an AArch64 `TRANSTAB` carries the
table base shifted left by four. `gpufs` wrote it unshifted and the GPU
never fetched the descriptor. Keep that in mind if AArch64 is revisited.

### The MMU really is in the path

`echo unmap > /srv/gpu/ctl` clears the leaf PTE for the job address and
submits a NULL job. The result is unambiguous:

```text
gpufs: unmap raw 0x0 js 0x8 mmuint 0x1        asfault 0x7c1002c3 asaddr 0x10000000
```

`0x...c3` is `TRANSLATION_FAULT_3`, access type 2 is READ, and the fault
address is the job VA. So the job manager walks our tables, reads the level-3
table, and faults on the missing leaf. The head fetch goes through the GPU
MMU.

### The root cause of DATA_INVALID_FAULT

`struct Gpu` declared `u64 *job`, but every job builder indexed it as 32-bit
dwords.  So the `JOB_HEADER` word 4 (`is64b | type << 1 | index << 16`) was
written at byte 32 instead of byte 16, and the job manager read a zero type
field.  That is the whole reason a `NULL` job returned `DATA_INVALID_FAULT`
with no MMU fault.  With `u32 *job`:

```text
gpufs: diag cfg 0x87300 slot 0 raw 0x1 status 0x0 ...
gpufs: diag got 0x0 0x0 0x0 0x0 0x10003 0x0 0x0 0x0
```

`raw 0x1` is the slot-0 completion bit and `status 0x0` is "no exception".
NULL jobs now complete on JS0 and JS1.

### GPU clear

`/dev/display/phys` publishes the scanout buffer's physical address and size
(1920x1080 XRGB8888 at `0x47400000`, 0x7e9000 bytes).  `gpufs` has a `clear`
command that builds, from the captured Linux chain:

- a framebuffer descriptor at the job page,
- one `RENDER_TARGET` at +0x80 (writeback base/stride at dwords 8..11, clear
  colour at 12..15),
- a `FRAGMENT` job at +0xc0 (`is64b`, type 9, index 1, `Next=0`) whose payload
  bounds are in 16x16 tiles,
- 4 KiB leaves for the whole scanout buffer, under 2 MiB tables.

It submits on JS0 and the job manager accepts it, but the first attempts ended
in `CONFIG_FAULT` (0x40) with no MMU fault.  Tracing that down took a Linux
control experiment; the resolution is in the next section.

A second capture with `PAN_MESA_DEBUG=trace,linear,nocrc` gives the exact
**linear** framebuffer descriptor, which is what scanout needs:

```text
Has ZS CRC Extension: false      CRC Write Enable: false
Tiler: 0x0                       Effective Tile Size: 256
Writeback Block Format: Linear
RGB: Base: 0xa004000  Row Stride: 256  Surface Stride: 0
Framebuffer tag: 0xa008001       Fragment Job Payload: bounds 0..3
```

The linear path has no CRC extension, no tiler and no tile-enable map, and
`Row Stride` is `width * 4` with `Surface Stride` 0.  `PAN_MESA_DEBUG=...dump`
also hexdumps every graphics buffer, which showed that `gpufs` matched the
captured descriptor byte for byte except for the addresses and one field:
`z_internal_format`, which is `1` (D24) even with no depth buffer.

Reading the GPU registers from Linux after its successful submit (via
`devmem` on `/dev/mem`, which reaches the device window) gives:

```text
0xfde61800 0x0A008140   JS0_HEAD    = the job header address
0xfde61818 0x00007B00   JS0_CONFIG  = AS 0, pri 8, start/end clean+invalidate,
                                     flush reduction, job-chain flag
0xfde61824 0x00000000   JS0_STATUS  = no exception
```

which is one of the values `gpufs` sweeps, and the writable configuration
registers (`JM_CONFIG 0x000f0000`, `SHADER_CONFIG`, `TILER_CONFIG` and
`L2_MMU_CONFIG` all zero) match as well.

`contrib/gpucap/panclr.c` is the control that settled the descriptor
question.  It submits the same chain from Linux userspace through the
kernel's own panfrost driver, at completely different addresses:

```text
PANCLR job va 0x2000000 handle 1, rt va 0x2004000 handle 2
PANCLR submitted
PANCLR pixel 0xff0000ff  PASS
```

So the descriptor, the slot, `JS_CONFIG` and the writable configuration were
all right, and the fault was in `gpufs`' own kernel-side setup.  The
`/dev/mem` version of the same capture also showed that a *reused* job header
fails with `DATA_INVALID_FAULT`: the GPU writes its exception status back
into the header, so a fresh header is required for every submission.

## The first GPU clear

`gpufs` now renders into the live VOP2 scanout buffer.  The command is

```text
echo 'clear 0xff00ff00' > /mnt/g/ctl
gpufs: clear pa 0x47400000 size 0x7e9000 1920 x 1080 stride 7680
gpufs: clear tiles 120 x 68 stride 7680 PASS raw 0x1 js 0x0 as 0x0 addr 0x0
```

and it is verifiable from the CPU side through `/dev/display/data`
(`echo 'peek 0' > /mnt/g/ctl`):

```text
gpufs: peek 4147200 n 16: 0xff00ff00 0xff00ff00 0xff00ff00 0xff00ff00
```

A 640x480 clear at the origin leaves the rest of the frame alone:

```text
gpufs: peek 0    : 0xff0000ff ...   (cleared)
gpufs: peek 2800 : 0xffff0000 ...   (x=700, outside the 640-wide rect)
gpufs: peek 3080000: 0xff0000ff ... (y=400, x=80, inside)
```

An arbitrary rectangle fill uses the same chain with the framebuffer
descriptor's bound box set to the rectangle and the fragment job's tile
range limited to the tiles that rectangle touches:

```text
echo 'fill 100 100 200 150 0xff00ff00' > /mnt/g/ctl
gpufs: clear tiles 13 x 10 stride 7680 PASS raw 0x1 js 0x0 as 0x0
```

over a red frame leaves `(150,150)` green, `(400,150)` red and `(150,400)`
red, so the clip is exact.

The chain is the one Mesa emits for a clear-only frame, and it needs no
tiler job and no shader: a framebuffer descriptor with per-render-target
clear colours, plus one `FRAGMENT` job whose payload carries the tile
bounds and points at the descriptor.  `gpufs` builds the descriptor, the
render target and the sample-locations table in DMA pages, maps the scanout
buffer into the GPU address space and submits on JS0.

### What actually blocked it

Three separate things, in the order they were found:

1. **`CONFIG_FAULT` (JS_STATUS 0x40).**  The framebuffer and render target
   were mapped with level-3 page tables filled through a `u32 *`.  Mali
   LPAE tables hold 512 **64-bit** descriptors, so this wrote half-width,
   double-spaced entries: each hardware descriptor covered 8 KiB of VA and
   pointed at a page twice as far along in PA, and only half the table was
   populated.  A 64x64 clear inside the first megabyte still "completed",
   but wrote to the wrong physical pages; anything larger hit
   `AS_FAULTSTATUS` at exactly +1 MiB.  The tables must be filled as `u64`.
2. **Leaf attributes.**  The Mali MMU uses *stage-2-like* permissions
   (`HAP`, bits 6:7) with *stage-1-like* attributes, so the correct leaf is

   ```text
   TYPE_BLOCK | ATTRIDX_CACHE | HAP_READ | HAP_WRITE | SH_OS  ==  0x2c5
   ```

   `HAP_READ|HAP_WRITE` is what lets the GPU fetch the job header *and*
   write the exception status back into it.  Copying the ARM stage-1
   `AP_RDONLY` bit (7) clears `HAP_WRITE` instead, and the job then faults
   on its own header address (observed as `0x7c0003c8` at `0x10000000`).
3. **Descriptor contents.**  The only field `gpufs` had wrong versus the
   captured chain was `z_internal_format`, which is `1` (D24) even with no
   depth buffer.

`contrib/gpucap/panclr.c` remains the control: it submits the same chain
through the Linux panfrost driver and gets the same pixels, which is what
proved the descriptor was right and the faults were driver-side.

### Two layouts, both working

`clear ... 1` packs the descriptor at the start of the job page with the
header at +0xc0.  `clear ... 2` gives the descriptor its own page and
starts the job at the base of the job page, which is how Linux lays it out.
Both complete, so the packing was never the problem.

## A real shader draw

`/srv/gpu/ctl` has a `draw` verb that replays the captured Mesa draw chain
(see `contrib/gpucap/panfrost-draw-job.txt`) at the captured virtual
addresses, with a 64x64 render target:

```text
echo draw > /mnt/g/ctl
gpufs: draw tiler PASS raw 0x2 js 0x0 as 0x0 addr 0x0
gpufs: draw frag  PASS raw 0x1 js 0x0 as 0x0 addr 0x0
gpufs: draw pixel 0xff0000ff 0xff0000ff (row 1 0xff0000ff, row 63 0xff0000ff)
```

To make the test unambiguous the framebuffer's clear colour is patched to
green and the fragment shader writes red, so a red pixel can only come from
the shader.  It does.

This is a full graphics chain on `gpufs`' own MMU, job manager and
submission path: a tiler job on JS1 with an attribute buffer, a vertex
shader, a tiler context and a tiler heap, then a fragment job on JS0 with a
fragment shader, renderer state, blend state and the framebuffer descriptor.

### What the replay established

- The **descriptor set from the capture is self-contained**.  Placing the
  sample table at `0xa000000`, the vertex renderer state at `0xa002000`, both
  shaders at `0xa003000`, the render target at `0xa004000`, the attribute
  buffer at `0xa008000` and the descriptor block at `0xa009000` works because
  every pointer inside the descriptors is absolute.
- The **tiler heap is GPU-initialised**.  Mesa creates it `PAN_BO_INVISIBLE`
  and writes nothing into it; the driver hands over `size/base/bottom/top`
  with `bottom == base` and a zero polygon list, and the hardware allocates
  the first chunk itself.
- The heap has to be **big enough for a chunk**.  128 KiB fails with
  `JS_STATUS 0x60` (out of memory) on JS1; 4 MiB works.  The capture's
  128 MiB is just the VA reservation.
- Both jobs submit through the same path the clear uses, one on JS1 and one
  on JS0, with `JS_CONFIG 0x87b00`.

The replay is scaffolding, not a design: it exists so the descriptor layout
is validated before `gpufs` starts emitting its own.

## The vendor userspace agrees

The descriptor set was inferred from Mesa's capture.  The vendor Mali
userspace carries its own field-name table for the same structures, and it
matches field for field.  `contrib/gpucap/libmali-fields.txt` has the
details; the short version is the tiler context order

```text
polygon_list, level_mask_and_sample_pattern, frame_buffer_width_height,
reserved_1, heap_descriptor_pointer, tile_weight, reserved_2, private_state
```

which is exactly the `TILER_CONTEXT` the replay writes, and the heap

```text
heap_size, heap_pointer, low_pointer, high_pointer
```

which is our `size / base / bottom / top`.  The vendor calls the two that
move `low_pointer` and `high_pointer`.

The blob is `libmali-bifrost-g52-g24p0-gbm.so` from the Rockchip libmali
mirror, whose chip table maps `bifrost-g52` to `rk3566_rk3568`.  Armbian's
own *vendor* image for this board still ships mainline panfrost and no
`libmali`; Orange Pi's official images carry it but are only distributed
through file-sharing links.

## Our own shaders and descriptors

The `draw` replay now runs with none of Mesa's shader code or shader
descriptors:

```text
echo 'draw 0' > /mnt/g/ctl
gpufs: draw tiler PASS raw 0x2 js 0x0(ok) as 0x0 addr 0x0
gpufs: draw frag  PASS raw 0x1 js 0x0(ok) as 0x0 addr 0x0
gpufs: draw pixel 0xffbf8040 0xffbf8040 (row 1 0xffbf8040, row 63 0xffbf8040)
```

`0xffbf8040` is `(0.25, 0.5, 0.75, 1.0)` written by `gpucc`'s fragment
shader, over a green clear, at every corner of the 64x64 target.  `draw 5`
keeps Mesa's chain as the control and renders its red.

Three shader-ABI facts were needed, none of them register-level:

1. **The blend takes packed fp16.**  Mesa's fragment ends with
   `r0 = 0x00003c00`, `r1 = 0x3c000000` for opaque red -- two fp16 channels
   per register.  An fp32 store feeds the blend halves of two registers of
   garbage and nothing lands.  `nir_imm_vec4_16` plus
   `src_type = nir_type_float16` fixes it.
2. **`no_idvs` must be false for the tiler path.**  With it set, the vertex
   ends in `ST_CVT ... @r3` (a register) instead of
   `ST_CVT.v4 t1, r58, r59, 0x0105e000, @r0` -- the preloaded position
   address plus the compact varying offset.  With it clear, gpucc emits the
   same instruction as Mesa, immediate included.
3. **The tiler wants pixels, not NDC.**  Mesa applies the viewport
   transform in the vertex shader with uniforms; we bake it (`x*32+32`,
   `y*32+32`, `z*0.5+0.5` for a 64x64 target).  Without it the triangle
   collapses to a single pixel.

What is still Mesa's in mode 0 is the *layout* around the shaders: the
attribute buffer and its descriptors, the varying buffer, the two draw
descriptors, the tiler context and the heap.  Those come next.



1. **A real submit interface.**  `clear` is a debug verb.  The display
   service needs a buffered submission path: a shared surface, a job that
   fills/copies it, and a fence to wait on before the CPU path touches the
   same pixels.  `JS_CONFIG` already asks for end-of-job cache clean and
   invalidate, so completion means the pixels are in DRAM.
2. **GPU-backed display surfaces.**  Have `o9display` allocate its
   composited surface from the same DMA pool the GPU can map, blit with the
   GPU, and keep the CPU memdraw path as the fallback when submission fails.
3. **Our own descriptors.**  The `draw` replay proves the chain; the next
   step is to emit the shader program, renderer state and draw descriptors
   from `contrib/gpucc`'s blobs instead of replaying Mesa's, then swap the
   fragment shader for a textured one.  That is the compositing primitive.
4. **DVFS and thermal.**  GPU OPPs are in the DTS and the RK809 can set
   `vdd_gpu`; neither is wired to a devfreq/thermal policy.

The board's PD_GPU, clock gates, resets, MMU, job manager and scanout path
are all reachable from a userspace 9P driver, and the GPU now produces
pixels that the VOP2 scans out.
