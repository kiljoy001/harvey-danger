#include <u.h>
#include <libc.h>

#include "lifecycle.h"

enum {
	Maxdrivers = 64,
	Maxname = 33,
	Maxpath = 256,
};

typedef struct DrvProcess DrvProcess;
typedef struct DrvExitMessage DrvExitMessage;

struct DrvProcess {
	char name[Maxname];
	char manager[Maxpath];
	char path[Maxpath];
	int configured;
	int pid;
	uint generation;
	int laststatus;
};

struct DrvExitMessage {
	int slot;
	int pid;
	uint generation;
	int status;
};

static Lock driverlock;
static DrvLifecycle drivers[Maxdrivers];
static DrvProcess processes[Maxdrivers];
static uchar initialized[Maxdrivers];
static int exitpipe[2] = {-1, -1};

static int
validslot(int slot)
{
	return slot >= 0 && slot < Maxdrivers && initialized[slot];
}

static int
procexitstatus(Waitmsg *wait)
{
	return wait == nil || wait->msg == nil || wait->msg[0] == 0 ? 0 : 1;
}

static void
monitorproc(int slot, uint generation, char *manager, char *name, char *path, int startfd)
{
	DrvExitMessage event;
	Waitmsg *wm;
	char slottext[32], generationtext[32];
	int pid;

	close(exitpipe[0]);
	pid = rfork(RFPROC|RFFDG|RFNAMEG|RFENVG|RFREND|RFNOTEG);
	if(pid == 0){
		close(startfd);
		close(exitpipe[1]);
		snprint(slottext, sizeof slottext, "%d", slot);
		putenv("O9_DRIVER_SLOT", slottext);
		snprint(generationtext, sizeof generationtext, "%u", generation);
		putenv("O9_DRIVER_GENERATION", generationtext);
		putenv("O9_DRIVER_NAME", name);
		execl(path, path, name, generationtext, slottext, manager, nil);
		exits("exec");
	}
	write(startfd, &pid, sizeof pid);
	close(startfd);
	event.slot = slot;
	event.pid = pid;
	event.generation = generation;
	event.status = 1;
	if(pid >= 0){
		while((wm = wait()) != nil){
			if(wm->pid == pid){
				event.status = procexitstatus(wm);
				free(wm);
				break;
			}
			free(wm);
		}
	}
	write(exitpipe[1], &event, sizeof event);
	close(exitpipe[1]);
	exits(nil);
}

int
o9drvmaninit(int slot)
{
	DrvPolicy policy;
	int result;

	if(slot < 0 || slot >= Maxdrivers)
		return -1;
	if(exitpipe[0] < 0 && pipe(exitpipe) < 0)
		return -1;
	policy.startup_timeout_ms = 5000;
	policy.heartbeat_timeout_ms = 30000;
	policy.base_backoff_ms = 250;
	policy.max_backoff_ms = 30000;
	policy.max_restarts = 5;
	lock(&driverlock);
	result = drv_lifecycle_init(&drivers[slot], &policy);
	if(result == 0)
		initialized[slot] = 1;
	if(result == 0){
		memset(&processes[slot], 0, sizeof(processes[slot]));
		processes[slot].laststatus = -1;
	}
	unlock(&driverlock);
	return result;
}

int
o9drvmanapply(int slot, int type, uvlong generation, uvlong now, uvlong progress)
{
	DrvEvent event;
	int result;

	if(!validslot(slot))
		return DrvRejected;
	event.type = (DrvEventType)type;
	event.generation = (uint32_t)generation;
	event.now_ms = now;
	event.progress = progress;
	lock(&driverlock);
	result = drv_lifecycle_apply(&drivers[slot], &event);
	unlock(&driverlock);
	return result;
}

vlong
o9drvmanfield(int slot, int field)
{
	DrvLifecycle *driver;
	vlong result;

	if(!validslot(slot))
		return -1;
	lock(&driverlock);
	driver = &drivers[slot];
	switch(field){
	case 0:
		result = driver->state;
		break;
	case 1:
		result = driver->generation;
		break;
	case 2:
		result = driver->restarts;
		break;
	case 3:
		result = driver->desired_running;
		break;
	case 4:
		result = driver->deadline_ms;
		break;
	case 5:
		result = driver->retry_at_ms;
		break;
	case 6:
		result = driver->last_progress;
		break;
	case 7:
		result = processes[slot].pid;
		break;
	case 8:
		result = processes[slot].laststatus;
		break;
	default:
		result = -1;
		break;
	}
	unlock(&driverlock);
	return result;
}

