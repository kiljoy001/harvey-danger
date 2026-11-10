/*
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */

#include <u.h>
#include <libc.h>
#include <regexp.h>
#include <thread.h>
#include <plumb.h>
#include <auth.h>
#include <fcall.h>
#include "plumber.h"

char	*plumbfile;
char *user;
char *home;
char *progname;
Ruleset **rules;
int	printerrors=1;
jmp_buf	parsejmp;
char	*lasterror;
int mainstacksize = 20*1024;

void
makeports(Ruleset *rules[])
{
	int i;

	addport("notice");
	addport("edit");
	addport("vdir");
	if(rules != nil){
		for(i=0; rules[i]; i++)
			addport(rules[i]->port);
	}
}

void
mainproc(void *v)
{
	Channel *c;

	c = v;
	printerrors = 0;
	makeports(rules);
	startfsys();
	sendp(c, nil);
}

void
threadmain(int argc, char *argv[])
{
	char buf[512];
	int fd, nrules, loaded;
	Dir *d;
	Channel *c;

	progname = "plumber";

	ARGBEGIN{
	case 'p':
		plumbfile = ARGF();
		break;
	}ARGEND

	user = getenv("user");
	home = getenv("home");
	if(user==nil || home==nil)
		error("can't initialize $user or $home: %r");

	rules = emalloc(sizeof(Ruleset*));
	rules[0] = nil;
	nrules = 0;
	loaded = 0;

	/* 1. Load modular drop-in rules directory /lib/plumb/rules.d */
	loaded += appendrulesdir("/lib/plumb/rules.d", &rules, &nrules);

	/* 2. If plumbfile specified, append it (or scan if it is a directory) */
	if(plumbfile != nil){
		d = dirstat(plumbfile);
		if(d != nil && (d->mode & DMDIR)){
			loaded += appendrulesdir(plumbfile, &rules, &nrules);
			free(d);
		}else{
			if(d != nil)
				free(d);
			fd = open(plumbfile, OREAD);
			if(fd >= 0){
				if(appendrulesfile(plumbfile, fd, &rules, &nrules) == 0)
					loaded++;
				close(fd);
			}
		}
	}

	/* 3. If nothing loaded yet, try $home/lib/plumbing or /lib/aspera/plumbing */
	if(loaded == 0){
		sprint(buf, "%s/lib/plumbing", home);
		fd = open(buf, OREAD);
		if(fd >= 0){
			appendrulesfile(buf, fd, &rules, &nrules);
			close(fd);
		}else{
			fd = open("/lib/aspera/plumbing", OREAD);
			if(fd >= 0){
				appendrulesfile("/lib/aspera/plumbing", fd, &rules, &nrules);
				close(fd);
			}
		}
	}

	/* 4. Append local user overrides if present */
	fd = open("/lib/plumb/rules.local", OREAD);
	if(fd >= 0){
		appendrulesfile("/lib/plumb/rules.local", fd, &rules, &nrules);
		close(fd);
	}
	sprint(buf, "%s/lib/plumbing.local", home);
	fd = open(buf, OREAD);
	if(fd >= 0){
		appendrulesfile(buf, fd, &rules, &nrules);
		close(fd);
	}

	/*
	 * Start all processes and threads from other proc
	 * so we (main pid) can return to user.
	 */
	c = chancreate(sizeof(void*), 0);
	proccreate(mainproc, c, 8192);
	recvp(c);
	chanfree(c);
	threadexits(nil);
}

void
error(char *fmt, ...)
{
	char buf[512];
	va_list args;

	va_start(args, fmt);
	vseprint(buf, buf+sizeof buf, fmt, args);
	va_end(args);

	fprint(2, "%s: %s\n", progname, buf);
	threadexitsall("error");
}

void
parseerror(char *fmt, ...)
{
	char buf[512];
	va_list args;

	va_start(args, fmt);
	vseprint(buf, buf+sizeof buf, fmt, args);
	va_end(args);

	if(printerrors){
		printinputstack();
		fprint(2, "%s\n", buf);
	}
	do; while(popinput());
	lasterror = estrdup(buf);
	longjmp(parsejmp, 1);
}

void*
emalloc(i32 n)
{
	void *p;

	p = malloc(n);
	if(p == nil)
		error("malloc failed: %r");
	memset(p, 0, n);
	return p;
}

void*
erealloc(void *p, i32 n)
{
	p = realloc(p, n);
	if(p == nil)
		error("realloc failed: %r");
	return p;
}

char*
estrdup(char *s)
{
	char *t;

	t = strdup(s);
	if(t == nil)
		error("estrdup failed: %r");
	setmalloctag(t, getcallerpc());
	return t;
}
