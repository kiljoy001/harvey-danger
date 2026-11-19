/*
 * disvm - run a Dis module on Harvey.
 *
 * One Harvey process hosts the Inferno VM (libinterp).  There is no
 * Inferno kernel below it: the Sys module's k* calls are in k9.c and go
 * straight to Harvey's libc, so a Dis program opens Harvey paths.
 *
 *	disvm /dis/hello.dis
 */
#include "dat.h"
#include "fns.h"
#include "error.h"
#include "interp.h"

#undef	mallocz

/* emu globals that its runtime expects (usually defined in emu/port/main.c) */
Procs	procs;
char	*eve;
char	*hosttype;
char	*cputype;
int	dflag;
int	vflag;
int	bflag = 1;
int	sflag;
int	qflag;
int	Xsize = 640;
int	Ysize = 480;
ulong	displaychan;
char	*tkfont;
int	tkstylus;
int	xtblbit;

Proc	**Xup;
static Proc *upstore;

static int
opencons(void)
{
	int n;

	n = open("/dev/cons", OREAD);
	if(n < 0)
		return -1;
	dup(n, 0);
	close(n);
	n = open("/dev/cons", OWRITE);
	if(n < 0)
		return -1;
	dup(n, 1);
	dup(n, 2);
	close(n);
	return 0;
}

void
main(int argc, char **argv)
{
	Proc *p;
	Osenv *e;

	if(argc < 2 || argv[1][0] == 0){
		fprint(2, "usage: disvm file.dis [args...]\n");
		exits("usage");
	}

	Xup = &upstore;

	p = newproc();
	if(p == nil)
		exits("no proc");
	/* newproc() owns the Osenv (embedded defenv); do not replace it,
	 * the VM frees it with its own allocator later. */
	e = p->env;
	e->fgrp = newfgrp(nil);
	e->pgrp = newpgrp();
	e->egrp = newegrp();
	if(e->fgrp == nil || e->pgrp == nil || e->egrp == nil)
		exits("no fd group");
	up = p;
	up->pid = up->sigid = getpid();
	strncpy(up->text, "disvm", sizeof up->text - 1);

	if(opencons() < 0)
		fprint(2, "disvm: no console: %r\n");

	/* disinit: load, link, schedule and run the module */
	disinit(argv[1]);
	exits(nil);
}
