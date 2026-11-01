/*
 * Exercise the keyroot 9P service: import a dp9ik key, read the sealed
 * blob, and run the client PAK initialisation.
 */
#include <u.h>
#include <libc.h>

static int
wrfile(char *path, void *buf, int n)
{
	int fd, r;

	fd = open(path, OWRITE);
	if(fd < 0)
		return -1;
	r = write(fd, buf, n);
	fprint(2, "keyroottest: write %s r=%d n=%d\n", path, r, n);
	if(r < 0){
		char e[ERRMAX];

		rerrstr(e, sizeof e);
		close(fd);
		werrstr("%s", e);
		return -1;
	}
	close(fd);
	return 0;
}

static int
rdfile(char *path, void *buf, int n)
{
	int fd, r;

	fd = open(path, OREAD);
	if(fd < 0)
		return -1;
	r = read(fd, buf, n);
	close(fd);
	return r;
}

void
main(void)
{
	u8 imp[16+28+32];
	u8 blob[4096];
	u8 tr[141+56];
	u8 out[4096];
	int n;

	memset(imp, 0, sizeof(imp));
	for(n = 0; n < 16; n++)
		imp[n] = n;
	strcpy((char*)imp+16, "testuser");
	strcpy((char*)imp+16+28, "dp9ik/test");

	if(wrfile("/mnt/keyroot/dp9ik/import", imp, sizeof(imp)) < 0){
		fprint(2, "keyroottest: import failed: %r\n");
		exits("import");
	}
	if((n = rdfile("/mnt/keyroot/ctl", out, sizeof(out)-1)) < 0){
		fprint(2, "keyroottest: ctl failed: %r\n");
		exits("ctl");
	}
	out[n] = 0;
	print("keyroot ctl: %s", out);

	n = rdfile("/mnt/keyroot/dp9ik/blob", blob, sizeof(blob));
	if(n <= 0){
		fprint(2, "keyroottest: blob read failed: %r\n");
		exits("blob read");
	}
	print("keyroot blob: %d bytes\n", n);

	memset(tr, 0, sizeof(tr));
	tr[0] = 19;			/* AuthPAK */
	memmove(tr+1, "auth", 4);
	memmove(tr+29, "test", 4);
	memmove(tr+77, "SRVCHAL1", 8);
	memmove(tr+85, "host", 4);
	memmove(tr+113, "glenda", 6);
	memmove(tr+141, "SERVERY0", 8);

	if(wrfile("/mnt/keyroot/dp9ik/init", tr, sizeof(tr)) < 0){
		fprint(2, "keyroottest: init write failed: %r\n");
		exits("init write");
	}
	n = rdfile("/mnt/keyroot/dp9ik/init", out, sizeof(out));
	if(n != 56){
		fprint(2, "keyroottest: init read %d: %r\n", n);
		exits("init read");
	}
	print("keyroot init: clientY %d bytes\n", n);

	{
		u8 bad[112+248];

		memset(bad, 0, sizeof(bad));
		if(wrfile("/mnt/keyroot/dp9ik/finish", bad, sizeof(bad)) < 0)
			print("keyroot finish rejected malformed AS reply\n");
		else
			print("keyroot finish unexpectedly accepted\n");
	}
	exits(nil);
}
