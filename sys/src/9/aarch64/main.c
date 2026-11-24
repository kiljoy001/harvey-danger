/*
 * This file is part of the Harvey operating system.  It is subject to the
 * license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
 * directory of this distribution at http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * Early ARM64 boot entry.  Firmware supplies the device tree in x0.
 */

#include "user.h"

#ifdef ARM64_FULL_KERNEL
void arm64kernelstart(Fdtinfo *);
#endif

int arm64contextprobe(void);
extern char arm64vectors[];
extern char _start[], __image_end[];
extern u64 arm64dmabase, arm64dmasize;
extern u64 arm64teeshmbase, arm64teeshmsize;
extern u64 arm64bootarchbase;
extern u32 arm64bootarchsize;

enum {
	Uartdr = 0x00 / 4,
	Uartfr = 0x18 / 4,
	Uarttxff = 1 << 5,
	Fdtmagic = 0xd00dfeed,
	Fdtmin = 40,
	Fdtmax = 16 * 1024 * 1024,
};

static usize uartbase;
static u32 uarttype;
static int is_bengal = 0;
/* SCMI shared-memory page: /chosen/harvey,scm when present. */
#ifdef ARM64_RK3566_NPU_PROBE
static u64 scmbase = 0x10f000ULL;
#endif

/*
 * Framebuffer console.  The base, geometry and stride come from the
 * /chosen simple-framebuffer node when the firmware provides one; the
 * fallback below exists only for boards whose DT still lacks it.
 */
static u32 *fb_base;
static u32 fb_width = 720;
static u32 fb_height = 1600;
static u32 fb_pxstride = 720;	/* pixels per scanline */
static u32 fb_bpp = 32;

static u32 *
fbmem(void)
{
	if(fb_base != nil)
		return fb_bpp == 32 ? fb_base : nil;
	if(is_bengal){
		fb_base = (u32 *)0x5c000000ULL;
		fb_width = 720;
		fb_height = 1600;
		fb_pxstride = 720;
		fb_bpp = 32;
	}
	return fb_base;
}

