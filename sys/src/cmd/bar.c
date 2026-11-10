#include <u.h>
#include <libc.h>
#include <bio.h>
#include <draw.h>
#include <mouse.h>
#include <thread.h>
#include <plumb.h>
#include <tos.h>

#define MAX(a,b) ((a)>=(b)?(a):(b))
#define MIN(a,b) ((a)<=(b)?(a):(b))

enum {
	Off = 6,
	DefDecay = 8,
};

static int wctl = -1;
static int decay_sec = DefDecay;

static char noticemsg[256];
static int notice_active = 0;
static uvlong notice_expiry = 0;

static char sys_status[64] = "SYS:RDY";
static char idle_tag[64] = "// ASPE·OS // IDLE";

static void setnotice(char *msg);
static void setstatus(char *st);

/* Hit-test rectangles for interactive pods */
static Rectangle r_pod0, r_pod1, r_pod2;

static Image *cback;
static Image *cborder;
static Image *crail;
static Image *cbullet;
static Image *cnoticehdr;
static Image *cnoticemsg;
static Image *cidle;
static Image *csys;
static Image *cclock;

static Font *fmain;
static Font *fclock;

static uvlong
nanosec(void)
{
	return (uvlong)nsec();
}

static void
synctime(void)
{
	static int synced = 0;
	int fd, tfd, n;
	char buf[32];

	if(synced)
		return;
	fd = open("/n/host/time", OREAD);
	if(fd < 0)
		fd = open("/vps/host/time", OREAD);
	if(fd < 0)
		fd = open("/n/host/host/time", OREAD);
	if(fd >= 0){
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if(n > 0){
			buf[n] = 0;
			tfd = open("/dev/time", OWRITE);
			if(tfd >= 0){
				write(tfd, buf, strlen(buf));
				close(tfd);
				synced = 1;
			}
		}
	}
}

static void
readclock(char *buf, int max)
{
	int fd, n;

	buf[0] = 0;
	fd = open("/n/host/clock", OREAD);
	if(fd < 0)
		fd = open("/vps/host/clock", OREAD);
	if(fd < 0)
		fd = open("/n/host/host/clock", OREAD);
	if(fd >= 0){
		n = read(fd, buf, max - 1);
		close(fd);
		if(n > 0){
			buf[n] = 0;
			while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
				buf[--n] = 0;
			if(buf[0] != '\0')
				return;
		}
	}

	Tm *tm = localtime(time(nil));
	if(tm != nil)
		snprint(buf, max, "%02d:%02d:%02d", tm->hour, tm->min, tm->sec);
	else
		snprint(buf, max, "--:--:--");
}

