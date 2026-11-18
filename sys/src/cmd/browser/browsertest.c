/*
 * browsertest.c - prove the pipeline over 9ring.
 *
 *	controller -> textfs -> layoutfs -> htmlfs
 *	                    \-> genwatch (blocks in htmlfs gen)
 *
 * Every stage mounts its immediate upstream, so the chain is wired by
 * 9ring service names and mount points.  The controller writes HTML to
 * src, textfs re-exports the terminal view, and a second consumer sleeps
 * in htmlfs's generation stream until the write publishes.
 */

#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "browser.h"
#include "ringfs.h"

/* libthread's main thread defaults to 8 KiB; this test keeps buffers on it */
int mainstacksize = 256*1024;

static char *sample =
	"<html><head><title>t</title><style>body{color:#000}</style></head>"
	"<body>"
	"<h1>Hello</h1>"
	"<p>world &amp; friends</p>"
	"<p>second <b>para</b></p>"
	"<a href=\"http://example.com/\">link</a>"
	"<script>var x = 1;</script>"
	"</body></html>";

static void
mkmtpt(char *path)
{
	char dir[64];
	int fd;
	char *p;

	/* the runner's ramfs has no /mnt; create parents as needed */
	snprint(dir, sizeof dir, "%s", path);
	for(p = dir + 1; *p; p++){
		if(*p != '/')
			continue;
		*p = 0;
		fd = create(dir, OREAD, DMDIR|0777);
		if(fd >= 0)
			close(fd);
		*p = '/';
	}
	fd = create(path, OREAD, DMDIR|0777);
	if(fd >= 0)
		close(fd);
}

static void
touch(char *path)
{
	int fd;

	fd = create(path, OWRITE, 0666);
	if(fd >= 0){
		write(fd, "1", 1);
		close(fd);
	}
}

static int
waitfile(char *path)
{
	int i, fd;

	for(i = 0; i < 100; i++){
		fd = open(path, OREAD);
		if(fd >= 0){
			close(fd);
			return 0;
		}
		sleep(50);
	}
	return -1;
}

static void
die(char *msg)
{
	print("browsertest: %s: %r\n", msg);
	exits("fail");
}

