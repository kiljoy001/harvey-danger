#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <linux/kvm.h>
#include <limits.h>
#include <elf.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <sched.h>
#include "hostfs.h"
#include "../../sys/include/9ring.h"

static struct timespec ts_boot_start;
static inline double get_boot_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec - ts_boot_start.tv_sec) * 1000.0 + (ts.tv_nsec - ts_boot_start.tv_nsec) / 1000000.0;
}

#define DEFAULT_RAM_MB 1024
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002
#define MBI_ADDR     0x10000
#define GDT_ADDR     0x1000
#define BOOT_STACK   0x70000

static P9HostShared *hostring;
static pthread_t hostring_thread;
static volatile int hostring_running;
static struct hostfs *hostring_fs;
static uint64_t hostring_requests;
static uint64_t virtio_host_kicks;
static uint64_t vm_io_exits;
static uint64_t vm_mmio_exits;
static struct vmdisplay *vm_display;
static int vm_display_fd = -1;
static char vm_display_name[128];

static void display_cleanup(void) {
    if (vm_display != NULL) {
        munmap(vm_display, sizeof *vm_display);
        vm_display = NULL;
    }
    if (vm_display_fd >= 0) {
        close(vm_display_fd);
        vm_display_fd = -1;
    }
    if (vm_display_name[0] != '\0')
        shm_unlink(vm_display_name);
}

static int display_init(const char *name) {
    if (name[0] != '/' || name[1] == '\0' || strchr(name + 1, '/') != NULL ||
        strlen(name) >= sizeof vm_display_name)
        return -1;
    snprintf(vm_display_name, sizeof vm_display_name, "%s", name);
    vm_display_fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (vm_display_fd < 0)
        return -1;
    if (ftruncate(vm_display_fd, sizeof(struct vmdisplay)) < 0)
        return -1;
    vm_display = mmap(NULL, sizeof(struct vmdisplay), PROT_READ | PROT_WRITE,
                      MAP_SHARED, vm_display_fd, 0);
    if (vm_display == MAP_FAILED) {
        vm_display = NULL;
        return -1;
    }
    memset(vm_display, 0, sizeof *vm_display);
    vm_display->version = 1;
    vm_display->width = VMDISPLAY_WIDTH;
    vm_display->height = VMDISPLAY_HEIGHT;
    __atomic_store_n(&vm_display->magic, VMDISPLAY_MAGIC, __ATOMIC_RELEASE);
    return 0;
}

struct bench_sample {
    struct timespec wall;
    struct timespec cpu;
    uint64_t io_exits;
    uint64_t mmio_exits;
    uint64_t virtio_kicks;
    uint64_t ring_requests;
};

static struct bench_sample bench_start;
static int bench_active;
static char uart_line[256];
static size_t uart_line_len;

static uint64_t elapsed_ns(struct timespec a, struct timespec b) {
    return (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ULL +
           (uint64_t)(b.tv_nsec - a.tv_nsec);
}

static struct bench_sample bench_sample_now(void) {
    struct bench_sample s;
    clock_gettime(CLOCK_MONOTONIC, &s.wall);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &s.cpu);
    s.io_exits = vm_io_exits;
    s.mmio_exits = vm_mmio_exits;
    s.virtio_kicks = virtio_host_kicks;
    s.ring_requests = __atomic_load_n(&hostring_requests, __ATOMIC_RELAXED);
    return s;
}

static void bench_serial(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char ch = (char)data[i];
        if (ch == '\n') {
            uart_line[uart_line_len] = '\0';
            if (strncmp(uart_line, "o9-bench: begin kind=", 21) == 0) {
                bench_start = bench_sample_now();
                bench_active = atoi(uart_line + 21) + 1;
            } else if (strncmp(uart_line, "o9-bench: end kind=", 19) == 0 &&
                       bench_active == atoi(uart_line + 19) + 1) {
                struct bench_sample end = bench_sample_now();
                fprintf(stderr,
                    "[9vm-bench] kind=%d wall_ns=%llu cpu_ns=%llu io_exits=%llu mmio_exits=%llu virtio_host_kicks=%llu ring_requests=%llu\n",
                    bench_active - 1,
                    (unsigned long long)elapsed_ns(bench_start.wall, end.wall),
                    (unsigned long long)elapsed_ns(bench_start.cpu, end.cpu),
                    (unsigned long long)(end.io_exits - bench_start.io_exits),
                    (unsigned long long)(end.mmio_exits - bench_start.mmio_exits),
                    (unsigned long long)(end.virtio_kicks - bench_start.virtio_kicks),
                    (unsigned long long)(end.ring_requests - bench_start.ring_requests));
                bench_active = 0;
            }
            uart_line_len = 0;
        } else if (ch != '\r' && uart_line_len < sizeof(uart_line) - 1) {
            uart_line[uart_line_len++] = ch;
        }
    }
}

static void *hostring_serve(void *arg) {
    P9Slot slot;
    P9Comp comp;
    P9HostBuffer *b;
    uint32_t len;
    struct timespec idle = { .tv_sec = 0, .tv_nsec = 50000 };
    unsigned active_spins = 0;
    (void)arg;
    while (__atomic_load_n(&hostring_running, __ATOMIC_ACQUIRE)) {
        if (!p9_tring_consume(&hostring->channel.t_ring, &slot)) {
            if (active_spins < 2000) {
                __asm__ __volatile__("pause");
                active_spins++;
            } else {
                nanosleep(&idle, NULL);
            }
            continue;
        }
        active_spins = 0;
        memset(&comp, 0, sizeof(comp));
        comp.tag = slot.tag;
        comp.result = slot.buf_addr;
        comp.status = 1;
        if (slot.buf_addr < P9_HOST_SLOTS && slot.count >= 7 &&
            slot.count <= P9_HOST_MSIZE) {
            b = &hostring->buffer[slot.buf_addr];
            uint32_t wirelen;
            memcpy(&wirelen, b->request, sizeof(wirelen));
            if (wirelen == slot.count && b->request[4] == slot.type &&
                b->request[5] == (uint8_t)slot.tag &&
                b->request[6] == (uint8_t)(slot.tag >> 8)) {
                len = hostfs_dispatch(hostring_fs, b->request, slot.count,
                                      b->response, P9_HOST_MSIZE);
                if (len >= 7 && len <= P9_HOST_MSIZE) {
                    comp.type = b->response[4];
                    comp.count = len;
                    comp.status = 0;
                }
            }
        }
        while (__atomic_load_n(&hostring_running, __ATOMIC_ACQUIRE) &&
               p9_rring_complete(&hostring->channel.r_ring, &comp) < 0)
            nanosleep(&idle, NULL);
        __atomic_fetch_add(&hostring_requests, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

static void hostring_stop(void) {
    if (hostring_running) {
        __atomic_store_n(&hostring_running, 0, __ATOMIC_RELEASE);
        pthread_join(hostring_thread, NULL);
        fprintf(stderr, "[9vm] Shared host 9ring completed %llu request(s)\n",
                (unsigned long long)hostring_requests);
    }
    if (hostring_fs) {
        hostfs_free(hostring_fs);
        free(hostring_fs);
        hostring_fs = NULL;
    }
}

static uint8_t uart_scr = 0;
static volatile uint8_t uart_rx_char = 0;   /* pending UART RX character (from stdin) */
static volatile int uart_rx_ready = 0;       /* 1 while a character is pending */
static int g_vm = -1;                        /* KVM VM fd, for the stdin thread */
static volatile sig_atomic_t vm_running = 1;
static void vm_sig_handler(int sig) {
    (void)sig;
    vm_running = 0;
}
static void stdin_wake_handler(int sig) {
    (void)sig;  /* no-op: just interrupt KVM_RUN so we can poll stdin */
}

struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed));

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
} __attribute__((packed));

static void set_segment(struct kvm_segment *seg, uint16_t sel, uint32_t base, uint32_t limit,
                        uint8_t type, uint8_t db, uint8_t l) {
    seg->selector = sel;
    seg->base = base;
    seg->limit = limit;
    seg->type = type;
    seg->present = 1;
    seg->dpl = 0;
    seg->db = db;
    seg->s = 1;
    seg->l = l;
    seg->g = 1;
    seg->avl = 0;
    seg->unusable = 0;
}