static void
redraw(void)
{
	Rectangle r;
	char clockstr[64];
	int curx, cy, fh, sqsz, sqy;
	int w0, w1, w2, left_w, right_w;

	synctime();
	readclock(clockstr, sizeof(clockstr));

	r = screen->r;

	/* Background: Rio deep sea teal matching welcome window header */
	draw(screen, r, cback, nil, ZP);

	/* Top and bottom hairline accents */
	draw(screen, Rect(r.min.x, r.min.y, r.max.x, r.min.y + 1), cborder, nil, ZP);
	draw(screen, Rect(r.min.x, r.max.y - 1, r.max.x, r.max.y), cborder, nil, ZP);

	fh = fmain->height;
	cy = r.min.y + (Dy(r) - fh) / 2;
	sqsz = 6;
	sqy = cy + (fh - sqsz) / 2;

	/* Calculate robust widths for status and clock pods */
	int sw = stringwidth(fmain, sys_status);
	if(sw <= 0)
		sw = strlen(sys_status) * 8;
	int cw = stringwidth(fclock, clockstr);
	if(cw <= 0)
		cw = strlen(clockstr) * 8;
	if(cw <= 0)
		cw = 64;

	int b1 = stringwidth(fmain, "+--[ ");
	if(b1 <= 0) b1 = 40;
	int b2 = stringwidth(fmain, " ]--");
	if(b2 <= 0) b2 = 32;
	int b3 = stringwidth(fmain, " ]-+");
	if(b3 <= 0) b3 = 32;

	w1 = b1 + sw + b2;
	w2 = b1 + cw + b3;
	right_w = w1 + w2;

	/* Calculate width of left pod */
	if(notice_active)
		w0 = stringwidth(fmain, "+-[  NOTICE: ") + stringwidth(fmain, noticemsg) + stringwidth(fmain, " ]--");
	else
		w0 = stringwidth(fmain, "+-[ ") + stringwidth(fmain, idle_tag) + stringwidth(fmain, " ]--");
	left_w = w0;

	/* --- Pod 0: Notice / Idle (Left-aligned) --- */
	curx = r.min.x + Off;
	r_pod0.min = Pt(curx, r.min.y);

	string(screen, Pt(curx, cy), crail, ZP, fmain, "+-[ ");
	curx += stringwidth(fmain, "+-[ ");

	if(notice_active){
		/* Bullet square ■ */
		draw(screen, Rect(curx, sqy, curx + sqsz, sqy + sqsz), cbullet, nil, ZP);
		curx += sqsz + 4;

		/* Header: "NOTICE: " */
		string(screen, Pt(curx, cy), cnoticehdr, ZP, fmain, "NOTICE: ");
		curx += stringwidth(fmain, "NOTICE: ");

		/* Message text */
		string(screen, Pt(curx, cy), cnoticemsg, ZP, fmain, noticemsg);
		curx += stringwidth(fmain, noticemsg);
	}else{
		/* Idle tag */
		string(screen, Pt(curx, cy), cidle, ZP, fmain, idle_tag);
		curx += stringwidth(fmain, idle_tag);
	}

	string(screen, Pt(curx, cy), crail, ZP, fmain, " ]--");
	curx += stringwidth(fmain, " ]--");
	r_pod0.max = Pt(curx, r.max.y);

	/* --- Connecting Trace Rail across middle --- */
	int rx = r.max.x - (right_w + Off);
	if(rx > curx + 4){
		int midy = r.min.y + Dy(r) / 2;
		draw(screen, Rect(curx, midy, rx, midy + 1), crail, nil, ZP);
		curx = rx;
	}else{
		curx = curx + 4;
	}

	/* --- Pod 1: Sys Status --- */
	r_pod1.min = Pt(curx, r.min.y);

	string(screen, Pt(curx, cy), crail, ZP, fmain, "+--[ ");
	curx += stringwidth(fmain, "+--[ ");

	string(screen, Pt(curx, cy), csys, ZP, fmain, sys_status);
	curx += stringwidth(fmain, sys_status);

	string(screen, Pt(curx, cy), crail, ZP, fmain, " ]--");
	curx += stringwidth(fmain, " ]--");
	r_pod1.max = Pt(curx, r.max.y);

	/* --- Pod 2: Clock (Real-time browser host) --- */
	r_pod2.min = Pt(curx, r.min.y);

	string(screen, Pt(curx, cy), crail, ZP, fmain, "+--[ ");
	curx += stringwidth(fmain, "+--[ ");

	string(screen, Pt(curx, cy), cclock, ZP, fclock, clockstr);
	curx += stringwidth(fclock, clockstr);

	string(screen, Pt(curx, cy), crail, ZP, fmain, " ]-+");
	curx += stringwidth(fmain, " ]-+");
	r_pod2.max = Pt(curx, r.max.y);

	flushimage(display, 1);
}

static void
checkhostnotice(void)
{
	int fd, n;
	char buf[256];

	fd = open("/n/host/notice", OREAD);
	if(fd < 0)
		fd = open("/vps/host/notice", OREAD);
	if(fd < 0)
		fd = open("/n/host/host/notice", OREAD);
	if(fd >= 0){
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if(n > 0){
			buf[n] = 0;
			while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
				buf[--n] = 0;
			if(buf[0] != '\0')
				setnotice(buf);
			remove("/n/host/notice");
			remove("/vps/host/notice");
			remove("/n/host/host/notice");
		}
	}
}

static void
timerproc(void *c)
{
	Channel *chan = c;
	int tick = 1;
	threadsetname("timer");
	for(;;){
		sleep(500);
		checkhostnotice();
		send(chan, &tick);
	}
}

static void
plumbproc(void *c)
{
	int pfd = -1;
	Plumbmsg *m;
	char *s;

	threadsetname("plumb");
	for(;;){
		if(pfd < 0){
			pfd = plumbopen("notice", OREAD);
			if(pfd < 0){
				sleep(1000);
				continue;
			}
		}
		m = plumbrecv(pfd);
		if(m == nil){
			close(pfd);
			pfd = -1;
			sleep(1000);
			continue;
		}
		if(m->data != nil && m->ndata > 0){
			s = malloc(m->ndata + 1);
			if(s != nil){
				memmove(s, m->data, m->ndata);
				s[m->ndata] = 0;
				while(m->ndata > 0 && (s[m->ndata-1] == '\n' || s[m->ndata-1] == '\r'))
					s[--m->ndata] = 0;
				sendp(c, s);
			}
		}
		plumbfree(m);
	}
}

static void
setnotice(char *msg)
{
	if(msg == nil || msg[0] == '\0'){
		notice_active = 0;
		return;
	}
	snprint(noticemsg, sizeof(noticemsg), "%s", msg);
	notice_active = 1;
	notice_expiry = nanosec() + (uvlong)decay_sec * 1000000000ULL;
}

static void
setstatus(char *st)
{
	if(st != nil && st[0] != '\0')
		snprint(sys_status, sizeof(sys_status), "%s", st);
}

