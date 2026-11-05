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

enum {
	Page		= 4096,
	Threadstack	= 128*1024,
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
	semrelease(&s->count, 1);
}

static void
hsem_down(struct lkl_sem *sem)
{
	Sem *s = (Sem*)sem;

	BDBG("T%d down %p c%d from %p\n", curtid(), s, s->count, RA());
	/* semacquire returns < 0 if a note interrupted the wait */
	while(semacquire(&s->count, 1) < 0)
		;
	BDBG("T%d got %p\n", curtid(), s);
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
	BDBG("T%d mlock %p c%d from %p\n", tid, m, m->sem, RA());
	while(semacquire(&m->sem, 1) < 0)
		;
	BDBG("T%d mlok %p\n", tid, m);
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
	unlock(&m->lk);
	BDBG("T%d munlk %p\n", curtid(), m);
	semrelease(&m->sem, 1);
}

/* ---- threads ---- */

extern void harvey_thread_tramp(void *sp, void *arg, void (*f)(void*));

void
hthread_entry(void *arg, void (*f)(void*))
{
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

static void*
hpage_alloc(unsigned long size)
{
	uchar *p, *a;

	HDBG("host: page_alloc %lud\n", size);
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
	if(p != nil)
		free((void*)((uintptr*)p)[-1]);
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
		sleep(1);
		if(nsec() - lastbeat > 500*1000*1000){
			lastbeat = nsec();
			HDBG("beat\n");
		}
		lock(&t->lk);
		if(t->stop){
			unlock(&t->lk);
			break;
		}
		if(!t->armed){
			unlock(&t->lk);
			continue;
		}
		d = t->deadline - nsec();
		if(d > 0){
			unlock(&t->lk);
			continue;
		}
		t->armed = 0;
		unlock(&t->lk);
		HDBG("fire\n");
		t->fn();
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
	vlong last = 0;

	USED(arg);
	for(;;){
		sleep(1);
		if(nsec() - last > 500*1000*1000){
			last = nsec();
			HDBG("wd\n");
		}
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

	HDBG("arm %lud\n", delta);
	lock(&t->lk);
	t->deadline = nsec() + delta;
	t->armed = 1;
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
	hthread_join(t->tid);
	free(t);
}

/* ---- I/O, MMIO, jmp_buf ---- */

/*
 * A kernel driver reaches registers through ioremap() + iomem_access().
 * Map the #P window of the node the service owns with segattach, then serve
 * both hooks out of the mapping.  The window is device memory, mapped by
 * the kernel; the guest touches it through these two entry points only.
 */
enum { Nmmio = 8 };
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
			if(write)
				memmove((void*)p, val, size);
			else
				memmove(val, (void*)p, size);
			return 0;
		}
	}
	return -1;
}

static void
hjmp_set(struct lkl_jmp_buf *jb, void (*f)(void))
{
	if(setjmp(*(jmp_buf*)jb) == 0)
		f();
}

static void
hjmp_longjmp(struct lkl_jmp_buf *jb, int val)
{
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

	.pci_ops		= nil,
};
