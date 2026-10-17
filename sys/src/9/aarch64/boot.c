/* Bring Harvey's shared process scheduler online after the ARM64 probes. */
#include <u.h>
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "fdt.h"
#include "phys.h"

extern void arm64uartputs(char *);
extern void (*consuartputs)(char *, int);
extern void arm64clockready(void);
extern void pl011configure(u64);
extern int pl011consoleinput(void);
#ifdef ARM64_TEST_CONSOLE_RX
extern Queue *pl011inputqueue(void);
#endif
extern Dev rootdevtab;
extern Dev uartdevtab;
static Chan *bootserial;

static void
uartwrite(char *buf, int n)
{
	char c[2];

	c[1] = 0;
	while(n-- > 0){
		c[0] = *buf++;
		arm64uartputs(c);
	}
}

static volatile int workerphase;

static void
worker(void *arg)
{
	int id;

	id = (usize)arg;
	if(id == 1){
		workerphase |= 1;
		while(workerphase != 3)
			sched();
		arm64uartputs("HARVEY-ARM64 SCHEDULER PASS\n");
		while(sys->ticks < 3)
			sched();
		arm64uartputs("HARVEY-ARM64 CLOCK PASS\n");
		{
			Chan *root;
			Walkqid *walk;
			char *name;

			root = rootdevtab.attach("");
			name = "dev";
			walk = rootdevtab.walk(root, nil, &name, 1);
			if(walk == nil || walk->nqid != 1 || walk->clone == nil ||
			   (walk->qid[0].type & QTDIR) == 0)
				panic("ARM64 root namespace walk failed");
			cclose(walk->clone);
			free(walk);
			cclose(root);
			arm64uartputs("HARVEY-ARM64 ROOT-WALK PASS\n");
		}
		{
			Chan *serial;
			Walkqid *walk;
			char *name;
			char marker[] = "HARVEY-ARM64 UART-IO PASS\n";

			serial = uartdevtab.attach("");
			name = "eia0";
			walk = uartdevtab.walk(serial, nil, &name, 1);
			if(walk == nil || walk->nqid != 1 || walk->clone == nil)
				panic("ARM64 PL011 not in #t");
			uartdevtab.open(walk->clone, OWRITE);
			if(uartdevtab.write(walk->clone, marker, sizeof(marker)-1, 0) !=
			   sizeof(marker)-1)
				panic("ARM64 PL011 write failed");
			if(pl011consoleinput() < 0)
				panic("ARM64 PL011 console input failed");
			bootserial = walk->clone;
			free(walk);
			cclose(serial);
			arm64uartputs("HARVEY-ARM64 SERIAL-CONSOLE PASS\n");
#ifdef ARM64_TEST_CONSOLE_RX
			{
				char input;

				if(qread(pl011inputqueue(), &input, 1) != 1 || input != 'Z')
					panic("ARM64 PL011 console receive failed");
				arm64uartputs("HARVEY-ARM64 UART-RX PASS\n");
			}
#endif
		}
	}else{
		workerphase |= 2;
	}
}

static void
startworker(int id)
{
	Proc *p;

	p = newproc();
	kstrdup(&p->user, eve);
	p->kp = 1;
	p->noswap = 1;
	procpriority(p, PriKproc, 0);
	kprocchild(p, worker, (void *)(usize)id);
	ready(p);
}

void
arm64kernelstart(Fdtinfo *info)
{
	u64 freq;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	sys->cyclefreq = freq;
	machp()->online = 1;
	consuartputs = uartwrite;
	mallocinit();
	conf.npage = info->ramsize / PGSZ;
	conf.upages = arm64physfreepages();
	conf.nproc = 64;
	conf.nimage = 64;
	eve = "root";
	procinit0();
	psinit(conf.nproc);
	initimage();
	printinit();
	timersinit();
	pl011configure(info->uartbase);
	devtabreset();
	arm64uartputs("HARVEY-ARM64 DEVICE-RESET PASS\n");
	pageinit();
	{
		Page *p;

		p = pgalloc(PGSZ, 0);
		if(p == nil)
			panic("ARM64 page allocator failed");
		pgfree(p);
	}
	arm64uartputs("HARVEY-ARM64 PAGE-ALLOC PASS\n");
	arm64clockready();
	startworker(1);
	startworker(2);
	schedinit();
	panic("ARM64 scheduler returned");
}
