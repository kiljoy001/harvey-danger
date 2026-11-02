/*
 * provision: copy a prepared ext2 metadata blob into an eMMC partition.
 *
 * The blob (see util/arm64/mkmmcdata.py) starts with
 *
 *	magic[8] "O9MMCD01"
 *	u32 count
 *	u32 reserved
 *	count * { u32 bloboff; u32 blk; u32 nblocks; u32 reserved }
 *
 * blk is a block index inside the partition, because emmcfs exposes
 * /partN with its own zero base.
 *
 * followed by the non-zero blocks of a filesystem image.  A 200 GiB
 * ext2 has only about 13 MiB of such blocks, so this is how a large
 * partition gets formatted without shipping 200 GiB over TFTP.
 *
 *	provision /boot/datameta.bin /mnt/emmc/part2
 */
#include <u.h>
#include <libc.h>

enum {
	Block	= 4096,
	Maxrun	= 64,		/* blocks per run, from the packing */
};

static int
pwriteall(int fd, void *buf, long n, vlong off)
{
	char *p = buf;
	long m;

	while(n > 0){
		m = pwrite(fd, p, n, off);
		if(m <= 0)
			return -1;
		p += m;
		off += m;
		n -= m;
	}
	return 0;
}

static u32
get32(u8 *p)
{
	return p[0] | p[1]<<8 | p[2]<<16 | (u32)p[3]<<24;
}

void
main(int argc, char **argv)
{
	u8 head[16], *tab, *buf;
	u32 n, i, off, blk, count;
	int bfd, dfd;

	if(argc != 3){
		fprint(2, "usage: %s blob partition\n", argv0);
		exits("usage");
	}
	bfd = open(argv[1], OREAD);
	if(bfd < 0)
		sysfatal("%s: %r", argv[1]);
	if(readn(bfd, head, sizeof head) != sizeof head)
		sysfatal("%s: short header", argv[1]);
	if(memcmp(head, "O9MMCD01", 8) != 0)
		sysfatal("%s: bad magic", argv[1]);
	n = get32(head + 8);
	if(n == 0 || n > 1000000)
		sysfatal("%s: bad run count %ud", argv[1], n);
	tab = malloc(n * 16);
	if(tab == nil)
		sysfatal("out of memory");
	if(readn(bfd, tab, n * 16) != n * 16)
		sysfatal("%s: short table", argv[1]);

	dfd = open(argv[2], ORDWR);
	if(dfd < 0)
		sysfatal("%s: %r", argv[2]);

	/*
	 * EXT2_SUPER_MAGIC at 1024+56 and s_inode_size at 1024+88: skip
	 * only if the partition already holds a filesystem in the layout
	 * this blob creates, so a changed blob still gets applied.
	 */
	{
		u8 sb[2];

		if(pread(dfd, sb, sizeof sb, 1024+56) == 2 &&
		   sb[0] == 0x53 && sb[1] == 0xef &&
		   pread(dfd, sb, sizeof sb, 1024+88) == 2 &&
		   sb[0] == 128 && sb[1] == 0){
			print("provision: %s already formatted\n", argv[2]);
			exits(nil);
		}
	}

	buf = malloc(Maxrun * Block);
	if(buf == nil)
		sysfatal("out of memory");

	print("provision: %ud runs into %s\n", n, argv[2]);
	for(i = 0; i < n; i++){
		off = get32(tab + i*16);
		blk = get32(tab + i*16 + 4);
		count = get32(tab + i*16 + 8);
		if(count == 0 || count > Maxrun)
			sysfatal("run %ud: bad block count %ud", i, count);
		USED(off);
		/* runs are packed in ascending LBA order: read straight through */
		if(readn(bfd, buf, count*Block) != count*Block)
			sysfatal("run %ud: short read", i);
		if(pwriteall(dfd, buf, count*Block, (vlong)blk*Block) < 0)
			sysfatal("run %ud: write failed at block %ud", i, blk);
		if((i+1) % 200 == 0)
			print("provision: %ud/%ud\n", i+1, n);
	}
	close(dfd);
	/* ask emmcfs to flush the card write cache */
	{
		char *slash, path[128];
		int ffd;

		snprint(path, sizeof path, "%s", argv[2]);
		if((slash = strrchr(path, '/')) != nil){
			*slash = 0;
			snprint(path + strlen(path), sizeof path - strlen(path),
				"/flush");
			ffd = open(path, OWRITE);
			if(ffd >= 0){
				write(ffd, "f", 1);
				close(ffd);
			}
		}
	}
	close(bfd);
	print("provision: done, %ud runs\n", n);
	exits(nil);
}
