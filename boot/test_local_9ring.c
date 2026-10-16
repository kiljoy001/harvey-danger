#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

static void
testread(Req *r)
{
	readstr(r, r->fid->file->aux);
	respond(r, nil);
}

static void
serveone(int fd)
{
	Srv s;
	File *f;

	memset(&s, 0, sizeof(s));
	s.tree = alloctree("root", "root", DMDIR|0555, nil);
	if(s.tree == nil)
		exits("tree");
	f = createfile(s.tree->root, "hello", "root", 0444, "9ring mount works\n");
	if(f == nil)
		exits("file");
	closefile(f);
	s.read = testread;
	s.infd = fd;
	s.outfd = fd;
	s.msize = 32768;
	srv(&s);
	close(fd);
}

static void
servetwo(void)
{
	int fd, i, pid;

	for(i = 0; i < 2; i++){
		fd = open("#~!ringtest/listen", ORDWR);
		if(fd < 0){
			fprint(2, "o9-local9ring: listen failed: %r\n");
			exits("listen");
		}
		pid = rfork(RFPROC|RFFDG|RFNOWAIT);
		if(pid < 0){
			fprint(2, "o9-local9ring: server fork failed: %r\n");
			close(fd);
			exits("fork");
		}
		if(pid == 0){
			serveone(fd);
			exits(nil);
		}
		close(fd);
	}
	exits(nil);
}

int
local9ringtest(void)
{
	char buf[64];
	char path[64];
	char *mounts[2];
	int announce, fd, n, i, ok, pid;

	mounts[0] = "/mnt/temp";
	mounts[1] = "/mnt/term";

	announce = open("#~!ringtest/announce", OREAD);
	if(announce < 0){
		fprint(2, "o9-local9ring: announce failed: %r\n");
		return -1;
	}
	pid = rfork(RFPROC|RFFDG|RFNOWAIT);
	if(pid < 0){
		fprint(2, "o9-local9ring: listener fork failed: %r\n");
		close(announce);
		return -1;
	}
	if(pid == 0)
		servetwo();
	ok = 1;
	for(i = 0; i < 2; i++){
		fd = open("#~ringtest/connect", ORDWR);
		if(fd < 0){
			fprint(2, "o9-local9ring: connect %d failed: %r\n", i);
			ok = 0;
			break;
		}
		if(mount(fd, -1, mounts[i], MREPL, "") < 0){
			fprint(2, "o9-local9ring: mount %d failed: %r\n", i);
			close(fd);
			ok = 0;
			break;
		}
		close(fd);
	}
	for(i = 0; i < 2 && ok; i++){
		snprint(path, sizeof(path), "%s/hello", mounts[i]);
		fd = open(path, OREAD);
		if(fd < 0){
			fprint(2, "o9-local9ring: open hello %d failed: %r\n", i);
			ok = 0;
		}else{
			n = read(fd, buf, sizeof(buf)-1);
			close(fd);
			if(n < 0)
				n = 0;
			buf[n] = 0;
			if(strcmp(buf, "9ring mount works\n") != 0){
				fprint(2, "o9-local9ring: read %d got %q\n", i, buf);
				ok = 0;
			}
		}
		if(!ok)
			break;
	}
	for(i = 0; i < 2; i++){
		if(unmount(nil, mounts[i]) < 0){
			fprint(2, "o9-local9ring: unmount %d failed: %r\n", i);
			ok = 0;
		}
	}
	close(announce);
	if(ok)
		print("o9-local9ring: PASS two concurrent 9P mounts and reads\n");
	else
		print("o9-local9ring: FAIL\n");
	return ok ? 1 : -1;
}