char *
o9drvmanstate(int slot)
{
	char *result;

	if(!validslot(slot))
		return "invalid";
	lock(&driverlock);
	result = (char *)drv_state_name(drivers[slot].state);
	unlock(&driverlock);
	return result;
}

int
o9drvmanconfigure(int slot, char *manager, char *name, char *path)
{
	Dir *dir;
	int n;

	if(!validslot(slot) || manager == nil || manager[0] != '/' ||
	   strlen(manager) >= Maxpath || strchr(manager, '\n') != nil ||
	   name == nil || name[0] == 0 ||
	   strlen(name) >= Maxname || path == nil || path[0] != '/')
		return -1;
	n = strlen(path);
	if(n < 2 || n >= Maxpath || strchr(path, '\n') != nil)
		return -1;
	dir = dirstat(path);
	if(dir == nil || (dir->mode & DMDIR) != 0){
		free(dir);
		return -1;
	}
	free(dir);
	lock(&driverlock);
	if(processes[slot].pid != 0){
		unlock(&driverlock);
		return -1;
	}
	strcpy(processes[slot].manager, manager);
	strcpy(processes[slot].name, name);
	strcpy(processes[slot].path, path);
	processes[slot].configured = 1;
	unlock(&driverlock);
	return 0;
}

int
o9drvmanconfigured(int slot)
{
	int configured;

	if(!validslot(slot))
		return 0;
	lock(&driverlock);
	configured = processes[slot].configured;
	unlock(&driverlock);
	return configured;
}

int
o9drvmanlaunch(int slot, uint generation)
{
	DrvProcess *process;
	char manager[Maxpath], marker[Maxpath + Maxname + 32], name[Maxname], path[Maxpath];
	int start[2], monitor, pid;

	if(!validslot(slot) || exitpipe[1] < 0 || pipe(start) < 0)
		return -1;
	lock(&driverlock);
	process = &processes[slot];
	if(!process->configured || process->pid != 0 ||
	   drivers[slot].state != DrvStarting ||
	   drivers[slot].generation != generation){
		unlock(&driverlock);
		close(start[0]);
		close(start[1]);
		return -1;
	}
	strcpy(path, process->path);
	strcpy(manager, process->manager);
	strcpy(name, process->name);
	snprint(marker, sizeof marker, "%s/%s.%u", manager, name, generation);
	process->pid = -1;
	process->generation = generation;
	unlock(&driverlock);
	remove(marker);
	snprint(marker, sizeof marker, "%s/%s.%u.stop", manager, name, generation);
	remove(marker);
	snprint(marker, sizeof marker, "%s/%s.%u.progress", manager, name, generation);
	remove(marker);
	monitor = rfork(RFPROC|RFFDG|RFNOTEG|RFNOWAIT);
	if(monitor == 0){
		close(start[0]);
		monitorproc(slot, generation, manager, name, path, start[1]);
	}
	close(start[1]);
	pid = -1;
	if(monitor < 0 || readn(start[0], &pid, sizeof pid) != sizeof pid)
		pid = -1;
	close(start[0]);
	lock(&driverlock);
	if(process->generation == generation)
		process->pid = pid > 0 ? pid : 0;
	unlock(&driverlock);
	return pid;
}

static int
readymarker(int slot, uint generation)
{
	char marker[Maxpath + Maxname + 32], text[8];
	int fd, n;

	lock(&driverlock);
	snprint(marker, sizeof marker, "%s/%s.%u", processes[slot].manager,
	        processes[slot].name, generation);
	unlock(&driverlock);
	fd = open(marker, OREAD);
	if(fd < 0)
		return 0;
	n = read(fd, text, sizeof text);
	close(fd);
	if(n != 6 || memcmp(text, "ready\n", 6) != 0)
		return 0;
	remove(marker);
	return 1;
}

static int
progressmarker(int slot, uint generation, uvlong *progress)
{
	char marker[Maxpath + Maxname + 32], text[32], *end;
	uvlong value;
	int fd, i, n;

	lock(&driverlock);
	snprint(marker, sizeof marker, "%s/%s.%u.progress", processes[slot].manager,
	        processes[slot].name, generation);
	unlock(&driverlock);
	fd = open(marker, OREAD);
	if(fd < 0)
		return 0;
	n = read(fd, text, sizeof text - 1);
	close(fd);
	remove(marker);
	if(n < 2 || text[n - 1] != '\n')
		return 0;
	text[--n] = 0;
	for(i = 0; i < n; i++)
		if(text[i] < '0' || text[i] > '9')
			return 0;
	value = strtoull(text, &end, 10);
	if(value == 0 || end == text || *end != 0)
		return 0;
	*progress = value;
	return 1;
}

