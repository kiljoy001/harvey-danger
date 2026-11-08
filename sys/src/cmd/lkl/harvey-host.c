/*
 * harvey-host.c -- LKL host operations for Harvey.
 *
 * LKL runs the Linux kernel as a library inside a Harvey process.  This
 * file provides the small host ABI it needs: threads, semaphores, mutexes,
 * timers, memory and console.  MMIO, IRQ and DMA are stubbed until the
 * #P platform devices land (M2); nothing in a banner boot touches them.
 */
#include <u.h>
#include <libc.h>
#include <lkl/asm/host_ops.h>
#include <lkl/asm/irq.h>

enum {
	Page		= 4096,
	Threadstack	= 512*1024,
	Ntls		= 16,
	Ntid		= 64,
	Nreap		= 64,
};

/*
 * Semaphores and mutexes are the kernel's semacquire/semrelease counters,
 * not libc QLock/Rendez.  The counter lives in the shared segment, so the
 * kernel's per-segment wait list gives us a cross-process primitive with a
 * single syscall on the fast path and no shared user-space rendezvous.
 */
typedef struct Sem Sem;
struct Sem {
	i32	count;
};

typedef struct Mut Mut;
struct Mut {
	Lock	lk;		/* protects owner/count, never held while blocking */
	i32	sem;		/* the permit */
	int	owner;
	int	count;
	int	recursive;
};

typedef struct Timer Timer;
struct Timer {
	Lock	lk;
	vlong	deadline;
	int	armed;
	int	stop;
	void	(*fn)(void);
	lkl_thread_t tid;
};

struct lkl_tls_key {
	int	id;
	void	(*dtor)(void*);
};

static Lock	tlslk;
static struct {
	int	tid;
	void	*val;
} tlsval[Ntls][Ntid];
static struct lkl_tls_key tlskeys[Ntls];
static int	ntlskeys;

static Lock	reaplk;
static int	reap[Nreap];
static int	nreap;

/*
 * getpid() is not usable in an RFMEM child (it reads #c/pid and returns
 * 0), so a host thread's identity lives at the base of its own stack.
 * The main thread is id 1; created threads start at 2.
 */
enum {
	Stackmagic = 0x4c4b4c31,
	Mainid = 1,
};
typedef struct Sthdr Sthdr;
struct Sthdr {
	u32	magic;
	u32	id;
	u32	pid;
};
typedef struct Hthread Hthread;
struct Hthread {
	int	id;
	int	pid;
	int	used;
};
static Lock	thlk;
static Hthread	threads[Ntid];
static int	nexttid = 2;

static int hostdebug = 0;	/* thread/timer/console lifecycle */
static int blocktrace = 0;	/* every sem/mutex wait: noisy, opt-in */

void
lklhost_debug(int enabled)
{
	hostdebug = enabled;
	blocktrace = enabled;
}
static int ndbg;

enum { Dbgmax = 6000 };

/*
 * All host diagnostics go through one fd.  The console write path is not
 * safe to hammer from many host threads at once: a writer that blocks
 * inside the driver takes the console down with it.  lklboot points this at
 * a file so the console stays usable while the kernel runs.
 */
int lklhost_out = 2;

void
lklhost_setout(int fd)
{
	lklhost_out = fd;
}

static void
say(char *s, int n)
{
	int r;

	if(lklhost_out < 0)
		return;
	while(n > 0){
		r = write(lklhost_out, s, n);
		if(r <= 0)
			return;
		s += r;
		n -= r;
	}
}

static void
hdbg(char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprint(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if(n > 0)
		say(buf, n);
}

#define HDBG(...) do{ if(hostdebug && ndbg++ < Dbgmax) hdbg(__VA_ARGS__); }while(0)
#define BDBG(...) do{ if(blocktrace && ndbg++ < Dbgmax) hdbg(__VA_ARGS__); }while(0)
#define RA() ((void*)__builtin_return_address(0))

static int curtid(void);
static void tls_cleanup(int);

/*
 * In-memory host state for the liveness watchdog.  The hot paths only do a
 * few stores here; nothing calls into the kernel, so the observation does
 * not perturb the handoff it is meant to catch.
 */
typedef struct Tstate Tstate;
struct Tstate {
	int	op;
	void	*p;
	int	v;
	uvlong	stamp;
};
static Tstate	 tstate[Ntid];

enum { Nseminfo = 256 };
static struct {
	void	*p;
	void	*ra;
	int	kind;	/* 's' semaphore, 'm' mutex */
} seminfo[Nseminfo];
static int nseminfo;

static void
semnote(void *p, void *ra, int kind)
{
	if(nseminfo < Nseminfo){
		seminfo[nseminfo].p = p;
		seminfo[nseminfo].ra = ra;
		seminfo[nseminfo].kind = kind;
		nseminfo++;
	}
}

static int
semkind(void *p)
{
	int i;

	for(i = nseminfo-1; i >= 0; i--)
		if(seminfo[i].p == p)
			return seminfo[i].kind;
	return 0;
}

static void*
semra(void *p)
{
	int i;

	for(i = nseminfo-1; i >= 0; i--)
		if(seminfo[i].p == p)
			return seminfo[i].ra;
	return nil;
}

#define TS(tid, o, ptr, val) do{ \
	int _t = (tid); \
	if(_t >= 0 && _t < Ntid){ \
		tstate[_t].op = (o); \
		tstate[_t].p = (void*)(uintptr)(ptr); \
		tstate[_t].v = (val); \
		tstate[_t].stamp = nsec(); \
	} \
}while(0)