void
threadmain(int argc, char **argv)
{
	char buf[4096], boxbuf[8192], hdr[64], jsbuf[1024], cssbuf[2048];
	int htmlhold, csshold, layhold, texthold, painthold, jshold, fd, n, i, nl;

	USED(argc);
	USED(argv);

	if(argc > 1 && strcmp(argv[1], "jsrun") == 0){
		char *out;
		int n;

		n = jsrun("print(1+2)", &out);
		print("JSONLY: n=%d out=%s\n", n, out == nil ? "(nil)" : out);
		exits(nil);
	}

	if(argc > 1 && strcmp(argv[1], "apexfree") == 0){
		char *av[4];
		int k;
		Waitmsg *w;

		k = 0;
		av[k++] = "apexfree";
		if(argc > 2)
			av[k++] = argv[2];
		if(argc > 3)
			av[k++] = argv[3];
		av[k] = nil;
		switch(rfork(RFPROC|RFFDG)){
		case 0:
			exec("/js/apexfree", av);
			exits("exec apexfree");
		}
		w = wait();
		print("APEXFREE-EXIT: %s\n", w != nil ? "reaped" : "(nil)");
		exits(nil);
	}
	if(argc > 1 && strcmp(argv[1], "v8hello") == 0){
		Waitmsg *w;

		switch(rfork(RFPROC|RFFDG)){
		case 0:
			execl("/js/v8hello", "v8hello", nil);
			exits("exec v8hello");
		}
		w = wait();
		print("V8HELLO-EXIT: %s\n", w != nil ? "reaped" : "(nil)");
		exits(nil);
	}
	if(argc > 1 && strcmp(argv[1], "hello") == 0){
		Waitmsg *w;

		switch(rfork(RFPROC|RFFDG)){
		case 0:
			execl("/js/hello", "hello", nil);
			exits("exec hello");
		}
		w = wait();
		print("HELLO-EXIT: %s\n", w != nil ? "reaped" : "(nil)");
		exits(nil);
	}
	if(argc > 1 && strcmp(argv[1], "wquit") == 0){
		Waitmsg *w;

		switch(rfork(RFPROC|RFFDG)){
		case 0:
			execl("/js/d8", "d8", "-e", "print(1+2); quit()", nil);
			exits("exec d8");
		}
		w = wait();
		print("WQUIT: %s\n", w != nil ? "reaped" : "(nil)");
		exits(nil);
	}
	if(argc > 1 && (strcmp(argv[1], "wexit") == 0
			|| strcmp(argv[1], "wexitst") == 0)){
		Waitmsg *w;

		switch(rfork(RFPROC|RFFDG)){
		case 0:
			if(strcmp(argv[1], "wexitst") == 0)
				execl("/js/d8", "d8", "--single-threaded",
				      "-e", "print(1+2)", nil);
			else
				execl("/js/d8", "d8", "-e", "print(1+2)", nil);
			exits("exec d8");
		}
		w = wait();
		print("WEXIT: %s\n", w != nil ? "reaped" : "(nil)");
		exits(nil);
	}

	if(argc > 1 && strcmp(argv[1], "d8") == 0){
		print("browsertest: exec d8\n");
		execl("/js/d8", "d8", "-e", "print(1+2)", nil);
		print("browsertest: exec d8 failed: %r\n");
		exits("exec");
	}

	htmlhold = ringannounce("browser.htmlfs");
	if(htmlhold < 0)
		die("announce htmlfs");
	csshold = ringannounce("browser.cssfs");
	if(csshold < 0)
		die("announce cssfs");
	texthold = ringannounce("browser.textfs");
	if(texthold < 0)
		die("announce textfs");
	layhold = ringannounce("browser.layoutfs");
	if(layhold < 0)
		die("announce layoutfs");
	painthold = ringannounce("browser.paintfs");
	if(painthold < 0)
		die("announce paintfs");
	jshold = ringannounce("browser.jsfs");
	if(jshold < 0)
		die("announce jsfs");
	print("browsertest: announced\n");

	/* stage 1: htmlfs */
	switch(rfork(RFPROC|RFFDG|RFNAMEG|RFNOWAIT)){
	case -1:
		die("fork htmlfs");
	case 0:
		print("browsertest: htmlfs serving\n");
		htmlfsserver("browser.htmlfs");
		exits(nil);
	}

	/* stage 2: cssfs mounts htmlfs, then serves */
	switch(rfork(RFPROC|RFFDG|RFNAMEG|RFNOWAIT)){
	case -1:
		die("fork cssfs");
	case 0:
		mkmtpt("/mnt/html");
		if(ringmount("browser.htmlfs", "/mnt/html") < 0)
			exits("mount htmlfs");
		print("browsertest: cssfs serving\n");
		cssfsserver("browser.cssfs");
		exits(nil);
	}

	/* stage 3: layoutfs mounts cssfs, then serves */
	switch(rfork(RFPROC|RFFDG|RFNAMEG|RFNOWAIT)){
	case -1:
		die("fork layoutfs");
	case 0:
		mkmtpt("/mnt/css");
		if(ringmount("browser.cssfs", "/mnt/css") < 0)
			exits("mount cssfs");
		print("browsertest: layoutfs serving\n");
		layoutfsserver("browser.layoutfs");
		exits(nil);
	}

	/* stage 3: textfs mounts layoutfs, then serves */
	switch(rfork(RFPROC|RFFDG|RFNAMEG|RFNOWAIT)){
	case -1:
		die("fork textfs");
	case 0:
		mkmtpt("/mnt/layout");
		if(ringmount("browser.layoutfs", "/mnt/layout") < 0)
			exits("mount layoutfs");
		print("browsertest: textfs serving\n");
		textfsserver("browser.textfs");
		exits(nil);
	}

	/* stage 4: paintfs mounts layoutfs, then serves */
	switch(rfork(RFPROC|RFFDG|RFNAMEG|RFNOWAIT)){
	case -1:
		die("fork paintfs");
	case 0:
		mkmtpt("/mnt/layout");
		if(ringmount("browser.layoutfs", "/mnt/layout") < 0)
			exits("mount layoutfs");
		print("browsertest: paintfs serving\n");
		paintfsserver("browser.paintfs");
		exits(nil);
	}

	/* stage 5: jsfs runs scripts through d8 (/js/d8) */
	switch(rfork(RFPROC|RFFDG|RFNOWAIT)){
	case -1:
		die("fork jsfs");
	case 0:
		print("browsertest: jsfs serving\n");
		jsfsserver("browser.jsfs");
		exits(nil);
	}

	/* a second consumer blocks in gen until the write publishes */
	switch(rfork(RFPROC|RFFDG|RFNAMEG|RFNOWAIT)){
	case -1:
		die("fork genwatch");
	case 0:
		mkmtpt("/mnt/gens");
		if(ringmount("browser.htmlfs", "/mnt/gens") < 0)
			exits("mount genwatch");
		fd = open("/mnt/gens/gen", OREAD);
		if(fd < 0)
			exits("open gen");
		touch("/genarmed");
		n = pread(fd, buf, sizeof buf - 1, 11);
		if(n < 0)
			exits("pread gen");
		close(fd);
		buf[n] = 0;
		print("browsertest: gen woke at %s", buf);
		touch("/genwake");
		exits(nil);
	}

	/* controller */
	mkmtpt("/mnt/text");
	if(ringmount("browser.textfs", "/mnt/text") < 0)
		die("mount textfs");
	print("browsertest: mounted textfs\n");
	if(waitfile("/genarmed") < 0)
		die("gen watcher not armed");
	fd = open("/mnt/text/src", OWRITE);
	if(fd < 0)
		die("open src");
	if(write(fd, sample, strlen(sample)) != strlen(sample))
		die("write src");
	close(fd);
	print("browsertest: wrote src\n");
	if(waitfile("/genwake") < 0)
		die("gen watcher did not wake");
	print("browsertest: gen watcher woke\n");

	fd = open("/mnt/text/text", OREAD);
	if(fd < 0)
		die("open text");
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n < 0)
		die("read text");
	buf[n] = 0;
	print("BROWSER-TEXT:\n%s\nBROWSER-TEXT-END\n", buf);

	fd = open("/mnt/text/boxes", OREAD);
	if(fd < 0)
		die("open boxes");
	n = read(fd, boxbuf, sizeof boxbuf - 1);
	close(fd);
	if(n < 0)
		die("read boxes");
	boxbuf[n] = 0;
	print("BROWSER-BOXES:\n%s\nBROWSER-BOXES-END\n", boxbuf);

	/* the style stage feeds layoutfs; check both views */
	fd = open("/mnt/text/rules", OREAD);
	if(fd < 0)
		die("open rules");
	n = read(fd, cssbuf, sizeof cssbuf - 1);
	close(fd);
	if(n < 0)
		die("read rules");
	cssbuf[n] = 0;
	print("BROWSER-CSS-RULES: %s", cssbuf);
	if(strstr(cssbuf, "body{color") == nil){
		print("BROWSER-CSS-FAIL\n");
		exits("fail");
	}
	fd = open("/mnt/text/styles", OREAD);
	if(fd < 0)
		die("open styles");
	n = read(fd, cssbuf, sizeof cssbuf - 1);
	close(fd);
	if(n < 0)
		die("read styles");
	cssbuf[n] = 0;
	print("BROWSER-CSS-STYLES: %s", cssbuf);
	if(strstr(cssbuf, "h1") == nil){
		print("BROWSER-CSS-FAIL\n");
		exits("fail");
	}

	/* the paint stage rasterizes the same boxes; check the framing */
	mkmtpt("/mnt/paint");
	if(ringmount("browser.paintfs", "/mnt/paint") < 0)
		die("mount paintfs");
	fd = open("/mnt/paint/ppm", OREAD);
	if(fd < 0)
		die("open ppm");
	n = read(fd, hdr, sizeof hdr - 1);
	close(fd);
	if(n < 0)
		die("read ppm");
	hdr[n] = 0;
	nl = 0;
	for(i = 0; i < n; i++)
		if(hdr[i] == '\n' && ++nl == 3){
			hdr[i+1] = 0;
			break;
		}
	print("BROWSER-PPM: %s", hdr);
	if(strstr(hdr, "P6\n640 480\n255\n") == nil){
		print("BROWSER-PAINT-FAIL\n");
		exits("fail");
	}

	fd = open("/mnt/paint/snapshot", OREAD);
	if(fd < 0)
		die("open snapshot");
	n = read(fd, hdr, sizeof hdr - 1);
	close(fd);
	if(n < 0)
		die("read snapshot");
	hdr[n] = 0;
	if(strncmp(hdr, "PAINT 640 480 4\n", 16) != 0){
		print("BROWSER-PAINT-FAIL\n");
		exits("fail");
	}
	print("browsertest: paint snapshot ok\n");

	/* the script stage runs JavaScript through d8 */
	mkmtpt("/mnt/js");
	if(ringmount("browser.jsfs", "/mnt/js") < 0)
		die("mount jsfs");
	print("browsertest: mounted jsfs\n");
	fd = open("/js/d8", OREAD);
	print("browsertest: /js/d8 %s\n", fd < 0 ? "missing" : "present");
	if(fd >= 0)
		close(fd);
	fd = open("/mnt/js/eval", OWRITE);
	if(fd < 0)
		die("open eval");
	if(write(fd, "print(1+2)", strlen("print(1+2)")) != strlen("print(1+2)"))
		die("write eval");
	close(fd);
	fd = open("/mnt/js/result", OREAD);
	if(fd < 0)
		die("open result");
	n = read(fd, jsbuf, sizeof jsbuf - 1);
	close(fd);
	if(n < 0)
		die("read result");
	jsbuf[n] = 0;
	print("BROWSER-JS: %s", jsbuf);
	if(strstr(jsbuf, "3") == nil){
		print("BROWSER-JS-FAIL\n");
		exits("fail");
	}

	if(strstr(buf, "Hello") == nil
	   || strstr(buf, "world & friends") == nil
	   || strstr(buf, "second para") == nil
	   || strstr(buf, "x = 1") != nil
	   || strstr(boxbuf, "Hello") == nil
	   || strstr(boxbuf, "0 0 0 ") == nil){
		print("BROWSER-TEXT-FAIL\n");
		exits("fail");
	}
	print("BROWSER-TEXT-OK\n");
	exits(nil);
}
