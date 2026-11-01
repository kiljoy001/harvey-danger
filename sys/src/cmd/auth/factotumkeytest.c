/*
 * Add a dp9ik key to a running factotum and print the key list.
 * This avoids shell quoting while testing the key path.
 */
#include <u.h>
#include <libc.h>

void
main(void)
{
	static char key[] =
		"key proto=dp9ik dom=test user=oracle !oracle=dp9ik/test\n";
	char buf[4096];
	int fd, n;

	fd = open("/mnt/factotum/ctl", OWRITE);
	if(fd < 0)
		exits("open ctl for write");
	if(write(fd, key, sizeof(key)-1) != sizeof(key)-1)
		exits("write ctl");
	close(fd);

	fd = open("/mnt/factotum/ctl", OREAD);
	if(fd < 0)
		exits("open ctl for read");
	n = read(fd, buf, sizeof(buf)-1);
	close(fd);
	if(n < 0)
		exits("read ctl");
	buf[n] = 0;
	print("ctl:\n%s\n", buf);
	exits(nil);
}
