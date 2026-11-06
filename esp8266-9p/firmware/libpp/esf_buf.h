/* esf_buf - clean-room, link-compatible replacement for the
 * blob esf_buf.o in libpp.a.
 *
 * Written only against doc/SPEC-WDEV-DMA.md section 5 (B22
 * disassembly + Ghidra cross-check) and B23. It exports the
 * exact symbols blob pp.o/lmac.o/wdev.o link against, with
 * the blob's 68-byte esf_buf ABI (pp_esf_blob in pp_esf.h),
 * so the rest of libpp links unchanged. Internals (pool
 * storage, list heads) are ours.
 *
 * Platform hooks are the blob's own: _heap_caps_malloc /
 * _heap_caps_zalloc / _heap_caps_free (caps argument is the
 * buffer type, as in the blob), vPortETSIntrLock/Unlock,
 * g_wifi_menuconfig (28-byte config written by
 * wifi_menuconfig_init: byte 3 = rx_pkt_num, byte 17 =
 * tx_buf_num - B23), and wDevCtrl (+24 = TX descriptor
 * ring base).
 *
 * Deliberate divergences from the blob, both where the blob
 * crashes: an alloc type other than 1/4/5 returns NULL
 * instead of trapping, and a type-4 allocation failure frees
 * what it took and returns NULL without the blob's DRAM
 * range re-checks (heap_caps already only returns DRAM).
 */
#ifndef ESF_BUF_H
#define ESF_BUF_H

#include <stdint.h>

void *esf_buf_alloc(void *data, uint32_t type, uint32_t len);
void esf_buf_recycle(void *eb, int type);
void esf_buf_setup(void);
void *esf_rx_buf_alloc(int type);
void esp_wifi_set_rx_pbuf_mem_type(int type);
int esp_wifi_get_rx_pbuf_mem_type(void);

/* Telemetry for the B21/B22 RX-stall probe: the blob's
 * counter is write-only inside esf_buf.o, so ours is
 * exported, and the true RX pool level is readable. */
extern uint32_t g_free_rxblock_eb_cnt;
uint32_t pp_esf_rx_free_count(void);
uint32_t pp_esf_general_free_count(void);
uint32_t pp_esf_mgmt_free_count(void);

#endif
