/* Host test for the clean-room esf_buf (firmware/libpp/esf_buf.c).
 * Provides the blob's platform hooks over malloc and checks the
 * B22/SPEC section 5 semantics: pool sizes from g_wifi_menuconfig
 * (byte 3 = rx_pkt_num, byte 17 = tx_buf_num), free-list classes,
 * type-1 control-word compose on recycle, type-4 heap lifecycle,
 * RX exhaustion. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../esf_buf.h"
#include "../pp_esf.h"

/* ---- platform hooks ---- */
static int lock_depth;
static long live_allocs;

void vPortETSIntrLock(void) { lock_depth++; }
void vPortETSIntrUnlock(void) { lock_depth--; }

void *_heap_caps_malloc(uint32_t size, uint32_t caps, const char *file,
			int line)
{
	(void)caps; (void)file; (void)line;
	live_allocs++;
	return malloc(size);
}

void *_heap_caps_zalloc(uint32_t size, uint32_t caps, const char *file,
			int line)
{
	(void)caps; (void)file; (void)line;
	live_allocs++;
	return calloc(1, size);
}

void _heap_caps_free(void *ptr, const char *file, int line)
{
	(void)file; (void)line;
	if (ptr != NULL)
		live_allocs--;
	free(ptr);
}

uint8_t g_wifi_menuconfig[28];

/* wDevCtrl stand-in: +24 holds the TX descriptor ring base. */
static uint32_t ring_desc[3 * 8];
uint8_t wDevCtrl[44];

/* ---- test harness ---- */
static int failures;

#define CHECK(cond)                                                     \
	do {                                                            \
		if (!(cond)) {                                          \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, \
			       #cond);                            \
			failures++;                                     \
		}                                                       \
	} while (0)

static void
setup_config(uint8_t rx_pkt, uint8_t tx_pkt)
{
	memset(g_wifi_menuconfig, 0, sizeof(g_wifi_menuconfig));
	g_wifi_menuconfig[3] = rx_pkt;
	g_wifi_menuconfig[17] = tx_pkt;
	memset(ring_desc, 0, sizeof(ring_desc));
	{
		int i;
		for (i = 0; i < 8; i++)
			ring_desc[i * 3 + 1] = 0x1000u + (uint32_t)i * 0x100u;
	}
	*(uint8_t **)(wDevCtrl + 24) = (uint8_t *)ring_desc;
}

int
main(void)
{
	pp_esf_blob *eb, *eb2;
	uint8_t payload[1600];
	uint8_t hdr[PP_ESF_HDR_SIZE];
	long before;

	setup_config(7, 6); /* esp9 sdkconfig values */
	esf_buf_setup();
	CHECK(pp_esf_general_free_count() == 6);
	CHECK(pp_esf_mgmt_free_count() == 8);
	CHECK(pp_esf_rx_free_count() == 8);
	CHECK(g_free_rxblock_eb_cnt == 8);

	/* RX: only type 7, pool exhausts at 8. */
	CHECK(esf_rx_buf_alloc(1) == NULL);
	{
		pp_esf_blob *held[8];
		int i;
		for (i = 0; i < 8; i++) {
			held[i] = esf_rx_buf_alloc(7);
			CHECK(held[i] != NULL);
		}
		CHECK(esf_rx_buf_alloc(7) == NULL);
		CHECK(g_free_rxblock_eb_cnt == 0);
		for (i = 0; i < 8; i++)
			esf_buf_recycle(held[i], 7);
		CHECK(pp_esf_rx_free_count() == 8);
	}

	/* Type 1: fields from spec 5.3. */
	eb = esf_buf_alloc(payload + 36, 1, 1500);
	CHECK(eb != NULL);
	CHECK(eb->data == payload + 36);
	CHECK(eb->base == payload);
	CHECK(eb->len == 1500);
	CHECK((*(uint32_t *)eb->hdr & PP_ESF_FLAG_TYPE1) != 0);
	CHECK(*(uint32_t *)((uint8_t *)eb->desc + 4) ==
	      (uint32_t)(uintptr_t)(payload + 36));
	/* len truncates to 16 bits like the blob. */
	eb2 = esf_buf_alloc(payload + 36, 1, 0x12345);
	CHECK(eb2 != NULL && eb2->len == 0x2345);
	esf_buf_recycle(eb2, 1);

	/* Type-1 recycle composes +64 via pp_esf_compose_ctrl. */
	memset(hdr, 0, sizeof(hdr));
	{
		uint32_t w8 = (0x2Au << 8) | (0x15u << 14);
		uint32_t w4 = 0xABu << 18;
		memcpy(hdr + 8, &w8, 4);
		memcpy(hdr + 4, &w4, 4);
		hdr[12] = 0xC3;
	}
	memcpy(eb->hdr, hdr, sizeof(hdr));
	esf_buf_recycle(eb, 1);
	CHECK(eb->ctrl == pp_esf_compose_ctrl(hdr));
	CHECK(pp_esf_general_free_count() == 6);

	/* Type 5: mgmt list, data = base set at setup. */
	eb = esf_buf_alloc(NULL, 5, 0);
	CHECK(eb != NULL);
	CHECK(eb->data == eb->base);
	CHECK((*(uint32_t *)eb->hdr & PP_ESF_FLAG_TYPE5) != 0);
	esf_buf_recycle(eb, 5);
	CHECK(pp_esf_mgmt_free_count() == 8);

	/* Type 4: heap lifecycle balances. */
	before = live_allocs;
	eb = esf_buf_alloc(NULL, 4, 512);
	CHECK(eb != NULL);
	CHECK(eb->data == eb->base);
	CHECK(eb->desc != NULL && eb->desc == eb->desc2);
	CHECK((*(uint32_t *)eb->hdr & PP_ESF_FLAG_TYPE4) != 0);
	esf_buf_recycle(eb, 4);
	CHECK(live_allocs == before);

	/* Unknown types: NULL, no crash (documented divergence). */
	CHECK(esf_buf_alloc(payload, 3, 10) == NULL);
	CHECK(esf_buf_alloc(payload, 0, 10) == NULL);

	/* mem-type accessors: values >= 2 ignored, bit 0 stored. */
	esp_wifi_set_rx_pbuf_mem_type(1);
	CHECK(esp_wifi_get_rx_pbuf_mem_type() == 1);
	esp_wifi_set_rx_pbuf_mem_type(5);
	CHECK(esp_wifi_get_rx_pbuf_mem_type() == 1);
	esp_wifi_set_rx_pbuf_mem_type(0);
	CHECK(esp_wifi_get_rx_pbuf_mem_type() == 0);

	CHECK(lock_depth == 0);

	if (failures != 0) {
		printf("test_esf_buf: %d failures\n", failures);
		return 1;
	}
	printf("test_esf_buf: all tests passed\n");
	return 0;
}
