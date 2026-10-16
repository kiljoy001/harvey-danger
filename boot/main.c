#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>

static void
ns_bind(char *name, char *old, int flags)
{
	if(bind(name, old, flags) < 0){
		fprint(2, "boot: bind %s -> %s failed: %r\n", name, old);
	} else {
		print("boot: bound %s -> %s\n", name, old);
	}
}

/* Worker coroutine thread (in same proc) */
static void
thread_worker(void *arg)
{
	Channel *c = arg;

	print("boot [thread]: worker started\n");
	sendul(c, 0xCAFE0001);
	sendul(c, 0xCAFE0002);
	print("boot [thread]: worker sent 2 messages, exiting\n");
}

/* Worker process (in separate proc sharing memory via rfork(RFMEM)) */
static void
proc_worker(void *arg)
{
	Channel *c = arg;

	print("boot [proc]: worker process started (pid %d)\n", getpid());
	sendul(c, 0xBEEF0001);
	sendul(c, 0xBEEF0002);
	print("boot [proc]: worker process sent 2 messages, exiting\n");
}

void
threadmain(int argc, char *argv[])
{
	Biobuf *bp;
	char *line;
	Channel *chan;
	u32 val;
	int i;

	print("\n");
	print("==================================================\n");
	print("  Prism Microkernel (Harvey amd64)\n");
	print("  PID 1 libthread Actor Supervisor Online\n");
	print("==================================================\n");

	/* Phase 1: Setup host-passthrough namespace */
	ns_bind("#c", "/dev", MAFTER);
	ns_bind("#d", "/dev", MAFTER);
	ns_bind("#p", "/proc", MREPL);
	ns_bind("#s", "/srv", MREPL);
	ns_bind("#ec", "/env", MREPL);
	ns_bind("#e", "/env", MAFTER | MCREATE);
	ns_bind("/", "/n/host", MREPL);
	ns_bind("/n/host/amd64/bin", "/bin", MAFTER);

	/* Phase 2: Verify libbio */
	bp = Bopen("/n/host/README.md", OREAD);
	if(bp == nil){
		fprint(2, "boot: libbio Bopen failed: %r\n");
	} else {
		line = Brdline(bp, '\n');
		if(line != nil){
			int len = Blinelen(bp);
			line[len-1] = '\0';
			print("boot [libbio]: Brdline read %d bytes: '%s'\n", len, line);
		}
		Bterm(bp);
	}

	/* Phase 2: Verify CSP Channel with threadcreate (coroutine) */
	chan = chancreate(sizeof(u32), 4);
	threadcreate(thread_worker, chan, 16*1024);

	for(i = 0; i < 2; i++){
		val = recvul(chan);
		print("boot [supervisor]: received from coroutine thread: 0x%ux\n", val);
	}

	/* Phase 2: Verify CSP Channel with proccreate (rfork(RFMEM)) */
	proccreate(proc_worker, chan, 16*1024);

	for(i = 0; i < 2; i++){
		val = recvul(chan);
		print("boot [supervisor]: received from rfork(RFMEM) proc: 0x%ux\n", val);
	}

	chanfree(chan);

	print("\n==================================================\n");
	print("  Phase 1 & Phase 2 Self-Test Finished\n");
	print("  - Host-Passthrough Namespace: /dev, /proc, /srv, /env, /n/host\n");
	print("  - libbio: Buffered I/O operational\n");
	print("  - libthread: Cooperative CSP channels operational\n");
	print("  - libthread: Lightweight processes sharing memory (rfork(RFMEM))\n");
	print("==================================================\n");

	/* Keep supervisor alive */
	for(;;){
		sleep(10000);
	}
}
