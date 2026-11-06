/* lmac_host.h - host-test register file for lmac.c (LMAC_HOST_TEST). */
#ifndef LMAC_HOST_H
#define LMAC_HOST_H

#include <stdint.h>

uint32_t *lmac_host_reg(uint32_t addr);
void lmac_host_reset_regs(void);

#endif
