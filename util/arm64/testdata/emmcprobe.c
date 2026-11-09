/* Guest probe: mount the eMMC 9P server and identify the card. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

extern int mount(int, int, const char *, int, const char *);
extern int mount_(int, int, char *, int, char *);

enum {
	MREPL	= 0,
	MCREATE	= 4,
};

static void
statprint(const char *p)
{
	struct stat st;

	if(stat(p, &st) == 0)
		printf("EMMCPROBE %s exists mode %#o\n", p, st.st_mode);
	else
		printf("EMMCPROBE %s stat: %s\n", p, strerror(errno));
}

int
main(void)
{
	char ctl[512], path[64];
	const char *mp = "/mnt/emmc";
	unsigned char b[512];
	int fd, n;

	mkdir("/mnt", 0777);
	if(mkdir("/mnt/emmc", 0777) < 0 && errno != EEXIST)
		printf("EMMCPROBE mkdir: %s\n", strerror(errno));
	statprint("/mnt");
	statprint("/mnt/emmc");

	fd = open("/srv/emmc", O_RDWR);
	if(fd < 0){
		printf("EMMCPROBE no srv: %s\n", strerror(errno));
		return 1;
	}
	if(mount_(fd, -1, "/mnt/emmc", MREPL | MCREATE, "") < 0){
		printf("EMMCPROBE mount: %s (fd %d)\n", strerror(errno), fd);
		if(mount_(fd, -1, "/mnt", MREPL | MCREATE, "") < 0){
			printf("EMMCPROBE mount /mnt: %s\n", strerror(errno));
			close(fd);
			return 1;
		}
		mp = "/mnt";
		printf("EMMCPROBE mounted at /mnt\n");
	}
	close(fd);

	snprintf(path, sizeof path, "%s/ctl", mp);
	fd = open(path, O_RDONLY);
	if(fd < 0){
		printf("EMMCPROBE no ctl: %s\n", strerror(errno));
		return 1;
	}
	n = read(fd, ctl, sizeof ctl - 1);
	close(fd);
	if(n <= 0){
		printf("EMMCPROBE ctl read %d\n", n);
		return 1;
	}
	ctl[n] = 0;
	printf("EMMCPROBE ctl: %s", ctl);

	snprintf(path, sizeof path, "%s/data", mp);
	fd = open(path, O_RDONLY);
	if(fd < 0){
		printf("EMMCPROBE no data: %s\n", strerror(errno));
		return 1;
	}
	if(lseek(fd, 0, 0) < 0)
		n = -1;
	else
		n = read(fd, b, sizeof b);
	close(fd);
	if(n != (int)sizeof b){
		printf("EMMCPROBE data read %d\n", n);
		return 1;
	}
	printf("EMMCPROBE mbr %02x%02x\n", b[510], b[511]);
	printf("EMMCPROBE-DONE\n");
	fflush(stdout);
	return 0;
}
