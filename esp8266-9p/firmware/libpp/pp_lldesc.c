/* pp_lldesc - clean-room implementation of SPEC-WDEV-DMA section 1. */
#include "pp_lldesc.h"

/* Fail the build if the descriptor layout ever drifts from spec. */
typedef char pp_desc_size_check[(sizeof(pp_desc) == PP_DESC_EXPECTED_SIZE) ? 1 : -1];

void *
pp_desc_buf(const pp_desc *d)
{
	return (void *)(uintptr_t)d->buf;
}

pp_desc *
pp_desc_next(const pp_desc *d)
{
	return (pp_desc *)(uintptr_t)d->next;
}

void
pp_desc_set_next(pp_desc *d, pp_desc *next)
{
	d->next = (uint32_t)(uintptr_t)next;
	PP_MEMW();
}

void
pp_desc_init(pp_desc *d, void *buf, uint32_t size)
{
	d->ctrl = size & PP_DESC_SIZE_MASK;
	d->buf = (uint32_t)(uintptr_t)buf;
	d->next = 0;
	PP_MEMW();
}

uint32_t
pp_desc_size(const pp_desc *d)
{
	return d->ctrl & PP_DESC_SIZE_MASK;
}

uint32_t
pp_desc_len(const pp_desc *d)
{
	return (d->ctrl >> PP_DESC_LEN_SHIFT) & PP_DESC_SIZE_MASK;
}

void
pp_desc_set_len(pp_desc *d, uint32_t len)
{
	uint32_t w = d->ctrl;

	w &= ~(PP_DESC_SIZE_MASK << PP_DESC_LEN_SHIFT);
	w |= (len & PP_DESC_SIZE_MASK) << PP_DESC_LEN_SHIFT;
	d->ctrl = w;
	PP_MEMW();
}

void
pp_desc_offer_rx(pp_desc *d, int reserve_fcs)
{
	uint32_t size = pp_desc_size(d);
	uint32_t len = size;
	uint32_t w;

	if (reserve_fcs)
		len = size > PP_RX_FCS_RESERVE ? size - PP_RX_FCS_RESERVE : 0;
	w = d->ctrl;
	w &= ~(PP_DESC_EOF | PP_DESC_SUB_SOF);
	w &= ~(PP_DESC_SIZE_MASK << PP_DESC_LEN_SHIFT);
	w |= (len & PP_DESC_SIZE_MASK) << PP_DESC_LEN_SHIFT;
	w |= PP_DESC_OWNER;
	PP_MEMW();
	d->ctrl = w;
	PP_MEMW();
}

int
pp_desc_done(const pp_desc *d)
{
	return (d->ctrl & PP_DESC_EOF) != 0;
}

void
pp_desc_reclaim(pp_desc *d)
{
	uint32_t w = d->ctrl;

	w &= ~(PP_DESC_OWNER | PP_DESC_EOF | PP_DESC_SUB_SOF);
	PP_MEMW();
	d->ctrl = w;
	PP_MEMW();
}

void
pp_desc_chain(pp_desc *array, uint32_t n, void *bufs, uint32_t bufsize)
{
	uint32_t i;
	char *p = (char *)bufs;

	for (i = 0; i < n; i++) {
		pp_desc_init(&array[i], p ? p + i * bufsize : NULL, bufsize);
		pp_desc_set_next(&array[i], (i + 1 < n) ? &array[i + 1] : NULL);
	}
	PP_MEMW();
}