/* Called from the LKL CPU handoff; only stores, never enters the kernel. */
static volatile int g_owner;
static void *cpusem;

void
lklhost_cpusem(void *p)
{
	cpusem = p;
}

/*
 * Ring of handoff transitions, dumped by the watchdog.  Entries are tiny
 * and appended with plain stores; a racing writer can only produce a
 * slightly torn sample, never perturb the handoff.
 */
enum { Ntrace = 256 };
typedef struct Trace Trace;
struct Trace {
	int	tid;
	int	op;
	int	a;
	int	b;
};
static Trace trace[Ntrace];
static volatile int trhead;

void
lklhost_note(int op, void *p, int v)
{
	int i;

	if(op == 'G' || op == 'O')
		g_owner = (int)(uintptr)p;
	else if(op == 'P')
		g_owner = 0;
	TS(curtid(), op, p, v);
	i = trhead++;
	trace[i % Ntrace].tid = curtid();
	trace[i % Ntrace].op = op;
	trace[i % Ntrace].a = (int)(uintptr)p;
	trace[i % Ntrace].b = v;
}

int
lkl_printf(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprint(buf, sizeof buf, (char*)fmt, ap);
	va_end(ap);
	if(n > 0)
		say(buf, n);
	return n;
}

void
lkl_bug(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprint(buf, sizeof buf, (char*)fmt, ap);
	va_end(ap);
	if(n > 0)
		say(buf, n);
	say("lkl: bug\n", 10);
	_exits("lkl bug");
}

static void
hprint(const char *str, int len)
{
	HDBG("K: ");
	say((char*)str, len);
}

static void
hpanic(void)
{
	HDBG("host: panic called\n");
	say("lkl: panic\n", 11);
	_exits("lkl panic");
}

/* ---- semaphores ---- */

static struct lkl_sem*
hsem_alloc(int count)
{
	Sem *s;

	if((s = mallocz(sizeof *s, 1)) == nil)
		return nil;
	s->count = count;
	semnote(s, RA(), 's');
	BDBG("sem_alloc %p count %d from %p\n", s, count, RA());
	return (struct lkl_sem*)s;
}

static void
hsem_free(struct lkl_sem *sem)
{
	BDBG("sem_free %p from %p\n", sem, RA());
	free(sem);
}

static void
hsem_up(struct lkl_sem *sem)
{
	Sem *s = (Sem*)sem;

	BDBG("T%d up %p c%d from %p\n", curtid(), s, s->count, RA());
	TS(curtid(), 'U', s, s->count);
	if(s == cpusem)
		lklhost_note('U', s, s->count);
	semrelease(&s->count, 1);
}

static void
hsem_down(struct lkl_sem *sem)
{
	Sem *s = (Sem*)sem;

	TS(curtid(), 'D', s, s->count);
	if(s == cpusem)
		lklhost_note('D', s, s->count);
	/* semacquire returns < 0 if a note interrupted the wait */
	while(semacquire(&s->count, 1) < 0)
		;
	TS(curtid(), 'd', s, s->count);
	if(s == cpusem)
		lklhost_note('d', s, s->count);
}

/* ---- mutexes ---- */

static struct lkl_mutex*
hmut_alloc(int recursive)
{
	Mut *m;

	if((m = mallocz(sizeof *m, 1)) == nil)
		return nil;
	m->recursive = recursive;
	m->sem = 1;
	semnote(m, RA(), 'm');
	return (struct lkl_mutex*)m;
}

static void
hmut_free(struct lkl_mutex *mutex)
{
	free(mutex);
}

static void
hmut_lock(struct lkl_mutex *mutex)
{
	Mut *m = (Mut*)mutex;
	int tid = curtid();

	if(m->recursive){
		lock(&m->lk);
		if(m->owner == tid){
			m->count++;
			unlock(&m->lk);
			return;
		}
		unlock(&m->lk);
	}
	TS(tid, 'L', m, m->sem);
	while(semacquire(&m->sem, 1) < 0)
		;
	TS(tid, 'l', m, m->sem);
	lock(&m->lk);
	m->owner = tid;
	m->count = 1;
	unlock(&m->lk);
}

static void
hmut_unlock(struct lkl_mutex *mutex)
{
	Mut *m = (Mut*)mutex;

	lock(&m->lk);
	if(--m->count > 0){
		unlock(&m->lk);
		return;
	}
	m->owner = 0;
	/* Publish the permit before dropping the meta-lock, so a waiter that
	 * observes owner==0 can never find the permit missing. */
	semrelease(&m->sem, 1);
	TS(curtid(), 'X', m, m->count);
	unlock(&m->lk);
}

/* ---- threads ---- */

extern void harvey_thread_tramp(void *sp, void *arg, void (*f)(void*));

void
hthread_entry(void *arg, void (*f)(void*))
{
	TS(curtid(), 'R', nil, 0);
	HDBG("thread %d enter\n", curtid());
	f(arg);
	tls_cleanup(curtid());
	_exits(nil);
}

static int
curtid(void)
{
	uintptr sp;
	Sthdr *h;

	__asm__ volatile("mov %0, sp" : "=r"(sp));
	h = (Sthdr*)(sp & ~(uintptr)(Threadstack-1));
	if(h->magic == Stackmagic)
		return h->id;
	return Mainid;
}

