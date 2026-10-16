ENTRY(_start)

SECTIONS
{
	. = 0x40080000;
	.text : { KEEP(*(.text.head)) *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	.data : { *(.data .data.*) }
	. = ALIGN(16);
	__bss_start = .;
	.bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }
	. = ALIGN(16);
	__bss_end = .;
	__image_end = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
