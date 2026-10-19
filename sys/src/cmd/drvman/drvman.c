/* drvman: the board-agnostic device-driver manager.
 *
 * It enumerates the peripherals the kernel discovered from the device tree
 * (#P), consults the universal class->program registry below, and launches
 * one file server per present device class.  Nothing here is board-specific:
 * which classes exist comes from #P at runtime, and the registry is the same
 * for every board.  One kernel + one userland therefore boots any RK356x
 * board with no per-board build.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>

typedef struct Driver Driver;
struct Driver {
	char *class;	/* #P class name */
	char *prog;	/* /boot program that owns this class */
	char *srv;	/* /srv/<name> the program posts */
};

/* Universal class -> program registry.  One program serves every instance of
 * its class; the program discovers its instances itself through #P. */
static Driver drivers[] = {
	{ "uart",	"/boot/uartfs",	"uart" },
	{ "gpio",	"/boot/gpiofs",	"gpio" },
	{ nil,	nil,	nil },
};

static void
say(char *s)
{
	write(1, s, strlen(s));
}

static long
readfile(char *path, char *buf, long n)
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

static Driver *
finddriver(char *class)
{
	int i;

	for(i = 0; drivers[i].class != nil; i++)
		if(strcmp(drivers[i].class, class) == 0)
			return &drivers[i];
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

static int
launch(Driver *d)
{
	char *argv[2];
	int pid;

	argv[0] = d->prog;
	argv[1] = nil;
	pid = rfork(RFPROC|RFNOWAIT|RFFDG|RFNAMEG|RFENVG|RFREND|RFNOTEG);
	if(pid < 0)
		return -1;
	if(pid == 0){
		exec(argv[0], argv);
		_exits("exec");
	}
	return 0;
}

void
main(int argc, char **argv)
{
	uchar dirbuf[4096];
	char strs[512], path[128], cls[32], srvpath[64];
	char present[32][24];
	int npresent, fd, i, j;
	long n;
	Dir d;
	Driver *drv;

	USED(argc);
	USED(argv);
	npresent = 0;
	fd = open("#P", OREAD);
	if(fd < 0)
		_exits("no #P");
	n = read(fd, dirbuf, sizeof dirbuf);
	close(fd);
	if(n <= 0)
		_exits("empty #P");
	for(i = 0; i < n; i += GBIT16(dirbuf + i) + BIT16SZ){
		if(convM2D(dirbuf + i, GBIT16(dirbuf + i) + BIT16SZ, &d, strs) == 0)
			break;
		if(strcmp(d.name, ".") == 0)
			continue;
		snprint(path, sizeof path, "#P/%s/class", d.name);
		if(readfile(path, cls, sizeof cls) < 0)
			continue;
		for(j = 0; cls[j] != 0 && cls[j] != '\n'; j++)
			;
		cls[j] = 0;
		if(finddriver(cls) == nil)
			continue;
		for(j = 0; j < npresent; j++)
			if(strcmp(present[j], cls) == 0)
				break;
		if(j == npresent && npresent < (int)(sizeof present / sizeof present[0]))
			strcpy(present[npresent++], cls);
	}
	for(i = 0; i < npresent; i++){
		drv = finddriver(present[i]);
		snprint(srvpath, sizeof srvpath, "/srv/%s", drv->srv);
		if(launch(drv) < 0){
			say("HARVEY-ARM64 DRVMAN ");
			say(drv->class);
			say(" EXEC FAIL\n");
			continue;
		}
		if(waitfor(srvpath, 50) == 0){
			say("HARVEY-ARM64 DRVMAN ");
			say(drv->class);
			say(" PASS\n");
		}else{
			say("HARVEY-ARM64 DRVMAN ");
			say(drv->class);
			say(" FAIL\n");
		}
	}
	exits(nil);
}