static uint64_t load_elf(const char *path, uint8_t *ram, size_t ram_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open kernel");
        exit(1);
    }

    uint8_t e_ident[EI_NIDENT];
    if (read(fd, e_ident, sizeof(e_ident)) != sizeof(e_ident)) {
        fprintf(stderr, "failed to read ELF header\n");
        exit(1);
    }
    lseek(fd, 0, SEEK_SET);

    uint64_t entry = 0;

    if (e_ident[EI_CLASS] == ELFCLASS32) {
        Elf32_Ehdr ehdr;
        if (read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
            fprintf(stderr, "invalid ELF32 header\n");
            exit(1);
        }
        entry = ehdr.e_entry;

        Elf32_Phdr *phdrs = malloc(ehdr.e_phnum * sizeof(Elf32_Phdr));
        lseek(fd, ehdr.e_phoff, SEEK_SET);
        if (read(fd, phdrs, ehdr.e_phnum * sizeof(Elf32_Phdr)) != (ssize_t)(ehdr.e_phnum * sizeof(Elf32_Phdr))) {
            fprintf(stderr, "failed to read ELF32 phdrs\n");
            exit(1);
        }

        for (int i = 0; i < ehdr.e_phnum; i++) {
            if (phdrs[i].p_type != PT_LOAD)
                continue;

            uint32_t paddr = phdrs[i].p_paddr;
            if (paddr + phdrs[i].p_memsz > ram_size) {
                fprintf(stderr, "kernel segment exceeds guest RAM (0x%x + 0x%x > 0x%zx)\n",
                        paddr, phdrs[i].p_memsz, ram_size);
                exit(1);
            }

            double t_before_read = get_boot_ms();
            lseek(fd, phdrs[i].p_offset, SEEK_SET);
            if (read(fd, ram + paddr, phdrs[i].p_filesz) != (ssize_t)phdrs[i].p_filesz) {
                fprintf(stderr, "failed to read segment data\n");
                exit(1);
            }
            double t_after_read = get_boot_ms();
            /* Note: Guest RAM is freshly allocated via MAP_ANONYMOUS, so memory is
             * guaranteed to be zero by Linux. Redundant memset on large BSS (262 MB)
             * triggers 65,000+ host page faults and wastes ~88 ms. */
            if (getenv("VM_TRACE")) {
                fprintf(stderr, "[perf seg %d] read %.2f MB (%.2f ms), memsz %.2f MB\n",
                        i, (double)phdrs[i].p_filesz / (1024*1024), t_after_read - t_before_read,
                        (double)phdrs[i].p_memsz / (1024*1024));
            }
        }
        free(phdrs);
    } else if (e_ident[EI_CLASS] == ELFCLASS64) {
        Elf64_Ehdr ehdr;
        if (read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
            fprintf(stderr, "invalid ELF64 header\n");
            exit(1);
        }
        entry = ehdr.e_entry;

        Elf64_Phdr *phdrs = malloc(ehdr.e_phnum * sizeof(Elf64_Phdr));
        lseek(fd, ehdr.e_phoff, SEEK_SET);
        if (read(fd, phdrs, ehdr.e_phnum * sizeof(Elf64_Phdr)) != (ssize_t)(ehdr.e_phnum * sizeof(Elf64_Phdr))) {
            fprintf(stderr, "failed to read ELF64 phdrs\n");
            exit(1);
        }

        for (int i = 0; i < ehdr.e_phnum; i++) {
            if (phdrs[i].p_type != PT_LOAD)
                continue;

            uint64_t paddr = phdrs[i].p_paddr;
            /* Strip high virtual bits if physical address is unset */
            if (paddr >= 0xFFFF800000000000ULL)
                paddr = paddr & 0x000000003FFFFFFFULL;

            if (paddr + phdrs[i].p_memsz > ram_size) {
                fprintf(stderr, "kernel segment exceeds guest RAM (0x%lx + 0x%lx > 0x%zx)\n",
                        paddr, phdrs[i].p_memsz, ram_size);
                exit(1);
            }

            lseek(fd, phdrs[i].p_offset, SEEK_SET);
            if (read(fd, ram + paddr, phdrs[i].p_filesz) != (ssize_t)phdrs[i].p_filesz) {
                fprintf(stderr, "failed to read segment data\n");
                exit(1);
            }
        }
        free(phdrs);
        if (entry >= 0xFFFF800000000000ULL)
            entry = entry & 0x000000003FFFFFFFULL;
    } else {
        fprintf(stderr, "unknown ELF class\n");
        exit(1);
    }

    close(fd);
    return entry;
}

static void setup_multiboot(uint8_t *ram, size_t ram_size, const char *cmdline,
                            bool use_hostring) {
    struct multiboot_info *mbi = (struct multiboot_info *)(ram + MBI_ADDR);
    memset(mbi, 0, sizeof(*mbi));

    mbi->flags = 0x01 | 0x02 | 0x04 | 0x40; /* meminfo, bootdevice, cmdline, mmap */
    mbi->mem_lower = 639;
    mbi->mem_upper = (ram_size - 0x100000) / 1024;

    uint32_t cmdline_addr = MBI_ADDR + sizeof(struct multiboot_info);
    if (cmdline && *cmdline) {
        strcpy((char *)(ram + cmdline_addr), cmdline);
        mbi->cmdline = cmdline_addr;
    } else {
        mbi->cmdline = 0;
    }

    uint32_t mmap_addr = (cmdline_addr + 256 + 15) & ~15;
    struct multiboot_mmap_entry *mmap = (struct multiboot_mmap_entry *)(ram + mmap_addr);

    /* Conventional memory */
    mmap[0].size = sizeof(struct multiboot_mmap_entry) - 4;
    mmap[0].addr = 0;
    mmap[0].len = 0x9FC00;
    mmap[0].type = 1;

    /* Reserved BIOS/VGA area */
    mmap[1].size = sizeof(struct multiboot_mmap_entry) - 4;
    mmap[1].addr = 0x9FC00;
    mmap[1].len = 0x60400;
    mmap[1].type = 2;

    /* Extended memory. Keep the last 2 MiB mapped and writable by the
     * guest kernel, but outside its page allocator. */
    mmap[2].size = sizeof(struct multiboot_mmap_entry) - 4;
    mmap[2].addr = 0x100000;
    mmap[2].len = ram_size - 0x100000 -
                  (use_hostring ? P9_HOST_REGION_SIZE : 0);
    mmap[2].type = 1;

    if (use_hostring) {
        mmap[3].size = sizeof(struct multiboot_mmap_entry) - 4;
        mmap[3].addr = ram_size - P9_HOST_REGION_SIZE;
        mmap[3].len = P9_HOST_REGION_SIZE;
        mmap[3].type = 0x9009; /* private 9vm/Harvey shared RAM type */
    }

    mbi->mmap_addr = mmap_addr;
    mbi->mmap_length = (use_hostring ? 4 : 3) *
                       sizeof(struct multiboot_mmap_entry);
}

static void setup_gdt(uint8_t *ram) {
    uint64_t *gdt = (uint64_t *)(ram + GDT_ADDR);
    gdt[0] = 0;                          /* 0x00: Null */
    gdt[1] = 0x00CF9A000000FFFFULL;      /* 0x08: Code 32-bit (base=0, limit=4GB, ring 0) */
    gdt[2] = 0x00CF92000000FFFFULL;      /* 0x10: Data 32-bit (base=0, limit=4GB, ring 0) */
    gdt[3] = 0x00AF9A000000FFFFULL;      /* 0x18: Code 64-bit */
    gdt[4] = 0x00CF92000000FFFFULL;      /* 0x20: Data 64-bit */
}

