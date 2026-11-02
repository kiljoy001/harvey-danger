#include <u.h>
#include <libc.h>

/* Plumbing target for this rio's directory browser and text editor. */
void
main(int argc, char **argv)
{
	char wd[4096], *path, *cmd, *wsys;
	int fd, srv;

	if(argc != 2)
		sysfatal("usage: rio-open path");
	if(argv[1][0] == '/')
		path = strdup(argv[1]);
	else{
		if(getwd(wd, sizeof wd) == nil)
			sysfatal("getwd: %r");
		path = smprint("%s/%s", wd, argv[1]);
	}
	if(path == nil)
		sysfatal("out of memory");
	cleanname(path);
	cmd = smprint("open %s", path);
	fd = open("/dev/wctl", OWRITE);
	/* Plumber starts before rio and has no window mounted on /dev. */
	if(fd < 0){
		wsys = getenv("wsys");
		if(wsys != nil){
			rfork(RFNAMEG);
			srv = open(wsys, ORDWR);
			if(srv >= 0){
				/* A successful mount consumes srv; its number may be reused. */
				if(mount(srv, -1, "/mnt/wsys", MREPL, "none") >= 0)
					fd = open("/mnt/wsys/wctl", OWRITE);
				else
					close(srv);
			}
			free(wsys);
		}
	}
	if(fd < 0 || write(fd, cmd, strlen(cmd)) != strlen(cmd))
		sysfatal("open %s: %r", path);
	close(fd);
	free(cmd);
	free(path);
	exits(nil);
}
