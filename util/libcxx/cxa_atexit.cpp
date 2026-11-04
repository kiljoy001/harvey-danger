/*
 * APEX has no __cxa_atexit.  On amd64 it comes from APEX's libcxxabi
 * archive; the ARM64 runtime builds libc++abi from source, so provide it
 * here on top of APEX's atexit list (stdlib.h maps atexit to __atexit).
 */
#if defined(__aarch64__)

#include <cstdlib>

extern "C" {

static void (*handlers[128])(void *);
static void *args[128];
static int count;

static void
run_all(void)
{
	while(count > 0){
		count--;
		handlers[count](args[count]);
	}
}

int
__cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
	(void)dso;
	if(count == 0 && atexit(run_all) != 0)
		return -1;
	if(count >= (int)(sizeof handlers / sizeof handlers[0]))
		return -1;
	handlers[count] = fn;
	args[count] = arg;
	count++;
	return 0;
}

void *__dso_handle = &__dso_handle;

}

#endif
