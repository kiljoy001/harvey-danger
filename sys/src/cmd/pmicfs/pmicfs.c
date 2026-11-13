/* pmicfs: RK809 PMIC service for the Orange Pi 3B.
 *
 * Level: L3 device driver over the i2cfs bus service (L2).  The RK809
 * sits at address 0x20 on i2c0 and owns the SoC rails, the RTC and the
 * 32 kHz clock outputs.
 *
 * The driver mounts /srv/i2c into its own namespace, probes each
 * controller for the PMIC, and serves:
 *
 *   /status   chip, rail voltages/enables, RTC and system state
 *   /regs     hex dump of registers 0x00..0xff
 *   /rtc      "YYYY-MM-DD HH:MM:SS" (BCD registers 0x00..0x05)
 *   /ctl      "r <reg> [n]"     read n registers
 *             "w <reg> <v>..."  write bytes starting at reg
 *             "on <rail>"       enable a rail
 *             "off <rail>"      disable a rail
 *
 * Nothing here knows about #P or MMIO; that belongs to i2cfs.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "../o9drvman/health.h"

enum {
	Pmicaddr = 0x20,
	Maxregs = 0x100,
	Maxread = 32,

	Syscfg1 = 0xf2,		/* hot-die, TSD and CLK32KOUT2 enable */
	Clk32kEn = 0x80,
	Idmsb = 0xed,
	Idlsb = 0xee,
	Intsts0 = 0xf8,
	Intsts2 = 0xfc,
	Onsource = 0xf5,
	Offsource = 0xf6,
	Syscfg3 = 0xf4,

	Fstatus = 1,
	Fregs,
	Frtc,
	Fctl,
	Fpwrkey,
};

typedef struct Rail Rail;
struct Rail {
	char *name;
	int kind;		/* 0 buck1/2, 1 buck3/4, 2 buck5, 3 ldo, 4 switch */
	u8 vsel;
	u8 enreg;
	u8 enbit;
	int minmv;
	int maxmv;		/* minmv == maxmv means fixed */
};

static Rail rails[] = {
	{"vdd_logic",      0, 0xbb, 0xb1, 0,  500, 1350},
	{"vdd_gpu",        0, 0xbe, 0xb1, 1,  500, 1350},
	{"vcc_ddr",        1, 0xc1, 0xb1, 2,    0,    0},
	{"vdd_npu",        1, 0xc4, 0xb1, 3,  500, 1350},
	{"vcc_1v8",        2, 0xde, 0xb4, 1, 1800, 1800},
	{"vdda0v9_image",  3, 0xcc, 0xb2, 0,  900,  900},
	{"vdda_0v9",       3, 0xce, 0xb2, 1,  900,  900},
	{"vdda0v9_pmu",    3, 0xd0, 0xb2, 2,  900,  900},
	{"vccio_acodec",   3, 0xd2, 0xb2, 3, 3300, 3300},
	{"vccio_sd",       3, 0xd4, 0xb3, 0, 1800, 3300},
	{"vcc3v3_pmu",     3, 0xd6, 0xb3, 1, 3300, 3300},
	{"vcca_1v8",       3, 0xd8, 0xb3, 2, 1800, 1800},
	{"vcca1v8_pmu",    3, 0xda, 0xb3, 3, 1800, 1800},
	{"vcca1v8_image",  3, 0xdc, 0xb4, 0, 1800, 1800},
	{"vcc_3v3",        4, 0,    0xb4, 2,    0,    0},
	{"vcc3v3_pcie",    4, 0,    0xb4, 3,    0,    0},
};

/* RK817/RK809 interrupt sources: 24 bits over registers 0xf8, 0xfa, 0xfc. */
static char *irqname[24] = {
	"pwrkey-fall", "pwrkey-rise", "pwrkey", "pwrkey-long",
	"hotdie", "rtc-alarm", "rtc-period", "vbat-low",
	"plug-in", "plug-out", "chg-term", "chg-time",
	"chg-ts", "usb-ov", "chg-in-clamp", "bat-dis-ilim",
	"gate-gpio", "ts-gpio", "codec-pd", "codec-po",
	"classd-mute", "classd-ocp", "bat-ovp", "chg-bat-hi",
};

static int ctlfd = -1, datafd = -1;
static int i2cbus = -1;
static QLock i2clk;
static char ctlresult[512];
static char ctlbuf[256];
static int ctllen;

static void
fail(char *msg)
{
	fprint(2, "pmicfs: %s\n", msg);
	_exits(msg);
}

/* ---- i2cfs client ---- */