static void
fb_flush(u64 start, u64 size)
{
	u64 end = start + size;
	for(u64 a = start & ~63ULL; a < end; a += 64)
		__asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

void
fb_stripe(int slot, u32 color)
{
	(void)slot;
	(void)color;
}

static const u8 hexfon5x7[16][7] = {
	{0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}, /* 0 */
	{0x04,0x0c,0x04,0x04,0x04,0x04,0x0e}, /* 1 */
	{0x0e,0x11,0x01,0x02,0x04,0x08,0x1f}, /* 2 */
	{0x1e,0x01,0x01,0x0e,0x01,0x01,0x1e}, /* 3 */
	{0x02,0x06,0x0a,0x12,0x1f,0x02,0x02}, /* 4 */
	{0x1f,0x10,0x1e,0x01,0x01,0x11,0x0e}, /* 5 */
	{0x06,0x08,0x10,0x1e,0x11,0x11,0x0e}, /* 6 */
	{0x1f,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
	{0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e}, /* 8 */
	{0x0e,0x11,0x11,0x0f,0x01,0x02,0x0c}, /* 9 */
	{0x0e,0x11,0x11,0x1f,0x11,0x11,0x11}, /* A */
	{0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e}, /* B */
	{0x0e,0x11,0x10,0x10,0x10,0x11,0x0e}, /* C */
	{0x1c,0x12,0x11,0x11,0x11,0x12,0x1c}, /* D */
	{0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f}, /* E */
	{0x1f,0x10,0x10,0x1e,0x10,0x10,0x10}, /* F */
};

static void
fb_draw_char(int x, int y, char c, u32 color)
{
	int d = -1;
	if(c >= '0' && c <= '9') d = c - '0';
	else if(c >= 'a' && c <= 'f') d = c - 'a' + 10;
	else if(c >= 'A' && c <= 'F') d = c - 'A' + 10;
	if(d < 0) return;
	volatile u32 *fb = (volatile u32 *)fbmem();
	if(fb == nil)
		return;
	for(int r = 0; r < 7; r++){
		u8 bits = hexfon5x7[d][r];
		for(int col = 0; col < 5; col++){
			if(bits & (1 << (4 - col))){
				for(int dy = 0; dy < 5; dy++){
					for(int dx = 0; dx < 5; dx++){
						int px = x + col*5 + dx;
						int py = y + r*5 + dy;
						if(px < fb_width && py < fb_height)
							fb[py * fb_pxstride + px] = color;
					}
				}
			}
		}
	}
}

void
fb_draw_hex(int x, int y, u64 val, u32 color)
{
	volatile u32 *fb;
	int i;

	fb = (volatile u32 *)fbmem();
	if(fb == nil)
		return;
	for(i = 15; i >= 0; i--){
		int nybble = (val >> (i * 4)) & 0xf;
		fb_draw_char(x + (15 - i) * 30, y, nybble < 10 ? '0' + nybble : 'A' + nybble - 10, color);
	}
	fb_flush((u64)&fb[y * fb_pxstride], 35 * fb_pxstride * (fb_bpp/8));
}

static const u8 font5x7[95][5] = {
	{0x00,0x00,0x00,0x00,0x00}, /*   */ {0x00,0x00,0x5f,0x00,0x00}, /* ! */
	{0x00,0x07,0x00,0x07,0x00}, /* " */ {0x14,0x7f,0x14,0x7f,0x14}, /* # */
	{0x24,0x2a,0x7f,0x2a,0x12}, /* $ */ {0x23,0x13,0x08,0x64,0x62}, /* % */
	{0x36,0x49,0x55,0x22,0x50}, /* & */ {0x00,0x05,0x03,0x00,0x00}, /* ' */
	{0x00,0x1c,0x22,0x41,0x00}, /* ( */ {0x00,0x41,0x22,0x1c,0x00}, /* ) */
	{0x14,0x08,0x3e,0x08,0x14}, /* * */ {0x08,0x08,0x3e,0x08,0x08}, /* + */
	{0x00,0x50,0x30,0x00,0x00}, /* , */ {0x08,0x08,0x08,0x08,0x08}, /* - */
	{0x00,0x60,0x60,0x00,0x00}, /* . */ {0x20,0x10,0x08,0x04,0x02}, /* / */
	{0x3e,0x51,0x49,0x45,0x3e}, /* 0 */ {0x00,0x42,0x7f,0x40,0x00}, /* 1 */
	{0x42,0x61,0x51,0x49,0x46}, /* 2 */ {0x21,0x41,0x45,0x4b,0x31}, /* 3 */
	{0x18,0x14,0x12,0x7f,0x10}, /* 4 */ {0x27,0x45,0x45,0x45,0x39}, /* 5 */
	{0x3c,0x4a,0x49,0x49,0x30}, /* 6 */ {0x01,0x71,0x09,0x05,0x03}, /* 7 */
	{0x36,0x49,0x49,0x49,0x36}, /* 8 */ {0x06,0x49,0x49,0x29,0x1e}, /* 9 */
	{0x00,0x36,0x36,0x00,0x00}, /* : */ {0x00,0x56,0x36,0x00,0x00}, /* ; */
	{0x08,0x14,0x22,0x41,0x00}, /* < */ {0x14,0x14,0x14,0x14,0x14}, /* = */
	{0x00,0x41,0x22,0x14,0x08}, /* > */ {0x02,0x01,0x51,0x09,0x06}, /* ? */
	{0x32,0x49,0x79,0x41,0x3e}, /* @ */ {0x7e,0x11,0x11,0x11,0x7e}, /* A */
	{0x7f,0x49,0x49,0x49,0x36}, /* B */ {0x3e,0x41,0x41,0x41,0x22}, /* C */
	{0x7f,0x41,0x41,0x22,0x1c}, /* D */ {0x7f,0x49,0x49,0x49,0x41}, /* E */
	{0x7f,0x09,0x09,0x09,0x01}, /* F */ {0x3e,0x41,0x49,0x49,0x7a}, /* G */
	{0x7f,0x08,0x08,0x08,0x7f}, /* H */ {0x00,0x41,0x7f,0x41,0x00}, /* I */
	{0x20,0x40,0x41,0x3f,0x01}, /* J */ {0x7f,0x08,0x14,0x22,0x41}, /* K */
	{0x7f,0x40,0x40,0x40,0x40}, /* L */ {0x7f,0x02,0x0c,0x02,0x7f}, /* M */
	{0x7f,0x04,0x08,0x10,0x7f}, /* N */ {0x3e,0x41,0x41,0x41,0x3e}, /* O */
	{0x7f,0x09,0x09,0x09,0x06}, /* P */ {0x3e,0x41,0x51,0x21,0x5e}, /* Q */
	{0x7f,0x09,0x19,0x29,0x46}, /* R */ {0x46,0x49,0x49,0x49,0x31}, /* S */
	{0x01,0x01,0x7f,0x01,0x01}, /* T */ {0x3f,0x40,0x40,0x40,0x3f}, /* U */
	{0x1f,0x20,0x40,0x20,0x1f}, /* V */ {0x3f,0x40,0x38,0x40,0x3f}, /* W */
	{0x63,0x14,0x08,0x14,0x63}, /* X */ {0x07,0x08,0x70,0x08,0x07}, /* Y */
	{0x61,0x51,0x49,0x45,0x43}, /* Z */ {0x00,0x7f,0x41,0x41,0x00}, /* [ */
	{0x02,0x04,0x08,0x10,0x20}, /* \ */ {0x00,0x41,0x41,0x7f,0x00}, /* ] */
	{0x04,0x02,0x01,0x02,0x04}, /* ^ */ {0x40,0x40,0x40,0x40,0x40}, /* _ */
	{0x00,0x01,0x02,0x04,0x00}, /* ` */ {0x20,0x54,0x54,0x54,0x78}, /* a */
	{0x7f,0x48,0x44,0x44,0x38}, /* b */ {0x38,0x44,0x44,0x44,0x20}, /* c */
	{0x38,0x44,0x44,0x48,0x7f}, /* d */ {0x38,0x54,0x54,0x54,0x18}, /* e */
	{0x08,0x7e,0x09,0x01,0x02}, /* f */ {0x0c,0x52,0x52,0x52,0x3e}, /* g */
	{0x7f,0x08,0x04,0x04,0x78}, /* h */ {0x00,0x44,0x7d,0x40,0x00}, /* i */
	{0x20,0x40,0x44,0x3d,0x00}, /* j */ {0x7f,0x10,0x28,0x44,0x00}, /* k */
	{0x00,0x41,0x7f,0x40,0x00}, /* l */ {0x7c,0x04,0x18,0x04,0x78}, /* m */
	{0x7c,0x08,0x04,0x04,0x78}, /* n */ {0x38,0x44,0x44,0x44,0x38}, /* o */
	{0x7c,0x14,0x14,0x14,0x08}, /* p */ {0x08,0x14,0x14,0x18,0x7c}, /* q */
	{0x7c,0x08,0x04,0x04,0x08}, /* r */ {0x48,0x54,0x54,0x54,0x20}, /* s */
	{0x04,0x3f,0x44,0x40,0x20}, /* t */ {0x3c,0x40,0x40,0x20,0x7c}, /* u */
	{0x1c,0x20,0x40,0x20,0x1c}, /* v */ {0x3c,0x40,0x30,0x40,0x3c}, /* w */
	{0x44,0x28,0x10,0x28,0x44}, /* x */ {0x0c,0x50,0x50,0x50,0x3c}, /* y */
	{0x44,0x64,0x54,0x4c,0x44}, /* z */ {0x00,0x08,0x36,0x41,0x00}, /* { */
	{0x00,0x00,0x7f,0x00,0x00}, /* | */ {0x00,0x41,0x36,0x08,0x00}, /* } */
	{0x10,0x08,0x08,0x10,0x08}, /* ~ */
};

static int fb_con_init = 0;
static int fb_con_top = 16;
static int fb_con_x = 12;
static int fb_con_y = 16;
static volatile int fb_con_lock = 0;

void
fb_con_expand(void)
{
}

static void
fb_con_newline(void)
{
	volatile u32 *fb = (volatile u32 *)fbmem();
	if(fb == nil)
		return;
	fb_con_x = 12;
	fb_con_y += 16;
	if(fb_con_y + 16 > fb_height - 16){
		for(int y = fb_con_top; y < fb_height - 32; y++)
			for(int x = 0; x < fb_width; x++)
				fb[y * fb_pxstride + x] = fb[(y + 16) * fb_pxstride + x];
		for(int y = fb_height - 32; y < fb_height - 16; y++)
			for(int x = 0; x < fb_width; x++)
				fb[y * fb_pxstride + x] = 0x00000000;
		fb_con_y = fb_height - 32;
		fb_flush((u64)&fb[fb_con_top * fb_pxstride],
		         (fb_height - 16 - fb_con_top) * fb_pxstride * (fb_bpp/8));
	}
}

static void
fb_con_putc(int c)
{
	volatile u32 *fb = (volatile u32 *)fbmem();
	if(fb == nil)
		return;
	while(__sync_lock_test_and_set(&fb_con_lock, 1))
		;
	if(!fb_con_init){
		fb_con_init = 1;
		for(int i = 0; i < fb_height * fb_width; i++)
			fb[i] = 0x00000000;
		fb_flush((u64)&fb[0], fb_height * fb_width * (fb_bpp/8));
	}
	if(c == '\r'){
		fb_con_x = 12;
		__sync_lock_release(&fb_con_lock);
		return;
	}
	if(c == '\n'){
		fb_con_newline();
		__sync_lock_release(&fb_con_lock);
		return;
	}
	if(c < 0x20 || c > 0x7e)
		c = '?';
	const u8 *g = font5x7[c - 0x20];
	for(int col = 0; col < 5; col++){
		u8 bits = g[col];
		for(int row = 0; row < 7; row++){
			u32 px = (bits & (1 << row)) ? 0x00ffffff : 0x00000000;
			int x0 = fb_con_x + col * 2;
			int y0 = fb_con_y + row * 2;
			fb[y0 * fb_pxstride + x0] = px;
			fb[y0 * fb_pxstride + x0 + 1] = px;
			fb[(y0 + 1) * fb_pxstride + x0] = px;
			fb[(y0 + 1) * fb_pxstride + x0 + 1] = px;
		}
	}
	fb_flush((u64)&fb[fb_con_y * fb_pxstride], 16 * fb_pxstride * (fb_bpp/8));
	fb_con_x += 12;
	if(fb_con_x + 12 > fb_width - 12)
		fb_con_newline();
	__sync_lock_release(&fb_con_lock);
}

/* The console is polled and shared by every CPU; without this the
 * secondaries interleave characters into the middle of a line.  Load/
 * store-exclusive is only usable on Normal memory, so the lock is
 * skipped until the MMU is on (early boot is uniprocessor anyway). */
static volatile int uart_lock;

static int
uartlock(void)
{
	if(!arm64mmuactive())
		return 0;
	while(__sync_lock_test_and_set(&uart_lock, 1))
		;
	return 1;
}

static void
uartunlock(int held)
{
	if(held)
		__sync_lock_release(&uart_lock);
}

static void
uartputc(int c)
{
	volatile u32 *uart;
	int n;

	if(fb_base != nil || is_bengal)
		fb_con_putc(c);
	if(uartbase == 0)
		return;
	uart = (volatile u32 *)uartbase;
	/*
	 * Never wait forever for FIFO room.  print() runs under ilock (IRQs
	 * masked), so an unbounded wait here stops the clock and parks every
	 * process if the console reader stalls.  Dropping output is much
	 * better than hanging the machine.
	 */
	if(uarttype == Fdtdwuart){
		/* DW UART: USR.TFNF reports room in the transmit FIFO. */
		for(n = 0; n < 1000000; n++)
			if(uart[0x7c/4] & (1<<1))
				break;
		if(n == 1000000)
			return;
	}else if(uarttype == Fdtpl011){
		for(n = 0; n < 1000000; n++)
			if((uart[Uartfr] & Uarttxff) == 0)
				break;
		if(n == 1000000)
			return;
	}else
		return;
	uart[Uartdr] = c;
}

void
arm64uartputs(char *s)
{
	int held = uartlock();

	for(; *s != 0; s++){
		if(*s == '\n')
			uartputc('\r');
		uartputc(*s);
	}
	uartunlock(held);
}
#define uartputs arm64uartputs

/* Optional U-Boot-supplied boot archive (programs for /boot). */
u64 arm64bootarchbase;
u32 arm64bootarchsize;

static u32
beget4(u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static void
uarthex(usize n)
{
	int i, c;

	int held;

	uartputs("0x");
	held = uartlock();
	for(i = 15; i >= 0; i--){
		c = (n >> (i * 4)) & 15;
		uartputc(c < 10 ? '0' + c : 'a' + c - 10);
	}
	uartunlock(held);
}

#ifdef ARM64_RK3566_NPU_PROBE
/*
 * Minimal SCMI client.  RK3566 TF-A owns the NPU (and GPU) clocks and
 * exposes them through the ARM SCMI clock protocol on the SMC transport:
 * function id 0x82000010, message buffer at the reserved 0x10f000 page.
 * Without enabling the NPU clock through it, any read of 0xfde4b000 or
 * 0xfde40000 wedges the AXI bus.
 */
typedef struct Scmishmem Scmishmem;
struct Scmishmem {
	u32	rsvd;
	u32	status;
	u32	rsvd1[2];
	u32	flags;
	u32	length;
	u32	header;
	u32	payload[32];
};

enum {
	Scmiprotobase	= 0x10,
	Scmiprotoclock	= 0x14,
	Scmiclockattr	= 3,
	Scmiclockrateget = 6,
	Scmiclockcfgset = 7,
	Scmiclockenable = 1,
};

static u64
smccall(u64 id, u64 a1, u64 a2)
{
	register u64 x0 __asm__("x0") = id;
	register u64 x1 __asm__("x1") = a1;
	register u64 x2 __asm__("x2") = a2;

	__asm__ volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2) : "memory");
	return x0;
}

static u32 scmiret;

static u32
scmicall(u32 proto, u32 cmd, u32 a0, u32 a1, u32 *out)
{
	volatile Scmishmem *s = (volatile Scmishmem *)scmbase;
	u64 r;
	u32 i, v;

	for(i = 0; i < 1000000 && !(s->status & 1); i++)
		__asm__ volatile("nop");
	s->status = 0;
	s->flags = 0;
	/* SCMI header: id[7:0], type[9:8], protocol[17:10], token[27:18]. */
	s->header = (cmd & 0xff) | ((proto & 0xff) << 10);
	if(a1 != 0){
		s->length = 12;
		s->payload[0] = a0;
		s->payload[1] = a1;
	}else{
		s->length = 8;
		s->payload[0] = a0;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	/* The vendor TF-A takes the SCMI shmem address in x1. */
	r = smccall(0x82000010ULL, scmbase, 0);
	scmiret = (u32)r;
	for(i = 0; i < 1000000 && !(s->status & 3); i++)
		__asm__ volatile("nop");
	v = s->payload[0];
	if(out != nil)
		*out = s->payload[1];
	return v;
}

static void
scmiprint(u32 id)
{
	u32 st, attr, rate;
	char name[17];
	int i;

	attr = 0;
	st = scmicall(Scmiprotoclock, Scmiclockattr, id, 0, &attr);
	uartputs("scmi clk ");
	uarthex(id);
	uartputs(" attr ");
	uarthex(attr);
	/* the short name follows the attributes word, 16 bytes */
	{
		volatile Scmishmem *s = (volatile Scmishmem *)scmbase;
		for(i = 0; i < 16; i++)
			name[i] = ((u8 *)&s->payload[2])[i];
		name[16] = 0;
	}
	uartputs(" st ");
	uarthex(st);
	uartputs(" name ");
	uartputs(name);
	rate = 0;
	st = scmicall(Scmiprotoclock, Scmiclockrateget, id, 0, &rate);
	uartputs(" rate ");
	uarthex(rate);
	uartputs(" st ");
	uarthex(st);
	uartputs(" smcret ");
	uarthex(scmiret);
	uartputs(" chan ");
	uarthex(((volatile Scmishmem *)scmbase)->status);
	uartputs("\n");
}

static void
rk3566npuprobe(u64 dmapage)
{
	u32 i, st, attr = 0;
	u64 v;

	(void)dmapage;

	uartputs("psci ");
	uarthex(smccall(0x84000000ULL, 0, 0));
	uartputs(" smc1 ");
	uarthex(smccall(0x82000010ULL, scmbase, 0));
	uartputs("\n");

	uartputs("npu scmi channel ");
	uarthex(((volatile Scmishmem *)scmbase)->status);
	uartputs("\n");

	/* BASE PROTOCOL_VERSION: no payload, reply is status + version. */
	{
		volatile Scmishmem *sh = (volatile Scmishmem *)scmbase;

		for(i = 0; i < 1000000 && !(sh->status & 1); i++)
			__asm__ volatile("nop");
		sh->status = 0;
		sh->flags = 0;
		sh->length = 4;
		sh->header = (0x10 << 10);
		__asm__ volatile("dsb sy" ::: "memory");
		smccall(0x82000010ULL, scmbase, 0);
		for(i = 0; i < 1000000 && !(sh->status & 3); i++)
			__asm__ volatile("nop");
		uartputs("base ver st ");
		uarthex(sh->payload[0]);
		uartputs(" ver ");
		uarthex(sh->payload[1]);
		uartputs(" len ");
		uarthex(sh->length);
		uartputs(" hdr ");
		uarthex(sh->header);
		uartputs(" w ");
		for(int k = 0; k < 6; k++){
			uarthex(sh->payload[k]);
			uartputs(" ");
		}
		uartputs("\n");
	}
	/* BASE DISCOVER_LIST_PROTOCOLS. */
	{
		volatile Scmishmem *sh = (volatile Scmishmem *)scmbase;

		for(i = 0; i < 1000000 && !(sh->status & 1); i++)
			__asm__ volatile("nop");
		sh->status = 0;
		sh->flags = 0;
		sh->length = 4;
		sh->header = 0x06 | (0x10 << 10);
		__asm__ volatile("dsb sy" ::: "memory");
		smccall(0x82000010ULL, scmbase, 0);
		for(i = 0; i < 1000000 && !(sh->status & 3); i++)
			__asm__ volatile("nop");
		uartputs("base protos st ");
		uarthex(sh->payload[0]);
		uartputs(" n ");
		uarthex(sh->payload[1]);
		uartputs(" w0 ");
		uarthex(sh->payload[2]);
		uartputs(" w1 ");
		uarthex(sh->payload[3]);
		uartputs("\n");
	}
	for(i = 0; i < 4; i++)
		scmiprint(i);
	st = scmicall(Scmiprotoclock, Scmiclockcfgset, 2, Scmiclockenable, &attr);
	uartputs("clk2 enable st ");
	uarthex(st);
	uartputs(" smcret ");
	uarthex(scmiret);
	uartputs("\n");
	v = ((volatile u64 *)scmbase)[0];
	uartputs("shmem0 ");
	uarthex(v);
	uartputs("\n");
	/*
	 * Bring the block up in the order the vendor genpd code uses:
	 * clocks and resets first, then power, then release the NIU idle
	 * request.  The NPU NIU appears to hold its idle ack until its
	 * hclk/aclk are actually running, which is why the earlier
	 * power-then-clock ordering never saw the ack drop.
	 */
	{
		volatile u32 *pmu = (volatile u32 *)0xfdd90000ULL;
		volatile u32 *cru = (volatile u32 *)0xfdd20000ULL;
		volatile u32 *npu = (volatile u32 *)0xfde40000ULL;

		uartputs("before st ");
		uarthex(pmu[0x0098/4]);
		uartputs(" req ");
		uarthex(pmu[0x0050/4]);
		uartputs(" ack ");
		uarthex(pmu[0x0060/4]);
		uartputs(" idle ");
		uarthex(pmu[0x0068/4]);
		uartputs("\n");

		/* Clocks: mux gpll/2, hclk/pclk /4, NPU gates 0-12 open. */
		cru[0x11c/4] = 0x81cf0000U | (1U<<6) | 1U;
		cru[0x120/4] = 0x00ff0000U | 0x33U;
		cru[0x30c/4] = 0x1fff0000U;
		/* srst_a/srst_h are bits 11/12 of SOFTRST_CON(2). */
		cru[0x408/4] = (0x1800U<<16) | 0x1800U;
		for(i = 0; i < 1000; i++)
			__asm__ volatile("nop");
		cru[0x408/4] = (0x1800U<<16) | 0U;
		__asm__ volatile("dsb sy" ::: "memory");
		uartputs("clk: sel7 ");
		uarthex(cru[0x11c/4]);
		uartputs(" sel8 ");
		uarthex(cru[0x120/4]);
		uartputs(" gate3 ");
		uarthex(cru[0x30c/4]);
		uartputs(" softrst2 ");
		uarthex(cru[0x408/4]);
		uartputs(" ack ");
		uarthex(pmu[0x0060/4]);
		uartputs("\n");

		/* Power on: clear pwr bit 1, wait for status bit 1 to clear. */
		pmu[0x00a0/4] = 0x00020000U;
		for(i = 0; i < 100000; i++){
			if((pmu[0x0098/4] & 2) == 0)
				break;
			__asm__ volatile("nop");
		}
		uartputs("powered st ");
		uarthex(pmu[0x0098/4]);
		uartputs(" w ");
		uarthex(i);
		uartputs(" ack ");
		uarthex(pmu[0x0060/4]);
		uartputs("\n");

		/* Release the NIU idle request (bit 2) and wait for the ack. */
		pmu[0x0050/4] = 0x00040000U;
		__asm__ volatile("dsb sy" ::: "memory");
		for(i = 0; i < 1000000; i++){
			if((pmu[0x0060/4] & 4) == 0 && (pmu[0x0068/4] & 4) == 0)
				break;
			__asm__ volatile("nop");
		}
		uartputs("released req ");
		uarthex(pmu[0x0050/4]);
		uartputs(" ack ");
		uarthex(pmu[0x0060/4]);
		uartputs(" idle ");
		uarthex(pmu[0x0068/4]);
		uartputs(" w ");
		uarthex(i);
		uartputs("\n");

		if((pmu[0x0060/4] & 4) != 0)
			uartputs("npu niu stays idle; not touching 0xfde40000\n");
		else{
			uartputs("npu version ");
			uarthex(npu[0x0000/4]);
			uartputs(" vnum ");
			uarthex(npu[0x0004/4]);
			uartputs(" iommu ");
			uarthex(((volatile u32 *)0xfde4b000ULL)[0x0004/4]);
			uartputs("\n");
		}
	}
	uartputs("HARVEY-ARM64 RK3566-NPU PROBE-DONE\n");
}
#endif

__attribute__((noreturn)) void
arm64fault(void)
{
	u64 esr, elr, far, mpidr;

	__asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
	__asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
	__asm__ volatile("mrs %0, far_el1" : "=r"(far));
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	fb_stripe(15, 0x00ff0022);	/* Slot 15: Bright Red/Magenta on CPU fault/trap */
	if(fb_base != nil || is_bengal){
		volatile u32 *fb = (volatile u32 *)fbmem();
		if(fb != nil){
			int y0 = fb_height * 850 / 1600;
			int y1 = fb_height * 1150 / 1600;
			for(int y = y0; y < y1; y++)
				for(int x = 0; x < fb_width; x++)
					fb[y * fb_pxstride + x] = 0x00000000;
			fb_draw_hex(60, fb_height * 880 / 1600, esr, 0x00ffff00);   /* ESR in Yellow */
			fb_draw_hex(60, fb_height * 940 / 1600, elr, 0x0000ffff);   /* ELR in Cyan */
			fb_draw_hex(60, fb_height * 1000 / 1600, far, 0x00ff00ff);  /* FAR in Magenta */
			fb_draw_hex(60, fb_height * 1060 / 1600, mpidr, 0x0000ff00); /* MPIDR in Green */
		}
	}
	uartputs("ARM64 exception esr ");
	uarthex(esr);
	uartputs(" elr ");
	uarthex(elr);
	uartputs(" far ");
	uarthex(far);
	{
		u64 spsr;
		__asm__ volatile("mrs %0, spsr_el1" : "=r"(spsr));
		uartputs(" spsr ");
		uarthex(spsr);
	}
	uartputs("\n");
	for(;;)
		__asm__ volatile("wfi");
}

void
arm64main(usize dtb)
{
	u8 *p;
	u32 size;
	int parsed;
	Fdtinfo info;
	u64 page0, page1, codepage, stackpage, codepage2, stackpage2;
	u64 root, userroot, userroot2, translated;
#ifdef ARM64_TEST_RESERVE
	u64 blocked;
#endif

	arm64machinit();
	__asm__ volatile("msr vbar_el1, %0; isb" :: "r"(arm64vectors) : "memory");
	fb_stripe(0, 0x00ffffff);	/* Slot 0: Entry (White) */
	if(dtb == 0 || (dtb & 7) != 0){
		fb_stripe(15, 0x00ff0000);	/* RED: dtb pointer invalid/unaligned */
		return;
	}
	p = (u8 *)dtb;
	size = beget4(p + 4);
	if(beget4(p) != Fdtmagic){
		fb_stripe(15, 0x00ffff00);	/* YELLOW: magic is not 0xd00dfeed */
		return;
	}
	if(size < Fdtmin || size > Fdtmax){
		fb_stripe(15, 0x00ff8000);	/* ORANGE: size out of bounds */
		return;
	}
	info.uartbase = 0;
	info.uarttype = 0;
	parsed = fdtparse(p, size, &info);
	uartbase = info.uartbase;
	uarttype = info.uarttype;
	if(parsed < 0){
		fb_stripe(15, 0x00ff00ff);	/* PURPLE: fdtparse internal error */
		uartputs("HARVEY-ARM64 FDT-ERROR\n");
		return;
	}
	/* The device tree is now the source of platform identity. */
	is_bengal = info.board == Fdtboardbengal;
#ifdef ARM64_RK3566_NPU_PROBE
	if(info.scmbase != 0)
		scmbase = info.scmbase;
#endif
	if(info.fbbase != 0 && info.fbbpp == 32 &&
	   info.fbbase >= info.rambase &&
	   info.fbbase + info.fbsize > info.fbbase &&
	   info.fbbase + info.fbsize <= info.rambase + info.ramsize){
		fb_base = (u32 *)info.fbbase;
		fb_width = info.fbwidth != 0 ? info.fbwidth : 720;
		fb_height = info.fbheight != 0 ? info.fbheight : 1600;
		fb_pxstride = info.fbstride != 0 ? info.fbstride/4 : fb_width;
		fb_bpp = 32;
	}
	fb_stripe(1, 0x0000ff00);	/* Slot 1: FDT parsed (Green) */
	if(fdtselectram(&info, (u64)_start, (u64)__image_end) < 0){
		fb_stripe(15, 0x00000088);	/* DARK BLUE: fdtselectram error */
		uartputs("HARVEY-ARM64 RAM-ERROR\n");
		return;
	}
	fb_stripe(2, 0x000000ff);	/* Slot 2: RAM selected (Blue) */
	uartputs("HARVEY-ARM64 ENTRY\n");
	uartputs("dtb: valid at ");
	uarthex(dtb);
	uartputs(", bytes ");
	uarthex(size);
	uartputs("\nram: ");
	uarthex(info.rambase);
	uartputs(" bytes ");
	uarthex(info.ramsize);
	uartputs("\nuart: ");
	uarthex(info.uartbase);
	uartputs("\ngic: ");
	uarthex(info.gicdist);
	uartputs(" cpu ");
	uarthex(info.giccpu);
	uartputs("\ntimer irq: ");
	uarthex(info.timerirq);
	uartputs("\nboard: ");
	uartputs((char *)fdtboardname(info.board));
	if(info.rootcompat[0] != 0){
		uartputs(" [");
		uartputs(info.rootcompat);
		uartputs("]");
	}
	uartputs("\nncpu: ");
	uarthex(info.ncpu);
	uartputs(" fb: ");
	uarthex(info.fbbase);
	uartputs(" bootarchive: ");
	uarthex(info.bootarchbase);
	uartputs(" teeshm: ");
	uarthex(info.teeshmbase);
	uartputs("\nHARVEY-ARM64 EARLY-BOOT PASS\n");
#ifdef ARM64_TEST_RESERVE
	if(info.nreserve == Maxfdtreserve)
		return;
	blocked = ((u64)__image_end+4095) & ~(u64)4095;
	info.reserve[info.nreserve].base = blocked;
	info.reserve[info.nreserve++].size = 4096;
#endif
	/*
	 * Reserve a contiguous DMA arena for user-space controllers.  It is
	 * excluded from the page allocator and published as #P/dma.
	 */
	if(is_bengal){
		/* Sized by the LKL instances this image runs: the pack
		 * tool sums their memory requirements into
		 * /chosen/harvey,lkl-arena-size.  The constant is only
		 * a fallback for unstamped images. */
		u64 sz = info.lklarena != 0 ? info.lklarena :
			320ULL * 1024 * 1024;
		if(info.nreserve < Maxfdtreserve &&
		   info.ramsize >= sz + (64ULL * 1024 * 1024)){
			u64 base = (info.rambase + info.ramsize - sz) & ~(2ULL*1024*1024 - 1);
			info.reserve[info.nreserve].base = base;
			info.reserve[info.nreserve++].size = sz;
			arm64dmabase = base;
			arm64dmasize = sz;
			/*
			 * Persistent boot log (devcons.c): a slice just
			 * below the arena, inside the mapped RAM window.
			 * The SoC ramoops address (0xaf000000) is in a
			 * DRAM bank this kernel does not map, so it
			 * cannot host the log; this reserved slice is
			 * never handed to the page allocator and DRAM
			 * contents survive warm resets.
			 */
			if(info.nreserve < Maxfdtreserve && base >=
			   info.rambase + 0xc0000ULL){
				extern u64 bootlogpaaddr;

				bootlogpaaddr = base - 0xc0000ULL;
				info.reserve[info.nreserve].base =
					bootlogpaaddr;
				info.reserve[info.nreserve++].size =
					0xc0000ULL;
			}
		}else{
			arm64dmasize = 0;
		}
	}
	if(!is_bengal && info.nreserve < Maxfdtreserve &&
	   info.rambase+info.ramsize > info.rambase &&
	   info.ramsize > arm64dmasize + 2*1024*1024){
		u64 base = (info.rambase + info.ramsize - arm64dmasize) &
		           ~((u64)(2*1024*1024 - 1));
		if(base > (u64)__image_end &&
		   base+arm64dmasize > base &&
		   base+arm64dmasize <= info.rambase+info.ramsize){
			info.reserve[info.nreserve].base = base;
			info.reserve[info.nreserve++].size = arm64dmasize;
			arm64dmabase = base;
		}
	}
	/*
	 * Static non-secure shared memory for OP-TEE.  Prefer the range the
	 * firmware/pack tool stamped into /chosen; the RK3566 vendor layout
	 * (0x09200000, 2 MiB, type MEM_AREA_NSEC_SHM) is kept only as a
	 * fallback for DTBs that predate the property.
	 */
	if(info.teeshmbase != 0 && info.teeshmsize != 0 &&
	   info.nreserve < Maxfdtreserve &&
	   info.teeshmbase >= info.rambase &&
	   info.teeshmbase + info.teeshmsize > info.teeshmbase &&
	   info.teeshmbase + info.teeshmsize <= info.rambase + info.ramsize){
		info.reserve[info.nreserve].base = info.teeshmbase;
		info.reserve[info.nreserve++].size = info.teeshmsize;
		arm64teeshmbase = info.teeshmbase;
		arm64teeshmsize = info.teeshmsize;
	}else if(info.board == Fdtboardrk3566 &&
	         info.nreserve < Maxfdtreserve &&
	         0x9200000 >= info.rambase &&
	         0x9200000 + 0x200000 <= info.rambase + info.ramsize){
		info.reserve[info.nreserve].base = 0x9200000;
		info.reserve[info.nreserve++].size = 0x200000;
		arm64teeshmbase = 0x9200000;
		arm64teeshmsize = 0x200000;
	}
	/*
	 * Boot archive (programs for /boot), if the loader or pack tool
	 * placed one.  /chosen/harvey,bootarchive names it directly; the
	 * magic scan at the legacy load addresses is only a fallback for
	 * images and loaders that do not stamp it.
	 */
	if(info.bootarchbase != 0 && info.bootarchsize >= 24 &&
	   info.bootarchsize <= 0xffffffffULL &&
	   info.nreserve < Maxfdtreserve &&
	   info.bootarchbase >= (u64)__image_end &&
	   info.bootarchbase + info.bootarchsize > info.bootarchbase &&
	   info.bootarchbase + info.bootarchsize <= info.rambase + info.ramsize){
		info.reserve[info.nreserve].base = info.bootarchbase;
		info.reserve[info.nreserve++].size = info.bootarchsize;
		arm64bootarchbase = info.bootarchbase;
		arm64bootarchsize = (u32)info.bootarchsize;
	}else if(!is_bengal){
		/*
		 * QEMU virt with 768 MiB puts the DMA arena at 0x50000000,
		 * which is also the board's archive address.  Scan the
		 * board address first, then the lower QEMU slot that
		 * clears the arena.
		 */
		static const u64 cand[] = { 0x50000000ULL, 0x49000000ULL };
		u64 addr;
		u32 ci;

		for(ci = 0; ci < sizeof(cand)/sizeof(cand[0]) && info.nreserve < Maxfdtreserve; ci++){
			u8 *ap;
			u32 total = 0;

			addr = cand[ci];
			if(addr < info.rambase ||
			   addr + 24 > info.rambase + info.ramsize)
				continue;
			ap = (u8 *)addr;
			if(ap[0]=='O' && ap[1]=='9' && ap[2]=='B' && ap[3]=='O' &&
			   ap[4]=='O' && ap[5]=='T' && ap[6]=='0' && ap[7]=='1')
				total = (u32)ap[16] | (u32)ap[17]<<8 |
				        (u32)ap[18]<<16 | (u32)ap[19]<<24;
			if(total < 24 || total > 1280u*1024*1024 ||
			   addr < (u64)__image_end ||
			   addr+total > info.rambase+info.ramsize)
				continue;
			info.reserve[info.nreserve].base = addr;
			info.reserve[info.nreserve++].size = total;
			arm64bootarchbase = addr;
			arm64bootarchsize = total;
			break;
		}
	}
	if(arm64physinit(&info, dtb, size) < 0 ||
	   (page0 = arm64physpage()) == 0 ||
	   (page1 = arm64physpage()) == 0 ||
	   page1 == page0 || ((page0|page1)&4095) != 0 ||
	   *(u64 *)page0 != 0 || *(u64 *)(page0+4096-8) != 0 ||
	   *(u64 *)page1 != 0 || *(u64 *)(page1+4096-8) != 0){
		fb_stripe(15, 0x00ff8000);	/* ORANGE: physinit/page0/page1 failed */
		uartputs("physical pages: initialization failed\n");
		return;
	}
	*(u64 *)page0 = 0x1122334455667788ULL;
	if(*(u64 *)page0 != 0x1122334455667788ULL || *(u64 *)page1 != 0){
		fb_stripe(15, 0x00ff00ff);	/* MAGENTA: isolation failed */
		uartputs("physical pages: isolation failed\n");
		return;
	}
	fb_stripe(3, 0x00ffff00);	/* Slot 3: Phys pages OK (Yellow) */
	uartputs("physical page: ");
	uarthex(page0);
	uartputs("\nHARVEY-ARM64 PHYS-PAGE PASS\n");
#ifdef ARM64_TEST_RESERVE
	if(page0 <= blocked){
		uartputs("reserved page: allocator reused it\n");
		return;
	}
	uartputs("HARVEY-ARM64 RESERVED-PAGE PASS\n");
#endif
	codepage = arm64physpage();
	stackpage = arm64physpage();
	codepage2 = arm64physpage();
	stackpage2 = arm64physpage();
	root = arm64mmubuild(&info);
	if(codepage == 0 || stackpage == 0 || codepage2 == 0 ||
	   stackpage2 == 0 || root == 0 ||
	   (userroot = arm64mmuuser(root, codepage, stackpage)) == 0 ||
	   (userroot2 = arm64mmuuser(root, codepage2, stackpage2)) == 0){
		fb_stripe(15, 0x0000ffff);	/* CYAN: mmubuild/mmuuser failed */
		uartputs("MMU: page table build failed\n");
		return;
	}
	fb_stripe(4, 0x00ff00ff);	/* Slot 4: MMU build OK (Magenta) */
	arm64mmuon(root);
	if(!arm64mmuactive() || *(u64 *)page0 != 0x1122334455667788ULL ||
	   *(u64 *)page1 != 0){
		fb_stripe(15, 0x00ffffff);	/* WHITE: mmuactive/RAM check failed */
		uartputs("MMU: translated RAM check failed\n");
		return;
	}
	if(arm64mmutranslate(page0, &translated) < 0 || translated != page0 ||
	   (info.uartbase != 0 && (arm64mmutranslate(info.uartbase, &translated) < 0 ||
	    translated != info.uartbase)) ||
	   arm64mmutranslate(info.rambase+info.ramsize-4096, &translated) < 0 ||
	   translated != info.rambase+info.ramsize-4096 ||
	   arm64mmutranslate(info.rambase+info.ramsize, &translated) == 0){
		fb_stripe(15, 0x0000ff00);	/* GREEN: translate boundaries failed */
		uartputs("MMU: translation boundaries failed\n");
		return;
	}
	fb_stripe(5, 0x0000ffff);	/* Slot 5: MMU Active & Translated (Cyan) */
	uartputs("HARVEY-ARM64 MMU-ON PASS\n");
	/*
	 * Probe the reserved DMA arena through its identity mapping.  This is
	 * the physical range user space receives as #P/dma; a bus abort here
	 * names the failing block instead of leaving a user fault looping.
	 */
	if(!is_bengal && arm64dmabase != 0 && arm64dmasize != 0){
		u64 off;
		volatile u64 *q;

		for(off = 0; off < arm64dmasize; off += 2*1024*1024){
			q = (volatile u64 *)(arm64dmabase + off);
			*q = 0x0123456789abcdefULL;
			if(*q != 0x0123456789abcdefULL){
				uartputs("ARM64 DMA-PROBE FAIL ");
				uarthex(arm64dmabase + off);
				uartputs("\n");
				break;
			}
		}
		if(off >= arm64dmasize)
			uartputs("HARVEY-ARM64 DMA-PROBE PASS\n");
	}
	fb_stripe(6, 0x00ff8000);	/* Slot 6: Timer Init Starting (Orange) */
	if(arm64timerinit(&info) < 0){
		fb_stripe(15, 0x00ff0000);
		uartputs("timer: initialization failed\n");
		return;
	}
	if(arm64timerwait() < 0){
		fb_stripe(15, 0x00ff0000);
		fb_draw_hex(60, 1280, arm64timerdiag(), 0x00ffff00);
		uartputs("timer: interrupts not delivered, state ");
		uarthex(arm64timerdiag());
		uartputs("\n");
		arm64timerstop();
		return;
	}
	fb_stripe(6, 0x00ff8000);	/* Slot 6: Timer OK (Orange) */
	uartputs("HARVEY-ARM64 TIMER-IRQ PASS\n");
	if(arm64irqmaskprobe() < 0){
		fb_stripe(15, 0x00ff0000);
		uartputs("ARM64 interrupt mask failed\n");
		return;
	}
	uartputs("HARVEY-ARM64 IRQ-MASK PASS\n");
	arm64mmuswitch(userroot);
	if(arm64mmutranslate(ARM64USERBASE, &translated) < 0 ||
	   translated != codepage){
		fb_stripe(15, 0x00ff0000);
		uartputs("EL0: first address space failed\n");
		return;
	}
	if(arm64userprobe(codepage, stackpage, page0) < 0){
		fb_stripe(15, 0x00ff0000);
		uartputs("EL0: trap/isolation check failed, ESR ");
		uarthex(arm64userdiag());
		uartputs(" detail ");
		uarthex(arm64userdiagdetails());
		uartputs("\n");
		return;
	}
	arm64mmuswitch(userroot2);
	if(arm64mmutranslate(ARM64USERBASE, &translated) < 0 ||
	   translated != codepage2 ||
	   arm64userprobe(codepage2, stackpage2, page0) < 0){
		fb_stripe(15, 0x00ff0000);
		uartputs("EL0: second address space failed, ESR ");
		uarthex(arm64userdiag());
		uartputs(" detail ");
		uarthex(arm64userdiagdetails());
		uartputs("\n");
		return;
	}
	arm64mmuswitch(root);
	if(arm64mmutranslate(ARM64USERBASE, &translated) == 0){
		fb_stripe(15, 0x00ff0000);
		uartputs("EL0: user mapping visible in kernel root\n");
		return;
	}
	if(arm64contextprobe() < 0){
		fb_stripe(15, 0x00ff0000);
		uartputs("ARM64 context switch failed\n");
		return;
	}
	uartputs("HARVEY-ARM64 CONTEXT-SWITCH PASS\n");
	arm64timerstop();
	if(arm64el0irqs == 0){
		fb_stripe(15, 0x00ff0000);
		uartputs("EL0: timer did not interrupt user code\n");
		return;
	}
	fb_stripe(7, 0x0000ff00);	/* Slot 7: EL0 Userland verified! (Green) */
	uartputs("HARVEY-ARM64 EL0-SVC PASS\n");
	uartputs("HARVEY-ARM64 EL0-ISOLATION PASS\n");
	uartputs("HARVEY-ARM64 EL0-PERMISSIONS PASS\n");
	uartputs("HARVEY-ARM64 ADDRESS-SPACE SWITCH PASS\n");
	uartputs("HARVEY-ARM64 EL0-TIMER-IRQ PASS\n");
#ifdef ARM64_RK3566_NPU_PROBE
	/*
	 * Board quirk, not a generic path: the RK3566 NPU needs the vendor
	 * clock/reset sequence before its registers can be touched.  Select
	 * it by platform identity; the UART address is a fallback for DTBs
	 * without a root compatible.
	 */
	if(info.board == Fdtboardrk3566 ||
	   (info.board == Fdtboardunknown && info.uartbase == 0xfe660000ULL))
		rk3566npuprobe(page1);
#endif
#ifdef ARM64_FULL_KERNEL
	if(arm64physprobe() < 0){
		uartputs("ARM64 full-kernel physical allocator failed\n");
		return;
	}
	uartputs("HARVEY-ARM64 FULL-KERNEL-LINK PASS\n");
	arm64kernelstart(&info);
#endif
#ifdef ARM64_TEST_FAULT
	__asm__ volatile("brk #0");
#endif
}
