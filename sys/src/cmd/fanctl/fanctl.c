/* fanctl: thermal fan policy for the Orange Pi 3B.
 *
 * Samples /srv/tsadc/temp0 and drives /srv/pwm/ctl.  The curve is the
 * vendor v1.1 one:
 *
 *	trip (mC)   level   duty
 *	 50000        1      50/255
 *	 55000        2     100/255
 *	 60000        3     150/255
 *	 65000        4     200/255
 *	 70000        5     255/255
 *
 * Level changes up immediately and down only after the temperature falls
 * hyst below the trip that raised it, so a fan sitting on a threshold
 * does not oscillate.
 *
 *	/status	read	mode, temperature, level, duty, trips
 *	/ctl	write	auto <0|1>
 *			level <0..5>	manual level
 *			temp <millideg|off>	force a temperature (test)
 *			hyst <millideg>
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

enum {
	Fstatus,
	Fctl,

	Interval	= 2000,		/* ms between samples */
	DefaultHyst	= 2000,		/* mC */
	Ntrip		= 5,
};

static int trips[Ntrip] = { 50000, 55000, 60000, 65000, 70000 };
static int levels[Ntrip+1] = { 0, 50, 100, 150, 200, 255 };

typedef struct Fan Fan;
struct Fan {
	Lock	lk;
	int	mode;		/* 1 auto, 0 manual */
	int	level;		/* 0..5 */
	int	duty;		/* last percent written */
	int	temp;		/* last sample, mC */
	int	forced;		/* forced temperature, -1 live */
	int	hyst;
	int	ok;
	ulong	updates;
	char	err[64];
	char	lastctl[64];
	char	ctlbuf[64];
	int	ctllen;
};

static Fan fan;
static Srv fs;

static void
sayf(char *fmt, ...)
{
	va_list arg;
	char buf[160];
	int n;

	va_start(arg, fmt);
	n = vsnprint(buf, sizeof buf, fmt, arg);
	va_end(arg);
	if(n > 0)
		write(2, buf, n);
}

#define	TEMPPATH	"/mnt/fan.ts/temp0"
#define	PWMCTL		"/mnt/fan.pwm/ctl"

static int mounted;

