/* usbpower: RK356x USB platform service.
 *
 * Owns the board-level USB resources that do not belong to the xHCI
 * controller driver: PMU power domains, CRU clocks/resets, USB2 PHY GRFs,
 * PHY tuning, and the shared VBUS enable.  It maps one-shot registers,
 * programs them, detaches the mappings, and exposes /srv/usbpower/status.
 *
 * The controller driver must not know about PMU, CRU, GPIO, or PHY registers.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "../o9drvman/health.h"

enum {
	Bigpage = 2*1024*1024,
	Fstatus = 0,
	Fctl = 1,
};

static int powered;
static QLock lk;

static long
readtext(char *path, char *buf, long n)
{
	int fd;
	long m;

	fd = open(path, OREAD);
	if(fd < 0)
		return -1;
	m = read(fd, buf, n - 1);
	close(fd);
	if(m < 0)
		return -1;
	buf[m] = 0;
	return m;
}

static void *
mapreg(u64 base, usize size, char *outname, int outlen)
{
	Dir *d;
	char path[128], buf[64], *p;
	void *m;
	u64 reg, len;
	int fd, i, n;

	fd = open("#P", OREAD);
	if(fd < 0)
		return nil;
	n = dirread(fd, &d);
	close(fd);
	if(n <= 0)
		return nil;
	for(i = 0; i < n; i++){
		if(strcmp(d[i].name, ".") == 0 || strcmp(d[i].name, "..") == 0)
			continue;
		snprint(path, sizeof path, "#P/%s/reg", d[i].name);
		if(readtext(path, buf, sizeof buf) < 0)
			continue;
		reg = strtoull(buf, &p, 0);
		if(reg != base)
			continue;
		len = (size + Bigpage - 1) & ~(Bigpage - 1);
		m = segattach(0, d[i].name, 0, len);
		if(m == (void *)-1){
			free(d);
			return nil;
		}
		if(outname != nil && outlen > 0){
			strncpy(outname, d[i].name, outlen - 1);
			outname[outlen - 1] = 0;
		}
		free(d);
		return (uchar *)m + (base & (Bigpage - 1));
	}
	free(d);
	return nil;
}

static int
waitfor(char *path, int tries)
{
	int fd;

	while(tries-- > 0){
		fd = open(path, OREAD);
		if(fd >= 0){
			close(fd);
			return 0;
		}
		sleep(100);
	}
	return -1;
}

static int gpiomounted;

static int
mountgpio(void)
{
	int fd;

	if(gpiomounted)
		return 0;
	if(waitfor("/srv/gpio", 50) < 0)
		return -1;
	fd = create("/mnt/usbgpio", OREAD, DMDIR | 0755);
	if(fd >= 0)
		close(fd);
	fd = open("/srv/gpio", ORDWR);
	if(fd < 0)
		return -1;
	alarm(5000);
	if(mount(fd, -1, "/mnt/usbgpio", MREPL, "") < 0){
		alarm(0);
		close(fd);
		return -1;
	}
	alarm(0);
	close(fd);
	gpiomounted = 1;
	return 0;
}

/* VBUS is owned by gpiofs: GPIO0_A5 and GPIO0_A6, active high. */
static int
vbus(int on)
{
	char cmd[32];
	int ctl, n;

	if(mountgpio() < 0)
		return -1;
	ctl = open("/mnt/usbgpio/gpio0/ctl", OWRITE);
	if(ctl < 0)
		return -1;
	n = snprint(cmd, sizeof cmd, "6 out %d", on != 0);
	if(write(ctl, cmd, n) != n){
		close(ctl);
		return -1;
	}
	n = snprint(cmd, sizeof cmd, "5 out %d", on != 0);
	if(write(ctl, cmd, n) != n){
		close(ctl);
		return -1;
	}
	close(ctl);
	return 0;
}

static void
detachall(volatile u32 **p, int n)
{
	int i;

	for(i = 0; i < n; i++)
		if(p[i] != nil)
			segdetach((void *)p[i]);
}