static void setup_cpuid(int kvm, int vcpu) {
    size_t size = sizeof(struct kvm_cpuid2) + 256 * sizeof(struct kvm_cpuid_entry2);
    struct kvm_cpuid2 *cpuid = malloc(size);
    if (!cpuid) return;
    memset(cpuid, 0, size);
    cpuid->nent = 256;
    if (ioctl(kvm, KVM_GET_SUPPORTED_CPUID, cpuid) < 0) {
        perror("KVM_GET_SUPPORTED_CPUID");
        free(cpuid);
        return;
    }

    int tsc_khz = ioctl(vcpu, KVM_GET_TSC_KHZ, 0);
    if (tsc_khz <= 0) {
        FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r");
        if (f) {
            if (fscanf(f, "%d", &tsc_khz) != 1) tsc_khz = 3000000;
            fclose(f);
        } else {
            tsc_khz = 3000000;
        }
    }

    /* Add or update CPUID leaf 0x40000010 (KVM PV clock TSC frequency) */
    int found = 0;
    for (uint32_t i = 0; i < cpuid->nent; i++) {
        if (cpuid->entries[i].function == 0x40000010) {
            cpuid->entries[i].eax = (uint32_t)tsc_khz;
            cpuid->entries[i].ebx = 0;
            cpuid->entries[i].ecx = 0;
            cpuid->entries[i].edx = 0;
            found = 1;
            break;
        }
    }
    if (!found && cpuid->nent < 256) {
        struct kvm_cpuid_entry2 *e = &cpuid->entries[cpuid->nent++];
        memset(e, 0, sizeof(*e));
        e->function = 0x40000010;
        e->eax = (uint32_t)tsc_khz;
    }

    if (ioctl(vcpu, KVM_SET_CPUID2, cpuid) < 0) {
        perror("KVM_SET_CPUID2");
    }
    free(cpuid);
}

static void acpi_checksum(uint8_t *table, size_t len, size_t csum_offset) {
    table[csum_offset] = 0;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += table[i];
    }
    table[csum_offset] = (uint8_t)(0 - sum);
}

static void setup_acpi(uint8_t *ram) {
    uint32_t rsdp_addr = 0xE0000;
    uint32_t rsdt_addr = 0xE0040;
    uint32_t fadt_addr = 0xE0100;
    uint32_t dsdt_addr = 0xE0200;
    uint32_t madt_addr = 0xE0240;

    /* 1. DSDT at 0xE0200 (36 bytes) */
    uint8_t *dsdt = ram + dsdt_addr;
    memset(dsdt, 0, 36);
    memcpy(dsdt, "DSDT", 4);
    *(uint32_t *)(dsdt + 4) = 36;
    dsdt[8] = 1;
    memcpy(dsdt + 10, "9VM   ", 6);
    memcpy(dsdt + 16, "9VMDSDT ", 8);
    *(uint32_t *)(dsdt + 24) = 1;
    memcpy(dsdt + 28, "9VM ", 4);
    *(uint32_t *)(dsdt + 32) = 1;
    acpi_checksum(dsdt, 36, 9);

    /* 2. FADT at 0xE0100 (116 bytes) */
    uint8_t *fadt = ram + fadt_addr;
    memset(fadt, 0, 116);
    memcpy(fadt, "FACP", 4);
    *(uint32_t *)(fadt + 4) = 116;
    fadt[8] = 1;
    memcpy(fadt + 10, "9VM   ", 6);
    memcpy(fadt + 16, "9VMFACP ", 8);
    *(uint32_t *)(fadt + 24) = 1;
    memcpy(fadt + 28, "9VM ", 4);
    *(uint32_t *)(fadt + 32) = 1;
    *(uint32_t *)(fadt + 40) = dsdt_addr;
    fadt[45] = 1; /* Desktop */
    acpi_checksum(fadt, 116, 9);

    /* 3. MADT at 0xE0240 (94 bytes) */
    uint8_t *madt = ram + madt_addr;
    memset(madt, 0, 94);
    memcpy(madt, "APIC", 4);
    *(uint32_t *)(madt + 4) = 94;
    madt[8] = 1;
    memcpy(madt + 10, "9VM   ", 6);
    memcpy(madt + 16, "9VMAPIC ", 8);
    *(uint32_t *)(madt + 24) = 1;
    memcpy(madt + 28, "9VM ", 4);
    *(uint32_t *)(madt + 32) = 1;
    *(uint32_t *)(madt + 36) = 0xFEE00000; /* Local APIC Address */
    *(uint32_t *)(madt + 40) = 1;          /* PCAT_COMPAT flags */

    /* Subtable 0: Processor Local APIC (offset 44, len 8) */
    madt[44] = 0;                         /* Type: ASlapic */
    madt[45] = 8;                         /* Length */
    madt[46] = 0;                         /* ACPI Processor ID */
    madt[47] = 0;                         /* APIC ID */
    *(uint32_t *)(madt + 48) = 1;         /* Flags: Enabled */

    /* Subtable 1: I/O APIC (offset 52, len 12) */
    madt[52] = 1;                         /* Type: ASioapic */
    madt[53] = 12;                        /* Length */
    madt[54] = 0;                         /* I/O APIC ID */
    madt[55] = 0;                         /* Reserved */
    *(uint32_t *)(madt + 56) = 0xFEC00000; /* I/O APIC Address */
    *(uint32_t *)(madt + 60) = 0;          /* Global System Interrupt Base */

    /* Subtable 2: Interrupt Source Override for IRQ 10 (offset 64, len 10) */
    madt[64] = 2;                         /* Type: ASintovr */
    madt[65] = 10;                        /* Length */
    madt[66] = 0;                         /* Bus: 0 (ISA) */
    madt[67] = 10;                        /* Source IRQ: 10 */
    *(uint32_t *)(madt + 68) = 10;        /* Global System Interrupt: 10 */
    *(uint16_t *)(madt + 72) = 0x000F;    /* Flags: active-low, level-triggered */

    /* Subtable 3: Interrupt Source Override for IRQ 11 (offset 74, len 10) */
    madt[74] = 2;                         /* Type: ASintovr */
    madt[75] = 10;                        /* Length */
    madt[76] = 0;                         /* Bus: 0 (ISA) */
    madt[77] = 11;                        /* Source IRQ: 11 */
    *(uint32_t *)(madt + 78) = 11;        /* Global System Interrupt: 11 */
    *(uint16_t *)(madt + 82) = 0x000F;    /* Flags: active-low, level-triggered */

    /* Subtable 4: Interrupt Source Override for IRQ 12 (offset 84, len 10) */
    madt[84] = 2;
    madt[85] = 10;
    madt[86] = 0;
    madt[87] = 12;
    *(uint32_t *)(madt + 88) = 12;
    *(uint16_t *)(madt + 92) = 0x000F;

    acpi_checksum(madt, 94, 9);

    /* 4. RSDT at 0xE0040 (44 bytes) */
    uint8_t *rsdt = ram + rsdt_addr;
    memset(rsdt, 0, 44);
    memcpy(rsdt, "RSDT", 4);
    *(uint32_t *)(rsdt + 4) = 44;
    rsdt[8] = 1;
    memcpy(rsdt + 10, "9VM   ", 6);
    memcpy(rsdt + 16, "9VMRSDT ", 8);
    *(uint32_t *)(rsdt + 24) = 1;
    memcpy(rsdt + 28, "9VM ", 4);
    *(uint32_t *)(rsdt + 32) = 1;
    *(uint32_t *)(rsdt + 36) = fadt_addr;
    *(uint32_t *)(rsdt + 40) = madt_addr;
    acpi_checksum(rsdt, 44, 9);

    /* 5. RSDP at 0xE0000 (20 bytes) */
    uint8_t *rsdp = ram + rsdp_addr;
    memset(rsdp, 0, 20);
    memcpy(rsdp, "RSD PTR ", 8);
    memcpy(rsdp + 9, "9VM   ", 6);
    rsdp[15] = 0;                         /* Revision 0 (ACPI 1.0) */
    *(uint32_t *)(rsdp + 16) = rsdt_addr;
    acpi_checksum(rsdp, 20, 8);
}

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define VIRTIO_9P_HOST_BAR0_ADDR 0xFEB00000ULL
#define VIRTIO_9P_NET_BAR0_ADDR  0xFEB01000ULL
#define VIRTIO_9P_DISK_BAR0_ADDR 0xFEB02000ULL
#define VIRTIO_BAR0_SIZE         0x1000

