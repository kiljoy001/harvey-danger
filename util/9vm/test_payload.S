.code32
.globl _start

#define MULTIBOOT_MAGIC    0x1BADB002
#define MULTIBOOT_FLAGS    0x00000003
#define MULTIBOOT_CHECKSUM -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

.section .multiboot, "a"
.align 4
.long MULTIBOOT_MAGIC
.long MULTIBOOT_FLAGS
.long MULTIBOOT_CHECKSUM

.section .text
_start:
    cli
    mov $msg, %esi

1:
    lodsb
    test %al, %al
    jz 2f
    mov $0x3f8, %dx
    outb %al, %dx
    jmp 1b

2:
    mov $0xf4, %dx
    mov $0x00, %al
    outb %al, %dx
    hlt
    jmp 2b

.section .rodata
msg:
    .asciz "\r\n[9vm] Hello from KVM micro-subsystem! Multiboot payload executed successfully.\r\n"
