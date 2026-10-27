/*
 * softscreen.c - Bochs DISPI linear framebuffer backend for devdraw.
 *
 * The v64 browser VM provides a Bochs VGA adapter (PCI 1234:1111).
 * attachscreen() programs the DISPI registers for 1024x768x32 and maps
 * the PCI BAR0 linear framebuffer as the kernel draw screen, so Rio
 * and other /dev/draw clients render directly into host-visible memory.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "ureg.h"
#include "../port/error.h"

#define Image IMAGE
#include <draw.h>
#include <memdraw.h>
#include <cursor.h>
#include "screen.h"

enum {
	DISPI_INDEX = 0x1CE,
	DISPI_DATA = 0x1CF,

	DISPI_ID = 0,
	DISPI_XRES = 1,
	DISPI_YRES = 2,
	DISPI_BPP = 3,
	DISPI_ENABLE = 4,
	DISPI_BANK = 5,
	DISPI_VIRT_WIDTH = 6,
	DISPI_VIRT_HEIGHT = 7,
	DISPI_X_OFFSET = 8,
	DISPI_Y_OFFSET = 9,

	DISPI_DISABLED = 0x00,
	DISPI_ENABLED = 0x01,
	DISPI_LFB_ENABLED = 0x40,
	DISPI_NOCLEARMEM = 0x80,

	ScrW = 1024,
	ScrH = 768,
	ScrDepth = 32,
};

Point ZP = {0, 0};
Rectangle physgscreenr;
Memdata gscreendata;
Memimage *gscreen;
VGAscr vgascreen[1];

Cursor arrow = {
	{-1, -1},
	{
		0xFF,
		0xFF,
		0x80,
		0x01,
		0x80,
		0x02,
		0x80,
		0x0C,
		0x80,
		0x10,
		0x80,
		0x10,
		0x80,
		0x08,
		0x80,
		0x04,
		0x80,
		0x02,
		0x80,
		0x01,
		0x80,
		0x02,
		0x8C,
		0x04,
		0x92,
		0x08,
		0x91,
		0x10,
		0xA0,
		0xA0,
		0xC0,
		0x40,
	},
	{
		0x00,
		0x00,
		0x7F,
		0xFE,
		0x7F,
		0xFC,
		0x7F,
		0xF0,
		0x7F,
		0xE0,
		0x7F,
		0xE0,
		0x7F,
		0xF0,
		0x7F,
		0xF8,
		0x7F,
		0xFC,
		0x7F,
		0xFE,
		0x7F,
		0xFC,
		0x73,
		0xF8,
		0x61,
		0xF0,
		0x60,
		0xE0,
		0x40,
		0x40,
		0x00,
		0x00,
	},
};

int didswcursorinit;

static void *lfb;
static int screenready;

static void
dispiwrite(int index, int value)
{
	outs(DISPI_INDEX, index);
	outs(DISPI_DATA, value);
}

static int
dispiread(int index)
{
	outs(DISPI_INDEX, index);
	return ins(DISPI_DATA);
}

unsigned char *
attachscreen(Rectangle *r, u32 *chan, int *d, int *width, int *softscreen)
{
	Pcidev *p;
	u64 paddr;
	int id;

	if(!screenready){
		id = dispiread(DISPI_ID);
		if(id < 0xB0C0 || id > 0xB0C6){
			print("softscreen: no Bochs DISPI adapter (id %#x)\n", id);
			return nil;
		}
		p = pcimatch(nil, 0x1234, 0x1111);
		if(p == nil || p->mem[0].size == 0){
			print("softscreen: Bochs VGA PCI device not found\n");
			return nil;
		}
		paddr = p->mem[0].bar & ~0xF;
		if(p->mem[0].size < ScrW * ScrH * (ScrDepth / 8)){
			print("softscreen: framebuffer too small (%d bytes)\n", p->mem[0].size);
			return nil;
		}
		lfb = vmap(paddr, p->mem[0].size);
		if(lfb == nil){
			print("softscreen: vmap of framebuffer failed\n");
			return nil;
		}
		dispiwrite(DISPI_ENABLE, DISPI_DISABLED);
		dispiwrite(DISPI_XRES, ScrW);
		dispiwrite(DISPI_YRES, ScrH);
		dispiwrite(DISPI_BPP, ScrDepth);
		dispiwrite(DISPI_BANK, 0);
		dispiwrite(DISPI_VIRT_WIDTH, ScrW);
		dispiwrite(DISPI_VIRT_HEIGHT, ScrH);
		dispiwrite(DISPI_X_OFFSET, 0);
		dispiwrite(DISPI_Y_OFFSET, 0);
		dispiwrite(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED | DISPI_NOCLEARMEM);

		memimageinit();
		gscreendata.bdata = (uchar *)lfb;
		gscreen = allocmemimaged(Rect(0, 0, ScrW, ScrH), ARGB32, &gscreendata);
		if(gscreen == nil){
			print("softscreen: allocmemimaged failed\n");
			return nil;
		}
		physgscreenr = gscreen->r;
		memset(lfb, 0, ScrW * ScrH * (ScrDepth / 8));
		screenready = 1;
		print("softscreen: Bochs LFB at %#llx, %dx%dx%d\n", paddr, ScrW, ScrH, ScrDepth);
	}

	*r = gscreen->r;
	*chan = gscreen->chan;
	*d = gscreen->depth;
	*width = gscreen->width;
	*softscreen = 0;
	return gscreendata.bdata;
}

void
flushmemscreen(Rectangle r)
{
	USED(r);
}

static u32 softpalette[256][3];

void
getcolor(u32 p, u32 *pr, u32 *pg, u32 *pb)
{
	if(p >= 256){
		*pr = *pg = *pb = 0;
		return;
	}
	*pr = softpalette[p][0];
	*pg = softpalette[p][1];
	*pb = softpalette[p][2];
}

int
setcolor(u32 p, u32 r, u32 g, u32 b)
{
	if(p >= 256)
		return -1;
	softpalette[p][0] = r;
	softpalette[p][1] = g;
	softpalette[p][2] = b;
	return 0;
}

void
drawerror(Display *d, char *s)
{
	USED(d);
	error(s);
}

int
screensize(int x, int y, int z, u32 chan)
{
	USED(x); USED(y); USED(z); USED(chan);
	error("softscreen: fixed resolution");
	return -1;
}

int
screenaperture(int size, int align)
{
	USED(size); USED(align);
	return 0;
}

void
blankscreen(int blank)
{
	USED(blank);
}

/*
 * Software cursor, after Plan 9's port/swcursor.c, but composited
 * with direct framebuffer accesses: Harvey's libmemdraw does not
 * reproduce the 16x16 cursor save/draw/restore cycle exactly (it
 * leaves red-channel residue in cycled regions), so the cycle is
 * done with plain pixel copies, which are exact by construction.
 * gscreen is the host-visible linear framebuffer.  devmouse drives
 * this through cursoron/cursoroff from its redraw process, draws
 * are kept off the screen by swcursoravoid (see hwdraw), and Rio's
 * cursor changes arrive through setcursor.
 */
