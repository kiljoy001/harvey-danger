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

## GPU MMU and the first job (work in progress)

The MMU work has moved on since the identification driver:

- The page tables are **AArch64 4K**, the format the Bifrost vendor driver
  uses for the job manager: three levels from `start_level 1`, 32 entries
  in the PGD, leaf PTEs with `AF`, `SH`, `ATTRINDX`, `AP_UNPRIV` and
  `TYPE_PAGE`. `AS_TRANSCFG` is `ADRMODE_AARCH64_4K`, with `INA_BITS(20)`
  for the 35-bit VA and the page-table-walk memory attributes.
- A leaf PTE without `AP_UNPRIV` produced a permission fault at the job
  header address; adding it made the walk succeed. That is the first
  evidence that the GPU is actually walking our table.
- Job submission is implemented for the write-value, null and cache-flush
  types on the vertex/tiler slot (JS1), with the AS number in `JS_CONFIG`,
  the start/end cache flushes, and the per-slot completion bits.

The remaining blocker is the job manager itself. Every job type returns
`JS_STATUS = 0x58` (`DATA_INVALID_FAULT`) with no MMU fault
(`AS_FAULTSTATUS = 0`). The header bytes match Mesa's `JOB_HEADER` packer,
the job index and chain flag were swept, and mapping the job page
non-cacheable did not change it. The next step is not more guessing: boot
the reference Linux panfrost stack on the board and capture a known-good
`JS_CONFIG`, job header and AS setup (or read the same from the vendor
kbase), then compare. The diagnostic path is `/srv/gpu/ctl`: `null`,
`test`, `matrix` and `identity` exercise the queue and the table.

## What is next

1. **Reference job trace.** Capture a working panfrost job (Linux or the
   vendor driver) and diff the header, `JS_CONFIG`, and AS registers
   against `gpufs`. Then make the first NULL / write-value job complete.
2. **Job submission.** Bifrost job descriptors in the 3 job slots, the
   `JOB_INT` block, and a ring of jobs exchanged with userspace. Panfrost's
   `panfrost_regs.h` and the Bifrost job layout are the reference.
3. **Shader/tiler power.** `SHADER_PWRON`/`TILER_PWRON`/`L2_PWRON` and the
   ready bitmaps (`SHADER_READY`, `TILER_READY`, `L2_READY`). The board
   currently reads one core ready after the CRU bring-up; that is not the
   same as the power-on handshake.
4. **A compute interface.** A Plan 9 job ABI (`/gpu/job`, or a `ctl` command
   with shared buffers) and a Bifrost instruction encoder. There is no Mesa
   on Plan 9, so graphics is a separate, much larger project; compute is
   the honest first use.
5. **DVFS and thermal.** GPU OPPs are in the DTS and the RK809 can set
   `vdd_gpu`; neither is wired to a devfreq/thermal policy.

Do not expect this to run a shader yet. What it does prove is that the
board's PD_GPU, clock gates, resets and register window are all reachable
from a userspace 9P driver, and that the GPU is a G52 with the expected
Bifrost feature set.