struct pci_dev {
    uint8_t  config[256];
    uint32_t bar0;
};

static void pci_dev_init(struct pci_dev *pci, uint16_t dev_id, uint16_t subsys_id, uint32_t bar0, uint8_t irq_line) {
    memset(pci, 0, sizeof(*pci));
    pci->bar0 = bar0;

    /* Header Type 0 */
    pci->config[0x00] = 0xF4; /* Vendor ID: 0x1AF4 */
    pci->config[0x01] = 0x1A;
    pci->config[0x02] = dev_id & 0xFF;
    pci->config[0x03] = dev_id >> 8;
    pci->config[0x04] = 0x07; /* Command: IO + MEM + BusMaster */
    pci->config[0x05] = 0x00;
    pci->config[0x06] = 0x10; /* Status: Capabilities List */
    pci->config[0x07] = 0x00;
    pci->config[0x08] = 0x01; /* Revision ID: 1 (VirtIO 1.0 modern) */
    pci->config[0x09] = 0x00; /* Prog IF */
    pci->config[0x0A] = 0x00; /* Subclass */
    pci->config[0x0B] = 0x00; /* Base class */
    pci->config[0x0E] = 0x00; /* Header type: 0 */

    *(uint32_t *)&pci->config[0x10] = bar0;

    /* Subsystem Vendor / ID */
    pci->config[0x2C] = 0xF4;
    pci->config[0x2D] = 0x1A;
    pci->config[0x2E] = subsys_id & 0xFF;
    pci->config[0x2F] = subsys_id >> 8;

    /* Cap Pointer: 0x40 */
    pci->config[0x34] = 0x40;

    /* Interrupt Line / Pin */
    pci->config[0x3C] = irq_line;
    pci->config[0x3D] = 1;

    /* Capability 1: Common Config at 0x40 (len 16) */
    pci->config[0x40] = 0x09; /* PciCapVND */
    pci->config[0x41] = 0x50; /* Next -> 0x50 */
    pci->config[0x42] = 16;   /* Cap length */
    pci->config[0x43] = 1;    /* cfg_type: 1 (common) */
    pci->config[0x44] = 0;    /* bar 0 */
    *(uint32_t *)&pci->config[0x48] = 0x000; /* offset 0x000 */
    *(uint32_t *)&pci->config[0x4C] = 56;    /* length 56 bytes */

    /* Capability 2: Notify Config at 0x50 (len 20) */
    pci->config[0x50] = 0x09; /* PciCapVND */
    pci->config[0x51] = 0x64; /* Next -> 0x64 */
    pci->config[0x52] = 20;   /* Cap length */
    pci->config[0x53] = 2;    /* cfg_type: 2 (notify) */
    pci->config[0x54] = 0;    /* bar 0 */
    *(uint32_t *)&pci->config[0x58] = 0x100; /* offset 0x100 */
    *(uint32_t *)&pci->config[0x5C] = 4;     /* length 4 bytes */
    *(uint32_t *)&pci->config[0x60] = 0;     /* notify_off_multiplier: 0 */

    /* Capability 3: ISR Config at 0x64 (len 16) */
    pci->config[0x64] = 0x09; /* PciCapVND */
    pci->config[0x65] = 0x74; /* Next -> 0x74 */
    pci->config[0x66] = 16;   /* Cap length */
    pci->config[0x67] = 3;    /* cfg_type: 3 (ISR) */
    pci->config[0x68] = 0;    /* bar 0 */
    *(uint32_t *)&pci->config[0x6C] = 0x200; /* offset 0x200 */
    *(uint32_t *)&pci->config[0x70] = 4;     /* length 4 bytes */

    /* Capability 4: Device Config at 0x74 (len 16) */
    pci->config[0x74] = 0x09; /* PciCapVND */
    pci->config[0x75] = 0x00; /* Next -> 0 (end) */
    pci->config[0x76] = 16;   /* Cap length */
    pci->config[0x77] = 4;    /* cfg_type: 4 (device) */
    pci->config[0x78] = 0;    /* bar 0 */
    *(uint32_t *)&pci->config[0x7C] = 0x300; /* offset 0x300 */
    *(uint32_t *)&pci->config[0x80] = 16;    /* length 16 bytes */
}

static void pci_io_write(struct pci_dev *p_host, struct pci_dev *p_net, struct pci_dev *p_disk, uint32_t *addr_reg, uint16_t port, uint8_t *data, uint32_t size) {
    if (port == PCI_CONFIG_ADDR && size == 4) {
        *addr_reg = *(uint32_t *)data;
        return;
    }
    if (port >= PCI_CONFIG_DATA && port <= PCI_CONFIG_DATA + 3) {
        uint32_t bus = (*addr_reg >> 16) & 0xFF;
        uint32_t dev = (*addr_reg >> 11) & 0x1F;
        uint32_t fn  = (*addr_reg >> 8) & 0x07;
        if ((*addr_reg & 0x80000000) && bus == 0 && fn == 0) {
            struct pci_dev *p = (dev == 1) ? p_host : ((dev == 2) ? p_net : ((dev == 3) ? p_disk : NULL));
            if (p) {
                uint32_t reg = (*addr_reg & 0xFC) + (port - PCI_CONFIG_DATA);
                if (reg == 0x10 && size == 4) {
                    uint32_t val = *(uint32_t *)data;
                    if ((val & 0xFFFFFFF0) == 0xFFFFFFF0) {
                        p->bar0 = 0xFFFFF000;
                        *(uint32_t *)&p->config[0x10] = 0xFFFFF000;
                    } else {
                        p->bar0 = val & 0xFFFFF000;
                        *(uint32_t *)&p->config[0x10] = p->bar0;
                    }
                } else if (reg + size <= 256) {
                    memcpy(&p->config[reg], data, size);
                }
            }
        }
    }
}

static void pci_io_read(struct pci_dev *p_host, struct pci_dev *p_net, struct pci_dev *p_disk, uint32_t *addr_reg, uint16_t port, uint8_t *data, uint32_t size) {
    if (port == PCI_CONFIG_ADDR && size == 4) {
        *(uint32_t *)data = *addr_reg;
        return;
    }
    if (port >= PCI_CONFIG_DATA && port <= PCI_CONFIG_DATA + 3) {
        uint32_t bus = (*addr_reg >> 16) & 0xFF;
        uint32_t dev = (*addr_reg >> 11) & 0x1F;
        uint32_t fn  = (*addr_reg >> 8) & 0x07;
        if ((*addr_reg & 0x80000000) && bus == 0 && fn == 0) {
            struct pci_dev *p = (dev == 1) ? p_host : ((dev == 2) ? p_net : ((dev == 3) ? p_disk : NULL));
            if (p) {
                uint32_t reg = (*addr_reg & 0xFC) + (port - PCI_CONFIG_DATA);
                if (reg + size <= 256) {
                    memcpy(data, &p->config[reg], size);
                    return;
                }
            }
        }
        memset(data, 0xFF, size);
        return;
    }
    memset(data, 0xFF, size);
}

struct virtio_9p {
    uint8_t  bar0[VIRTIO_BAR0_SIZE];
    uint16_t last_avail_idx;
    int      srv_fd;
    struct hostfs *hostfs;
    uint32_t irq;
    uint64_t bar_addr;
};

