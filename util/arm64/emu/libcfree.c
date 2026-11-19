/*
 * Free memory that Harvey's libc allocated, from emu code that has the
 * lib9.h shim in force (where "free" is emu_free).
 *
 * This file deliberately includes only libc: no emu headers, no shim.
 */
#include <u.h>
#include <libc.h>

void
k9_libcfree(void *p)
{
	free(p);
}
