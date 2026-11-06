/* pp_esf - clean-room buffer pool; see pp_esf.h for scope. */
#include "pp_esf.h"

#include <stddef.h>

static uint32_t
get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	       (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint32_t
pp_esf_compose_ctrl(const uint8_t hdr[PP_ESF_HDR_SIZE])
{
	uint32_t w8 = get32(hdr + 8);
	uint32_t w4 = get32(hdr + 4);
	uint32_t v;

	v = ((w8 >> 8) & 0x3f) << 8;
	v |= ((w8 >> 14) & 0x3f) << 14;
	v |= (w4 >> 18) & 0xff;
	v |= (uint32_t)hdr[12] << 20;
	return v;
}

void
pp_pool_init(pp_pool *pool, pp_buf *bufs, uint8_t *storage,
	     uint32_t count, uint32_t bufsize)
{
	uint32_t i;

	pool->bufs = bufs;
	pool->count = count;
	pool->bufsize = bufsize;
	pool->freehead = NULL;
	pool->nfree = 0;
	for (i = 0; i < count; i++) {
		pp_buf *b = &bufs[i];

		b->data = storage ? storage + i * bufsize : NULL;
		b->cap = bufsize;
		b->len = 0;
		b->inuse = 0;
		pp_desc_init(&b->desc, b->data, bufsize);
		b->next = pool->freehead;
		pool->freehead = b;
		pool->nfree++;
	}
}

pp_buf *
pp_buf_alloc(pp_pool *pool)
{
	pp_buf *b = pool->freehead;

	if (b == NULL)
		return NULL;
	pool->freehead = b->next;
	pool->nfree--;
	b->next = NULL;
	b->inuse = 1;
	b->len = 0;
	pp_desc_reclaim(&b->desc);
	return b;
}

void
pp_buf_free(pp_pool *pool, pp_buf *b)
{
	if (b == NULL || !b->inuse)
		return;
	pp_desc_reclaim(&b->desc);
	b->inuse = 0;
	b->len = 0;
	b->next = pool->freehead;
	pool->freehead = b;
	pool->nfree++;
}

void
pp_buf_offer_rx(pp_pool *pool, pp_buf *b, int reserve_fcs)
{
	(void)pool;
	pp_desc_offer_rx(&b->desc, reserve_fcs);
}