static void
tls_cleanup(int tid)
{
	int i, j;
	void (*dtor)(void*);

	lock(&tlslk);
	for(i = 0; i < ntlskeys; i++){
		dtor = tlskeys[i].dtor;
		for(j = 0; j < Ntid; j++)
			if(tlsval[i][j].tid == tid){
				void *v = tlsval[i][j].val;
				tlsval[i][j].tid = 0;
				tlsval[i][j].val = nil;
				if(dtor != nil && v != nil){
					unlock(&tlslk);
					dtor(v);
					lock(&tlslk);
				}
				break;
			}
	}
	unlock(&tlslk);
}

static lkl_thread_t
hthread_create(void (*f)(void*), void *arg)
{
	uchar *raw, *st;
	Sthdr *h;
	int pid, id, i;

	if((raw = mallocz(2*Threadstack, 1)) == nil)
		return 0;
	st = (uchar*)(((uintptr)raw + Threadstack - 1) & ~(uintptr)(Threadstack-1));
	lock(&thlk);
	id = nexttid++;
	unlock(&thlk);
	pid = rfork(RFPROC|RFMEM);
	if(pid < 0){
		free(raw);
		return 0;
	}
	if(pid == 0){
		h = (Sthdr*)st;
		h->magic = Stackmagic;
		h->id = id;
		harvey_thread_tramp((void*)(st + Threadstack - 16), arg, f);
		_exits("tramp returned");
	}
	lock(&thlk);
	for(i = 0; i < Ntid; i++)
		if(!threads[i].used){
			threads[i].used = 1;
			threads[i].id = id;
			threads[i].pid = pid;
			break;
		}
	unlock(&thlk);
	HDBG("thread %d pid %d f %p from %p\n", id, pid, f, RA());
	return (lkl_thread_t)id;
}

/*
 * Host interrupt source.  A #P device node has a "wait" file: opening it
 * routes the GIC line to this process, and each read blocks until the
 * device raises it.  Forward every edge to the LKL guest through
 * lkl_trigger_irq(); the guest's driver reads and clears the controller's
 * status, which deasserts the level line.
 */
typedef struct Irqsrc Irqsrc;
struct Irqsrc {
	char	node[64];
	int	irq;
};

static void
irqwatch(void *x)
{
	Irqsrc *s = x;
	char path[96], buf[32], msg[128];
	int fd, n;

	for(;;){
		snprint(path, sizeof path, "#P/%s/wait", s->node);
		if((fd = open(path, OREAD)) < 0){
			n = snprint(msg, sizeof msg, "lklhost: irq %d: open %s: %r\n",
				    s->irq, path);
			write(2, msg, n);
			sleep(100);
			continue;
		}
		while((n = read(fd, buf, sizeof buf)) > 0)
			lkl_trigger_irq(s->irq);
		close(fd);
	}
}

int
lklhost_irqthread(char *node, int irq)
{
	Irqsrc *s;

	if((s = mallocz(sizeof *s, 1)) == nil)
		return -1;
	snprint(s->node, sizeof s->node, "%s", node);
	s->irq = irq;
	if(hthread_create(irqwatch, s) == 0){
		free(s);
		return -1;
	}
	return 0;
}

static void
hthread_detach(void)
{
}

static void
hthread_exit(void)
{
	tls_cleanup(curtid());
	_exits(nil);
}

static int
take_reap(lkl_thread_t tid)
{
	int i;

	lock(&reaplk);
	for(i = 0; i < nreap; i++)
		if((lkl_thread_t)reap[i] == tid){
			reap[i] = reap[--nreap];
			unlock(&reaplk);
			return 1;
		}
	unlock(&reaplk);
	return 0;
}

static void
add_reap(int pid)
{
	lock(&reaplk);
	if(nreap < Nreap)
		reap[nreap++] = pid;
	unlock(&reaplk);
}

static int
hthread_join(lkl_thread_t tid)
{
	int i, pid, want;

	/* thread_join(self) is not a deadlock in the LKL host ABI; the POSIX
	 * host returns an error and the kernel shutdown path ignores it. */
	if(tid == (lkl_thread_t)curtid())
		return -1;

	lock(&thlk);
	want = -1;
	for(i = 0; i < Ntid; i++)
		if(threads[i].used && (lkl_thread_t)threads[i].id == tid){
			want = threads[i].pid;
			threads[i].used = 0;
			break;
		}
	unlock(&thlk);
	if(want < 0)
		return -1;

	for(;;){
		if(take_reap(want))
			return 0;
		pid = waitpid();
		if(pid < 0)
			return -1;
		if(pid == want)
			return 0;
		add_reap(pid);
	}
}

static lkl_thread_t
hthread_self(void)
{
	return (lkl_thread_t)curtid();
}

static int
hthread_equal(lkl_thread_t a, lkl_thread_t b)
{
	return a == b;
}

static void*
hthread_stack(unsigned long *size)
{
	if(size != nil)
		*size = 0;
	return nil;
}

/* ---- TLS ---- */

static struct lkl_tls_key*
htls_alloc(void (*destructor)(void*))
{
	struct lkl_tls_key *k;

	lock(&tlslk);
	if(ntlskeys >= Ntls){
		unlock(&tlslk);
		return nil;
	}
	k = &tlskeys[ntlskeys];
	k->id = ntlskeys++;
	k->dtor = destructor;
	unlock(&tlslk);
	return k;
}

static void
htls_free(struct lkl_tls_key *key)
{
	/* keys are not reused; slots are small and few */
	USED(key);
}

