/*
 * <link.h> for the Harvey/APEX target.
 *
 * Harvey has no dynamic linker, but code such as libunwind asks the loader
 * where the unwind tables are via dl_iterate_phdr().  A static, non-PIE
 * executable is a single module: compat.cpp reports it using the ELF header,
 * so the callbacks see the real program headers.
 */

#ifndef HARVEY_LINK_H
#define HARVEY_LINK_H

#include <stddef.h>
#include <stdint.h>

typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

#define ElfW(type) Elf64_##type

#define PT_LOAD          1
#define PT_GNU_EH_FRAME  0x6474e550

#define PF_X 1
#define PF_W 2
#define PF_R 4

struct dl_phdr_info {
    Elf64_Addr dlpi_addr;
    const char *dlpi_name;
    const Elf64_Phdr *dlpi_phdr;
    Elf64_Half dlpi_phnum;

    /* Present in glibc's layout; libunwind may size the struct. */
    unsigned long long dlpi_adds;
    unsigned long long dlpi_subs;
    size_t dlpi_tls_modid;
    void *dlpi_tls_data;
};

#ifdef __cplusplus
extern "C" {
#endif

int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *, size_t, void *),
                    void *data);

#ifdef __cplusplus
}
#endif

#endif
