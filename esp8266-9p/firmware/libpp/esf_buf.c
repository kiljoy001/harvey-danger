/* esf_buf - clean-room implementation; see esf_buf.h. */
#include "esf_buf.h"

#include <stddef.h>
#include <string.h>

#include "pp_esf.h"

/* ---- platform hooks (provided by the target image) ---- */
extern void *_heap_caps_malloc(uint32_t size, uint32_t caps,
			       const char *file, int line);
extern void *_heap_caps_zalloc(uint32_t size, uint32_t caps,
			       const char *file, int line);
extern void _heap_caps_free(void *ptr, const char *file, int line);
extern void vPortETSIntrLock(void);
extern void vPortETSIntrUnlock(void);
extern uint8_t g_wifi_menuconfig[28];
extern uint8_t wDevCtrl[]; /* +24: TX descriptor ring base */

#define ESF_FILE "esf_buf.c"
#define CAPS_OF(type) (type)

/* ebCxt equivalent: three free-list heads (spec 5.2). */
static pp_esf_blob *list_general;
static pp_esf_blob *list_mgmt;
static pp_esf_blob *list_rx;

uint32_t g_free_rxblock_eb_cnt;

static uint8_t *hdr_pool;	/* 28-byte headers, types 1/5 */
static uint8_t *rxhdr_pool;	/* 8-byte headers, type 7 */
static pp_esf_blob *eb_pool;
static int setup_done;

/* 8 management payload slots of 76 bytes, as in the blob. */
#define MGMT_SLOTS 8
#define MGMT_STRIDE 76
static uint8_t mgmt_sbuf[MGMT_SLOTS * MGMT_STRIDE];

static uint8_t rx_mem_type; /* s_wifi_data_dram equivalent */

static void
push(pp_esf_blob **head, pp_esf_blob *eb)
{
	vPortETSIntrLock();
	eb->next = *head;
	*head = eb;
	vPortETSIntrUnlock();
}

static pp_esf_blob *
pop(pp_esf_blob **head)
{
	pp_esf_blob *eb;

	vPortETSIntrLock();
	eb = *head;
	if (eb != NULL) {
		*head = eb->next;
		eb->next = NULL;
	}
	vPortETSIntrUnlock();
	return eb;
}

static uint32_t
list_len(const pp_esf_blob *head)
{
	uint32_t n = 0;

	while (head != NULL) {
		n++;
		head = head->next;
	}
	return n;
}

uint32_t pp_esf_rx_free_count(void) { return list_len(list_rx); }
uint32_t pp_esf_general_free_count(void) { return list_len(list_general); }
uint32_t pp_esf_mgmt_free_count(void) { return list_len(list_mgmt); }

void *
esf_rx_buf_alloc(int type)
{
	pp_esf_blob *eb;

	if (type != 7)
		return NULL;
	eb = pop(&list_rx);
	if (eb != NULL)
		g_free_rxblock_eb_cnt--;
	return eb;
}

void *
esf_buf_alloc(void *data, uint32_t type, uint32_t len)
{
	pp_esf_blob *eb;
	uint32_t *hdr;

	len &= 0xFFFFu;
	if (type == 1) {
		if (data == NULL)
			return NULL;
		eb = pop(&list_general);
		if (eb == NULL)
			return NULL;
		eb->data = data;
		eb->base = (uint8_t *)data - PP_ESF_HEADROOM;
		eb->len = (uint16_t)len;
		hdr = (uint32_t *)eb->hdr;
		*hdr |= PP_ESF_FLAG_TYPE1;
		*(uint32_t *)((uint8_t *)eb->desc + 4) = (uint32_t)(uintptr_t)data;
		hdr[1] &= PP_ESF_HDR1_MASK;
		return eb;
	}
	if (type == 4) {
		void *desc, *payload;

		eb = _heap_caps_malloc(sizeof(pp_esf_blob), CAPS_OF(type),
				       ESF_FILE, 400);
		if (eb == NULL)
			return NULL;
		desc = _heap_caps_malloc(12, CAPS_OF(type), ESF_FILE, 403);
		if (desc == NULL) {
			_heap_caps_free(eb, ESF_FILE, 442);
			return NULL;
		}
		eb->desc = desc;
		eb->desc2 = desc;
		eb->class8 = 1;
		eb->hdr = _heap_caps_zalloc(PP_ESF_HDR_SIZE, CAPS_OF(type),
					    ESF_FILE, 417);
		if (eb->hdr == NULL) {
			_heap_caps_free(desc, ESF_FILE, 424);
			eb->desc = NULL;
			_heap_caps_free(eb, ESF_FILE, 426);
			return NULL;
		}
		hdr = (uint32_t *)eb->hdr;
		*hdr |= PP_ESF_FLAG_TYPE4;
		payload = _heap_caps_malloc(len, CAPS_OF(type),
					    ESF_FILE, 433);
		if (payload == NULL) {
			_heap_caps_free(eb->hdr, ESF_FILE, 440);
			_heap_caps_free(desc, ESF_FILE, 442);
			eb->desc = NULL;
			_heap_caps_free(eb, ESF_FILE, 444);
			return NULL;
		}
		eb->base = payload;
		eb->data = payload;
		*(uint32_t *)((uint8_t *)eb->desc + 4) =
			(uint32_t)(uintptr_t)payload;
		hdr[1] &= PP_ESF_HDR1_MASK;
		return eb;
	}
	if (type == 5) {
		eb = pop(&list_mgmt);
		if (eb == NULL)
			return NULL;
		memset(eb->hdr, 0, PP_ESF_HDR_SIZE);
		eb->data = eb->base;
		hdr = (uint32_t *)eb->hdr;
		*hdr |= PP_ESF_FLAG_TYPE5;
		*(uint32_t *)((uint8_t *)eb->desc + 4) =
			(uint32_t)(uintptr_t)eb->data;
		hdr[1] &= PP_ESF_HDR1_MASK;
		return eb;
	}
	return NULL; /* blob traps here; see esf_buf.h */
}