static int
htls_set(struct lkl_tls_key *key, void *data)
{
	int i, tid = curtid();
	int slot = -1;

	lock(&tlslk);
	for(i = 0; i < Ntid; i++){
		if(tlsval[key->id][i].tid == tid){
			tlsval[key->id][i].val = data;
			unlock(&tlslk);
			return 0;
		}
		if(slot < 0 && tlsval[key->id][i].tid == 0)
			slot = i;
	}
	if(slot >= 0){
		tlsval[key->id][slot].tid = tid;
		tlsval[key->id][slot].val = data;
	}
	unlock(&tlslk);
	return slot < 0 ? -1 : 0;
}

static void*
htls_get(struct lkl_tls_key *key)
{
	int i, tid = curtid();
	void *v = nil;

	lock(&tlslk);
	for(i = 0; i < Ntid; i++)
		if(tlsval[key->id][i].tid == tid){
			v = tlsval[key->id][i].val;
			break;
		}
	unlock(&tlslk);
	return v;
}

/* ---- memory ---- */

static void*
hmem_alloc(unsigned long size)
{
	return mallocz(size, 1);
}

static void
hmem_free(void *p)
{
	free(p);
}

/*
 * Guest RAM arena.  LKL takes its whole mem= region from page_alloc once,
 * and every DMA buffer is inside that region.  Use the physically
 * contiguous #P/dma segment so a guest pointer can be translated to a real
 * device address.  The kernel maps the "dma" physseg uncached, so the DMA
 * path needs no cache maintenance.
 */
static uchar *lklram;
static u64 lklrampa, lklramsize, lklramoff;

static u64 pnpnum(char*, char*);
unsigned long lkl_host_dma_pa(void*, unsigned long);

/*
 * Allocate a chunk of the arena from the kernel's bump allocator and
 * return its physical base.  Each LKL family instance takes a disjoint
 * chunk so two guests can run at once.
 */
static int
dmaalloc(u64 size, u64 align, u64 *pa)
{
	char buf[64];
	int fd, n;

	if((fd = open("#P/dma/alloc", ORDWR)) < 0)
		return -1;
	snprint(buf, sizeof buf, "%llud %llud\n", size, align);
	if(write(fd, buf, strlen(buf)) != strlen(buf)){
		close(fd);
		return -1;
	}
	n = pread(fd, buf, sizeof buf - 1, 0);
	close(fd);
	if(n <= 0)
		return -1;
	buf[n] = 0;
	*pa = strtoull(buf, nil, 0);
	return 0;
}

static void
raminit(unsigned long want)
{
	u64 base, size, pa;
	void *va;

	if(lklram != nil)
		return;
	base = pnpnum("dma", "base");
	size = pnpnum("dma", "size");
	if(base == ~0ULL || size == ~0ULL || size < 64*1024*1024){
		print("host: no dma arena (base %#llux size %#llux)\n",
		      base, size);
		return;
	}
	/*
	 * Map the whole segment so any allocated chunk has a virtual
	 * address; use only [pa, pa+want) of it.
	 */
	va = segattach(0, "dma", 0, size);
	if(va == (void*)-1){
		print("host: segattach dma %#llux: %r\n", size);
		return;
	}
	pa = base;
	if(want != 0 && want < size){
		if(dmaalloc(want, 0x200000, &pa) < 0 ||
		   pa < base || pa + want > base + size){
			print("host: dma alloc %lud failed; using whole arena\n",
			      want);
			pa = base;
			want = 0;
		}
	}
	if(want == 0)
		want = size;
	lklram = va + (pa - base);
	lklrampa = pa;
	lklramsize = want;
	print("host: arena va %p pa %#llux size %#llux%s\n",
	      lklram, pa, want, pa == base ? "" : " (chunk)");
}

/*
 * Cache maintenance for the guest's DMA buffers.  op 0=clean (CPU wrote),
 * 1=invalidate (device wrote), 2=both.
 */
void
lkl_host_dma_sync(unsigned long pa, unsigned long size, int op)
{
	static char *name[] = { "clean", "inval", "cleaninval" };
	static int fd[3] = { -1, -1, -1 };
	char buf[64], path[32];

	if(size == 0 || op < 0 || op > 2)
		return;
	if(fd[op] < 0){
		snprint(path, sizeof path, "#P/dma/%s", name[op]);
		if((fd[op] = open(path, OWRITE)) < 0)
			return;
	}
	snprint(buf, sizeof buf, "0x%lux 0x%lux\n", pa, size);
	pwrite(fd[op], buf, strlen(buf), 0);
}

/*
 * Map the guest arena before any host thread exists, so every RFMEM
 * child inherits the segment.  A segattach done later, from a child,
 * is not visible in the main process's segment table.
 */
void
lklhost_raminit(unsigned long want)
{
	raminit(want);
}

static void*
hpage_alloc(unsigned long size)
{
	uchar *p, *a;

	HDBG("host: page_alloc %lud\n", size);
	raminit(0);
	if(lklram != nil){
		u64 off = (lklramoff + Page - 1) & ~(u64)(Page - 1);

		if(off + size <= lklramsize){
			p = lklram + off;
			lklramoff = off + size;
			print("host: guest ram va %p dma %#llux size %lud\n",
			      p, lkl_host_dma_pa(p, size), size);
			memset(p, 0, size);
			return p;
		}
		print("host: dma arena full: want %lud, used %llud\n",
		      size, lklramoff);
	}
	if((p = mallocz(size + Page + 16, 1)) == nil){
		print("host: page_alloc %lud failed\n", size);
		return nil;
	}
	a = (uchar*)(((uintptr)p + Page + 15) & ~(uintptr)(Page-1));
	((uintptr*)a)[-1] = (uintptr)p;
	return a;
}