static void virtio_9p_init(struct virtio_9p *v9, int srv_fd, const char *tag, uint32_t irq, uint64_t bar_addr) {
    memset(v9, 0, sizeof(*v9));
    v9->srv_fd = srv_fd;
    v9->irq = irq;
    v9->bar_addr = bar_addr;

    /* Common config defaults */
    *(uint32_t *)(v9->bar0 + 0x00) = 0;      /* devfeatsel = 0 */
    *(uint32_t *)(v9->bar0 + 0x04) = 1;      /* devfeat = 1 (Fmounttag) */
    *(uint16_t *)(v9->bar0 + 0x18) = 16;     /* queuesize = 16 */
    *(uint16_t *)(v9->bar0 + 0x12) = 1;      /* nqueues = 1 */
    *(uint16_t *)(v9->bar0 + 0x10) = 0xFFFF; /* msixcfg = none */
    *(uint16_t *)(v9->bar0 + 0x1A) = 0xFFFF; /* queuemsixvect = none */

    /* Device config (offset 0x300): taglen, tag */
    size_t taglen = strlen(tag);
    if (taglen > 64) taglen = 64;
    *(uint16_t *)(v9->bar0 + 0x300) = (uint16_t)taglen;
    memcpy(v9->bar0 + 0x302, tag, taglen);
}

static inline int is_valid_ram_range(uint64_t addr, uint64_t len, size_t ram_size) {
    if (len == 0) return 0;
    if (addr >= ram_size) return 0;
    if (len > ram_size) return 0;
    if (addr > ram_size - len) return 0;
    return 1;
}

static void virtio_9p_process_queue(int vm, uint8_t *ram, size_t ram_size, struct virtio_9p *v9) {
    if (v9->hostfs)
        virtio_host_kicks++;
    uint16_t queue_enable = *(uint16_t *)(v9->bar0 + 0x1C);
    if (!queue_enable || (v9->srv_fd < 0 && !v9->hostfs))
        return;

    uint64_t desc_table_pa = *(uint64_t *)(v9->bar0 + 0x20);
    uint64_t avail_ring_pa = *(uint64_t *)(v9->bar0 + 0x28);
    uint64_t used_ring_pa  = *(uint64_t *)(v9->bar0 + 0x30);
    uint16_t queue_size    = *(uint16_t *)(v9->bar0 + 0x18);
    if (queue_size == 0 || queue_size > 32768)
        return;

    uint64_t desc_bytes  = (uint64_t)queue_size * 16;
    uint64_t avail_bytes = 4 + (uint64_t)queue_size * 2 + 2;
    uint64_t used_bytes  = 4 + (uint64_t)queue_size * 8 + 2;

    if (!is_valid_ram_range(desc_table_pa, desc_bytes, ram_size) ||
        !is_valid_ram_range(avail_ring_pa, avail_bytes, ram_size) ||
        !is_valid_ram_range(used_ring_pa, used_bytes, ram_size)) {
        fprintf(stderr, "[9vm] virtio9p queue structures out of range\n");
        return;
    }

    uint16_t avail_idx = *(uint16_t *)(ram + avail_ring_pa + 2);
    int did_work = 0;

    while (v9->last_avail_idx != avail_idx) {
        uint16_t ring_idx = v9->last_avail_idx % queue_size;
        uint16_t head = *(uint16_t *)(ram + avail_ring_pa + 4 + ring_idx * 2);
        if (head >= queue_size) {
            fprintf(stderr, "[9vm] virtio9p invalid head index %u >= %u\n", head, queue_size);
            v9->last_avail_idx++;
            continue;
        }

        /* Walk descriptor chain starting from head */
        uint16_t desc_idx = head;
        uint8_t *req_buf = NULL;
        uint32_t req_len = 0;
        uint8_t *resp_buf = NULL;
        uint32_t resp_max = 0;
        int desc_ok = 1;

        for (int i = 0; i < queue_size; i++) {
            if (desc_idx >= queue_size) {
                desc_ok = 0;
                break;
            }
            uint64_t d_addr  = *(uint64_t *)(ram + desc_table_pa + desc_idx * 16);
            uint32_t d_len   = *(uint32_t *)(ram + desc_table_pa + desc_idx * 16 + 8);
            uint16_t d_flags = *(uint16_t *)(ram + desc_table_pa + desc_idx * 16 + 12);
            uint16_t d_next  = *(uint16_t *)(ram + desc_table_pa + desc_idx * 16 + 14);

            if (!is_valid_ram_range(d_addr, d_len, ram_size)) {
                fprintf(stderr, "[9vm] virtio9p invalid descriptor buffer [0x%lx, len %u]\n",
                        (unsigned long)d_addr, d_len);
                desc_ok = 0;
                break;
            }

            if (d_flags & 2) { /* VRING_DESC_F_WRITE: device writes */
                resp_buf = ram + d_addr;
                resp_max = d_len;
            } else {          /* device reads */
                req_buf = ram + d_addr;
                req_len = d_len;
            }

            if (d_flags & 1) {    /* VRING_DESC_F_NEXT */
                desc_idx = d_next;
            } else {
                break;
            }
        }

        if (!desc_ok) {
            v9->last_avail_idx++;
            continue;
        }

        uint32_t resp_len = 0;
        if (req_buf && resp_buf && req_len > 0) {
            if (v9->hostfs) {
                resp_len = hostfs_dispatch(v9->hostfs, req_buf, req_len, resp_buf, resp_max);
            } else if (v9->srv_fd >= 0) {
                ssize_t nw = write(v9->srv_fd, req_buf, req_len);
                if (nw == (ssize_t)req_len) {
                    uint32_t total_size = 0;
                    ssize_t nr = read(v9->srv_fd, &total_size, 4);
                    if (nr == 4 && total_size <= resp_max && total_size >= 4) {
                        memcpy(resp_buf, &total_size, 4);
                        size_t remaining = total_size - 4;
                        size_t total_read = 4;
                        while (remaining > 0) {
                            size_t chunk = remaining;
                            if (total_read + chunk > resp_max)
                                break;
                            nr = read(v9->srv_fd, resp_buf + total_read, chunk);
                            if (nr <= 0) break;
                            total_read += nr;
                            remaining -= nr;
                        }
                        resp_len = total_read;
                    } else {
                        fprintf(stderr, "[9vm] 9p server invalid reply (size %u, max %u)\n", total_size, resp_max);
                    }
                }
            }
        }

        /* Write to used ring */
        uint16_t used_idx = *(uint16_t *)(ram + used_ring_pa + 2);
        uint16_t used_slot = used_idx % queue_size;
        *(uint32_t *)(ram + used_ring_pa + 4 + used_slot * 8) = head;
        *(uint32_t *)(ram + used_ring_pa + 4 + used_slot * 8 + 4) = resp_len;
        *(uint16_t *)(ram + used_ring_pa + 2) = used_idx + 1;

        v9->last_avail_idx++;
        did_work = 1;
    }

    if (did_work) {
        v9->bar0[0x200] |= 1; /* Set ISR status bit 0 */
        struct kvm_irq_level irq = { .irq = v9->irq, .level = 1 };
        ioctl(vm, KVM_IRQ_LINE, &irq);
    }
}

static void virtio_9p_mmio_write(int vm, uint8_t *ram, size_t ram_size, struct virtio_9p *v9,
                                 uint64_t addr, uint8_t *data, uint32_t len) {
    uint64_t offset = addr - v9->bar_addr;
    if (offset + len > VIRTIO_BAR0_SIZE)
        return;

    memcpy(v9->bar0 + offset, data, len);

    if (offset == 0x00 && len >= 4) {
        uint32_t sel = *(uint32_t *)(v9->bar0 + 0x00);
        uint32_t feat = 0;
        if (sel == 0) {
            feat = 1; /* bit 0: Fmounttag */
        } else if (sel == 1) {
            feat = 1; /* bit 32: VIRTIO_F_VERSION_1 */
        }
        *(uint32_t *)(v9->bar0 + 0x04) = feat;
    } else if (offset == 0x14) {
        uint8_t status = v9->bar0[0x14];
        if (status == 0) {
            v9->last_avail_idx = 0;
            v9->bar0[0x200] = 0;
            struct kvm_irq_level irq = { .irq = v9->irq, .level = 0 };
            ioctl(vm, KVM_IRQ_LINE, &irq);
        }
    } else if (offset == 0x16) {
        uint16_t sel = *(uint16_t *)(v9->bar0 + 0x16);
        *(uint16_t *)(v9->bar0 + 0x18) = (sel == 0) ? 16 : 0;
    } else if (offset >= 0x100 && offset < 0x104) {
        virtio_9p_process_queue(vm, ram, ram_size, v9);
    }
}