static int
hexval(int c)
{
	if(c >= '0' && c <= '9')
		return c - '0';
	if(c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if(c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int
hexparse(char *s, u8 *buf, int max)
{
	int hi, lo, n;

	n = 0;
	for(;;){
		while(*s == ' ' || *s == '\n' || *s == '\t')
			s++;
		if(*s == 0 || n >= max)
			break;
		hi = hexval(s[0]);
		lo = s[1] ? hexval(s[1]) : -1;
		if(hi < 0 || lo < 0)
			break;
		buf[n++] = (hi << 4) | lo;
		s += 2;
	}
	return n;
}

static int
pmread(int reg, u8 *buf, int n)
{
	char cmd[64], data[256];
	int m, got;

	if(n <= 0 || n > Maxread)
		return -1;
	snprint(cmd, sizeof cmd, "r 0x%x 0x%x %d", Pmicaddr, reg, n);
	qlock(&i2clk);
	if(write(ctlfd, cmd, strlen(cmd)) != strlen(cmd)){
		snprint(ctlresult, sizeof ctlresult, "i2c read 0x%x: %r", reg);
		qunlock(&i2clk);
		return -1;
	}
	m = pread(datafd, data, sizeof data - 1, 0);
	qunlock(&i2clk);
	if(m < 0){
		snprint(ctlresult, sizeof ctlresult, "i2c data: %r");
		return -1;
	}
	data[m] = 0;
	got = hexparse(data, buf, n);
	return got == n ? 0 : -1;
}

static int
pmwrite(int reg, u8 *buf, int n)
{
	char cmd[256];
	int i, k;

	k = snprint(cmd, sizeof cmd, "w 0x%x 0x%x", Pmicaddr, reg);
	for(i = 0; i < n; i++)
		k += snprint(cmd + k, sizeof cmd - k, " 0x%x", buf[i]);
	qlock(&i2clk);
	if(write(ctlfd, cmd, strlen(cmd)) != strlen(cmd)){
		snprint(ctlresult, sizeof ctlresult, "i2c write 0x%x: %r", reg);
		qunlock(&i2clk);
		return -1;
	}
	qunlock(&i2clk);
	return 0;
}

static int
regread(int reg, u8 *val)
{
	return pmread(reg, val, 1);
}

/* ---- RK809 policy ---- */

static int
railmillivolt(Rail *r, u8 v)
{
	int sel;

	switch(r->kind){
	case 0:
	case 1:			/* buck1..4: 0.5-1.5 V in 12.5 mV, then 100 mV */
		sel = v & 0x7f;
		if(sel <= 80)
			return 500 + sel * 125 / 10;
		return 1600 + (sel - 81) * 100;
	case 2: {		/* buck5 table, millivolts by selector */
		static int tab[8] = {1500, 1800, 2000, 2200, 2800, 3000, 3300, 3600};
		return tab[v & 7];
	}
	case 3:			/* ldo: 0.6 V + 25 mV steps */
		return 600 + (v & 0x7f) * 25;
	default:
		return 0;
	}
}

static int
railenabled(Rail *r, u8 en)
{
	return (en >> r->enbit) & 1;
}

static void
railctl(char *name, int on)
{
	u8 en;
	int i;

	for(i = 0; i < nelem(rails); i++){
		Rail *r = &rails[i];

		if(strcmp(r->name, name) != 0)
			continue;
		if(r->kind == 4){
			snprint(ctlresult, sizeof ctlresult,
				"%s is a switch with no soft control", name);
			return;
		}
		if(regread(r->enreg, &en) < 0){
			snprint(ctlresult, sizeof ctlresult, "read %s: %r", name);
			return;
		}
		if(on)
			en |= 1 << r->enbit;
		else
			en &= ~(1 << r->enbit);
		/*
		 * RK809 enable registers use bits 7:4 as a write mask for
		 * the four enables in bits 3:0.  U-Boot leaves vdd_npu off
		 * precisely because a plain write is ignored, so pass all
		 * four mask bits with every change.
		 */
		en = (en & 0x0f) | 0xf0;
		if(pmwrite(r->enreg, &en, 1) < 0){
			snprint(ctlresult, sizeof ctlresult, "write %s: %r", name);
			return;
		}
		snprint(ctlresult, sizeof ctlresult, "%s %s", name,
			on ? "on" : "off");
		return;
	}
	snprint(ctlresult, sizeof ctlresult, "no rail %s", name);
}

/* Closest selector for a buck or ldo target, or -1. */
static int
railselector(Rail *r, int mv)
{
	int sel, best, bestd, d;

	best = -1;
	bestd = 1 << 30;
	for(sel = 0; sel < 128; sel++){
		d = railmillivolt(r, sel) - mv;
		if(d < 0)
			d = -d;
		if(d < bestd){
			bestd = d;
			best = sel;
		}
		if(d == 0)
			break;
	}
	return best;
}

static void
railset(char *name, int mv)
{
	u8 v;
	int i, sel;

	for(i = 0; i < nelem(rails); i++){
		Rail *r = &rails[i];

		if(strcmp(r->name, name) != 0)
			continue;
		if(r->kind == 4){
			snprint(ctlresult, sizeof ctlresult,
				"%s is a switch", name);
			return;
		}
		if(r->minmv == 0 && r->maxmv == 0){
			snprint(ctlresult, sizeof ctlresult,
				"%s is fixed by external feedback", name);
			return;
		}
		if(r->minmv == r->maxmv){
			snprint(ctlresult, sizeof ctlresult,
				"%s is fixed at %d mV", name, r->minmv);
			return;
		}
		if(mv < r->minmv || mv > r->maxmv){
			snprint(ctlresult, sizeof ctlresult,
				"%s range is %d..%d mV", name, r->minmv, r->maxmv);
			return;
		}
		sel = railselector(r, mv);
		if(sel < 0){
			snprint(ctlresult, sizeof ctlresult, "%s has no selector", name);
			return;
		}
		v = sel;
		if(pmwrite(r->vsel, &v, 1) < 0)
			return;
		snprint(ctlresult, sizeof ctlresult, "%s -> %d mV (vsel 0x%02x)",
			name, railmillivolt(r, v), v);
		return;
	}
	snprint(ctlresult, sizeof ctlresult, "no rail %s", name);
}

/* Append the names of latched interrupt sources; returns the count. */
static int
irqstr(char *buf, int max, int *len)
{
	u8 v;
	int i, b, n, any;

	n = *len;
	any = 0;
	for(i = 0; i < 3; i++){
		if(pmread(Intsts0 + 2*i, &v, 1) < 0)
			continue;
		for(b = 0; b < 8; b++){
			if(!(v & (1 << b)))
				continue;
			n += snprint(buf + n, max - n, "%s%s",
				any ? " " : "", irqname[i*8 + b]);
			any++;
		}
	}
	if(!any)
		n += snprint(buf + n, max - n, "none");
	*len = n;
	return any;
}

/* Power-key events live in the low four bits of INT_STS0. */
static char *pwrname[4] = { "fall", "rise", "press", "long" };

static int
pwrkeystr(char *buf, int max, int *len, int *anyp)
{
	u8 v;
	int b, n, any;

	n = *len;
	any = 0;
	if(regread(Intsts0, &v) < 0)
		return -1;
	for(b = 0; b < 4; b++)
		if(v & (1 << b)){
			n += snprint(buf + n, max - n, "%s%s", any ? " " : "",
				pwrname[b]);
			any++;
		}
	if(!any)
		n += snprint(buf + n, max - n, "none");
	*len = n;
	if(anyp != nil)
		*anyp = any;
	return 0;
}

static int
pwrkeyclear(void)
{
	u8 m = 0x0f;

	return pmwrite(Intsts0, &m, 1);
}

/* DEV_OFF: SYS_CFG3 bit 0 drops the PMIC rails. */
static int
poweroff(void)
{
	u8 c;

	if(regread(Syscfg3, &c) < 0)
		return -1;
	c |= 0x01;
	return pmwrite(Syscfg3, &c, 1);
}

/* Acknowledge (clear) every latched interrupt source. */
static int
irqclear(void)
{
	u8 ff = 0xff;
	int i;

	for(i = 0; i < 3; i++)
		if(pmwrite(Intsts0 + 2*i, &ff, 1) < 0)
			return -1;
	return 0;
}

static int
bcd(u8 v)
{
	return (v >> 4) * 10 + (v & 0xf);
}

static u8
tobcd(int v)
{
	return ((v / 10) << 4) | (v % 10);
}

/* Days since 1970-01-01 for a proleptic Gregorian date (month 1..12). */
static vlong
daysfromcivil(int y, int m, int d)
{
	vlong era, doe;
	unsigned yoe, doy;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned)(y - era * 400);
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static void
civilfromdays(vlong z, int *py, int *pm, int *pd)
{
	vlong era, doe, yoe, y, doy, mp;
	unsigned doy2;

	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y = yoe + era * 400;
	doy2 = (unsigned)(doe - (365 * yoe + yoe / 4 - yoe / 100));
	mp = (5 * doy2 + 2) / 153;
	*pd = (int)(doy2 - (153 * mp + 2) / 5 + 1);
	*pm = (int)(mp + (mp < 10 ? 3 : -9));
	*py = (int)(y + (*pm <= 2));
}

/* The RK8xx calendar gives November 31 days, so Linux stores dates shifted
 * by one day per year since 2016, plus one in December.  Match that. */
static int
rckextra(int y, int mo)
{
	return (y - 2016) + (mo > 11);
}

/* Read the RTC, converting the Rockchip calendar to Gregorian. */
static int
rtcget(int *py, int *pmo, int *pd, int *ph, int *pmi, int *ps)
{
	u8 ctrl, t[7], v;
	vlong days;

	if(regread(0x0d, &ctrl) < 0)
		return -1;
	v = ctrl | 0x40;			/* GET_TIME latch */
	if(pmwrite(0x0d, &v, 1) < 0)
		return -1;
	v = ctrl & ~0x40;
	if(pmwrite(0x0d, &v, 1) < 0)
		return -1;
	if(pmread(0x00, t, 7) < 0)
		return -1;
	*ps = bcd(t[0] & 0x7f);
	*pmi = bcd(t[1] & 0x7f);
	*ph = bcd(t[2] & 0x3f);
	*pd = bcd(t[3] & 0x3f);
	*pmo = bcd(t[4] & 0x1f);
	*py = 2000 + bcd(t[5]);
	if(*pmo < 1 || *pmo > 12 || *pd < 1 || *pd > 31 ||
	   *ph > 23 || *pmi > 59 || *ps > 59)
		return -1;
	days = daysfromcivil(*py, *pmo, *pd) + rckextra(*py, *pmo);
	civilfromdays(days, py, pmo, pd);
	return 0;
}

/* Write the RTC from a Gregorian date/time. */
static int
rtcset(int y, int mo, int d, int h, int mi, int sec)
{
	u8 t[7], ctrl, v;
	vlong days;

	if(y < 2016 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
	   h > 23 || mi > 59 || sec > 59)
		return -1;
	days = daysfromcivil(y, mo, d) - rckextra(y, mo);
	civilfromdays(days, &y, &mo, &d);
	t[0] = tobcd(sec);
	t[1] = tobcd(mi);
	t[2] = tobcd(h);
	t[3] = tobcd(d);
	t[4] = tobcd(mo);
	t[5] = tobcd(y - 2000);
	t[6] = tobcd((int)((daysfromcivil(y, mo, d) + 4) % 7));
	if(regread(0x0d, &ctrl) < 0)
		return -1;
	v = ctrl | 0x01;			/* stop the counter */
	if(pmwrite(0x0d, &v, 1) < 0)
		return -1;
	if(pmwrite(0x00, t, 7) < 0)
		return -1;
	v = ctrl & ~0x01;
	return pmwrite(0x0d, &v, 1);
}

/* Alarm registers 0x07..0x0c; RTC_INT 0x0f bit 3 enables the alarm. */
static int
alarmget(int *py, int *pmo, int *pd, int *ph, int *pmi, int *ps, int *pen)
{
	u8 t[6], irq;
	vlong days;

	if(pmread(0x07, t, 6) < 0 || regread(0x0f, &irq) < 0)
		return -1;
	*ps = bcd(t[0] & 0x7f);
	*pmi = bcd(t[1] & 0x7f);
	*ph = bcd(t[2] & 0x3f);
	*pd = bcd(t[3] & 0x3f);
	*pmo = bcd(t[4] & 0x1f);
	*py = 2000 + bcd(t[5]);
	if(*pmo < 1 || *pmo > 12)
		return -1;
	days = daysfromcivil(*py, *pmo, *pd) + rckextra(*py, *pmo);
	civilfromdays(days, py, pmo, pd);
	*pen = (irq & 0x08) != 0;
	return 0;
}

static int
alarmset(int y, int mo, int d, int h, int mi, int sec)
{
	u8 t[6], irq;
	vlong days;

	if(y < 2016 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
	   h > 23 || mi > 59 || sec > 59)
		return -1;
	days = daysfromcivil(y, mo, d) - rckextra(y, mo);
	civilfromdays(days, &y, &mo, &d);
	t[0] = tobcd(sec);
	t[1] = tobcd(mi);
	t[2] = tobcd(h);
	t[3] = tobcd(d);
	t[4] = tobcd(mo);
	t[5] = tobcd(y - 2000);
	if(pmwrite(0x07, t, 6) < 0 || regread(0x0f, &irq) < 0)
		return -1;
	irq |= 0x08;
	return pmwrite(0x0f, &irq, 1);
}

static int
alarmoff(void)
{
	u8 irq;

	if(regread(0x0f, &irq) < 0)
		return -1;
	irq &= ~0x08;
	return pmwrite(0x0f, &irq, 1);
}

/* Parse one ctl line and leave a message in ctlresult. */
static void
ctlcmd(char *s)
{
	u8 buf[Maxread];
	char *f[16];
	int nf, n, reg, i, k;

	nf = tokenize(s, f, nelem(f));
	if(nf == 0){
		snprint(ctlresult, sizeof ctlresult, "empty");
		return;
	}
	if(strcmp(f[0], "r") == 0 && nf >= 2){
		reg = strtol(f[1], nil, 0);
		n = nf >= 3 ? strtol(f[2], nil, 0) : 1;
		if(n > Maxread)
			n = Maxread;
		if(pmread(reg, buf, n) < 0){
			if(ctlresult[0] == 0)
				snprint(ctlresult, sizeof ctlresult,
					"read 0x%x failed", reg);
			return;
		}
		k = snprint(ctlresult, sizeof ctlresult, "r 0x%x:", reg);
		for(i = 0; i < n; i++)
			k += snprint(ctlresult + k, sizeof ctlresult - k,
				" %02x", buf[i]);
		return;
	}
	if(strcmp(f[0], "w") == 0 && nf >= 3){
		reg = strtol(f[1], nil, 0);
		n = 0;
		for(i = 2; i < nf && n < Maxread; i++)
			buf[n++] = strtol(f[i], nil, 0);
		if(pmwrite(reg, buf, n) < 0)
			return;
		snprint(ctlresult, sizeof ctlresult, "w 0x%x: %d bytes", reg, n);
		return;
	}
	if(strcmp(f[0], "on") == 0 && nf >= 2){
		railctl(f[1], 1);
		return;
	}
	if(strcmp(f[0], "off") == 0 && nf >= 2){
		railctl(f[1], 0);
		return;
	}
	if(strcmp(f[0], "set") == 0 && nf >= 3){
		railset(f[1], strtol(f[2], nil, 0));
		return;
	}
	if(strcmp(f[0], "irq") == 0){
		int n2;

		if(nf >= 2 && strcmp(f[1], "clear") == 0){
			if(irqclear() < 0)
				return;
			n2 = snprint(ctlresult, sizeof ctlresult, "irq cleared");
			irqstr(ctlresult, sizeof ctlresult, &n2);
			return;
		}
		n2 = snprint(ctlresult, sizeof ctlresult, "irq:");
		irqstr(ctlresult, sizeof ctlresult, &n2);
		return;
	}
	if(strcmp(f[0], "alarm") == 0){
		if(nf >= 2 && strcmp(f[1], "off") == 0){
			if(alarmoff() < 0)
				return;
			snprint(ctlresult, sizeof ctlresult, "alarm off");
			return;
		}
		if(nf >= 7){
			if(alarmset(strtol(f[1], nil, 0), strtol(f[2], nil, 0),
				    strtol(f[3], nil, 0), strtol(f[4], nil, 0),
				    strtol(f[5], nil, 0), strtol(f[6], nil, 0)) < 0)
				return;
			snprint(ctlresult, sizeof ctlresult, "alarm set");
			return;
		}
		{
			int ay, amo, ad, ah, ami, as, aen;

			if(alarmget(&ay, &amo, &ad, &ah, &ami, &as, &aen) < 0)
				return;
			if(aen)
				snprint(ctlresult, sizeof ctlresult,
					"alarm %04d-%02d-%02d %02d:%02d:%02d",
					ay, amo, ad, ah, ami, as);
			else
				snprint(ctlresult, sizeof ctlresult, "alarm off");
		}
		return;
	}
	if(strcmp(f[0], "hotdie") == 0){
		u8 cfg;

		if(regread(Syscfg1, &cfg) < 0)
			return;
		if(nf >= 2){
			int t = strtol(f[1], nil, 0);

			if(t != 85 && t != 95 && t != 105 && t != 115){
				snprint(ctlresult, sizeof ctlresult,
					"hotdie 85|95|105|115");
				return;
			}
			cfg &= ~0x30;
			cfg |= ((t - 85) / 10) << 4;
			if(pmwrite(Syscfg1, &cfg, 1) < 0)
				return;
			/* Re-arming the threshold latches a hotdie edge. */
			{ u8 m = 0x10; pmwrite(Intsts0, &m, 1); }
		}
		snprint(ctlresult, sizeof ctlresult, "hotdie %d C",
			85 + 10 * ((cfg >> 4) & 3));
		return;
	}
	if(strcmp(f[0], "pwrkey") == 0){
		int n2;

		if(nf >= 2 && strcmp(f[1], "clear") == 0){
			if(pwrkeyclear() < 0)
				return;
		}
		n2 = snprint(ctlresult, sizeof ctlresult, "pwrkey:");
		if(pwrkeystr(ctlresult, sizeof ctlresult, &n2, nil) < 0)
			return;
		return;
	}
	if(strcmp(f[0], "clk32k") == 0){
		u8 cfg;

		if(regread(Syscfg1, &cfg) < 0)
			return;
		if(nf >= 2 && strcmp(f[1], "on") == 0)
			cfg |= Clk32kEn;
		else if(nf >= 2 && strcmp(f[1], "off") == 0)
			cfg &= ~Clk32kEn;
		else{
			snprint(ctlresult, sizeof ctlresult, "clk32k %s",
				(cfg & Clk32kEn) ? "on" : "off");
			return;
		}
		if(pmwrite(Syscfg1, &cfg, 1) < 0)
			return;
		snprint(ctlresult, sizeof ctlresult, "clk32k %s",
			(cfg & Clk32kEn) ? "on" : "off");
		return;
	}
	snprint(ctlresult, sizeof ctlresult,
		"want r|w|on|off|set|clk32k|irq|pwrkey|alarm|hotdie");
}

/* ---- 9P files ---- */

static int
statusstr(char *buf, int max)
{
	u8 en[5], v, sts, id[2], cfg, src[2];
	int i, n, m;

	USED(max);
	n = 0;
	n += snprint(buf + n, 4096 - n, "rk809 addr 0x%x\n", Pmicaddr);
	if(pmread(0xb1, en, 4) < 0)
		return snprint(buf, max, "cannot read power registers\n");
	for(i = 0; i < nelem(rails); i++){
		Rail *rl = &rails[i];

		if(rl->kind == 4){
			n += snprint(buf + n, 4096 - n, "%-16s switch %s\n",
				rl->name,
				railenabled(rl, en[rl->enreg - 0xb1]) ? "on" : "off");
			continue;
		}
		if(regread(rl->vsel, &v) < 0){
			n += snprint(buf + n, 4096 - n, "%-16s read failed\n",
				rl->name);
			continue;
		}
		if(strcmp(rl->name, "vcc_ddr") == 0){
			n += snprint(buf + n, 4096 - n,
				"%-16s fixed (external fb) %s (vsel 0x%02x)\n",
				rl->name,
				railenabled(rl, en[rl->enreg - 0xb1]) ? "on" : "off", v);
			continue;
		}
		m = railmillivolt(rl, v);
		n += snprint(buf + n, 4096 - n, "%-16s %4d mV %s (vsel 0x%02x)\n",
			rl->name, m,
			railenabled(rl, en[rl->enreg - 0xb1]) ? "on" : "off", v);
	}
	{
		int y, mo, d, h, mi, sec;

		if(rtcget(&y, &mo, &d, &h, &mi, &sec) == 0)
			n += snprint(buf + n, 4096 - n,
				"rtc %04d-%02d-%02d %02d:%02d:%02d\n",
				y, mo, d, h, mi, sec);
	}
	if(regread(0xf0, &sts) == 0)
		n += snprint(buf + n, 4096 - n, "sys status 0x%02x\n", sts);
	if(pmread(Idmsb, id, 2) == 0)
		n += snprint(buf + n, 4096 - n, "chip id %02x%02x\n",
			id[0], id[1]);
	if(regread(Syscfg1, &cfg) == 0)
		n += snprint(buf + n, 4096 - n,
			"clkout2 32kHz %s, hotdie %d C, tsd %d C\n",
			(cfg & Clk32kEn) ? "on" : "off",
			85 + 10 * ((cfg >> 4) & 3),
			(cfg & 0x40) ? 160 : 140);
	if(pmread(Onsource, src, 2) == 0)
		n += snprint(buf + n, 4096 - n,
			"power source on 0x%02x off 0x%02x\n", src[0], src[1]);
	{
		int ay, amo, ad, ah, ami, as, aen;

		if(alarmget(&ay, &amo, &ad, &ah, &ami, &as, &aen) == 0){
			if(aen)
				n += snprint(buf + n, 4096 - n,
					"alarm %04d-%02d-%02d %02d:%02d:%02d\n",
					ay, amo, ad, ah, ami, as);
			else
				n += snprint(buf + n, 4096 - n, "alarm off\n");
		}
	}
	n += snprint(buf + n, 4096 - n, "latched irq: ");
	irqstr(buf, 4096, &n);
	n += snprint(buf + n, 4096 - n, "\n");
	return n;
}

static void
statusread(Req *r)
{
	char *buf;

	buf = malloc(4096);
	if(buf == nil){
		respond(r, "out of memory");
		return;
	}
	statusstr(buf, 4096);
	readstr(r, buf);
	free(buf);
	respond(r, nil);
}

static void
regsread(Req *r)
{
	char *buf;
	u8 b[16];
	int i, k, n;

	buf = malloc(8192);
	if(buf == nil){
		respond(r, "out of memory");
		return;
	}
	n = 0;
	for(i = 0; i < Maxregs; i += sizeof b){
		if(pmread(i, b, sizeof b) < 0){
			n += snprint(buf + n, 8192 - n, "%02x: read failed\n", i);
			continue;
		}
		n += snprint(buf + n, 8192 - n, "%02x:", i);
		for(k = 0; k < (int)sizeof b; k++)
			n += snprint(buf + n, 8192 - n, " %02x", b[k]);
		n += snprint(buf + n, 8192 - n, "\n");
	}
	readstr(r, buf);
	free(buf);
	respond(r, nil);
}

static void
rtcread(Req *r)
{
	char buf[64];
	int y, mo, d, h, mi, sec, n;

	if(rtcget(&y, &mo, &d, &h, &mi, &sec) < 0){
		respond(r, "rtc read failed");
		return;
	}
	n = snprint(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d\n",
		y, mo, d, h, mi, sec);
	readstr(r, buf);
	respond(r, nil);
}

/* Like ctl, the rtc file may be written in several Twrites (echo sends
   each argument separately), so buffer until the newline. */
static void
rtcwrite(Req *r)
{
	static char buf[64];
	static int len;
	char *p, *e, *q;
	int y, mo, d, h, mi, sec;

	p = r->ifcall.data;
	e = p + r->ifcall.count;
	while(p < e){
		if(*p == '\n'){
			buf[len] = 0;
			q = buf;
			y = strtol(q, &q, 10);
			if(*q == '-') q++;
			mo = strtol(q, &q, 10);
			if(*q == '-') q++;
			d = strtol(q, &q, 10);
			h = strtol(q, &q, 10);
			if(*q == ':') q++;
			mi = strtol(q, &q, 10);
			if(*q == ':') q++;
			sec = strtol(q, &q, 10);
			len = 0;
			if(rtcset(y, mo, d, h, mi, sec) < 0){
				respond(r, "rtc set failed");
				return;
			}
			p++;
			continue;
		}
		if(len < (int)sizeof buf - 1)
			buf[len++] = *p;
		p++;
	}
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

static void
pwrkeyread(Req *r)
{
	char buf[64];
	int n, any;

	n = 0;
	if(pwrkeystr(buf, sizeof buf - 1, &n, &any) < 0){
		respond(r, "pwrkey read failed");
		return;
	}
	buf[n++] = '\n';
	buf[n] = 0;
	readstr(r, buf);
	respond(r, nil);
}

static void
fsread(Req *r)
{
	switch((uintptr)r->fid->file->aux){
	case Fstatus:
		statusread(r);
		break;
	case Fregs:
		regsread(r);
		break;
	case Frtc:
		rtcread(r);
		break;
	case Fctl:
		readstr(r, ctlresult);
		respond(r, nil);
		break;
	case Fpwrkey:
		pwrkeyread(r);
		break;
	default:
		respond(r, "bad file");
		break;
	}
}

/* Ctl clients may write a command in several Twrites (echo writes the
   text and the newline separately), so buffer until a line ends. */
static void
fswrite(Req *r)
{
	char *p, *e;
	int n, k;

	if((uintptr)r->fid->file->aux == Frtc){
		rtcwrite(r);
		return;
	}
	if((uintptr)r->fid->file->aux == Fpwrkey){
		if(pwrkeyclear() < 0)
			respond(r, "pwrkey clear failed");
		else{
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
		}
		return;
	}
	if((uintptr)r->fid->file->aux != Fctl){
		respond(r, "write not permitted");
		return;
	}
	p = r->ifcall.data;
	e = p + r->ifcall.count;
	while(p < e){
		if(*p == '\n'){
			ctlbuf[ctllen] = 0;
			ctlresult[0] = 0;
			if(ctllen > 0)
				ctlcmd(ctlbuf);
			ctllen = 0;
			p++;
			continue;
		}
		if(ctllen < (int)sizeof ctlbuf - 1)
			ctlbuf[ctllen++] = *p;
		p++;
	}
	n = r->ifcall.count;
	k = 0;
	USED(k);
	r->ofcall.count = n;
	respond(r, nil);
}

static Srv fs = {
	.read = fsread,
	.write = fswrite,
};

/* Poll the PWRON key and power the board off on a long press.  Runs in
   its own process with its own namespace and fds so the 9P server keeps
   working; the long bit is latched, so reading it twice protects the
   decision against a crossed i2cfs read buffer. */
static void
keywatch(void)
{
	char path[64];
	u8 v;
	int fd, armed, gwait, tries;

	if(rfork(RFNAMEG|RFFDG) < 0)
		_exits("rfork");
	fd = open("/srv/i2c", ORDWR);
	if(fd < 0)
		_exits("no i2c");
	if(mount(fd, -1, "/n", MREPL, "") < 0)
		_exits("mount");
	close(fd);
	snprint(path, sizeof path, "/n/i2c%d/ctl", i2cbus);
	ctlfd = open(path, ORDWR);
	snprint(path, sizeof path, "/n/i2c%d/data", i2cbus);
	datafd = open(path, OREAD);
	if(ctlfd < 0 || datafd < 0)
		_exits("no ctl");

	pwrkeyclear();			/* forget anything latched before us */
	armed = 0;

	/* Prefer the PMIC INT line (GPIO0_A3, level low) over polling. */
	gwait = -1;
	for(tries = 0; tries < 20 && gwait < 0; tries++){
		fd = open("/srv/gpio", ORDWR);
		if(fd < 0){
			sleep(100);
			continue;
		}
		if(mount(fd, -1, "/mnt", MREPL, "") < 0){
			close(fd);
			sleep(100);
			continue;
		}
		close(fd);
		snprint(path, sizeof path, "/mnt/gpio0/ctl");
		fd = open(path, OWRITE);
		if(fd >= 0){
			write(fd, "irq 3 low", 9);
			close(fd);
		}
		snprint(path, sizeof path, "/mnt/gpio0/wait");
		gwait = open(path, OREAD);
		if(gwait < 0)
			sleep(100);
	}
	if(gwait >= 0)
		fprint(2, "pmic: key on gpio0 irq\n");
	else
		fprint(2, "pmic: no gpio irq, polling\n");

	for(;;){
		if(gwait >= 0){
			char tmp[32];

			if(read(gwait, tmp, sizeof tmp) <= 0){
				gwait = -1;
				continue;
			}
		}else
			sleep(200);
		if(regread(Intsts0, &v) < 0)
			continue;
		if(v & 0x10){			/* hot die warning: report only */
			u8 m = 0x10;

			fprint(2, "pmic: hot die warning\n");
			pmwrite(Intsts0, &m, 1);
		}
		if(v & 0x20){			/* rtc alarm */
			u8 sts = 0xfe, m = 0x20;

			fprint(2, "pmic: rtc alarm\n");
			pmwrite(0x0e, &sts, 1);
			pmwrite(Intsts0, &m, 1);
		}
		if(!(v & 0x08)){
			armed = 1;	/* key is up: a later edge is a real press */
			continue;
		}
		if(!armed)
			continue;	/* already down when the service started */
		sleep(10);
		if(regread(Intsts0, &v) < 0 || !(v & 0x08))
			continue;
		fprint(2, "pmic: long press, powering off\n");
		poweroff();
		sleep(1000);
	}
}

void
main(int argc, char **argv)
{
	DrvHealth *health;
	int fd, i, found, probe, watch;
	char path[64];

	USED(argc);
	USED(argv);

	/* Private namespace: mounting i2cfs at /n must not disturb clients. */
	if(rfork(RFNAMEG) < 0)
		fail("rfork RFNAMEG");

	fd = open("/srv/i2c", ORDWR);
	if(fd < 0)
		fail("no /srv/i2c, start i2cfs first");
	if(mount(fd, -1, "/n", MREPL, "") < 0)
		fail("cannot mount /srv/i2c at /n");
	close(fd);

	found = -1;
	for(i = 0; i < 6 && found < 0; i++){
		u8 v;

		snprint(path, sizeof path, "/n/i2c%d/ctl", i);
		ctlfd = open(path, ORDWR);
		snprint(path, sizeof path, "/n/i2c%d/data", i);
		datafd = open(path, OREAD);
		if(ctlfd < 0 || datafd < 0){
			if(ctlfd >= 0)
				close(ctlfd);
			if(datafd >= 0)
				close(datafd);
			ctlfd = datafd = -1;
			continue;
		}
		if(pmread(0xb1, &v, 1) == 0)
			found = i;
		else{
			close(ctlfd);
			close(datafd);
			ctlfd = datafd = -1;
		}
	}
	if(found < 0)
		fail("rk809 not found on i2c0..5");
	i2cbus = found;
	fprint(2, "pmicfs: rk809 on i2c%d\n", found);

	for(i = 1; i < argc; i++)
		if(strcmp(argv[i], "-c") == 0){
			u8 cfg;

			if(regread(Syscfg1, &cfg) == 0 && !(cfg & Clk32kEn)){
				cfg |= Clk32kEn;
				if(pmwrite(Syscfg1, &cfg, 1) == 0)
					fprint(2, "pmicfs: clkout2 32kHz on\n");
			}
		}

	probe = 0;
	for(i = 1; i < argc; i++)
		if(strcmp(argv[i], "-p") == 0)
			probe = 1;
	if(probe){
		char buf[4096];
		u8 b[16];
		int k;

		statusstr(buf, sizeof buf);
		write(2, buf, strlen(buf));
		if(pmread(0xb0, b, 16) == 0){
			snprint(buf, sizeof buf, "reg b0:");
			k = strlen(buf);
			for(int j = 0; j < 16; j++)
				k += snprint(buf + k, sizeof buf - k, " %02x", b[j]);
			k += snprint(buf + k, sizeof buf - k, "\n");
			write(2, buf, k);
		}
		if(pmread(0xc0, b, 16) == 0){
			snprint(buf, sizeof buf, "reg c0:");
			k = strlen(buf);
			for(int j = 0; j < 16; j++)
				k += snprint(buf + k, sizeof buf - k, " %02x", b[j]);
			k += snprint(buf + k, sizeof buf - k, "\n");
			write(2, buf, k);
		}
		exits(nil);
	}

	fs.tree = alloctree(nil, nil, DMDIR | 0777, nil);
	createfile(fs.tree->root, "status", nil, 0444, (void*)Fstatus);
	createfile(fs.tree->root, "regs", nil, 0444, (void*)Fregs);
	createfile(fs.tree->root, "rtc", nil, 0666, (void*)Frtc);
	createfile(fs.tree->root, "ctl", nil, 0666, (void*)Fctl);
	createfile(fs.tree->root, "pwrkey", nil, 0666, (void*)Fpwrkey);
	postmountsrv(&fs, "pmic", nil, MREPL | MCREATE);

	watch = 1;
	for(i = 1; i < argc; i++)
		if(strcmp(argv[i], "-n") == 0)
			watch = 0;
	if(watch){
		if(rfork(RFPROC|RFNOWAIT) == 0){
			keywatch();
			_exits(0);
		}
	}

	health = drvhealth_openenv();
	if(health != nil){
		drvhealth_ready(health);
		for(;;){
			drvhealth_report(health, DrvHealthHealthy, 0, 0, 0,
			                 0, 0, 0, DrvHealthRecommendNone, "none");
			if(drvhealth_stopwait(health, 1000))
				break;
		}
		drvhealth_close(health);
	}else
		for(;;)
			sleep(1000);
	exits(nil);
}
