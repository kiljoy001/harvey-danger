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

typedef struct Sem Sem;
struct Sem {
	QLock	l;
	Rendez	r;
	int	count;
};

typedef struct Mut Mut;
struct Mut {
	QLock	l;
	Rendez	r;
	int	owner;
	int	count;
	int	recursive;
};

typedef struct Timer Timer;
struct Timer {
	QLock	l;
	Rendez	r;
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

static int hostdebug = 1;

static void
say(char *s, int n)
{
	write(2, s, n);
}

#define HDBG(...) do{ if(hostdebug) print(__VA_ARGS__); }while(0)

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

	HDBG("host: sem_alloc %d\n", count);
	if((s = mallocz(sizeof *s, 1)) == nil)
		return nil;
	s->r.l = &s->l;
	s->count = count;
	return (struct lkl_sem*)s;
}

static void
hsem_free(struct lkl_sem *sem)
{
	free(sem);
}

static void
hsem_up(struct lkl_sem *sem)
{
	Sem *s = (Sem*)sem;

	qlock(&s->l);
	s->count++;
	rwakeup(&s->r);
	qunlock(&s->l);
}

static void
hsem_down(struct lkl_sem *sem)
{
	Sem *s = (Sem*)sem;

	qlock(&s->l);
	while(s->count == 0)
		rsleep(&s->r);
	s->count--;
	qunlock(&s->l);
}

/* ---- mutexes ---- */

static struct lkl_mutex*
hmut_alloc(int recursive)
{
	Mut *m;

	HDBG("host: mutex_alloc %d\n", recursive);
	if((m = mallocz(sizeof *m, 1)) == nil)
		return nil;
	m->r.l = &m->l;
	m->recursive = recursive;
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
	int pid = getpid();

	qlock(&m->l);
	if(m->recursive && m->owner == pid){
		m->count++;
		qunlock(&m->l);
		return;
	}
	while(m->owner != 0)
		rsleep(&m->r);
	m->owner = pid;
	m->count = 1;
	qunlock(&m->l);
}

static void
hmut_unlock(struct lkl_mutex *mutex)
{
	Mut *m = (Mut*)mutex;

	qlock(&m->l);
	if(--m->count == 0){
		m->owner = 0;
		rwakeup(&m->r);
	}
	qunlock(&m->l);
}

/* ---- threads ---- */

extern void harvey_thread_tramp(void *sp, void *arg, void (*f)(void*));

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

	HDBG("host: thread_create %p\n", f);
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
		HDBG("host: thread start %d\n", id);
		harvey_thread_tramp((void*)(st + Threadstack - 16), arg, f);
		tls_cleanup(id);
		_exits(nil);
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
	vlong d, ms;

	for(;;){
		qlock(&t->l);
		while(!t->armed && !t->stop)
			rsleep(&t->r);
		if(t->stop){
			qunlock(&t->l);
			break;
		}
		d = t->deadline - nsec();
		if(d <= 0){
			t->armed = 0;
			qunlock(&t->l);
			HDBG("host: timer fire\n");
			t->fn();
			continue;
		}
		qunlock(&t->l);
		ms = (d + 999999) / 1000000;
		if(ms > 5)
			ms = 5;		/* notice a re-arm within 5 ms */
		if(ms < 1)
			ms = 1;
		sleep(ms);
	}
	_exits(nil);
}

static void*
htimer_alloc(void (*fn)(void))
{
	Timer *t;

	HDBG("host: timer_alloc\n");
	if((t = mallocz(sizeof *t, 1)) == nil)
		return nil;
	t->r.l = &t->l;
	t->fn = fn;
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

	HDBG("host: timer arm %lud\n", delta);
	qlock(&t->l);
	t->deadline = nsec() + delta;
	t->armed = 1;
	rwakeup(&t->r);
	qunlock(&t->l);
	return 0;
}

static void
htimer_free(void *timer)
{
	Timer *t = timer;

	qlock(&t->l);
	t->stop = 1;
	t->armed = 0;
	rwakeup(&t->r);
	qunlock(&t->l);
	hthread_join(t->tid);
	free(t);
}

/* ---- I/O, MMIO, jmp_buf ---- */

static void*
hioremap(long addr, int size)
{
	USED(addr);
	USED(size);
	return nil;
}

static int
hiomem_access(const volatile void *addr, void *val, int size, int write)
{
	USED(addr);
	USED(val);
	USED(size);
	USED(write);
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