static int
mountsrv(char *srv, char *mtpt)
{
	int fd;

	create(mtpt, OREAD, DMDIR|0755);
	fd = open(srv, ORDWR);
	if(fd < 0)
		return -1;
	if(mount(fd, -1, mtpt, MREPL, "") < 0){
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static int
readtemp(void)
{
	char buf[64], *f[4];
	int fd, n;

	fd = open(TEMPPATH, OREAD);
	if(fd < 0)
		return -1;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0)
		return -1;
	buf[n] = 0;
	if(tokenize(buf, f, nelem(f)) < 2)
		return -1;
	return atoi(f[1]);
}

static void
setfan(int pct)
{
	char buf[32];
	int fd, n;

	if(pct < 0)
		pct = 0;
	if(pct > 100)
		pct = 100;
	fd = open(PWMCTL, OWRITE);
	if(fd < 0)
		return;
	n = snprint(buf, sizeof buf, "fan %d\n", pct);
	write(fd, buf, n);
	close(fd);
}

static int
dutyfor(int level)
{
	if(level < 0)
		level = 0;
	if(level > Ntrip)
		level = Ntrip;
	return levels[level] * 100 / 255;
}

static int
levelfor(int temp, int cur, int hyst)
{
	int i, want;

	want = 0;
	for(i = 0; i < Ntrip; i++)
		if(temp >= trips[i])
			want = i + 1;
	if(want > cur)
		return want;
	while(cur > 0 && temp <= trips[cur-1] - hyst)
		cur--;
	return cur;
}

static void
sampler(void)
{
	int temp, live, level, pct, changed, first = 1;

	for(;;){
		if(!mounted){
			mounted = mountsrv("/srv/tsadc", "/mnt/fan.ts") == 0 &&
			          mountsrv("/srv/pwm", "/mnt/fan.pwm") == 0;
			if(mounted)
				sayf("fanctl: services mounted\n");
		}
		live = readtemp();
		changed = 0;
		lock(&fan.lk);
		fan.updates++;
		if(live >= 0){
			fan.temp = live;
			fan.ok = 1;
			fan.err[0] = 0;
		}else{
			fan.ok = 0;
			snprint(fan.err, sizeof fan.err, "temp read failed");
		}
		temp = fan.forced >= 0 ? fan.forced : live;
		if(temp >= 0 && fan.mode){
			level = levelfor(temp, fan.level, fan.hyst);
			if(level != fan.level){
				fan.level = level;
				changed = 1;
			}
			pct = dutyfor(fan.level);
			if(pct != fan.duty){
				fan.duty = pct;
				changed = 1;
			}
		}
		level = fan.level;
		pct = fan.duty;
		unlock(&fan.lk);
		if((changed || first) && temp >= 0){
			first = 0;
			setfan(pct);
			sayf("fanctl: temp %d level %d duty %d\n",
				temp, level, pct);
		}
		sleep(Interval);
	}
}

static void
statusread(Req *r)
{
	char buf[512];
	int n;

	lock(&fan.lk);
	n = snprint(buf, sizeof buf,
		"mode %s\n"
		"temp %d\n"
		"forced %s\n"
		"level %d\n"
		"duty %d\n"
		"hyst %d\n"
		"trips %d %d %d %d %d\n"
		"levels %d %d %d %d %d %d\n"
		"updates %lud\n"
		"error %s\n"
		"lastctl %s\n",
		fan.mode ? "auto" : "manual",
		fan.temp,
		fan.forced >= 0 ? "yes" : "off",
		fan.level,
		fan.duty,
		fan.hyst,
		trips[0], trips[1], trips[2], trips[3], trips[4],
		levels[0], levels[1], levels[2], levels[3], levels[4], levels[5],
		fan.updates,
		fan.err[0] ? fan.err : "-",
		fan.lastctl);
	unlock(&fan.lk);
	readstr(r, buf);
	respond(r, nil);
}

static void
ctlwrite(Req *r)
{
	char *s, *f[4];
	char buf[64];
	int i, n, nf, complete, ok, val, mode;

	n = r->ifcall.count;
	if(n >= (int)sizeof buf)
		n = sizeof buf - 1;
	memmove(buf, r->ifcall.data, n);
	buf[n] = 0;

	complete = 0;
	ok = 0;
	lock(&fan.lk);
	if(n > 0 && fan.ctllen + n < (int)sizeof fan.ctlbuf){
		memmove(fan.ctlbuf + fan.ctllen, buf, n);
		fan.ctllen += n;
		fan.ctlbuf[fan.ctllen] = 0;
		for(i = 0; i < fan.ctllen; i++)
			if(fan.ctlbuf[i] == '\n')
				break;
		if(i < fan.ctllen){
			fan.ctlbuf[i] = 0;
			complete = 1;
			snprint(fan.lastctl, sizeof fan.lastctl, "%s", fan.ctlbuf);
			s = fan.ctlbuf;
			nf = tokenize(s, f, nelem(f));
			if(nf == 2 && strcmp(f[0], "auto") == 0){
				fan.mode = atoi(f[1]) != 0;
				ok = 1;
			}else if(nf == 2 && strcmp(f[0], "level") == 0){
				val = atoi(f[1]);
				if(val >= 0 && val <= Ntrip){
					fan.mode = 0;
					fan.level = val;
					fan.duty = dutyfor(val);
					ok = 1;
				}
			}else if(nf == 2 && strcmp(f[0], "temp") == 0){
				if(strcmp(f[1], "off") == 0)
					fan.forced = -1;
				else
					fan.forced = atoi(f[1]);
				ok = 1;
			}else if(nf == 2 && strcmp(f[0], "hyst") == 0){
				val = atoi(f[1]);
				if(val >= 0 && val <= 20000){
					fan.hyst = val;
					ok = 1;
				}
			}
			fan.ctllen = 0;
			fan.ctlbuf[0] = 0;
		}
	}
	val = fan.duty;
	mode = fan.mode;
	unlock(&fan.lk);

	if(complete && !ok){
		respond(r, "ctl: auto/level/temp/hyst");
		return;
	}
	if(ok && mode == 0)
		setfan(val);
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

static void
fsread(Req *r)
{
	switch((uintptr)r->fid->file->aux){
	case Fstatus:
		statusread(r);
		break;
	default:
		respond(r, "read not permitted");
		break;
	}
}

static void
fswrite(Req *r)
{
	switch((uintptr)r->fid->file->aux){
	case Fctl:
		ctlwrite(r);
		break;
	default:
		respond(r, "write not permitted");
		break;
	}
}

void
main(int argc, char **argv)
{
	USED(argc);
	USED(argv);
	fan.mode = 1;
	fan.level = 0;
	fan.duty = -1;
	fan.forced = -1;
	fan.hyst = DefaultHyst;

	fs.tree = alloctree(nil, nil, DMDIR|0777, nil);
	if(fs.tree == nil)
		sysfatal("alloctree: %r");
	createfile(fs.tree->root, "status", nil, 0444, (void*)Fstatus);
	createfile(fs.tree->root, "ctl", nil, 0222, (void*)Fctl);
	fs.read = fsread;
	fs.write = fswrite;

	/*
	 * postmountsrv forks with RFPROC|RFMEM, so the serving process and
	 * this one share the policy state; the sampler runs here.
	 */
	postmountsrv(&fs, "fan", nil, MREPL|MCREATE);
	mounted = mountsrv("/srv/tsadc", "/mnt/fan.ts") == 0 &&
	          mountsrv("/srv/pwm", "/mnt/fan.pwm") == 0;
	if(!mounted)
		sayf("fanctl: mount failed, retrying\n");
	sampler();
	exits(nil);
}
