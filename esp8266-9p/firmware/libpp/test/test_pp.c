/* Host tests for pp_lldesc and pp_esf. Run: see ../README.md. */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../pp_lldesc.h"
#include "../pp_esf.h"
#include "../pp_arena.h"

static void
test_desc_fields(void)
{
	static uint8_t buf[1600];
	pp_desc d;

	pp_desc_init(&d, buf, 1600);
	assert(sizeof(pp_desc) == 12);
	assert(pp_desc_size(&d) == 1600);
	assert(pp_desc_len(&d) == 0);
	assert(!pp_desc_done(&d));

	pp_desc_offer_rx(&d, 0);
	assert((d.ctrl & PP_DESC_OWNER) != 0);
	assert((d.ctrl & (PP_DESC_EOF | PP_DESC_SUB_SOF)) == 0);
	assert(pp_desc_len(&d) == 1600);
	assert(pp_desc_size(&d) == 1600);

	pp_desc_offer_rx(&d, 1);
	assert(pp_desc_len(&d) == 1600 - PP_RX_FCS_RESERVE);

	/* Simulate hardware completion: sets eof, hands back owner. */
	d.ctrl |= PP_DESC_EOF;
	d.ctrl &= ~PP_DESC_OWNER;
	assert(pp_desc_done(&d));

	pp_desc_reclaim(&d);
	assert((d.ctrl & (PP_DESC_OWNER | PP_DESC_EOF | PP_DESC_SUB_SOF)) == 0);
	assert(pp_desc_size(&d) == 1600);
}

static void
test_chain(void)
{
	static pp_desc descs[4];
	static uint8_t bufs[4 * 256];
	int i;

	pp_desc_chain(descs, 4, bufs, 256);
	for (i = 0; i < 4; i++) {
		/* buf/next are 32-bit hardware addresses: compare raw
		 * fields, since host pointers are 64-bit. */
		assert(descs[i].buf == (uint32_t)(uintptr_t)(bufs + i * 256));
		assert(pp_desc_size(&descs[i]) == 256);
		assert(descs[i].next ==
		       (uint32_t)(uintptr_t)(i < 3 ? &descs[i + 1] : NULL));
	}
}

static void
test_pool(void)
{
	static pp_buf bufs[3];
	static uint8_t storage[3 * 512];
	pp_pool pool;
	pp_buf *a, *b, *c;

	pp_pool_init(&pool, bufs, storage, 3, 512);
	assert(pool.nfree == 3);

	a = pp_buf_alloc(&pool);
	b = pp_buf_alloc(&pool);
	c = pp_buf_alloc(&pool);
	assert(a != NULL && b != NULL && c != NULL);
	assert(a != b && b != c && a != c);
	assert(pp_buf_alloc(&pool) == NULL);
	assert(pool.nfree == 0);

	pp_buf_offer_rx(&pool, a, 1);
	assert((a->desc.ctrl & PP_DESC_OWNER) != 0);
	assert(pp_desc_len(&a->desc) == 512 - PP_RX_FCS_RESERVE);

	pp_buf_free(&pool, a);
	assert(pool.nfree == 1);
	assert((a->desc.ctrl & PP_DESC_OWNER) == 0);
	assert(pp_buf_alloc(&pool) == a);
	pp_buf_free(&pool, a);
	pp_buf_free(&pool, b);
	pp_buf_free(&pool, c);
	assert(pool.nfree == 3);
	pp_buf_free(&pool, a); /* double free is a no-op after reclaim */
	assert(pool.nfree == 3);
}

static void
test_arena(void)
{
	static uint8_t mem[1024];
	pp_arena a;
	uint32_t mark;
	char *p1, *p2, *g, *before;

	pp_arena_init(&a, mem, sizeof(mem));
	p1 = pp_arena_alloc(&a, 100, 8);
	assert(p1 != NULL);
	assert(((uintptr_t)p1 & 7) == 0);
	memset(p1, 0xAB, 100);

	mark = pp_arena_mark(&a);
	p2 = pp_arena_alloc(&a, 200, 4);
	assert(p2 != NULL);
	pp_arena_release(&a, mark);
	/* After rewind the same address is handed out again. */
	assert(pp_arena_alloc(&a, 200, 4) == p2);
	/* Earlier allocations never moved. */
	assert((uint8_t)p1[0] == 0xAB && (uint8_t)p1[99] == 0xAB);
	pp_arena_release(&a, mark);

	g = pp_arena_growable(&a, 16);
	assert(g != NULL);
	memset(g, 1, 16);
	before = g;
	g = pp_arena_grow(&a, g, 512);
	assert(g == before); /* growth never moves data */
	assert((uint8_t)g[15] == 1);
	assert(pp_arena_grow(&a, p1, 900) == NULL); /* not growable */
	pp_arena_release(&a, mark);
	assert(pp_arena_alloc(&a, 2000, 8) == NULL); /* exhaustion */
	assert(pp_arena_highwater(&a) >= 512);
	pp_arena_reset(&a);
	assert(pp_arena_mark(&a) == 0);
}

static void
test_blob_layout(void)
{
	uint8_t hdr[PP_ESF_HDR_SIZE];
	uint32_t v;

	/* Blob offsets verified against RTOS esf_buf.o (spec 5.1);
	 * the mirror struct is exact on the 32-bit target. */
	assert(PP_ESF_BLOB_SIZE == 68);
	assert(PP_ESF_HDR_SIZE == 28 && PP_ESF_RXHDR_SIZE == 8);
	assert(PP_ESF_HEADROOM == 36);

	/* Compose: hand-computed from spec 5.3. */
	memset(hdr, 0, sizeof(hdr));
	/* hdr+8 word 0x00056a00: bits [13:8]=0x2a, [19:14]=0x15 */
	hdr[8] = 0; hdr[9] = 0x6a; hdr[10] = 0x05; hdr[11] = 0;
	/* hdr+4 word 0x030c0000: bits [25:18] = 0xc3 */
	hdr[4] = 0; hdr[5] = 0; hdr[6] = 0x0c; hdr[7] = 0x03;
	hdr[12] = 0x5a;
	v = pp_esf_compose_ctrl(hdr);
	assert((v & 0x3f00) == (0x2au << 8));
	assert((v & 0xfc000) == (0x15u << 14));
	assert((v & 0xff) == 0xc3);
	assert((v & 0xff00000u) == (0x5au << 20));
}

int
main(void)
{
	test_desc_fields();
	test_chain();
	test_pool();
	test_arena();
	test_blob_layout();
	puts("test_pp: all tests passed");
	return 0;
}