static void
hpage_free(void *p, unsigned long size)
{
	USED(size);
	if(p == nil)
		return;
	if(lklram != nil && (uchar*)p >= lklram &&
	   (uchar*)p < lklram + lklramsize)
		return;		/* arena is bump-allocated, never freed */
	free((void*)((uintptr*)p)[-1]);
}

/*
 * Device address of a guest buffer, for arch/lkl's platform DMA path.
 */
unsigned long
lkl_host_dma_pa(void *va, unsigned long size)
{
	uintptr p = (uintptr)va;

	if(lklram != nil && p >= (uintptr)lklram &&
	   p + size <= (uintptr)lklram + lklramsize)
		return lklrampa + (p - (uintptr)lklram);
	return p;
}

static unsigned long long
htime(void)
{
	return (unsigned long long)nsec();
}

/* ---- timers ---- */

static void
htimer_thread(void *x)
{
	Timer *t = x;
	vlong d, lastbeat = 0;

	for(;;){
		if(nsec() - lastbeat > 500*1000*1000){
			lastbeat = nsec();
			HDBG("beat\n");
		}
		lock(&t->lk);
		if(t->stop){
			unlock(&t->lk);
			/* the timer thread owns t from here */
			free(t);
			_exits(nil);
		}
		if(!t->armed){
			unlock(&t->lk);
			/* Idle: still notice a new arming quickly. */
			sleep(1);
			continue;
		}
		d = t->deadline - nsec();
		if(d > 0){
			unlock(&t->lk);
			/* Sleep until the deadline (kill(1) ticks) instead of
			 * sampling once a second: the guest must see its
			 * clockevent at the requested time. */
			if(d > 1000000)
				sleep((d + 999999) / 1000000);
			else
				sleep(1);
			continue;
		}
		t->armed = 0;
		unlock(&t->lk);
		HDBG("fire\n");
		TS(curtid(), 'F', t, 0);
		t->fn();
		TS(curtid(), 'f', t, 0);
		HDBG("fired\n");
	}
	_exits(nil);
}

/*
 * Independent liveness probe: sleeps and prints, touching no LKL state.
 * If this stops, sleep() itself has stopped for the whole group.
 */
static void
hwatchdog(void *arg)
{
	USED(arg);
	for(;;){
		int i;
		char *k;

		int h, n;

		sleep(1000);
		if(!hostdebug)
			continue;
		USED(k);

		hdbg("own=%d cpusem=%p st:", g_owner, cpusem);
		for(i = 0; i < Ntid; i++){
			if(tstate[i].op == 0)
				continue;
			hdbg(" %d%c%p/%d", i, tstate[i].op,
			     tstate[i].p, tstate[i].v);
			if(semkind(tstate[i].p) == 's')
				hdbg("c%d@%p", ((Sem*)tstate[i].p)->count,
				     semra(tstate[i].p));
			hdbg("/%lud", (ulong)((nsec()-tstate[i].stamp)/1000000));
		}
		hdbg("\ntr:");
		h = trhead;
		for(n = (h > 60 ? h-60 : 0); n < h; n++){
			Trace *t = &trace[n % Ntrace];
			hdbg(" %d%c%d/%d", t->tid, t->op, t->a, t->b);
		}
		say("\n", 1);
	}
}

static int wdstarted;

static void*
htimer_alloc(void (*fn)(void))
{
	Timer *t;

	if((t = mallocz(sizeof *t, 1)) == nil)
		return nil;
	t->fn = fn;
	if(!wdstarted){
		wdstarted = 1;
		hthread_create(hwatchdog, nil);
	}
	t->tid = hthread_create(htimer_thread, t);
	if(t->tid == 0){
		free(t);
		return nil;
	}
	return t;
}

static int
htimer_set_oneshot(void *timer, unsigned long delta)
{
	Timer *t = timer;
	vlong now;
	uvlong far;

	HDBG("arm %lud\n", delta);
	now = nsec();
	if(now < 0)
		now = 0;
	lock(&t->lk);
	/*
	 * LKL stops the tick by programming KTIME_MAX (NO_HZ_IDLE).  A signed
	 * now + delta overflows into the past, so the timer would fire
	 * immediately, forever.  Treat anything past the representable
	 * deadline as "no event scheduled".
	 */
	far = (uvlong)~0ULL >> 1;	/* INT64_MAX */
	if((uvlong)delta > far - (uvlong)now)
		t->armed = 0;
	else{
		t->deadline = now + (vlong)delta;
		t->armed = 1;
	}
	unlock(&t->lk);
	return 0;
}

static void
htimer_free(void *timer)
{
	Timer *t = timer;

	lock(&t->lk);
	t->stop = 1;
	t->armed = 0;
	unlock(&t->lk);
	/*
	 * The timer thread owns the Timer from here and frees it just before
	 * exiting.  A shutdown invoked from the callback itself (self-join)
	 * must not free memory the thread is about to touch.
	 */
	if((lkl_thread_t)curtid() == t->tid)
		return;
	hthread_join(t->tid);
}

/* ---- I/O, MMIO, jmp_buf ---- */