void
esf_buf_recycle(void *veb, int type)
{
	pp_esf_blob *eb = (pp_esf_blob *)veb;

	if (eb == NULL)
		return;
	switch (type) {
	case 1:
		eb->ctrl = pp_esf_compose_ctrl((const uint8_t *)eb->hdr);
		if (eb->cb != NULL)
			eb->cb(eb->cbarg);
		memset(eb->hdr, 0, PP_ESF_HDR_SIZE);
		push(&list_general, eb);
		break;
	case 2:
		memset(eb->hdr, 0, PP_ESF_HDR_SIZE);
		push(&list_general, eb);
		break;
	case 4:
		_heap_caps_free(eb->desc, ESF_FILE, 612);
		_heap_caps_free(eb->hdr, ESF_FILE, 614);
		_heap_caps_free(eb->base, ESF_FILE, 615);
		_heap_caps_free(eb, ESF_FILE, 616);
		break;
	case 5:
		memset(eb->hdr, 0, PP_ESF_HDR_SIZE);
		push(&list_mgmt, eb);
		break;
	case 7:
		memset(eb->hdr, 0, PP_ESF_RXHDR_SIZE);
		push(&list_rx, eb);
		break;
	default:
		break; /* blob: no-op for unknown types */
	}
}

void
esf_buf_setup(void)
{
	uint32_t n_rx, n_tx, i;
	uint8_t *ring;

	if (setup_done)
		return;
	n_rx = g_wifi_menuconfig[3];	/* rx_pkt_num (B23) */
	n_tx = g_wifi_menuconfig[17];	/* tx_buf_num (B23) */

	hdr_pool = _heap_caps_zalloc((n_tx + 8) * PP_ESF_HDR_SIZE, 4,
				     ESF_FILE, 720);
	if (hdr_pool == NULL)
		return;
	rxhdr_pool = _heap_caps_zalloc((n_rx + 1) * PP_ESF_RXHDR_SIZE, 4,
				       ESF_FILE, 734);
	if (rxhdr_pool == NULL)
		return;
	/* 68 bytes per buffer on the target (sizeof check in
	 * test_pp); sizeof keeps the pool correct on hosts. */
	eb_pool = _heap_caps_zalloc((n_rx + n_tx + 9) * sizeof(pp_esf_blob),
				    4, ESF_FILE, 743);
	if (eb_pool == NULL)
		return;
	setup_done = 1;
	g_free_rxblock_eb_cnt = n_rx + 1;

	/* Type 1 (TX): descriptors from the wDevCtrl+24 ring,
	 * taken in descending ring order, payload base loaded
	 * from each descriptor's word 1 (spec 5.3). */
	ring = *(uint8_t **)(wDevCtrl + 24);
	for (i = 0; i < n_tx; i++) {
		pp_esf_blob *eb = &eb_pool[i];
		uint8_t *desc = ring + (n_tx - 1 - i) * 12;

		eb->class8 = 1;
		eb->desc = desc;
		eb->desc2 = desc;
		eb->base = *(void **)(desc + 4);
		eb->hdr = hdr_pool + i * PP_ESF_HDR_SIZE;
		esf_buf_recycle(eb, 1);
	}

	/* Type 5 (mgmt): 8 slots, payload at sbuf+12, each
	 * sbuf word 0 masked to a 64-byte length (spec B22
	 * addendum; register meaning still open). */
	for (i = 0; i < MGMT_SLOTS; i++) {
		pp_esf_blob *eb = &eb_pool[n_tx + i];
		uint8_t *sbuf = mgmt_sbuf + i * MGMT_STRIDE;
		uint32_t w;

		memcpy(&w, sbuf, 4);
		w = (w & 0xFFFFF000u) | 64u;
		PP_MEMW();
		memcpy(sbuf, &w, 4);
		PP_MEMW();
		eb->class8 = 1;
		eb->desc = sbuf;
		eb->desc2 = sbuf;
		eb->base = sbuf + 12;
		eb->hdr = hdr_pool + (n_tx + i) * PP_ESF_HDR_SIZE;
		esf_buf_recycle(eb, 5);
	}

	/* Type 7 (RX): 8-byte headers only. */
	for (i = 0; i < n_rx + 1; i++) {
		pp_esf_blob *eb = &eb_pool[n_tx + MGMT_SLOTS + i];

		eb->hdr = rxhdr_pool + i * PP_ESF_RXHDR_SIZE;
		esf_buf_recycle(eb, 7);
	}
}

void
esp_wifi_set_rx_pbuf_mem_type(int type)
{
	if (type >= 0 && type < 2)
		rx_mem_type = (uint8_t)(type & 1);
}

int
esp_wifi_get_rx_pbuf_mem_type(void)
{
	return rx_mem_type;
}
