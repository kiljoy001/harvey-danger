/* pp_esf - fixed-size buffer pool with one DMA descriptor per buffer.
 *
 * Clean-room replacement for the role of the blob esf_buf layer,
 * written against doc/SPEC-WDEV-DMA.md. The blob's esf_buf layout
 * is now fully decoded (68 bytes, spec section 5, re/B22); the
 * pp_esf_blob mirror below records that ABI for interop work, but
 * pp_buf remains a new component with the same job - a free list
 * of frame buffers, each carrying a pp_desc - to be integrated
 * behind the existing blobs first.
 */
#ifndef PP_ESF_H
#define PP_ESF_H

#include <stdint.h>
#include "pp_lldesc.h"

typedef struct pp_buf pp_buf;
struct pp_buf {
	pp_desc desc; /* desc.buf points at data */
	uint8_t *data;
	uint32_t cap;
	uint16_t len;
	uint16_t inuse;
	pp_buf *next; /* free-list link when not in use */
};

typedef struct {
	pp_buf *bufs;
	uint32_t count;
	uint32_t bufsize;
	pp_buf *freehead;
	uint32_t nfree;
} pp_pool;

/* Blob esf_buf ABI, verified in spec section 5 (B22). Field order
 * and sizes reproduce the blob's 68-byte struct exactly; unmapped
 * offsets are named for their offset so the layout stays honest. */
#define PP_ESF_BLOB_SIZE	68
#define PP_ESF_HDR_SIZE		28	/* +40 header, types 1/4/5 */
#define PP_ESF_RXHDR_SIZE	8	/* +40 header, type 7 */
#define PP_ESF_HEADROOM		36	/* type 1: base = data-36 */
#define PP_ESF_FLAG_TYPE1	0x2000	/* header word0 bit, reuse */
#define PP_ESF_FLAG_TYPE4	0x4000	/* header word0 bit, fresh */
#define PP_ESF_FLAG_TYPE5	0x8000	/* header word0 bit, mgmt */
#define PP_ESF_HDR1_MASK	0xfc03ffffu /* header +4 AND mask */

typedef struct pp_esf_blob pp_esf_blob;
struct pp_esf_blob {
	void *desc;		/* +0: 12-byte DMA descriptor */
	void *desc2;		/* +4: same pointer stored again */
	uint16_t class8;	/* +8 */
	uint8_t _pad10[2];
	void *data;		/* +12 */
	void *_unk16;
	void *base;		/* +20 */
	uint16_t _unk24;
	uint16_t len;		/* +26 */
	uint8_t _pad28[8];
	pp_esf_blob *next;	/* +36: free-list link */
	void *hdr;		/* +40 */
	uint8_t cbarg[12];	/* +44: callback argument area */
	void (*cb)(void *);	/* +56 */
	uint32_t _unk60;
	uint32_t ctrl;		/* +64: composed DMA control word */
};

/* Compose the +64 control word from a 28-byte type-1 header,
 * exactly as blob esf_buf_recycle does (spec 5.3):
 *   hdr+8 bits [13:8] -> [13:8], hdr+8 bits [19:14] -> [19:14],
 *   hdr+4 bits [25:18] -> [7:0], hdr byte +12 -> [27:20]. */
uint32_t pp_esf_compose_ctrl(const uint8_t hdr[PP_ESF_HDR_SIZE]);

void pp_pool_init(pp_pool *pool, pp_buf *bufs, uint8_t *storage,
		  uint32_t count, uint32_t bufsize);
pp_buf *pp_buf_alloc(pp_pool *pool);
void pp_buf_free(pp_pool *pool, pp_buf *b);
/* Offer a buffer's descriptor to hardware for RX. */
void pp_buf_offer_rx(pp_pool *pool, pp_buf *b, int reserve_fcs);

#endif
