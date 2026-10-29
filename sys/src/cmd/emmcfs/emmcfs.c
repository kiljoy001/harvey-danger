/*
 * emmcfs: a 9P file server for the RK3566 eMMC.
 *
 * The card is identified once at startup and then served as:
 *
 *   /ctl    negotiated speed, geometry and capacity (read)
 *   /data   the raw device, byte addressable (read/write)
 *
 * The driver core is ../emmc/emmc.c, built with -DEMMCFS_LIB.  The
 * controller has a single command queue, so every transfer is serialised
 * by one lock.  Byte ranges that are not block aligned become a
 * read-modify-write through a bounce buffer.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#include "../emmc/emmc.h"

enum {
	Block		= 512,
	Maxblocks	= 128,

	Qctl,
	Qdata,
};

static void fsread(Req *);
static void fswrite(Req *);

static Srv fs = {
	.read = fsread,
	.write = fswrite,
};
static QLock cardlk;
static u64 capacity;
static char ctl[256];

static void
fatal(char *fmt, ...)
{
	va_list a;

	va_start(a, fmt);
	vfprint(2, fmt, a);
	va_end(a);
	fprint(2, "\n");
	exits("emmcfs");
}

static void
fsdestroyfile(File *f)
{
	USED(f);
}

/*
 * Move n bytes at byte offset off to or from the card.  Only whole
 * blocks reach the hardware; a partial block turns into a
 * read-modify-write.  The caller holds cardlk.
 */
static int
cardio(u64 off, void *vbuf, long n, int write)
{
	static u8 tmp[Maxblocks*Block];
	u8 *b = vbuf;
	u32 lba;
	int boff, nblk, m;

	while(n > 0){
		lba = off / Block;
		boff = off % Block;
		nblk = (boff + n + Block - 1) / Block;
		if(nblk > Maxblocks)
			nblk = Maxblocks;
		if(boff == 0 && n >= nblk * Block){
			/* whole blocks straight from the caller's buffer */
			if(write ? emmcwrite(lba, nblk, b) < 0
			         : emmcread(lba, nblk, b) < 0)
				return -1;
			m = nblk * Block;
		}else{
			if(emmcread(lba, nblk, tmp) < 0)
				return -1;
			m = nblk * Block - boff;
			if(m > n)
				m = n;
			if(write){
				memmove(tmp + boff, b, m);
				if(emmcwrite(lba, nblk, tmp) < 0)
					return -1;
			}else
				memmove(b, tmp + boff, m);
		}
		b += m;
		off += m;
		n -= m;
	}
	return 0;
}

static void
fsread(Req *r)
{
	u64 off;
	long n;

	if((uintptr)r->fid->file->aux != Qdata){
		readstr(r, ctl);
		respond(r, nil);
		return;
	}
	off = r->ifcall.offset;
	n = r->ifcall.count;
	if(off >= capacity)
		n = 0;
	else if((u64)n > capacity - off)
		n = capacity - off;
	if(n > 0){
		qlock(&cardlk);
		if(cardio(off, r->ofcall.data, n, 0) < 0){
			qunlock(&cardlk);
			respond(r, "emmc read failed");
			return;
		}
		qunlock(&cardlk);
	}
	r->ofcall.count = n;
	respond(r, nil);
}

static void
fswrite(Req *r)
{
	u64 off;
	long n;

	if((uintptr)r->fid->file->aux != Qdata){
		respond(r, "write not permitted");
		return;
	}
	off = r->ifcall.offset;
	n = r->ifcall.count;
	if(off >= capacity){
		respond(r, "write past end");
		return;
	}
	if((u64)n > capacity - off)
		n = capacity - off;
	if(n > 0){
		qlock(&cardlk);
		if(cardio(off, r->ifcall.data, n, 1) < 0){
			qunlock(&cardlk);
			respond(r, "emmc write failed");
			return;
		}
		qunlock(&cardlk);
	}
	r->ofcall.count = n;
	respond(r, nil);
}

void
main(int argc, char **argv)
{
	File *f;

	USED(argc);
	USED(argv);
	if(emmcinit() < 0)
		fatal("emmcfs: card init failed");
	capacity = emmccapacity();
	if(capacity == 0)
		fatal("emmcfs: cannot read EXT_CSD");

	snprint(ctl, sizeof ctl,
		"emmc rk3566\n"
		"mode %s\n"
		"blocks %llud\n"
		"blocksize %d\n"
		"bytes %llud\n",
		emmcmodename(), capacity/Block, Block, capacity);
	print("emmcfs: %s, %llud blocks\n", emmcmodename(),
	      capacity/Block);

	fs.tree = alloctree(nil, nil, DMDIR|0777, fsdestroyfile);
	if(fs.tree == nil)
		fatal("emmcfs: alloctree: %r");
	f = createfile(fs.tree->root, "ctl", nil, 0444, (void*)Qctl);
	if(f == nil)
		fatal("emmcfs: createfile ctl: %r");
	f->length = strlen(ctl);
	f = createfile(fs.tree->root, "data", nil, 0666, (void*)Qdata);
	if(f == nil)
		fatal("emmcfs: createfile data: %r");
	f->length = capacity;

	postmountsrv(&fs, "emmc", nil, MREPL|MCREATE);
	for(;;)
		sleep(1000);
}