/*
 * A kernel driver reaches registers through ioremap() + iomem_access().
 * Map the #P window of the node the service owns with segattach, then serve
 * both hooks out of the mapping.  The window is device memory, mapped by
 * the kernel; the guest touches it through these two entry points only.
 */
enum { Nmmio = 32 };
static struct {
	u64	pa;
	u64	len;
	uchar	*va;
} mmio[Nmmio];
static int nmmio;

static u64
pnpnum(char *node, char *file)
{
	char path[80], buf[64];
	int fd, n;
	u64 v;

	snprint(path, sizeof path, "#P/%s/%s", node, file);
	if((fd = open(path, OREAD)) < 0)
		return ~0ULL;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0)
		return ~0ULL;
	buf[n] = 0;
	v = strtoull(buf, nil, 0);
	return v;
}

/*
 * lklhost_mapdev - map a #P node's register window.
 * Returns 0 and the window's physical base and size.  The service is
 * expected to have claimed the node first.
 */
int
lklhost_mapdev(char *node, u64 *base, u64 *size)
{
	u64 pa, len, grant;
	void *va;

	if(nmmio >= Nmmio)
		return -1;
	pa = pnpnum(node, "reg");
	len = pnpnum(node, "size");
	if(pa == ~0ULL || len == ~0ULL || len == 0)
		return -1;
	/*
	 * segattach grants the 2 MiB block that contains the device, not the
	 * device window; the window starts at pa & (2 MiB - 1) inside it.
	 */
	grant = (len + 0x1fffffULL) & ~0x1fffffULL;
	va = segattach(0, node, 0, grant);
	if(va == (void*)-1)
		return -1;
	va = (uchar*)va + (pa & 0x1fffffULL);
	mmio[nmmio].pa = pa;
	mmio[nmmio].len = len;
	mmio[nmmio].va = va;
	nmmio++;
	*base = pa;
	*size = len;
	return 0;
}

static int hiomem_access(const volatile void*, void*, int, int);
static void *hioremap(long, int);

int
lklhost_iomem(u64 pa, void *val, int size, int write)
{
	void *va;

	if((va = hioremap((long)pa, size)) == nil)
		return -1;
	return hiomem_access(va, val, size, write);
}

static void*
hioremap(long addr, int size)
{
	int i;

	USED(size);
	for(i = 0; i < nmmio; i++)
		if((u64)addr >= mmio[i].pa && (u64)addr < mmio[i].pa + mmio[i].len)
			return mmio[i].va + ((u64)addr - mmio[i].pa);
	return nil;
}

static int
hiomem_access(const volatile void *addr, void *val, int size, int write)
{
	int i;
	uchar *p = (uchar*)addr;

	for(i = 0; i < nmmio; i++){
		if(p >= mmio[i].va && p + size <= mmio[i].va + mmio[i].len){
			/*
			 * Use a single volatile store/load of the access
			 * width.  A memmove may split a 16-bit register into
			 * two byte accesses; the SDHCI command register is
			 * triggered by the 16-bit write and a split write
			 * never starts the command.
			 */
			if(write){
				switch(size){
				case 1: *(volatile u8*)p = *(u8*)val; break;
				case 2: *(volatile u16*)p = *(u16*)val; break;
				case 4: *(volatile u32*)p = *(u32*)val; break;
				case 8: *(volatile u64*)p = *(u64*)val; break;
				default: memmove((void*)p, val, size); break;
				}
			}else{
				switch(size){
				case 1: *(u8*)val = *(volatile u8*)p; break;
				case 2: *(u16*)val = *(volatile u16*)p; break;
				case 4: *(u32*)val = *(volatile u32*)p; break;
				case 8: *(u64*)val = *(volatile u64*)p; break;
				default: memmove(val, (void*)p, size); break;
				}
			}
			/*
			 * LKL's readl/writel go through this function rather
			 * than a volatile load/store, so the compiler and the
			 * normal-memory copy are not ordered with respect to
			 * the device.  Order every access before the guest
			 * continues; the eMMC controller latches the command
			 * register and needs the argument and transfer mode
			 * to have arrived first.
			 */
			__asm__ volatile("dsb sy" ::: "memory");
			return 0;
		}
	}
	return -1;
}

static void
hjmp_set(struct lkl_jmp_buf *jb, void (*f)(void))
{
	int r;

	TS(curtid(), 'J', jb, 0);
	r = setjmp(*(jmp_buf*)jb);
	TS(curtid(), 'j', jb, r);
	if(r == 0)
		f();
	TS(curtid(), 'k', jb, r);
}

static void
hjmp_longjmp(struct lkl_jmp_buf *jb, int val)
{
	TS(curtid(), 'L', jb, val);
	longjmp(*(jmp_buf*)jb, val);
}

static void*
hmemcpy(void *d, const void *s, unsigned long n)
{
	return memcpy(d, (void*)s, n);
}

static void*
hmemset(void *s, int c, unsigned long n)
{
	return memset(s, c, n);
}

static void*
hmemmove(void *d, const void *s, unsigned long n)
{
	return memmove(d, (void*)s, n);
}

static void*
hmmap(void *addr, unsigned long size, enum lkl_prot prot)
{
	USED(addr);
	USED(prot);
	return mallocz(size, 1);
}

static int
hmunmap(void *addr, unsigned long size)
{
	USED(size);
	free(addr);
	return 0;
}

