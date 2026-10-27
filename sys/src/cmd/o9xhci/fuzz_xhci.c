/* SPDX-License-Identifier: GPL-2.0-only */
#include <stddef.h>
#include <stdint.h>
#include "xhci.h"
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	unsigned speed;
	if(size>65536) return 0;
	for(speed=1;speed<=4;speed++) (void)xhci_validate_config(data,(unsigned)size,speed);
	return 0;
}
