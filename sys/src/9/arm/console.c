/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in the top-level LICENSE.gpl file.
 *
 * Before the first process and its file table exist, route the kernel
 * libc's write(2) (which is how print() formats) straight at the console.
 * The real syscall implementation in port/sysfile.c replaces this once
 * user processes and /dev/cons are up.
 */
#include "u.h"
#include "mem.h"
#include "arm.h"

long
write(int fd, void *buf, long n)
{
	char *p;

	if(fd != 1 && fd != 2)
		return -1;
	p = buf;
	while(n-- > 0)
		uartputc(*p++);
	return (long)(p - (char*)buf);
}

long
read(int fd, void *buf, long n)
{
	USED(fd);
	USED(buf);
	USED(n);
	return 0;
}
