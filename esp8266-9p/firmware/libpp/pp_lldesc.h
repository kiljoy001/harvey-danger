/* pp_lldesc - ESP8266 DMA descriptor (lldesc), clean-room.
 *
 * Written only against doc/SPEC-WDEV-DMA.md section 1, which was
 * derived from RE of the stock firmware (see ../../re/). No blob
 * code or SDK source was used.
 *
 * Descriptor (12 bytes, little-endian):
 *   +0x00 ctrl: size[11:0] | len[23:12] | reserved[28:24]
 *               | sub_sof[29] | eof[30] | owner[31]
 *   +0x04 buf:  buffer pointer
 *   +0x08 next: next descriptor or NULL
 * owner=1 means hardware owns the descriptor. Hardware sets eof
 * on the last descriptor of a completed frame.
 */
#ifndef PP_LLDESC_H
#define PP_LLDESC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __XTENSA__
#define PP_MEMW() __asm__ volatile("memw" ::: "memory")
#else
#define PP_MEMW() ((void)0)
#endif

#define PP_DESC_SIZE_MASK 0x00000FFFu
#define PP_DESC_LEN_SHIFT 12
#define PP_DESC_SUB_SOF (1u << 29)
#define PP_DESC_EOF (1u << 30)
#define PP_DESC_OWNER (1u << 31)
/* RX buffers reserve 4 bytes for the frame check sequence. */
#define PP_RX_FCS_RESERVE 4u

/* buf/next are 32-bit hardware addresses, not host pointers, so
 * this struct is 12 bytes on any host. Convert with the helpers
 * in pp_lldesc.c (pp_desc_buf / pp_desc_next). */
typedef struct pp_desc pp_desc;
struct pp_desc {
	volatile uint32_t ctrl;
	uint32_t buf;
	uint32_t next;
};

void *pp_desc_buf(const pp_desc *d);
pp_desc *pp_desc_next(const pp_desc *d);
void pp_desc_set_next(pp_desc *d, pp_desc *next);

/* Compile-time layout check (C99-friendly form lives in the .c). */
#define PP_DESC_EXPECTED_SIZE 12

void pp_desc_init(pp_desc *d, void *buf, uint32_t size);
uint32_t pp_desc_size(const pp_desc *d);
uint32_t pp_desc_len(const pp_desc *d);
void pp_desc_set_len(pp_desc *d, uint32_t len);

/* Offer one empty RX descriptor to hardware: owner=1, eof and
 * sub_sof cleared, len = size (or size-4 with FCS reserve). */
void pp_desc_offer_rx(pp_desc *d, int reserve_fcs);

/* Hardware completion test: eof set (and owner handed back). */
int pp_desc_done(const pp_desc *d);
void pp_desc_reclaim(pp_desc *d);

/* Link n contiguous descriptors into a chain ending in NULL and
 * initialise each with successive buffers of bufsize bytes. */
void pp_desc_chain(pp_desc *array, uint32_t n, void *bufs, uint32_t bufsize);

#endif