/*
 * Board resource control.  The guest's regulator/clk/reset/gpio/power
 * drivers call this with a native service path; the service owns the
 * hardware and executes the request.  The guest decides when, the
 * native side decides how.
 */
/*
 * /srv/<name> is a 9P server file, not a directory.  A resource path of
 * the form /srv/<name>/<file> is mounted once at /mnt/lkl-<name> and the
 * remaining path is opened there.  #P paths are opened directly.
 */
enum { Nresmnt = 8 };
static Lock reslk;
static char resmnt[Nresmnt][32];
static int nresmnt;

static int
resourcepath(char *path, char *out, int outlen)
{
	char svc[32], *rest, *slash;
	int i, fd;

	if(strncmp(path, "/srv/", 5) != 0)
		return 0;
	rest = path + 5;
	slash = strchr(rest, '/');
	if(slash == nil)
		return 0;
	*slash = 0;
	snprint(svc, sizeof svc, "%s", rest);
	rest = slash + 1;
	lock(&reslk);
	for(i = 0; i < nresmnt; i++)
		if(strcmp(resmnt[i], svc) == 0)
			break;
	if(i == nresmnt && nresmnt < Nresmnt){
		char mp[64], srv[64];

		snprint(mp, sizeof mp, "/mnt/lkl-%s", svc);
		snprint(srv, sizeof srv, "/srv/%s", svc);
		create(mp, OREAD, DMDIR|0755);
		fd = open(srv, ORDWR);
		if(fd >= 0){
			mount(fd, -1, mp, MREPL|MCREATE, "");
			close(fd);
		}
		snprint(resmnt[nresmnt++], sizeof resmnt[0], "%s", svc);
		i = nresmnt - 1;
	}
	unlock(&reslk);
	if(i >= nresmnt)
		return -1;
	snprint(out, outlen, "/mnt/lkl-%s/%s", svc, rest);
	return 0;
}

static int
hresource(const char *path, int wr, const char *cmd, char *buf, int len)
{
	char pbuf[128], rpath[160];
	int fd, n;

	if(path == nil)
		return -1;
	snprint(pbuf, sizeof pbuf, "%s", (char*)path);
	if(resourcepath(pbuf, rpath, sizeof rpath) < 0)
		return -1;
	if(rpath[0] != 0)
		snprint(pbuf, sizeof pbuf, "%s", rpath);
	print("host: resource %s %s\n", pbuf, wr ? cmd : "read");
	if((fd = open(pbuf, wr ? OWRITE : OREAD)) < 0){
		print("host: resource open %s: %r\n", pbuf);
		return -1;
	}
	if(wr){
		n = strlen(cmd);
		n = write(fd, (void*)cmd, n);
	}else
		n = read(fd, buf, len);
	close(fd);
	return n;
}

/* ---- host-layer self-test ------------------------------------------- */

/*
 * Deterministic check of the primitives LKL's scheduler is built on:
 * a semaphore ping-pong between host threads and a contended mutex.
 * If either of these loses a wakeup or a lock, every guest handoff is
 * suspect, so run this before blaming the kernel.
 */
typedef struct Bounce Bounce;
struct Bounce {
	struct lkl_sem	*wait;
	struct lkl_sem	*post;
	struct lkl_sem	*done;
	int		n;
};

static void
sembounce(void *x)
{
	Bounce *b = x;
	int i;

	for(i = 0; i < b->n; i++){
		hsem_down(b->wait);
		hsem_up(b->post);
	}
	hsem_up(b->done);
}

typedef struct Mstress Mstress;
struct Mstress {
	struct lkl_mutex	*m;
	struct lkl_sem		*done;
	int			*counter;
	int			n;
};

static void
mutstress(void *x)
{
	Mstress *s = x;
	int i;

	for(i = 0; i < s->n; i++){
		hmut_lock(s->m);
		(*s->counter)++;
		hmut_unlock(s->m);
	}
	hsem_up(s->done);
}

typedef struct Waiter Waiter;
struct Waiter {
	struct lkl_sem	*sem;
	vlong		delay;
};

/* Post after a delay, so the other side is already blocked. */
static void
latepost(void *x)
{
	Waiter *w = x;
	vlong t0 = nsec();

	lkl_printf("hostselftest: latepost delay=%lld\n", w->delay);
	while(nsec() - t0 < w->delay)
		;
	hsem_up(w->sem);
}

/*
 * Can a spinning host thread be preempted?  LKL's idle loop runs in user
 * mode without issuing a syscall, so if the host cannot preempt it, every
 * other thread (notably the timer) starves.  The spinner is bounded so a
 * failure is reported instead of hanging the test.
 */
static void
spinthread(void *x)
{
	volatile int i;

	for(i = 0; i < 300000000; i++)
		;
	hsem_up((struct lkl_sem*)x);
}

/*
 * jmp_buf_set/longjmp is the primitive LKL's host-thread switch is built on:
 * jmp_buf_set calls f() with a jump-back point; a callee longjmps back and
 * setjmp returns nonzero.  Exercise it directly.
 */
static struct lkl_jmp_buf gjb;
static int jmp_ok;

static void
jmp_target(void)
{
	jmp_ok = 1;
	hjmp_longjmp(&gjb, 1);
}