static Cursor swcur;
static Rectangle swrect;	/* screen rect actually saved/drawn */
static Point swvispt;		/* cursor origin (may be off-screen) */
static int swvisible;
static u32 swbackpx[16 * 16];	/* saved pixels, indexed from swvispt */

static u32 *
swfbpt(int x, int y)
{
	return (u32 *)gscreendata.bdata + y * ScrW + x;
}

static void
swhide(void)
{
	int x, y;

	if(!swvisible)
		return;
	swvisible = 0;
	if(gscreen == nil)
		return;
	for(y = swrect.min.y; y < swrect.max.y; y++)
		for(x = swrect.min.x; x < swrect.max.x; x++)
			*swfbpt(x, y) = swbackpx[(y - swvispt.y) * 16 + (x - swvispt.x)];
}

static void
swdraw(Point p)
{
	int x, y, sbit, cbit;

	if(swvisible)
		return;
	if(gscreen == nil)
		return;
	swvispt = addpt(swcur.offset, p);
	swrect = Rect(swvispt.x, swvispt.y, swvispt.x + 16, swvispt.y + 16);
	if(!rectclip(&swrect, gscreen->r))
		return;
	for(y = swrect.min.y; y < swrect.max.y; y++)
		for(x = swrect.min.x; x < swrect.max.x; x++)
			swbackpx[(y - swvispt.y) * 16 + (x - swvispt.x)] = *swfbpt(x, y);
	for(y = swrect.min.y; y < swrect.max.y; y++){
		for(x = swrect.min.x; x < swrect.max.x; x++){
			sbit = swcur.set[(y - swvispt.y) * 2 + (x - swvispt.x) / 8] & (0x80 >> ((x - swvispt.x) % 8));
			cbit = swcur.clr[(y - swvispt.y) * 2 + (x - swvispt.x) / 8] & (0x80 >> ((x - swvispt.x) % 8));
			if(sbit)
				*swfbpt(x, y) = 0xFF000000;
			else if(cbit)
				*swfbpt(x, y) = 0xFFFFFFFF;
		}
	}
	swvisible = 1;
}

int
cursoron(int dolock)
{
	USED(dolock);
	qlock(&drawlock);
	swdraw(mousexy());
	qunlock(&drawlock);
	return 0;
}

void
cursoroff(int dolock)
{
	USED(dolock);
	qlock(&drawlock);
	swhide();
	qunlock(&drawlock);
}

void
setcursor(Cursor *c)
{
	if(c == nil)
		c = &arrow;
	swcur = *c;
	mouseredraw();	/* mouse process redraws it under drawlock */
}

VGAcur swcursor;

void
swcursorinit(void)
{
	didswcursorinit = 1;
	swvisible = 0;
	swcur = arrow;
}

void
swcursorhide(void)
{
	swhide();
}

void
swcursoravoid(Rectangle r)
{
	if(swvisible && rectXrect(r, swrect)){
		swhide();
		mouseredraw();	/* schedule cursor redraw after this draw */
	}
}

/*
 * Called by memimagedraw before every draw.  This backend has no
 * hardware acceleration, but the hook is where the software cursor
 * is hidden before any draw that touches (or reads) the screen, so
 * window moves and other bitblts cannot overdraw the cursor or be
 * corrupted by its saved-background restore.  Always declines.
 */
int
hwdraw(Memdrawparam *par)
{
	Memimage *dst, *src, *mask;

	if((dst = par->dst) == nil || dst->data == nil)
		return 0;
	if((src = par->src) == nil || src->data == nil)
		return 0;
	if((mask = par->mask) == nil || mask->data == nil)
		return 0;
	if(dst->data->bdata == gscreendata.bdata)
		swcursoravoid(par->r);
	if(src->data->bdata == gscreendata.bdata)
		swcursoravoid(par->sr);
	if(mask->data->bdata == gscreendata.bdata)
		swcursoravoid(par->mr);
	return 0;
}

void
swcursorunhide(void)
{
	mouseredraw();
}