static void
processmsg(char *s)
{
	if(s == nil)
		return;

	if(strncmp(s, "status ", 7) == 0){
		setstatus(s + 7);
	}else if(strncmp(s, "idle ", 5) == 0){
		snprint(idle_tag, sizeof(idle_tag), "%s", s + 5);
	}else if(strcmp(s, "clear") == 0){
		notice_active = 0;
	}else if(strncmp(s, "notice ", 7) == 0){
		setnotice(s + 7);
	}else{
		setnotice(s);
	}
}

static void
clicked(Point p, int buttons)
{
	if((buttons & 1) == 0)
		return;

	if(ptinrect(p, r_pod0)){
		/* Dismiss notice immediately on click */
		if(notice_active){
			notice_active = 0;
			redraw();
		}
	}
}

static void
usage(void)
{
	fprint(2, "usage: %s [-t decay_sec] [-s status] [-f mainfont] [-c clockfont]\n", argv0);
	threadexitsall("usage");
}

static Font*
loadfont(char *path)
{
	Font *f = nil;
	if(path != nil && path[0] != '\0')
		f = openfont(display, path);
	return f;
}

void
threadmain(int argc, char **argv)
{
	Mousectl *mctl = nil;
	Mouse m;
	char *mainfont_arg = nil;
	char *clockfont_arg = nil;
	char *s;
	int oldbuttons;
	int timerval = 0;
	uvlong now;

	enum {
		Emouse,
		Eresize,
		Eplumb,
		Etimer,
		Eend,
	};
	Alt a[] = {
		[Emouse] = { nil, &m, CHANNOP },
		[Eresize] = { nil, nil, CHANNOP },
		[Eplumb] = { nil, &s, CHANRCV },
		[Etimer] = { nil, &timerval, CHANRCV },
		[Eend] = { nil, nil, CHANEND },
	};

	ARGBEGIN{
	case 't':
		decay_sec = atoi(EARGF(usage()));
		if(decay_sec <= 0)
			decay_sec = DefDecay;
		break;
	case 's':
		setstatus(EARGF(usage()));
		break;
	case 'f':
		mainfont_arg = EARGF(usage());
		break;
	case 'c':
		clockfont_arg = EARGF(usage());
		break;
	case 'p':
		EARGF(usage());
		break;
	default:
		usage();
	}ARGEND

	if(initdraw(nil, nil, "bar") < 0)
		sysfatal("initdraw: %r");

	/* Load fonts: use display default font for rock-solid stability and zero subfont latency */
	fmain = display->defaultfont;
	if(mainfont_arg)
		fmain = loadfont(mainfont_arg);
	if(fmain == nil)
		fmain = display->defaultfont;

	fclock = fmain;
	if(clockfont_arg)
		fclock = loadfont(clockfont_arg);
	if(fclock == nil)
		fclock = fmain;

	mctl = initmouse(nil, screen);
	if(mctl != nil){
		a[Emouse].c = mctl->c;
		a[Emouse].op = CHANRCV;
		a[Eresize].c = mctl->resizec;
		a[Eresize].op = CHANRCV;
	}

	/* Palette: Rio deep sea teal & luminous aqua accents - fits the desktop perfectly */
	cback      = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x185860FF); /* Signature Rio deep teal */
	cborder    = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x48A0A8FF); /* Crisp aqua border */
	crail      = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x78D0D8FF); /* Luminous brackets & rails */
	cbullet    = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0xFFAA00FF); /* Safety amber bullet */
	cnoticehdr = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0xFFFFFFFF); /* Clean white "NOTICE:" */
	cnoticemsg = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0xF4FCFFFF); /* Crisp message text */
	cidle      = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x98CCD0FF); /* Calm aqua idle text */
	csys       = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x55FFFFFF); /* Bright aqua "SYS:RDY" */
	cclock     = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0xFFFFFFFF); /* High-contrast white clock */

	if(cback == nil || cborder == nil || crail == nil || cbullet == nil ||
	   cnoticehdr == nil || cnoticemsg == nil || cidle == nil || csys == nil || cclock == nil){
		fprint(2, "bar: allocimage failed: %r\n");
		sysfatal("allocimage: %r");
	}

	a[Eplumb].c = chancreate(sizeof(char*), 0);
	a[Etimer].c = chancreate(sizeof(int), 0);

	redraw();

	proccreate(timerproc, a[Etimer].c, 4096);
	proccreate(plumbproc, a[Eplumb].c, 8192);

	m.buttons = 0;
	for(;;){
		oldbuttons = m.buttons;

		switch(alt(a)){
		case -1:
			sleep(100);
			break;
		case Emouse:
			if(m.buttons != oldbuttons && m.buttons != 0)
				clicked(m.xy, m.buttons);
			break;

		case Eresize:
			if(getwindow(display, Refnone) < 0)
				threadexitsall(nil);
			redraw();
			break;

		case Eplumb:
			processmsg(s);
			free(s);
			redraw();
			break;

		case Etimer:
			now = nanosec();
			if(notice_active && now >= notice_expiry){
				notice_active = 0;
			}
			redraw();
			break;
		}
	}
}