void
lklhost_selftest(void)
{
	static Bounce b;
	static Mstress ms;
	static Waiter w;
	static int counter;
	struct lkl_sem *a, *c, *done;
	struct lkl_mutex *m;
	vlong t0, dt;
	int i, n = 20000;

	jmp_ok = 0;
	hjmp_set(&gjb, jmp_target);
	lkl_printf("hostselftest: jmp_buf %s\n", jmp_ok ? "PASS" : "FAIL");

	t0 = nsec();
	sleep(1000);
	dt = (nsec() - t0) / 1000000;
	lkl_printf("hostselftest: clock sleep(1000) %lldms %s\n",
		   dt, dt > 900 && dt < 3000 ? "PASS" : "FAIL");

	/*
	 * Preemption: while the spinner runs, the main thread must still be
	 * able to run a 10ms sleep.  If creation returns only after the
	 * spinner finishes, the host does not preempt user code.
	 */
	a = hsem_alloc(0);
	if(a == nil){
		lkl_printf("hostselftest: sem_alloc failed\n");
		return;
	}
	t0 = nsec();
	if(hthread_create(spinthread, a) == 0)
		lkl_printf("hostselftest: spinner create failed\n");
	sleep(10);
	dt = (nsec() - t0) / 1000000;
	hsem_down(a);
	/* One 100ms scheduling quantum is the expected delay. */
	lkl_printf("hostselftest: preempt sleep-returns %lldms %s\n",
		   dt, dt < 500 ? "PASS" : "FAIL");
	hsem_free(a);

	/* Block first, post later: exercises a real wakeup. */
	a = hsem_alloc(0);
	if(a == nil){
		lkl_printf("hostselftest: sem_alloc failed\n");
		return;
	}
	w.sem = a;
	w.delay = 100*1000*1000;
	if(hthread_create(latepost, &w) == 0)
		lkl_printf("hostselftest: latepost create failed\n");
	lkl_printf("hostselftest: block/wake count before=%d\n", ((Sem*)a)->count);
	t0 = nsec();
	hsem_down(a);
	dt = (nsec() - t0) / 1000000;
	lkl_printf("hostselftest: block/wake count after=%d\n", ((Sem*)a)->count);
	lkl_printf("hostselftest: block/wake %lldms %s\n",
		   dt, dt < 10000 ? "PASS" : "FAIL");
	hsem_free(a);

	a = hsem_alloc(0);
	c = hsem_alloc(0);
	done = hsem_alloc(0);
	if(a == nil || c == nil || done == nil){
		lkl_printf("hostselftest: sem_alloc failed\n");
		return;
	}
	b.wait = a;
	b.post = c;
	b.done = done;
	b.n = n;
	if(hthread_create(sembounce, &b) == 0)
		lkl_printf("hostselftest: thread_create failed\n");
	t0 = nsec();
	for(i = 0; i < n; i++){
		hsem_up(a);
		hsem_down(c);
	}
	hsem_down(done);
	dt = (nsec() - t0) / 1000000;
	lkl_printf("hostselftest: sem bounce %d in %lldms %s\n",
		   n, dt, dt < 60000 ? "PASS" : "FAIL");
	hsem_free(a);
	hsem_free(c);
	hsem_free(done);

	m = hmut_alloc(0);
	done = hsem_alloc(0);
	if(m == nil || done == nil){
		lkl_printf("hostselftest: mutex alloc failed\n");
		return;
	}
	ms.m = m;
	ms.done = done;
	ms.counter = &counter;
	ms.n = n;
	for(i = 0; i < 4; i++)
		if(hthread_create(mutstress, &ms) == 0)
			lkl_printf("hostselftest: mutex thread_create failed\n");
	for(i = 0; i < 4; i++)
		hsem_down(done);
	lkl_printf("hostselftest: mutex %d/%d %s\n", counter, 4*n,
		   counter == 4*n ? "PASS" : "FAIL");
	hsem_free(done);
	hmut_free(m);
}

struct lkl_host_operations lkl_harvey_ops = {
	.virtio_devices		= nil,
	.print			= hprint,
	.panic			= hpanic,

	.sem_alloc		= hsem_alloc,
	.sem_free		= hsem_free,
	.sem_up			= hsem_up,
	.sem_down		= hsem_down,

	.mutex_alloc		= hmut_alloc,
	.mutex_free		= hmut_free,
	.mutex_lock		= hmut_lock,
	.mutex_unlock		= hmut_unlock,

	.thread_create		= hthread_create,
	.thread_detach		= hthread_detach,
	.thread_exit		= hthread_exit,
	.thread_join		= hthread_join,
	.thread_self		= hthread_self,
	.thread_equal		= hthread_equal,
	.thread_stack		= hthread_stack,

	.tls_alloc		= htls_alloc,
	.tls_free		= htls_free,
	.tls_set		= htls_set,
	.tls_get		= htls_get,

	.mem_alloc		= hmem_alloc,
	.mem_free		= hmem_free,
	.page_alloc		= hpage_alloc,
	.page_free		= hpage_free,

	.time			= htime,

	.timer_alloc		= htimer_alloc,
	.timer_set_oneshot	= htimer_set_oneshot,
	.timer_free		= htimer_free,

	.ioremap		= hioremap,
	.iomem_access		= hiomem_access,

	.jmp_buf_set		= hjmp_set,
	.jmp_buf_longjmp	= hjmp_longjmp,

	.memcpy			= hmemcpy,
	.memset			= hmemset,
	.memmove		= hmemmove,

	.mmap			= hmmap,
	.munmap			= hmunmap,

	.shmem_init		= nil,
	.shmem_mmap		= nil,

	.resource		= hresource,

	.pci_ops		= nil,
};