static int
power_on(void)
{
	volatile u32 *pmu, *pmucru, *cru, *g0, *g1, *p0, *p1;
	volatile u32 *maps[5];
	char name[64];

	if(vbus(1) < 0)
		return -1;

	fprint(2, "usbpower: map pmu\n");
	pmu = mapreg(0xfdd90000ULL, 0x10000, name, sizeof name);
	fprint(2, "usbpower: map pmucru\n");
	pmucru = mapreg(0xfdd00000ULL, 0x10000, name, sizeof name);
	fprint(2, "usbpower: map cru\n");
	cru = mapreg(0xfdd20000ULL, 0x10000, name, sizeof name);
	fprint(2, "usbpower: map phy grf\n");
	g0 = mapreg(0xfdca0000ULL, 0x10000, name, sizeof name);
	g1 = g0 == nil ? nil : g0 + 0x8000/4;
	fprint(2, "usbpower: map phy\n");
	p0 = mapreg(0xfe8a0000ULL, 0x20000, name, sizeof name);
	p1 = p0 == nil ? nil : p0 + 0x10000/4;
	maps[0] = pmu;
	maps[1] = pmucru;
	maps[2] = cru;
	maps[3] = g0;
	maps[4] = p0;
	if(pmu == nil || pmucru == nil || cru == nil || g0 == nil || p0 == nil){
		fprint(2, "usbpower: missing board register block\n");
		detachall(maps, nelem(maps));
		return -1;
	}

	fprint(2, "usbpower: program\n");
	/* PD_PIPE power on, then idle request clear. */
	pmu[0x00a0/4] = (1U<<8)<<16 | 0;
	pmu[0x0050/4] = (1U<<11)<<16 | 0;

	/* PMUCRU: GPIO0 clocks, PHY reference oscillators, combphy select. */
	pmucru[0x184/4] = (3U<<9)<<16 | 0;
	pmucru[0x188/4] = ((1U<<1)|(1U<<2))<<16 | 0;
	pmucru[0x124/4] = (((0x7U<<4)|(1U<<7))<<16) | ((1U<<4)|(1U<<7));

	/* CRU: PIPE/USB3OTG1 gates, USB2 host gates, reset deassert. */
	cru[0x328/4] = ((1U<<0)|(1U<<1)|(1U<<12)|(1U<<13)|(1U<<14))<<16 | 0;
	cru[0x340/4] = ((1U<<0)|(1U<<1)|(1U<<2)|(1U<<12)|(1U<<13)|(1U<<14)|(1U<<15))<<16 | 0;
	cru[0x424/4] = (1U<<5)<<16 | 0;

	/* PHY0 host, PHY1 host/otg: power-on 0x1d2, clear suspend. */
	g0[0x0008/4] = (1U<<4)<<16 | 0;
	g0[0x0004/4] = (0x1ffU<<16) | 0x1d2;
	g0[0x0080/4] = (1U<<1)<<16 | (1U<<1);
	g1[0x0008/4] = (1U<<4)<<16 | 0;
	g1[0x0004/4] = (0x1ffU<<16) | 0x1d2;
	g1[0x0000/4] = (0x1ffU<<16) | 0x1d2;
	g1[0x0080/4] = ((1U<<0)|(1U<<1))<<16 | ((1U<<0)|(1U<<1));

	/* Linux rk3568_usb2phy_tuning(): PHY base normal registers. */
	p0[0x30/4] &= ~(1U<<2);
	p0[0x30/4] = (p0[0x30/4] & ~7U) | 0x04;
	p0[0x400/4] = (p0[0x400/4] & ~7U) | 0x04;
	p0[0x30/4] = (p0[0x30/4] & ~(7U<<4)) | (6U<<4);
	p1[0x30/4] &= ~(1U<<2);
	p1[0x30/4] = (p1[0x30/4] & ~7U) | 0x04;
	p1[0x400/4] = (p1[0x400/4] & ~7U) | 0x04;

	/* Linux rk3568_usb2phy_tuning(): GRF filters and host wakeup. */
	g0[0x0048/4] = 0xF4240;
	g0[0x004c/4] = 0xF4240;
	g1[0x0048/4] = 0xF4240;
	g1[0x004c/4] = 0xF4240;
	g0[0x000c/4] = 0x80008000;
	g1[0x000c/4] = 0x80008000;

	/* One-shot mappings: do not hold board register blocks for the service. */
	detachall(maps, nelem(maps));
	fprint(2, "usbpower: ready\n");
	return 0;
}

static void
fsread(Req *r)
{
	char buf[32];
	uintptr tag;

	tag = (uintptr)r->fid->file->aux;
	if(tag == Fstatus){
		qlock(&lk);
		snprint(buf, sizeof buf, "%s\n", powered ? "ready" : "off");
		qunlock(&lk);
		readstr(r, buf);
		return;
	}
	respond(r, "permission denied");
}

static void
fswrite(Req *r)
{
	char cmd[16];
	long n;

	n = r->ifcall.count;
	if(n <= 0 || n >= sizeof cmd){
		respond(r, "bad command");
		return;
	}
	memmove(cmd, r->ifcall.data, n);
	cmd[n] = 0;
	if(strncmp(cmd, "on", 2) == 0){
		qlock(&lk);
		if(!powered && power_on() < 0){
			qunlock(&lk);
			respond(r, "usb power on failed");
			return;
		}
		powered = 1;
		qunlock(&lk);
	}else if(strncmp(cmd, "off", 3) == 0){
		qlock(&lk);
		if(vbus(0) < 0){
			qunlock(&lk);
			respond(r, "usb power off failed");
			return;
		}
		powered = 0;
		qunlock(&lk);
	}else{
		respond(r, "want on|off");
		return;
	}
	respond(r, nil);
}

static void
fsdestroyfile(File *f)
{
	USED(f);
}

static Srv fs = {
	.read = fsread,
	.write = fswrite,
};

void
main(int argc, char **argv)
{
	DrvHealth *health;

	USED(argc);
	USED(argv);
	if(power_on() < 0)
		_exits("usb power");
	qlock(&lk);
	powered = 1;
	qunlock(&lk);

	fs.tree = alloctree(nil, nil, DMDIR | 0777, fsdestroyfile);
	createfile(fs.tree->root, "status", nil, 0444, (void *)Fstatus);
	createfile(fs.tree->root, "ctl", nil, 0666, (void *)Fctl);
	postmountsrv(&fs, "usbpower", nil, MREPL | MCREATE);
	{
		int mfd = create("/mnt/usbpower.ready", OWRITE, 0644);
		if(mfd >= 0){
			write(mfd, "ready\n", 6);
			close(mfd);
		}
	}
	health = drvhealth_openenv();
	if(health != nil){
		drvhealth_ready(health);
		for(;;){
			drvhealth_report(health, DrvHealthHealthy, 1, 0, 0,
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
