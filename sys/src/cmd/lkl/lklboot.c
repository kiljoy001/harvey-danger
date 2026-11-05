/*
 * lklboot -- M1 probe: start LKL inside a Harvey process and print the
 * Linux banner.  No devices are attached yet; the point is to prove the
 * host layer, the arena and the kernel init path.
 */
#include <u.h>
#include <libc.h>
#include <lkl/asm/host_ops.h>

extern struct lkl_host_operations lkl_harvey_ops;

extern int	lkl_init(struct lkl_host_operations*);
extern int	lkl_start_kernel(const char *fmt, ...);

void
main(void)
{
	print("lklboot: start\n");
	if(lkl_init(&lkl_harvey_ops) < 0){
		print("lklboot: lkl_init failed\n");
		exits("init");
	}
	print("lklboot: init ok\n");
	if(lkl_start_kernel("mem=128M") < 0){
		print("lklboot: lkl_start_kernel failed\n");
		exits("start");
	}
	print("lklboot: kernel up\n");
	exits(nil);
}
