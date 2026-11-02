/*
 * mount9 - mount a 9P2000 file server reached over the network.
 *
 * The kernel's mount takes a file descriptor that speaks 9P; everything that
 * turns a service into a mount (srv, dial+mount, import) is a user-space
 * convention. This is the smallest useful one: dial, mount, exit. The mount
 * keeps the connection open after this process is gone.
 *
 *	mount9 tcp!192.168.2.2!5650 /n/dig
 *	ls /n/dig/q/newest
 */
#include <u.h>
#include <libc.h>

static void
usage(void)
{
	fprint(2, "usage: mount9 [-b] [-C] addr [mntpt]\n");
	fprint(2, "\taddr: net!host!port, e.g. tcp!192.168.2.2!5650\n");
	fprint(2, "\tmntpt defaults to /n/<host>\n");
	exits("usage");
}

/* tcp!host!port -> host */
static void
hostof(char *addr, char *buf, int n)
{
	char *p, *q;

	p = strchr(addr, '!');
	if(p == nil){
		snprint(buf, n, "%s", addr);
		return;
	}
	q = strchr(p+1, '!');
	if(q == nil)
		snprint(buf, n, "%s", p+1);
	else
		snprint(buf, n, "%.*s", (int)(q-(p+1)), p+1);
}

static int
mkmountpoint(char *mnt)
{
	int fd;

	if(access(mnt, AEXIST) >= 0)
		return 0;
	fd = create(mnt, OREAD, DMDIR|0777);
	if(fd < 0)
		return -1;
	close(fd);
	return 0;
}

void
main(int argc, char **argv)
{
	char host[128], mnt[256], *addr, *mntpt;
	int fd, flags;

	flags = MAFTER | MCACHE;
	ARGBEGIN{
	case 'b':
		flags &= ~MAFTER;
		flags |= MBEFORE;
		break;
	case 'C':
		flags &= ~MCACHE;
		break;
	default:
		usage();
	}ARGEND

	if(argc < 1 || argc > 2)
		usage();
	addr = argv[0];
	if(argc == 2){
		mntpt = argv[1];
	}else{
		hostof(addr, host, sizeof host);
		snprint(mnt, sizeof mnt, "/n/%s", host);
		mntpt = mnt;
	}

	if(mkmountpoint(mntpt) < 0)
		fprint(2, "mount9: cannot make %s: %r\n", mntpt);

	fd = dial(addr, nil, nil, nil);
	if(fd < 0){
		fprint(2, "mount9: dial %s: %r\n", addr);
		exits("dial");
	}
	if(mount(fd, -1, mntpt, flags, "", 'M') < 0){
		fprint(2, "mount9: mount %s on %s: %r\n", addr, mntpt);
		close(fd);
		exits("mount");
	}
	print("mounted %s on %s\n", addr, mntpt);
	exits(nil);
}
