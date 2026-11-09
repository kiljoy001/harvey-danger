/* Guest setup: format the eMMC data partition as ext2 and mount it. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

extern int mount_(int, int, char *, int, char *);
extern int wait4(int, int *, int, void *);
extern int __sys_sleep(int);

enum {
	MREPL	= 0,
	MCREATE	= 4,
};

static int
run(char *label, char *const argv[])
{
	pid_t pid;
	int st;

	pid = fork();
	if(pid == 0){
		execv(argv[0], argv);
		fprintf(stderr, "EMMCSETUP %s exec: %s\n", label,
		    strerror(errno));
		_exit(127);
	}
	if(pid < 0){
		printf("EMMCSETUP %s fork: %s\n", label, strerror(errno));
		return -1;
	}
	st = 0;
	if(wait4(pid, &st, 0, 0) < 0){
		printf("EMMCSETUP %s wait: %s\n", label, strerror(errno));
		return -1;
	}
	if(!WIFEXITED(st) || WEXITSTATUS(st) != 0){
		printf("EMMCSETUP %s: exit %d\n", label,
		    WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		return -1;
	}
	printf("EMMCSETUP %s OK\n", label);
	return 0;
}

static int
waitfile(const char *path)
{
	int i, fd;

	for(i = 0; i < 1000; i++){
		fd = open(path, O_RDONLY);
		if(fd >= 0){
			close(fd);
			return 0;
		}
		__sys_sleep(10);
	}
	return -1;
}

int
main(void)
{
	char *prov[] = { "/initramfs/bin/provision",
	    "/initramfs/datameta.bin", "/mnt/emmc/part2", 0 };
	char *srv[] = { "/initramfs/bin/ext2srv", "-f",
	    "/mnt/emmc/part2", 0 };
	char buf[64];
	int fd, n;

	mkdir("/mnt", 0777);
	mkdir("/mnt/emmc", 0777);
	mkdir("/mnt/data", 0777);

	fd = open("/srv/emmc", O_RDWR);
	if(fd < 0){
		printf("EMMCSETUP no emmc srv: %s\n", strerror(errno));
		return 1;
	}
	if(mount_(fd, -1, "/mnt/emmc", MREPL | MCREATE, "") < 0){
		printf("EMMCSETUP mount emmc: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);
	printf("EMMCSETUP emmc mounted\n");

	if(run("provision", prov) < 0)
		return 1;

	{
		pid_t pid = fork();

		if(pid == 0){
			execv(srv[0], srv);
			fprintf(stderr, "EMMCSETUP ext2srv exec: %s\n",
			    strerror(errno));
			_exit(127);
		}
		if(pid < 0){
			printf("EMMCSETUP ext2srv fork: %s\n", strerror(errno));
			return 1;
		}
	}
	if(waitfile("/srv/ext2") < 0){
		printf("EMMCSETUP no ext2 srv\n");
		return 1;
	}
	printf("EMMCSETUP ext2 serving\n");

	fd = open("/srv/ext2", O_RDWR);
	if(fd < 0){
		printf("EMMCSETUP open ext2 srv: %s\n", strerror(errno));
		return 1;
	}
	if(mount_(fd, -1, "/mnt/data", MREPL | MCREATE, "") < 0){
		printf("EMMCSETUP mount data: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);
	printf("EMMCSETUP data mounted\n");

	fd = open("/mnt/data/hello", O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if(fd < 0){
		printf("EMMCSETUP create: %s\n", strerror(errno));
		return 1;
	}
	write(fd, "emmc works\n", 11);
	close(fd);
	fd = open("/mnt/data/hello", O_RDONLY);
	if(fd < 0){
		printf("EMMCSETUP readback open: %s\n", strerror(errno));
		return 1;
	}
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n != 11 || strncmp(buf, "emmc works\n", 11) != 0){
		printf("EMMCSETUP readback failed (%d)\n", n);
		return 1;
	}
	printf("EMMCSETUP-DONE\n");
	fflush(stdout);
	return 0;
}
