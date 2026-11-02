/*
 * emmcfs: a 9P file server for the RK3566 eMMC.
 *
 * The card is identified once at startup and then served as:
 *
 *   /ctl    negotiated speed, geometry, capacity and partitions (read)
 *   /data   the raw device, byte addressable (read/write)
 *   /partN  the Nth DOS partition of /data (read/write)
 *   /flush  flush the card write cache (write)
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
	Qflush,

	Qpart		= 0x100,	/* + slot index */
	Maxpart		= 4,
};

typedef struct Part Part;
struct Part {
	u64	off;		/* byte offset on the card */
	u64	len;		/* byte length */
	int	slot;		/* 1-based MBR slot */
	u8	type;		/* MBR partition type */
};

static void fsread(Req *);
static void fswrite(Req *);

static Srv fs = {
	.read = fsread,
	.write = fswrite,
};
static QLock cardlk;
static u64 capacity;
static char ctl[512];
static Part part[Maxpart];
static int npart;
static u64 nread, nwrite, nreadreq, nwritereq;
static u64 wear;

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
		if(boff != 0){
			/* head: the first block is shared with earlier data */
			if(emmcread(lba, 1, tmp) < 0)
				return -1;
			m = Block - boff;
			if(m > n)
				m = n;
			if(write){
				memmove(tmp + boff, b, m);
				if(emmcwrite(lba, 1, tmp) < 0)
					return -1;
			}else
				memmove(b, tmp + boff, m);
		}else if(n >= Block){
			/* whole blocks straight from the caller's buffer */
			nblk = n / Block;
			if(nblk > Maxblocks)
				nblk = Maxblocks;
			if(write ? emmcwrite(lba, nblk, b) < 0
			         : emmcread(lba, nblk, b) < 0)
				return -1;
			m = nblk * Block;
		}else{
			/* tail: less than one block left */
			if(emmcread(lba, 1, tmp) < 0)
				return -1;
			m = n;
			if(write){
				memmove(tmp, b, m);
				if(emmcwrite(lba, 1, tmp) < 0)
					return -1;
			}else
				memmove(b, tmp, m);
		}
		b += m;
		off += m;
		n -= m;
	}
	return 0;
}

/*
 * Read the DOS partition table in the master boot sector.  The card may
 * have none, or may carry a Rockchip IDB at sector 0x40; either way only
 * sector 0 is inspected here, so the bootloader area is never touched.
 */
static void
readparts(void)
{
	u8 mbr[Block];
	u64 base, len;
	u8 *e;
	int i, n;

	if(emmcread(0, 1, mbr) < 0)
		return;
	if(mbr[510] != 0x55 || mbr[511] != 0xaa)
		return;
	for(i = 0; i < Maxpart && npart < Maxpart; i++){
		e = mbr + 446 + i*16;
		len = (u64)e[12] | (u64)e[13]<<8 | (u64)e[14]<<16 |
		      (u64)e[15]<<24;
		base = (u64)e[8] | (u64)e[9]<<8 | (u64)e[10]<<16 |
		       (u64)e[11]<<24;
		if(e[4] == 0 || len == 0 || base >= capacity/Block)
			continue;
		n = capacity/Block - base;
		if(len > (u64)n)
			len = n;
		part[npart].off = base * Block;
		part[npart].len = len * Block;
		part[npart].slot = i + 1;
		part[npart].type = e[4];
		npart++;
	}
}

/* The partition backing this fid, or nil for /data and friends. */
static Part *
partof(Req *r)
{
	int i;

	i = (int)(uintptr)r->fid->file->aux - Qpart;
	if(i < 0 || i >= npart)
		return nil;
	return &part[i];
}

/*
 * Clip a request against a window [base, base+len) and return the number
 * of bytes to move.
 */
