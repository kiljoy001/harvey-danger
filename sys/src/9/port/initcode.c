/*
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */

/*
 * IMPORTANT!  DO NOT ADD LIBRARY CALLS TO THIS FILE.
 * The entire text image must fit on one page
 * (and there's no data segment, so any read/write data must be on the stack).
 */

#include <u.h>
#include <libc.h>

char cons[] = "#c/cons";
char boot[] = "/boot/boot";
char dev[] = "/dev";
char c[] = "#c";
char e[] = "#e";
char ec[] = "#ec";
char s[] = "#s";
char srv[] = "/srv";
char env[] = "/env";
char v9p[] = "#9/0";
char nhost[] = "/n/host";
char ringp[] = "#~/host";
char root[] = "/";
char readme[] = "/README.md";
char ownerpath[] = "#c/hostowner";
char userpath[] = "#e/user";
char defaultowner[] = "glenda";
char gefsroot[] = "#e/gefsroot";
char gefsboot[] = "/boot/gefsinit";

static void
puts(int fd, const char *str)
{
	int len = 0;
	while(str[len])
		len++;
	write(fd, (void*)str, len);
}

void
startboot(char *argv0, char **argv)
{
	char buf[200];
	int pfd;
	int mounted;
	int ufd, ofd, n, i;
	char *owner;

	USED(argv0);

	open(cons, OREAD);
	open(cons, OWRITE);
	open(cons, OWRITE);
	bind(c, dev, MAFTER);
	bind(ec, env, MAFTER);
	bind(e, env, MCREATE | MAFTER);
	bind(s, srv, MREPL | MCREATE);
	owner = defaultowner;
	n = sizeof(defaultowner)-1;
	ufd = open(userpath, OREAD);
	if(ufd >= 0){
		i = read(ufd, buf, sizeof(buf)-1);
		close(ufd);
		if(i > 0 && i < 64){
			while(i > 0 && (buf[i-1] == '\n' || buf[i-1] == '\r'))
				i--;
			if(i > 0){
				owner = buf;
				n = i;
			}
		}
	}
	ofd = open(ownerpath, OWRITE);
	if(ofd >= 0){
		write(ofd, owner, n);
		close(ofd);
	}
	ufd = open(gefsroot, OREAD);
	if(ufd >= 0){
		i = read(ufd, buf, 1);
		close(ufd);
		if(i == 1 && buf[0] == '1'){
			puts(1, "init: starting embedded GEFS bootstrap\n");
			exec(gefsboot, argv);
			rerrstr(buf, sizeof buf);
			puts(2, "init: GEFS bootstrap exec failed: ");
			puts(2, buf);
			puts(2, "\n");
			_exits("gefsboot");
		}
	}

	mounted = 0;
	pfd = open(ringp, ORDWR);
	if(pfd >= 0){
		puts(1, "init: mounting #~/host to /...\n");
		if(mount(pfd, -1, root, MREPL, "", 'M') < 0){
			close(pfd);
			pfd = -1;
		}else
			mounted = 1;
	}
	if(pfd < 0){
		puts(1, "init: mounting #9/0 to /...\n");
		pfd = open(v9p, ORDWR);
	}
	if(pfd < 0){
		rerrstr(buf, sizeof buf);
		puts(2, "init: open #9/0 failed: ");
		puts(2, buf);
		puts(2, "\n");
	} else if(!mounted){
		if(mount(pfd, -1, root, MREPL, "", 'M') < 0){
			rerrstr(buf, sizeof buf);
			puts(2, "init: mount failed: ");
			puts(2, buf);
			puts(2, "\n");
		}
	}

	/* #9 is also the /n/host browser gateway; / stays independent. */
	{
		int hfd = open(v9p, ORDWR);
		if(hfd >= 0){
			if(mount(hfd, -1, nhost, MREPL, "", 'M') < 0)
				puts(1, "init: mount #9/0 at /n/host failed\n");
			else
				puts(1, "init: mounted #9/0 at /n/host\n");
			close(hfd);
		}
	}

	exec(boot, argv);

	rerrstr(buf, sizeof buf);
	buf[sizeof buf - 1] = '\0';
	_exits(buf);
}
