/* p9_os_xtensa - debug hooks for the Xtensa substrate.
 *
 * Stack high-water comes from pattern painting: each proc stack
 * is filled with P9_XT_STACK_PATTERN at creation, stacks grow
 * down, and the untouched run at the low end is unused space.
 *
 * Register capture is a snapshot at the capture call site, not an
 * exception frame: pc is the caller's return address, sp is the
 * caller's stack pointer, and the special registers are read with
 * rsr. Useful for "what state was it in" dumps after an action.
 */
#ifndef P9_OS_XTENSA_H
#define P9_OS_XTENSA_H

#include <stdint.h>

#define P9_XT_STACK_PATTERN 0xA5u

typedef struct {
	uint32_t pc;        /* caller return address */
	uint32_t sp;        /* caller stack pointer */
	uint32_t a0;
	uint32_t ps;
	uint32_t exccause;
	uint32_t excvaddr;
	uint32_t epc1;
	uint32_t intenable;
	uint32_t interrupt;
	uint32_t ccount;
	uint32_t sar;
	uint32_t vecbase;
	uint32_t prid;
} P9XtRegs;

void p9_os_capture_regs(P9XtRegs *r);

uint32_t p9_os_stack_slots(void);
/* Bytes used on proc slot's stack, 0 if the slot is unused. */
uint32_t p9_os_stack_highwater(uint32_t slot);

/* Proc table occupancy (B26): total slots, live procs, and
 * the name in a slot (NULL if free) - so boot can print the
 * budget instead of discovering exhaustion by a missing
 * server. */
uint32_t p9_os_proc_slots(void);
uint32_t p9_os_proc_used(void);
const char *p9_os_proc_name(uint32_t slot);

#endif