static void virtio_9p_mmio_read(int vm, struct virtio_9p *v9,
                                uint64_t addr, uint8_t *data, uint32_t len) {
    uint64_t offset = addr - v9->bar_addr;
    if (offset + len > VIRTIO_BAR0_SIZE) {
        memset(data, 0, len);
        return;
    }

    if (offset == 0x04) {
        uint32_t sel = *(uint32_t *)(v9->bar0 + 0x00);
        uint32_t feat = 0;
        if (sel == 0) {
            feat = 1; /* bit 0: Fmounttag */
        } else if (sel == 1) {
            feat = 1; /* bit 32: VIRTIO_F_VERSION_1 */
        }
        memcpy(data, &feat, len);
        return;
    }

    if (offset == 0x200) {
        memcpy(data, v9->bar0 + offset, len);
        v9->bar0[0x200] = 0;
        struct kvm_irq_level irq = { .irq = v9->irq, .level = 0 };
        ioctl(vm, KVM_IRQ_LINE, &irq);
        return;
    }

    memcpy(data, v9->bar0 + offset, len);
}static int connect_unix_socket(const char *path) {
    if (!path || !*path)
        return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, path, sizeof(sun.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv) {
    clock_gettime(CLOCK_MONOTONIC, &ts_boot_start);
    const char *kernel_path = NULL;
    const char *cmdline = "bootargs=local!#9/0 nobootprompt=local";
    const char *root_dir = ".";
    const char *boot_override = NULL;
    const char *display_name = NULL;
    const char *display_root = NULL;
    const char *gefs_image = NULL;
    bool legacy_host_root = false;
    bool isolated_gefs_root;
    char image_cmdline[256];
    const char *netfs_path = "/tmp/netfs.sock";
    size_t ram_mb = DEFAULT_RAM_MB;
    bool use_hostring = true;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            ram_mb = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            cmdline = argv[++i];
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            root_dir = argv[++i];
        } else if (strcmp(argv[i], "-boot") == 0 && i + 1 < argc) {
            boot_override = argv[++i];
        } else if (strcmp(argv[i], "-display-shm") == 0 && i + 1 < argc) {
            display_name = argv[++i];
        } else if (strcmp(argv[i], "-display-root") == 0 && i + 1 < argc) {
            display_root = argv[++i];
        } else if (strcmp(argv[i], "-gefs-image") == 0 && i + 1 < argc) {
            gefs_image = argv[++i];
        } else if (strcmp(argv[i], "-legacy-host-root") == 0) {
            legacy_host_root = true;
        } else if (strcmp(argv[i], "-u9fs") == 0 && i + 1 < argc) {
            i++; /* legacy u9fs option ignored: now in-process */
        } else if (strcmp(argv[i], "-netfs") == 0 && i + 1 < argc) {
            netfs_path = argv[++i];
        } else if (strcmp(argv[i], "-no9ring") == 0) {
            use_hostring = false;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            i++; /* legacy tap ignored */
        } else if (!kernel_path) {
            kernel_path = argv[i];
        }
    }

    if (!kernel_path) {
        fprintf(stderr, "Usage: %s [-m <mb>] [-c <cmdline>] [-r <root>] [-boot <guest>] [-display-shm <name>] [-display-root <dir>] [-netfs <sock>] [-gefs-image <image>] [-legacy-host-root] [-no9ring] <kernel-elf>\n", argv[0]);
        return 1;
    }
    isolated_gefs_root = gefs_image != NULL && boot_override == NULL && !legacy_host_root;
    if (isolated_gefs_root) {
        if (snprintf(image_cmdline, sizeof image_cmdline, "%s gefsroot=1", cmdline) >= (int)sizeof image_cmdline) {
            fprintf(stderr, "[9vm] GEFS kernel command line too long\n");
            return 1;
        }
        cmdline = image_cmdline;
        use_hostring = false;
    }
    if (display_name != NULL) {
        atexit(display_cleanup);
        if (display_init(display_name) < 0) {
            perror("[9vm] display shared memory");
            return 1;
        }
    }

    if (use_hostring) {
        cpu_set_t available;
        if (sched_getaffinity(0, sizeof(available), &available) == 0 &&
            CPU_COUNT(&available) < 2) {
            fprintf(stderr, "[9vm] Only one CPU available; using VirtIO 9P for hostfs\n");
            use_hostring = false;
        }
    }

    char real_root[PATH_MAX];
    if (!isolated_gefs_root) {
        if (realpath(root_dir, real_root) == NULL) {
            perror("[9vm] realpath root_dir");
            return 1;
        }
        if (strcmp(real_root, "/") == 0) {
            fprintf(stderr, "[9vm] Error: Refusing to serve host root '/' over 9P for security.\n");
            return 1;
        }
        root_dir = real_root;
    }

    struct hostfs hostfs_inst;
    if (isolated_gefs_root && display_root != NULL) {
        if (display_name == NULL || realpath(display_root, real_root) == NULL ||
            strcmp(real_root, "/") == 0) {
            fprintf(stderr, "[9vm] Invalid display exchange directory\n");
            return 1;
        }
        hostfs_init(&hostfs_inst, real_root);
    } else if (isolated_gefs_root) {
        if (hostfs_init_image(&hostfs_inst, gefs_image) < 0) {
            fprintf(stderr, "[9vm] Invalid GEFS image: %s\n", gefs_image);
            return 1;
        }
    } else
        hostfs_init(&hostfs_inst, root_dir);
    struct hostfs *diskfs_inst = NULL;
    if (gefs_image != NULL) {
        diskfs_inst = calloc(1, sizeof(*diskfs_inst));
        if (diskfs_inst == NULL || hostfs_init_image(diskfs_inst, gefs_image) < 0) {
            fprintf(stderr, "[9vm] Invalid GEFS image: %s\n", gefs_image);
            free(diskfs_inst);
            return 1;
        }
    }
    hostfs_set_display(&hostfs_inst, vm_display);
    if (boot_override != NULL && hostfs_set_boot_override(&hostfs_inst, boot_override) < 0) {
        fprintf(stderr, "[9vm] Invalid boot override: %s\n", boot_override);
        return 1;
    }
    signal(SIGINT, vm_sig_handler);
    signal(SIGTERM, vm_sig_handler);
    signal(SIGALRM, stdin_wake_handler);
    if (isolated_gefs_root)
        fprintf(stderr, "[9vm] GEFS root mode: only the image file is exported\n");
    else
        fprintf(stderr, "[9vm] In-process hostfs online serving '%s'\n", root_dir);

    int netfs_fd = connect_unix_socket(netfs_path);
    if (netfs_fd < 0 && (access("util/9vm/netfs", X_OK) == 0 || access("/home/scott/Repo/harvey/util/9vm/netfs", X_OK) == 0)) {
        const char *netfs_bin = access("util/9vm/netfs", X_OK) == 0 ? "util/9vm/netfs" : "/home/scott/Repo/harvey/util/9vm/netfs";
        pid_t npid = fork();
        if (npid == 0) {
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, 0);
                dup2(devnull, 1);
                dup2(devnull, 2);
                close(devnull);
            }
            execl(netfs_bin, "netfs", "-s", netfs_path, NULL);
            _exit(1);
        }
        for (int retry = 0; retry < 50; retry++) {
            usleep(1000);
            netfs_fd = connect_unix_socket(netfs_path);
            if (netfs_fd >= 0) break;
        }
    }
    if (netfs_fd >= 0) {
        fprintf(stderr, "[9vm] Connected virtio9p tag 'net' to '%s' (fd %d)\n", netfs_path, netfs_fd);
    } else {
        fprintf(stderr, "[9vm] Note: netfs socket not found at '%s', virtio9p net idle\n", netfs_path);
    }

    if (getenv("VM_TRACE")) fprintf(stderr, "[perf +%.2f ms] netfs init\n", get_boot_ms());

    uint32_t pci_addr_reg = 0;
    struct pci_dev pci_9p_host;
    pci_dev_init(&pci_9p_host, 0x1049, 9, VIRTIO_9P_HOST_BAR0_ADDR, 10);

    struct pci_dev pci_9p_net;
    pci_dev_init(&pci_9p_net, 0x1049, 9, VIRTIO_9P_NET_BAR0_ADDR, 11);

    struct pci_dev pci_9p_disk;
    if (gefs_image != NULL)
        pci_dev_init(&pci_9p_disk, 0x1049, 9, VIRTIO_9P_DISK_BAR0_ADDR, 12);

    struct virtio_9p v9_host;
    virtio_9p_init(&v9_host, -1, "host", 10, VIRTIO_9P_HOST_BAR0_ADDR);
    v9_host.hostfs = &hostfs_inst;

    struct virtio_9p v9_net;
    virtio_9p_init(&v9_net, netfs_fd, "net", 11, VIRTIO_9P_NET_BAR0_ADDR);

    struct virtio_9p v9_disk;
    if (gefs_image != NULL) {
        virtio_9p_init(&v9_disk, -1, "gefs-image", 12, VIRTIO_9P_DISK_BAR0_ADDR);
        v9_disk.hostfs = diskfs_inst;
        fprintf(stderr, "[9vm] GEFS image exported as VirtIO 9P tag 'gefs-image' (#9/2)\n");
    }

    size_t ram_size = ram_mb * 1024 * 1024;

    int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm < 0) {
        perror("open /dev/kvm");
        return 1;
    }

    int version = ioctl(kvm, KVM_GET_API_VERSION, 0);
    if (version != KVM_API_VERSION) {
        fprintf(stderr, "KVM API version mismatch: expected %d, got %d\n", KVM_API_VERSION, version);
        return 1;
    }

    int vm = ioctl(kvm, KVM_CREATE_VM, 0);
    if (vm < 0) {
        perror("KVM_CREATE_VM");
        return 1;
    }
    g_vm = vm;

    if (ioctl(vm, KVM_CREATE_IRQCHIP, 0) < 0) {
        perror("KVM_CREATE_IRQCHIP");
        return 1;
    }

    struct kvm_pit_config pit = { 0 };
    if (ioctl(vm, KVM_CREATE_PIT2, &pit) < 0) {
        perror("KVM_CREATE_PIT2");
        return 1;
    }

    uint8_t *ram = mmap(NULL, ram_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (ram == MAP_FAILED) {
        perror("mmap RAM");
        return 1;
    }
    size_t early_ram = ram_size < (64 * 1024 * 1024) ? ram_size : (64 * 1024 * 1024);
    madvise(ram, early_ram, MADV_WILLNEED);
    madvise(ram, ram_size, MADV_HUGEPAGE);

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .flags = 0,
        .guest_phys_addr = 0,
        .memory_size = ram_size,
        .userspace_addr = (uint64_t)ram,
    };
    if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION");
        return 1;
    }
    if (getenv("VM_TRACE")) fprintf(stderr, "[perf +%.2f ms] vm & ram created\n", get_boot_ms());

    uint64_t entry_point = load_elf(kernel_path, ram, ram_size);
    if (getenv("VM_TRACE")) fprintf(stderr, "[perf +%.2f ms] load_elf done\n", get_boot_ms());

    if (use_hostring && (ram_size <= P9_HOST_REGION_SIZE + 0x100000 ||
        sizeof(P9HostShared) > P9_HOST_REGION_SIZE)) {
        fprintf(stderr, "[9vm] RAM too small for host 9ring\n");
        return 1;
    }
    if (use_hostring) {
        hostring = (P9HostShared *)(ram + ram_size - P9_HOST_REGION_SIZE);
        memset(hostring, 0, sizeof(*hostring));
        hostring->magic = P9_HOST_MAGIC;
        hostring->version = P9_HOST_VERSION;
        hostring_fs = malloc(sizeof(*hostring_fs));
        if (!hostring_fs) {
            perror("[9vm] hostring filesystem allocation");
            return 1;
        }
        hostfs_init(hostring_fs, root_dir);
        hostfs_set_display(hostring_fs, vm_display);
        if (boot_override != NULL &&
            hostfs_set_boot_override(hostring_fs, boot_override) < 0) {
            fprintf(stderr, "[9vm] Invalid hostring boot override\n");
            hostring_stop();
            return 1;
        }
        __atomic_store_n(&hostring_running, 1, __ATOMIC_RELEASE);
        if (pthread_create(&hostring_thread, NULL, hostring_serve, NULL) != 0) {
            perror("[9vm] hostring thread");
            hostring_running = 0;
            hostring_stop();
            return 1;
        }
        fprintf(stderr, "[9vm] Shared host 9ring at guest physical 0x%zx\n",
                ram_size - P9_HOST_REGION_SIZE);
    }

    setup_multiboot(ram, ram_size, cmdline, use_hostring);
    setup_gdt(ram);
    setup_acpi(ram);

    int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
    if (vcpu < 0) {
        perror("KVM_CREATE_VCPU");
        return 1;
    }
    setup_cpuid(kvm, vcpu);

    int vcpu_mmap_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    struct kvm_run *run = mmap(NULL, vcpu_mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
    if (run == MAP_FAILED) {
        perror("mmap vcpu");
        return 1;
    }

    struct kvm_sregs sregs;
    if (ioctl(vcpu, KVM_GET_SREGS, &sregs) < 0) {
        perror("KVM_GET_SREGS");
        return 1;
    }

    sregs.gdt.base = GDT_ADDR;
    sregs.gdt.limit = 5 * sizeof(uint64_t) - 1;

    set_segment(&sregs.cs, 0x08, 0, 0xFFFFFFFF, 11, 1, 0);
    set_segment(&sregs.ds, 0x10, 0, 0xFFFFFFFF, 3,  1, 0);
    set_segment(&sregs.es, 0x10, 0, 0xFFFFFFFF, 3,  1, 0);
    set_segment(&sregs.fs, 0x10, 0, 0xFFFFFFFF, 3,  1, 0);
    set_segment(&sregs.gs, 0x10, 0, 0xFFFFFFFF, 3,  1, 0);
    set_segment(&sregs.ss, 0x10, 0, 0xFFFFFFFF, 3,  1, 0);

    sregs.cr0 = 0x00000001; /* PE = 1 (protected mode) */
    sregs.cr4 = 0;
    sregs.efer = 0;

    if (ioctl(vcpu, KVM_SET_SREGS, &sregs) < 0) {
        perror("KVM_SET_SREGS");
        return 1;
    }

    struct kvm_regs regs = { 0 };
    regs.rax = MULTIBOOT_BOOTLOADER_MAGIC;
    regs.rbx = MBI_ADDR;
    regs.rip = entry_point;
    regs.rsp = BOOT_STACK;
    regs.rflags = 0x02;

    if (ioctl(vcpu, KVM_SET_REGS, &regs) < 0) {
        perror("KVM_SET_REGS");
        return 1;
    }
    if (getenv("VM_TRACE")) fprintf(stderr, "[perf +%.2f ms] vcpu ready, starting KVM_RUN\n", get_boot_ms());

    fprintf(stderr, "[9vm] Booting %s (entry 0x%lx, RAM %zu MB)...\n", kernel_path, entry_point, ram_mb);

    static bool first_exit = true;
    static bool first_uart = true;

    /* make stdin non-blocking and set a periodic SIGALRM so KVM_RUN is
     * interrupted regularly and we can poll stdin even while the guest is
     * idle/HLTed (the KVM_IRQ_LINE from another thread isn't delivered). */
    {
        int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (fl >= 0)
            fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
    }
    struct itimerval it = { {0, 200000}, {0, 200000} };  /* 200ms periodic */
    setitimer(ITIMER_REAL, &it, NULL);

    while (vm_running) {
        /* poll stdin and feed it to the UART (0x3F8) as receive data */
        if (!uart_rx_ready) {
            uint8_t c;
            ssize_t n = read(STDIN_FILENO, &c, 1);
            if (n == 1) {
                uart_rx_char = c;
                uart_rx_ready = 1;
            }
        }

        if (ioctl(vcpu, KVM_RUN, 0) < 0) {
            if (errno == EINTR) {
                if (getenv("VM_TRACE")) fprintf(stderr, "[9vm] EINTR\n");
                if (!vm_running) break;
                continue;
            }
            perror("KVM_RUN");
            break;
        }

        if (first_exit && getenv("VM_TRACE")) {
            fprintf(stderr, "[perf +%.2f ms] first vmexit (reason %d)\n", get_boot_ms(), run->exit_reason);
            first_exit = false;
        }

        if (run->exit_reason == KVM_EXIT_IO)
            vm_io_exits++;
        else if (run->exit_reason == KVM_EXIT_MMIO)
            vm_mmio_exits++;

        switch (run->exit_reason) {
        case KVM_EXIT_IO: {
            uint8_t *data = (uint8_t *)run + run->io.data_offset;
            if (run->io.direction == KVM_EXIT_IO_OUT) {
                if (run->io.port == 0x3F8) {
                    if (first_uart && getenv("VM_TRACE")) {
                        fprintf(stderr, "[perf +%.2f ms] first uart write (char '%c')\n", get_boot_ms(), data[0]);
                        first_uart = false;
                    }
                    fwrite(data, 1, run->io.count, stdout);
                    fflush(stdout);
                    bench_serial(data, run->io.count);
                } else if (run->io.port == 0x3FF) {
                    /* UART Scratchpad */
                    uart_scr = *data;
                } else if (run->io.port == 0xF4) {
                    fprintf(stderr, "\n[9vm] Guest requested shutdown.\n");
                    hostring_stop();
                    hostfs_free(&hostfs_inst);
                    if (diskfs_inst != NULL) { hostfs_free(diskfs_inst); free(diskfs_inst); }
                    if (netfs_fd >= 0) close(netfs_fd);
                    return 0;
                } else if (run->io.port == 0x80) {
                    /* POST debug port */
                } else if (run->io.port >= 0xCF8 && run->io.port <= 0xCFF) {
                    for (__u32 i = 0; i < run->io.count; i++) {
                        pci_io_write(&pci_9p_host, &pci_9p_net, gefs_image != NULL ? &pci_9p_disk : NULL, &pci_addr_reg, run->io.port, data + i * run->io.size, run->io.size);
                    }
                }
            } else {
                /* IN instruction */
                if (run->io.port == 0x3FD) {
                    /* Line Status Register: bit0 = RX data ready, bits5-6 = THR/TX empty */
                    *data = 0x20 | 0x40 | (uart_rx_ready ? 0x01 : 0x00);
                } else if (run->io.port == 0x3F8) {
                    /* RBR: return the received character, clear data-ready */
                    *data = uart_rx_char;
                    uart_rx_ready = 0;
                } else if (run->io.port == 0x3FA) {
                    /* IIR: 0x04 = receive data available, 0x01 = no interrupt */
                    *data = uart_rx_ready ? 0x04 : 0x01;
                } else if (run->io.port == 0x3FF) {
                    *data = uart_scr;
                } else if (run->io.port >= 0xCF8 && run->io.port <= 0xCFF) {
                    for (__u32 i = 0; i < run->io.count; i++) {
                        pci_io_read(&pci_9p_host, &pci_9p_net, gefs_image != NULL ? &pci_9p_disk : NULL, &pci_addr_reg, run->io.port, data + i * run->io.size, run->io.size);
                    }
                } else {
                    memset(data, 0xFF, run->io.size);
                }
            }
            break;
        }
        case KVM_EXIT_MMIO: {
            uint64_t paddr = run->mmio.phys_addr;
            if (paddr >= VIRTIO_9P_HOST_BAR0_ADDR && paddr < VIRTIO_9P_HOST_BAR0_ADDR + VIRTIO_BAR0_SIZE) {
                if (run->mmio.is_write) {
                    virtio_9p_mmio_write(vm, ram, ram_size, &v9_host, paddr, run->mmio.data, run->mmio.len);
                } else {
                    virtio_9p_mmio_read(vm, &v9_host, paddr, run->mmio.data, run->mmio.len);
                }
            } else if (paddr >= VIRTIO_9P_NET_BAR0_ADDR && paddr < VIRTIO_9P_NET_BAR0_ADDR + VIRTIO_BAR0_SIZE) {
                if (run->mmio.is_write) {
                    virtio_9p_mmio_write(vm, ram, ram_size, &v9_net, paddr, run->mmio.data, run->mmio.len);
                } else {
                    virtio_9p_mmio_read(vm, &v9_net, paddr, run->mmio.data, run->mmio.len);
                }
            } else if (gefs_image != NULL && paddr >= VIRTIO_9P_DISK_BAR0_ADDR &&
                       paddr < VIRTIO_9P_DISK_BAR0_ADDR + VIRTIO_BAR0_SIZE) {
                if (run->mmio.is_write)
                    virtio_9p_mmio_write(vm, ram, ram_size, &v9_disk, paddr, run->mmio.data, run->mmio.len);
                else
                    virtio_9p_mmio_read(vm, &v9_disk, paddr, run->mmio.data, run->mmio.len);
            } else {
                if (!run->mmio.is_write) {
                    memset(run->mmio.data, 0, run->mmio.len);
                }
            }
            break;
        }
        case KVM_EXIT_HLT:
            fprintf(stderr, "\n[9vm] Guest CPU halted.\n");
            hostring_stop();
            return 0;
        case KVM_EXIT_FAIL_ENTRY:
            fprintf(stderr, "[9vm] Hardware entry failure: 0x%llx\n",
                    (unsigned long long)run->fail_entry.hardware_entry_failure_reason);
            return 1;
        case KVM_EXIT_INTERNAL_ERROR:
            fprintf(stderr, "[9vm] Internal error: %u\n", run->internal.suberror);
            return 1;
        case KVM_EXIT_SHUTDOWN: {
            fprintf(stderr, "[9vm] Guest shutdown / triple fault.\n");
            struct kvm_regs r;
            struct kvm_sregs sr;
            if (ioctl(vcpu, KVM_GET_REGS, &r) == 0 && ioctl(vcpu, KVM_GET_SREGS, &sr) == 0) {
                fprintf(stderr, "[9vm] RIP: 0x%016llx  RSP: 0x%016llx  RFLAGS: 0x%08llx\n",
                        (unsigned long long)r.rip, (unsigned long long)r.rsp, (unsigned long long)r.rflags);
                fprintf(stderr, "[9vm] RAX: 0x%016llx  RBX: 0x%016llx  RCX: 0x%016llx  RDX: 0x%016llx\n",
                        (unsigned long long)r.rax, (unsigned long long)r.rbx, (unsigned long long)r.rcx, (unsigned long long)r.rdx);
                fprintf(stderr, "[9vm] RSI: 0x%016llx  RDI: 0x%016llx  RBP: 0x%016llx\n",
                        (unsigned long long)r.rsi, (unsigned long long)r.rdi, (unsigned long long)r.rbp);
                fprintf(stderr, "[9vm] CR0: 0x%016llx  CR3: 0x%016llx  CR4: 0x%016llx  EFER: 0x%016llx\n",
                        (unsigned long long)sr.cr0, (unsigned long long)sr.cr3, (unsigned long long)sr.cr4, (unsigned long long)sr.efer);
                fprintf(stderr, "[9vm] CS: sel=0x%04x base=0x%llx lim=0x%x dpl=%d db=%d l=%d\n",
                        sr.cs.selector, (unsigned long long)sr.cs.base, sr.cs.limit, sr.cs.dpl, sr.cs.db, sr.cs.l);
            }
            return 1;
        }
        default:
            fprintf(stderr, "[9vm] Unhandled exit reason: %u\n", run->exit_reason);
            return 1;
        }
    }

    hostring_stop();
    hostfs_free(&hostfs_inst);
    if (diskfs_inst != NULL) { hostfs_free(diskfs_inst); free(diskfs_inst); }
    if (netfs_fd >= 0) close(netfs_fd);
    return 0;
}