int
o9drvmansignal(int slot, uint generation)
{
	char marker[Maxpath + Maxname + 32];
	int fd, pid;

	if(!validslot(slot))
		return -1;
	lock(&driverlock);
	if(processes[slot].generation != generation){
		unlock(&driverlock);
		return -1;
	}
	pid = processes[slot].pid;
	snprint(marker, sizeof marker, "%s/%s.%u.stop", processes[slot].manager,
	        processes[slot].name, generation);
	unlock(&driverlock);
	if(pid <= 0)
		return -1;
	fd = create(marker, OWRITE, 0600);
	if(fd < 0)
		return -1;
	pid = write(fd, "shutdown\n", 9) == 9 ? 0 : -1;
	close(fd);
	return pid;
}

static int
forceproc(int slot, uint generation)
{
	char path[64];
	int fd, pid;

	lock(&driverlock);
	if(processes[slot].generation != generation){
		unlock(&driverlock);
		return -1;
	}
	pid = processes[slot].pid;
	unlock(&driverlock);
	if(pid <= 0)
		return -1;
	snprint(path, sizeof path, "/proc/%d/notepg", pid);
	fd = open(path, OWRITE);
	if(fd < 0)
		return -1;
	pid = write(fd, "shutdown", 8) == 8 ? 0 : -1;
	close(fd);
	return pid;
}

int
o9drvmanreapone(void)
{
	DrvExitMessage exit;
	DrvEvent event;
	char marker[Maxpath + Maxname + 32], progresspath[Maxpath + Maxname + 32];
	char stopmarker[Maxpath + Maxname + 32];
	int result;

	if(exitpipe[0] < 0 || readn(exitpipe[0], &exit, sizeof exit) != sizeof exit)
		return -1;
	if(!validslot(exit.slot))
		return -1;
	lock(&driverlock);
	if(processes[exit.slot].generation != exit.generation ||
	   processes[exit.slot].pid != exit.pid){
		unlock(&driverlock);
		return 0;
	}
	processes[exit.slot].pid = 0;
	processes[exit.slot].laststatus = exit.status;
	snprint(marker, sizeof marker, "%s/%s.%u", processes[exit.slot].manager,
	        processes[exit.slot].name, exit.generation);
	snprint(stopmarker, sizeof stopmarker, "%s.stop", marker);
	snprint(progresspath, sizeof progresspath, "%s.progress", marker);
	event.type = DrvExit;
	event.generation = exit.generation;
	event.now_ms = nsec() / 1000000ULL;
	event.progress = 0;
	result = drv_lifecycle_apply(&drivers[exit.slot], &event);
	unlock(&driverlock);
	remove(marker);
	remove(stopmarker);
	remove(progresspath);
	return result == DrvRejected ? -1 : exit.slot + 1;
}

int
o9drvmansuperviseonce(uvlong now)
{
	uvlong progress;
	uint generation;
	int current, launched, old, pid, slot, status;

	launched = 0;
	for(slot = 0; slot < Maxdrivers; slot++){
		if(!validslot(slot))
			continue;
		old = o9drvmanfield(slot, 0);
		generation = o9drvmanfield(slot, 1);
		pid = o9drvmanfield(slot, 7);
		if(old == DrvStarting && pid > 0 && readymarker(slot, generation))
			o9drvmanapply(slot, DrvReady, generation, now, 0);
		current = o9drvmanfield(slot, 0);
		if(current == DrvRunning && pid > 0 &&
		   progressmarker(slot, generation, &progress))
			o9drvmanapply(slot, DrvProgress, generation, now, progress);
		status = o9drvmanapply(slot, DrvTick, 0, now, 0);
		current = o9drvmanfield(slot, 0);
		if(status == DrvApplied &&
		   (old == DrvStarting || old == DrvRunning || old == DrvStopping) &&
		   (current == DrvBackoff || current == DrvStopped) && pid > 0)
			forceproc(slot, generation);
		if(current != DrvStarting || pid != 0 || !o9drvmanconfigured(slot))
			continue;
		generation = o9drvmanfield(slot, 1);
		if(o9drvmanlaunch(slot, generation) > 0){
			launched++;
			continue;
		}
		o9drvmanapply(slot, DrvExit, generation, now, 0);
	}
	return launched;
}

int
o9drvmanreaploop(void)
{
	for(;;)
		if(o9drvmanreapone() < 0)
			sleep(25);
	return 0;
}

int
o9drvmansuperviseloop(void)
{
	for(;;){
		o9drvmansuperviseonce(nsec() / 1000000ULL);
		sleep(25);
	}
	return 0;
}
