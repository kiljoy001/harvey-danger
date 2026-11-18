/*
 * ringfs.c - named #~ (9ring) helpers.
 *
 * #~ layout (see sys/src/9/port/dev9ring.c):
 *
 *	#~!name/announce	hold the service open (owner only)
 *	#~!name/listen		accept one client -> 9P byte stream
 *	#~name/connect		connect to it     -> 9P byte stream
 *
 * The names are global to the device and scoped to the owning user, so a
 * pipeline picks a unique per-tab prefix rather than relying on the mount
 * namespace for isolation.
 */

#include <u.h>
#include <libc.h>

int
ringannounce(char *name)
{
	char path[128];

	snprint(path, sizeof path, "#~!%s/announce", name);
	return open(path, OREAD);
}

int
ringlisten(char *name)
{
	char path[128];

	snprint(path, sizeof path, "#~!%s/listen", name);
	return open(path, ORDWR);
}

int
ringconnect(char *name)
{
	char path[128];

	snprint(path, sizeof path, "#~%s/connect", name);
	return open(path, ORDWR);
}

int
ringmount(char *name, char *mtpt)
{
	int fd, r;

	fd = ringconnect(name);
	if(fd < 0)
		return -1;
	r = mount(fd, -1, mtpt, MREPL, "");
	close(fd);
	return r;
}
