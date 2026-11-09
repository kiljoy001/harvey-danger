/*
 * Install /boot/sdk.tar into the eMMC ext2 data partition.
 *
 * Mounts the eMMC 9P server, starts ext2srv on /mnt/emmc/part2, mounts
 * /mnt/data and unpacks the boot archive's SDK tar under /mnt/data/sdk.
 * Run once over TFTP; afterwards the board can boot without the big
 * archive and use /mnt/data/sdk directly.
 */
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
mkdirs(char *path)
{
	char *p;

	for(p = path + 1; *p != 0; p++){
		if(*p != '/')
			continue;
		*p = 0;
		if(mkdir(path, 0777) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	return 0;
}

static unsigned long
otoul(const char *field, int len)
{
	unsigned long v = 0;
	int i;

	for(i = 0; i < len && field[i] >= '0' && field[i] <= '7'; i++)
		v = v * 8 + (field[i] - '0');
	return v;
}

static int
untar(const char *name, const char *dest)
{
	char hdr[512], path[512], buf[16384];
	unsigned long size, left;
	int fd, out, n, nfiles = 0;

	fd = open(name, O_RDONLY);
	if(fd < 0){
		fprintf(stderr, "sdkinstall: %s: %s\n", name, strerror(errno));
		return -1;
	}
	for(;;){
		for(n = 0; n < 512; ){
			int m = read(fd, hdr + n, 512 - n);

			if(m <= 0)
				goto done;
			n += m;
		}
		if(hdr[0] == 0)
			break;
		if(strncmp(hdr+257, "ustar", 5) != 0){
			fprintf(stderr, "sdkinstall: %s: bad tar header\n", name);
			close(fd);
			return -1;
		}
		size = otoul(hdr + 124, 12);
		if(hdr[345] != 0){
			if(snprintf(path, sizeof path, "%s/%s/%s", dest,
			    hdr + 345, hdr) >= (int)sizeof path){
				fprintf(stderr, "sdkinstall: name too long\n");
				close(fd);
				return -1;
			}
		}else if(snprintf(path, sizeof path, "%s/%s", dest, hdr) >=
		    (int)sizeof path){
			fprintf(stderr, "sdkinstall: name too long\n");
			close(fd);
			return -1;
		}
		nfiles++;
		if((nfiles & 255) == 0)
			fprintf(stderr, "sdkinstall: %d %s\n", nfiles, path);
		if(hdr[156] == '5'){
			if(mkdirs(path) < 0){
				fprintf(stderr, "sdkinstall: mkdirs %s: %s\n",
				    path, strerror(errno));
				close(fd);
				return -1;
			}
			if(mkdir(path, 0777) < 0 && errno != EEXIST){
				fprintf(stderr, "sdkinstall: %s: %s\n", path,
				    strerror(errno));
				close(fd);
				return -1;
			}
		}else if(hdr[156] == '0' || hdr[156] == 0){
			if(mkdirs(path) < 0){
				fprintf(stderr, "sdkinstall: mkdirs %s: %s\n",
				    path, strerror(errno));
				close(fd);
				return -1;
			}
			out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
			if(out < 0){
				fprintf(stderr, "sdkinstall: create %s: %s\n",
				    path, strerror(errno));
				close(fd);
				return -1;
			}
			left = size;
			while(left > 0){
				unsigned long want = left < sizeof buf ?
				    left : sizeof buf;
				int m = read(fd, buf, want);

				if(m <= 0){
					fprintf(stderr, "sdkinstall: %s: short\n",
					    name);
					close(fd);
					close(out);
					return -1;
				}
				if(write(out, buf, m) != m){
					fprintf(stderr,
					    "sdkinstall: write %s: %s\n",
					    path, strerror(errno));
					close(fd);
					close(out);
					return -1;
				}
				left -= m;
			}
			close(out);
			left = size % 512;
			if(left != 0)
				read(fd, buf, 512 - left);
			continue;
		}
		/* Skip the body of anything we do not create (links, etc). */
		left = size + ((512 - size % 512) % 512);
		while(left > 0){
			int m = read(fd, buf,
			    left < sizeof buf ? left : sizeof buf);

			if(m <= 0)
				goto done;
			left -= m;
		}
	}
done:
	close(fd);
	fprintf(stderr, "sdkinstall: %d entries, done\n", nfiles);
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
	char *srv[] = { "/initramfs/bin/ext2srv", "-F", "-f",
	    "/mnt/emmc/part2", 0 };
	int fd;

	if(mkdir("/mnt", 0777) < 0 && errno != EEXIST)
		printf("SDKINSTALL mkdir /mnt: %s\n", strerror(errno));
	if(mkdir("/mnt/emmc", 0777) < 0 && errno != EEXIST)
		printf("SDKINSTALL mkdir /mnt/emmc: %s\n", strerror(errno));
	if(mkdir("/mnt/data", 0777) < 0 && errno != EEXIST)
		printf("SDKINSTALL mkdir /mnt/data: %s\n", strerror(errno));

	fd = open("/srv/emmc", O_RDWR);
	if(fd < 0){
		printf("SDKINSTALL no emmc srv: %s\n", strerror(errno));
		return 1;
	}
	if(mount_(fd, -1, "/mnt/emmc", MREPL | MCREATE, "") < 0){
		printf("SDKINSTALL mount emmc: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);

	{
		pid_t pid = fork();

		if(pid == 0){
			execv(srv[0], srv);
			fprintf(stderr, "SDKINSTALL exec ext2srv: %s\n",
			    strerror(errno));
			_exit(127);
		}
		if(pid < 0){
			printf("SDKINSTALL fork: %s\n", strerror(errno));
			return 1;
		}
	}
	if(waitfile("/srv/ext2") < 0){
		printf("SDKINSTALL no ext2 srv\n");
		return 1;
	}
	fd = open("/srv/ext2", O_RDWR);
	if(fd < 0){
		printf("SDKINSTALL open ext2: %s\n", strerror(errno));
		return 1;
	}
	if(mount_(fd, -1, "/mnt/data", MREPL | MCREATE, "") < 0){
		printf("SDKINSTALL mount data: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);
	printf("SDKINSTALL data mounted, untarring\n");

	if(untar("/boot/sdk.tar", "/mnt/data/sdk") < 0){
		printf("SDKINSTALL untar failed\n");
		return 1;
	}
	printf("SDKINSTALL-DONE\n");
	fflush(stdout);
	return 0;
}
