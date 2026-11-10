#include <u.h>
#include <libc.h>

enum { Preview = 4096 };

static int
interrupted(void *unused, char *note)
{
	USED(unused);
	return strcmp(note, "alarm") == 0;
}

static int
wctl(void)
{
	char *wsys;
	int fd, srv;

	fd = open("/dev/wctl", OWRITE);
	if(fd >= 0)
		return fd;
	wsys = getenv("wsys");
	if(wsys == nil)
		return -1;
	rfork(RFNAMEG);
	srv = open(wsys, ORDWR);
	free(wsys);
	if(srv < 0)
		return -1;
	/* A successful mount consumes srv. */
	if(mount(srv, -1, "/mnt/wsys", MREPL, "none") < 0){
		close(srv);
		return -1;
	}
	return open("/mnt/wsys/wctl", OWRITE);
}

static void
preview(char *path, char *reason)
{
	uchar buf[Preview];
	char line[100], *p, *label;
	int fd, n, i, j;
	Dir *d;

	label = smprint("Read-only preview: %s", path);
	fd = open("/dev/label", OWRITE);
	if(fd >= 0){
		write(fd, label, strlen(label));
		close(fd);
	}
	free(label);
	print("%s\n%s\n\n", path, reason);
	/* A failed/stalled server must not be read again just to report it. */
	if(strcmp(reason, "binary or non-UTF-8 file; opening a read-only preview") != 0
	&& strcmp(reason, "file exceeds the 4 MiB editor limit") != 0)
		return;
	atnotify(interrupted, 1);
	alarm(5000);
	d = dirstat(path);
	if(d == nil){
		print("Cannot inspect file: %r\n");
		alarm(0);
		return;
	}
	print("Size: %lld bytes. Read-only byte preview (up to %d bytes).\n", d->length, Preview);
	free(d);
	alarm(5000);
	fd = open(path, OREAD);
	if(fd < 0){
		print("Cannot read file: %r\n");
		alarm(0);
		return;
	}
	/* One bounded read also handles files with no EOF, such as /dev/zero. */
	alarm(5000);
	n = read(fd, buf, sizeof buf);
	close(fd);
	alarm(0);
	if(n < 0){
		print("Cannot read preview (5 second timeout): %r\n");
		return;
	}
	for(i=0; i<n; i+=16){
		p = seprint(line, line+sizeof line, "%08ux  ", i);
		for(j=0; j<16; j++)
			if(i+j<n)
				p = seprint(p, line+sizeof line, "%02ux ", buf[i+j]);
			else
				p = seprint(p, line+sizeof line, "   ");
		p = seprint(p, line+sizeof line, " | ");
		for(j=0; j<16 && i+j<n; j++)
			*p++ = buf[i+j]>=32 && buf[i+j]<127 ? buf[i+j] : '.';
		*p++ = '\n';
		write(1, line, p-line);
	}
	print("\nRead-only preview: %s\nOriginal file unchanged. Scroll up for the header and bytes.\n", path);
}

void
main(int argc, char **argv)
{
	char wd[4096], err[ERRMAX], *dir, *path, *cmd, *qp, *qe, *script, *qs;
	int fd, show;

	dir = nil;
	show = 0;
	ARGBEGIN{
	case 'd':
		dir = EARGF(sysfatal("missing working directory"));
		break;
	case 'e':
		show = 2;
		break;
	case 'p':
		show = 1;
		break;
	default:
		sysfatal("usage: rio-open [-d directory] path");
	}ARGEND
	if(argc != (show ? 2 : 1))
		sysfatal("usage: rio-open [-d directory] path");
	if(argv[0][0] == '/')
		path = strdup(argv[0]);
	else{
		if(dir == nil){
			if(getwd(wd, sizeof wd) == nil)
				sysfatal("getwd: %r");
			dir = wd;
		}
		path = smprint("%s/%s", dir, argv[0]);
	}
	if(path == nil)
		sysfatal("out of memory");
	cleanname(path);
	if(show == 1){
		preview(path, argv[1]);
		free(path);
		exits(nil);
	}
	fd = wctl();
	if(fd < 0)
		sysfatal("cannot connect to rio: %r");
	if(show == 2)
		utfecpy(err, err+sizeof err, argv[1]);
	else{
		cmd = smprint("open %s", path);
		if(write(fd, cmd, strlen(cmd)) == strlen(cmd)){
			close(fd);
			free(cmd);
			free(path);
			exits(nil);
		}
		rerrstr(err, sizeof err);
		free(cmd);
	}
	/* Plumber has no visible stderr: present failures in their own window. */
	qp = quotestrdup(path);
	qe = quotestrdup(err);
	script = smprint("/bin/rio-open -p %s %s; exec /bin/rc -i", qp, qe);
	qs = quotestrdup(script);
	cmd = smprint("new /bin/rc -c %s", qs);
	if(write(fd, cmd, strlen(cmd)) != strlen(cmd))
		sysfatal("open %s: %s (cannot show error: %r)", path, err);
	close(fd);
	free(qp);
	free(qe);
	free(qs);
	free(script);
	free(cmd);
	free(path);
	exits(nil);
}
