/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
 * directory of this distribution at http://www.gnu.org/licenses/gpl-2.0.txt
 */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

SECTIONS
{
	. = 0x40010000;
	__image_start = .;

	.text : {
		*(.text.head)
		*(.text.vectors)
		*(.text*)
	}

	etext = .;

	.rodata : {
		*(.rodata*)
	}

	.data : {
		*(.data*)
	}

	.bss : {
		__bss_start = .;
		*(.bss.stack)
		*(.bss*)
		*(COMMON)
		. = ALIGN(4);
		__bss_end = .;
	}

	end = .;
	__image_end = .;
}