static long
clamp(u64 off, long n, u64 base, u64 limit, u64 *cardoff)
{
	if(off >= limit)
		return 0;
	if((u64)n > limit - off)
		n = limit - off;
	*cardoff = base + off;
	return n;
}

static void
fsread(Req *r)
{
	Part *p;
	u64 off;
	long n;

	if((uintptr)r->fid->file->aux == Qctl){
		char buf[512];
		int i, l;

		l = snprint(buf, sizeof buf,
			"%smsize %ud\n"
			"read %llud bytes in %llud requests\n"
			"write %llud bytes in %llud requests\n",
			ctl, fs.msize, nread, nreadreq, nwrite, nwritereq);
		for(i = 0; i < npart && l < (int)sizeof buf; i++)
			l += snprint(buf+l, sizeof buf-l,
				"part%d type %#ux offset %llud bytes %llud\n",
				part[i].slot, part[i].type,
				part[i].off, part[i].len);
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	p = partof(r);
	if(p == nil && (uintptr)r->fid->file->aux != Qdata){
		respond(r, "permission denied");
		return;
	}
	off = r->ifcall.offset;
	n = r->ifcall.count;
	if(p != nil)
		n = clamp(off, n, p->off, p->len, &off);
	else
		n = clamp(off, n, 0, capacity, &off);
	if(n > 0){
		qlock(&cardlk);
		nreadreq++;
		nread += n;
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
	Part *p;
	u64 off;
	long n;

	if((uintptr)r->fid->file->aux == Qflush){
		qlock(&cardlk);
		if(emmcflush() < 0){
			qunlock(&cardlk);
			respond(r, "emmc flush failed");
			return;
		}
		qunlock(&cardlk);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}
	p = partof(r);
	if(p == nil && (uintptr)r->fid->file->aux != Qdata){
		respond(r, "write not permitted");
		return;
	}
	off = r->ifcall.offset;
	n = r->ifcall.count;
	if(p != nil)
		n = clamp(off, n, p->off, p->len, &off);
	else
		n = clamp(off, n, 0, capacity, &off);
	if(n > 0){
		qlock(&cardlk);
		nwritereq++;
		nwrite += n;
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
	char name[16];
	int i;

	USED(argc);
	USED(argv);
	if(emmcinit() < 0)
		fatal("emmcfs: card init failed");
	capacity = emmccapacity();
	if(capacity == 0)
		fatal("emmcfs: cannot read EXT_CSD");
	readparts();

	wear = emmcwear();
	snprint(ctl, sizeof ctl,
		"emmc rk3566\n"
		"mode %s\n"
		"blocks %llud\n"
		"blocksize %d\n"
		"bytes %llud\n"
		"partitions %d\n"
		"wear pre-eol %d type-a %d type-b %d\n",
		emmcmodename(), capacity/Block, Block, capacity, npart,
		(int)((wear>>16)&0xff), (int)((wear>>8)&0xff), (int)(wear&0xff));
	print("emmcfs: %s, %llud blocks, %d partitions\n", emmcmodename(),
	      capacity/Block, npart);

	/* Match the kernel's MAXRPC: one 128 KiB message per transfer. */
	fs.msize = 128*1024 + IOHDRSZ;
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
	for(i = 0; i < npart; i++){
		snprint(name, sizeof name, "part%d", part[i].slot);
		f = createfile(fs.tree->root, name, nil, 0666,
			       (void*)(uintptr)(Qpart + i));
		if(f == nil)
			fatal("emmcfs: createfile %s: %r", name);
		f->length = part[i].len;
		print("emmcfs: %s type %#ux offset %llud bytes %llud\n",
		      name, part[i].type, part[i].off, part[i].len);
	}
	f = createfile(fs.tree->root, "flush", nil, 0222, (void*)Qflush);
	if(f == nil)
		fatal("emmcfs: createfile flush: %r");

	postmountsrv(&fs, "emmc", nil, MREPL|MCREATE);
	for(;;)
		sleep(1000);
}
