/*
 * Embed the trimmed LKL device tree.  build_lkl.py assembles this with
 * -I pointing at the directory holding lkl-emmc.dtb.
 */
	.section .rodata
	.balign 8
	.global lkl_dtb_blob
	.global lkl_dtb_end
lkl_dtb_blob:
	.incbin "lkl-emmc.dtb"
lkl_dtb_end:
