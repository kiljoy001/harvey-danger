/*
 * sdcard: DesignWare MSHC (dw_mmc) driver for the RK3566 SD slot.
 *
 * The SD slot on the Orange Pi 3B is mmc@fe2b0000, a Rockchip
 * "dw-mshc" controller, not an SDHCI controller.  When U-Boot boots
 * from the card it has already powered the slot, set the bus width
 * and clock, and left the card in the transfer state, so this driver
 * takes the controller over in place: no CRU or card initialisation,
 * only the DesignWare command and FIFO (PIO) path.  That is enough
 * to read the card from user space.
 *
 *   sdcard probe            identify the controller and read block 0
 *   sdcard read <lba> [n]   read n blocks and show the first bytes
 *   sdcard info             show controller state only
 */
#include <u.h>
#include <libc.h>

/* DesignWare Mobile Storage Host Controller registers. */
enum {
	Ctrl		= 0x000,
	Pwren		= 0x004,
	Clkdiv		= 0x008,
	Clkena		= 0x010,
	Tmout		= 0x014,
	Ctype		= 0x018,
	Blksiz		= 0x01C,
	Bytcnt		= 0x020,
	Intmask		= 0x024,
	Cmdarg		= 0x028,
	Cmdreg		= 0x02C,
	Resp0		= 0x030,
	Mintsts		= 0x040,
	Rintsts		= 0x044,
	Status		= 0x048,
	Fifoth		= 0x04C,
	Cdetect		= 0x050,
	Uhs		= 0x074,
	Bmod		= 0x080,
	Data		= 0x200,
};

/* CTRL */
enum {
	CtrlReset	= 1<<0,
	CtrlFifoReset	= 1<<1,
	CtrlDmaReset	= 1<<2,
	DmaEn		= 1<<5,
	IdmacEn		= 1<<25,
};

/* CMD */
enum {
	CmdRespExp	= 1<<6,
	CmdRespLength	= 1<<7,
	CmdCheckCrc	= 1<<8,
	CmdDataExp	= 1<<9,
	CmdRw		= 1<<10,
	CmdSendStop	= 1<<12,
	CmdPrvDatWait	= 1<<13,
	CmdAbortStop	= 1<<14,
	CmdUpdClk	= 1<<21,
	CmdUseHold	= 1<<29,
	CmdStart	= 1<<31,
};

/* RINTSTS */
enum {
	IntRe		= 1<<1,
	IntCdone	= 1<<2,
	IntDto		= 1<<3,
	IntTxdr		= 1<<4,
	IntRxdr		= 1<<5,
	IntRcrc		= 1<<6,
	IntDcrc		= 1<<7,
	IntRto		= 1<<8,
	IntDrto		= 1<<9,
	IntHto		= 1<<10,
	IntFrun		= 1<<11,
	IntHle		= 1<<12,
	IntSbe		= 1<<13,
	IntAcd		= 1<<14,
	IntEbe		= 1<<15,
	IntDataErr	= IntEbe|IntSbe|IntHle|IntFrun|IntDcrc,
	IntDataTout	= IntHto|IntDrto,
};

/* STATUS */
enum {
	FifoEmpty	= 1<<2,
	FifoFull	= 1<<3,
	Busy		= 1<<9,
	FifoMask	= 0x1fff,
	FifoShift	= 17,
};

static u8 *mmio;
static int verbose = 1;

static u32
rd32(int off)
{
	return *(volatile u32 *)(mmio + off);
}

static void
wr32(int off, u32 v)
{
	*(volatile u32 *)(mmio + off) = v;
}

static void
fail(char *fmt, ...)
{
	va_list a;

	va_start(a, fmt);
	vfprint(2, fmt, a);
	va_end(a);
	fprint(2, "\n");
	exits("sdcard");
}

static u8 *
mapnode(char *node, u64 want)
{
	char path[128], buf[128], *p;
	u64 base, size, len;
	u8 *m;
	int fd;

	snprint(path, sizeof path, "#P/%s/reg", node);
	if((fd = open(path, OREAD)) < 0)
		fail("sdcard: no %s in #P: %r", node);
	if(read(fd, buf, sizeof buf - 1) <= 0)
		fail("sdcard: cannot read %s reg", node);
	close(fd);
	buf[sizeof buf - 1] = 0;
	base = strtoull(buf, &p, 0);
	snprint(path, sizeof path, "#P/%s/size", node);
	if((fd = open(path, OREAD)) < 0)
		fail("sdcard: cannot open %s size", node);
	if(read(fd, buf, sizeof buf - 1) <= 0)
		fail("sdcard: cannot read %s size", node);
	close(fd);
	buf[sizeof buf - 1] = 0;
	size = strtoull(buf, &p, 0);
	if(base != want || size == 0)
		fail("sdcard: unexpected %s %#llux+%#llux", node, base, size);
	len = (size + 0x1fffff) & ~0x1fffffULL;
	m = segattach(0, node, 0, len);
	if(m == (void *)-1)
		fail("sdcard: segattach %s failed: %r", node);
	m += base & 0x1fffff;
	return m;
}

/*
 * Boot the controller into the FIFO (PIO) path.  U-Boot may have left
 * it in internal-DMA mode, and its DMA was programmed for U-Boot's own
 * buffer, so disable both DMA engines before touching the FIFO.
 */
static void
fifomode(void)
{
	int i;
	u32 c;

	c = rd32(Ctrl);
	wr32(Ctrl, c & ~(IdmacEn | DmaEn));
	wr32(Bmod, rd32(Bmod) & ~(1<<7 | 1<<1 | 1<<0));

	wr32(Ctrl, rd32(Ctrl) | CtrlFifoReset);
	for(i = 0; i < 10000; i++)
		if((rd32(Ctrl) & CtrlFifoReset) == 0)
			break;
		else
			sleep(1);
	wr32(Intmask, 0);
	wr32(Rintsts, 0xffffffff);
}

static int
waitwhilebusy(void)
{
	int i;

	for(i = 0; i < 2000; i++){
		if((rd32(Status) & Busy) == 0)
			return 0;
		sleep(1);
	}
	return -1;
}

/*
 * Send a command.  resp may be nil for commands without a response.
 * Data (if any) must be set up by the caller before the command.
 */
static int
command(int idx, u32 arg, u32 flags, u32 *resp)
{
	int i;
	u32 st;

	if(waitwhilebusy() < 0){
		if(verbose)
			print("sdcard: cmd %d card busy\n", idx);
		return -1;
	}
	wr32(Rintsts, 0xffffffff);
	wr32(Cmdarg, arg);
	flags |= idx | CmdStart | CmdUseHold | CmdPrvDatWait;
	wr32(Cmdreg, flags);
	__asm__ volatile("dsb sy" ::: "memory");

	for(i = 0; i < 100000; i++){
		st = rd32(Rintsts);
		if(st & (IntRto|IntRe|IntRcrc|IntDataErr|IntDataTout)){
			if(verbose)
				print("sdcard: cmd %d error rint %ux\n", idx, st);
			wr32(Rintsts, st);
			return -1;
		}
		if(st & IntCdone)
			break;
		if((i & 0x3ff) == 0 && i != 0)
			sleep(1);
	}
	if(i == 100000){
		if(verbose)
			print("sdcard: cmd %d timeout rint %ux status %ux\n",
			      idx, st, rd32(Status));
		return -1;
	}
	if(resp != nil)
		resp[0] = rd32(Resp0);
	if((flags & CmdDataExp) == 0)
		wr32(Rintsts, st);
	return 0;
}

/*
 * Read nbytes from the card into buf through the FIFO.  The command
 * itself is issued by the caller, so this only moves data.
 */
static int
readfifo(u8 *buf, int nbytes)
{
	int i, n, left, want, got;
	u32 st;

	left = nbytes / 4;
	for(n = 0; n < 100000; n++){
		st = rd32(Rintsts);
		if(st & (IntDataErr | IntDataTout)){
			if(verbose)
				print("sdcard: data error rint %ux\n", st);
			wr32(Rintsts, st);
			return -1;
		}
		if(st & (IntRxdr | IntDto)){
			wr32(Rintsts, st & (IntRxdr | IntDto));
			while(left > 0){
				got = (rd32(Status) >> FifoShift) & FifoMask;
				if(got == 0)
					break;
				want = got < left ? got : left;
				for(i = 0; i < want; i++){
					*(u32 *)buf = rd32(Data);
					buf += 4;
				}
				left -= want;
			}
		}
		if((st & IntDto) && left == 0)
			break;
		sleep(1);
	}
	if(left != 0){
		if(verbose)
			print("sdcard: short data, %d words left, rint %ux\n",
			      left, rd32(Rintsts));
		return -1;
	}
	wr32(Rintsts, 0xffffffff);
	return 0;
}

static int
readblock(u32 lba, u8 *buf)
{
	u32 st;
	int i;

	wr32(Blksiz, 512);
	wr32(Bytcnt, 512);
	wr32(Ctrl, rd32(Ctrl) | CtrlFifoReset);
	for(i = 0; i < 10000; i++){
		if((rd32(Ctrl) & CtrlFifoReset) == 0)
			break;
		sleep(1);
	}
	if(i == 10000)
		return -1;
	wr32(Rintsts, 0xffffffff);
	if(command(17, lba, CmdRespExp|CmdCheckCrc|CmdDataExp, nil) < 0)
		return -1;
	if(readfifo(buf, 512) < 0)
		return -1;
	st = rd32(Rintsts);
	wr32(Rintsts, st);
	return 0;
}

static void
hexdump(u8 *b, int n)
{
	int i;

	for(i = 0; i < n; i += 16)
		print("%04x: %02x %02x %02x %02x %02x %02x %02x %02x  "
		      "%02x %02x %02x %02x %02x %02x %02x %02x\n", i,
		      b[i], b[i+1], b[i+2], b[i+3], b[i+4], b[i+5], b[i+6], b[i+7],
		      b[i+8], b[i+9], b[i+10], b[i+11], b[i+12], b[i+13],
		      b[i+14], b[i+15]);
}

static void
state(u32 r)
{
	print("sdcard: card status %08ux state %ud%s\n", r, (r >> 9) & 0xf,
	      ((r >> 9) & 0xf) == 4 ? " (transfer)" : "");
}

static void
info(void)
{
	print("sdcard: ctrl %08ux pwren %ux clkdiv %ux clkena %ux ctype %ux\n",
	      rd32(Ctrl), rd32(Pwren), rd32(Clkdiv), rd32(Clkena),
	      rd32(Ctype));
	print("sdcard: cdetect %ux status %ux fifoth %ux uhs %ux\n",
	      rd32(Cdetect), rd32(Status), rd32(Fifoth), rd32(Uhs));
}

static void
probe(void)
{
	u32 r;
	u8 b[512];

	info();
	if(rd32(Cdetect) & 1)
		print("sdcard: no card detected\n");

	fifomode();
	if(command(13, 0, CmdRespExp|CmdCheckCrc, &r) < 0){
		print("sdcard: CMD13 failed\n");
		return;
	}
	state(r);

	if(readblock(0, b) < 0){
		print("sdcard: block 0 read failed\n");
		return;
	}
	hexdump(b, 64);
	if(b[510] == 0x55 && b[511] == 0xaa)
		print("sdcard: MBR signature OK\n");
	else
		print("sdcard: no MBR signature (%02x %02x)\n", b[510], b[511]);
}

void
main(int argc, char **argv)
{
	u8 b[512*16];
	u32 lba;
	int i, cnt;

	ARGBEGIN{
	case 'v':
		verbose++;
		break;
	}ARGEND;

	mmio = mapnode("mmc@fe2b0000", 0xfe2b0000);
	if(argc == 0 || strcmp(argv[0], "probe") == 0){
		probe();
		exits(0);
	}
	if(strcmp(argv[0], "info") == 0){
		info();
		exits(0);
	}
	if(strcmp(argv[0], "read") == 0){
		lba = 0;
		cnt = 1;
		if(argc > 1)
			lba = strtoul(argv[1], nil, 0);
		if(argc > 2)
			cnt = atoi(argv[2]);
		if(cnt < 1)
			cnt = 1;
		if(cnt > nelem(b)/512)
			cnt = nelem(b)/512;
		fifomode();
		for(i = 0; i < cnt; i++)
			if(readblock(lba + i, b + i*512) < 0)
				fail("sdcard: read %ud failed", lba + i);
		hexdump(b, cnt*512 < 256 ? cnt*512 : 256);
		print("sdcard: read %d blocks at %ud\n", cnt, lba);
		exits(0);
	}
	fprint(2, "usage: sdcard probe | info | read <lba> [n]\n");
	exits("usage");
}
