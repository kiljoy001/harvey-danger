# Booting Linux on the board over TFTP to capture a panfrost job chain

This is the on-hardware route: boot a stock Linux with panfrost, run a GL
workload with `PAN_MESA_DEBUG=trace`, and let Mesa's `pandecode_jc()` print
the job chain it submits.

It works.  The board boots the Armbian 6.18.21 `Image` with a custom
initramfs, panfrost probes the G52, and the register dump matches gpufs
exactly:

```
panfrost fde60000.gpu: Features: L2:0x07110206 Shader:0x00000002 Tiler:0x00000209 Mem:0x1 MMU:0x00002823 AS:0xff JS:0x7
panfrost fde60000.gpu: shader_present=0x1 l2_present=0x1
[drm] Initialized panfrost 1.4.0 for fde60000.gpu on minor 0
```

That independently confirms `gpufs`'s feature block and the MMU/JS layout.

## Pieces

- Kernel: `/boot/vmlinuz-6.18.21-current-rockchip64` from
  `Armbian_community_26.2.0-trunk.703_Orangepi3b_trixie_current_6.18.21_minimal.img`
  (partition 1, offset 32768 sectors).  It is a raw arm64 `Image`, 40 MB.
- DTB: `orangepi-3b-v1.1.dtb` (U-Boot's `stdout-path` is `serial2:1500000`).
- Modules: `panfrost.ko`, `gpu-sched.ko`, `drm_shmem_helper.ko` from the
  same image's `/lib/modules/6.18.21-current-rockchip64`.
- Userland: Debian Trixie **arm64** debs unpacked into the initramfs —
  `busybox-static`, `libc6`, `libdrm2`, `libglvnd0`, `libegl1`, `libgles2`,
  `libgbm1`, `libegl-mesa0`, `mesa-libgallium`, `libgl1-mesa-dri`
  (25.0.7 — side-loads `panfrost_dri.so`), `mesa-utils-bin` (`es2_info`),
  `libstdc++6`, `libgcc-s1`, `libllvm19` (+ `libedit2`, `libz3-4`,
  `libtinfo6`, `libxml2`, `libffi8`), `zlib1g`, `libzstd1`, `libexpat1`,
  libxcb/libx11/libwayland runtimes, and the `-dev` debs used only as a
  cross sysroot (`libc6-dev`, `libegl-dev`, `libgles-dev`, `libglvnd-dev`,
  `libgbm-dev`, `libgcc-14-dev`, `linux-libc-dev`).

  `libgallium` links LLVM even with `-Dgallium-drivers=panfrost`; without
  `libLLVM.so.19.1` the loader fails with `MESA-LOADER: failed to open dri`.

## Build the initramfs

```sh
# root tree: unpack every deb above with `dpkg-deb -x`, then:
ln -sfn usr/bin bin
ln -sfn usr/lib lib
cp initramfs-init.sh root/init && chmod +x root/init
(cd root && find . | cpio -o -H newc | gzip -1 > ../initramfs.cpio.gz)
```

U-Boot's `booti` rejects a bare gzip initramfs ("Wrong Ramdisk Image
Format"), so wrap it in a 64-byte legacy U-Boot header: magic
`0x27051956`, os 5, arch 22, type 3 (ramdisk), comp 1 (gzip), sizes and
CRCs, name in bytes 32..63.  That image is what `booti` accepts.

## Boot

```
tftpboot 0x60000000 lx-k.img      # Image, 40 MB
tftpboot 0x20000000 lx-i.img      # uInitrd, ~95 MB
tftpboot 0x48000000 orangepi-3b-v1.1.dtb
setenv bootargs console=ttyS2,1500000 rdinit=/init loglevel=7 ignore_loglevel nowatchdog
booti 0x60000000 0x20000000 0x48000000
```

`nowatchdog` matters: the Armbian kernel starts the RK3566 watchdog and the
board resets about 30 s into userspace otherwise.  The kernel has 8 GiB, so
0x20000000/0x48000000/0x60000000 are all free (U-Boot relocates itself near
0xefff0000).

The init script (`initramfs-init.sh`) mounts devtmpfs/proc/sysfs, inserts the
three modules, then runs:

```
EGL_PLATFORM=surfaceless PAN_MESA_DEBUG=trace /capture
PAN_MESA_DEBUG=trace /capture_gbm
```

`/capture` is `capture_egl.c` cross-compiled for aarch64 with
`clang --target=aarch64-linux-gnu --sysroot=root -fuse-ld=lld`.
`PAN_MESA_DEBUG=trace` makes `pan_jm.c` call `pandecode_jc()`, which prints
the whole chain (job header, payload, RSD) straight to the console.

## Remaining

The last run got to `=== CAPTURE gbm` and failed on the missing
`libLLVM.so.19.1`; that library is now in the initramfs.  Every later
attempt was cut short because the board was reset and booted into Harvey by
another session (the mesh switch and the serial port are shared).  With the
board free, one boot should print the chain.
