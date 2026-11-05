/*
 * lklboot -- M1 probe: start LKL inside a Harvey process, print the Linux
 * banner, then shut the kernel down again.
 *
 * No devices are attached yet.  The point is to prove the host layer, the
 * kernel init path through /init, and a clean shutdown.
 *
 * Diagnostics are written to a file by default: writing to the Harvey
 * console from many host threads at once can wedge the console driver.
 */
#include <u.h>
#include <libc.h>
#include <lkl/asm/host_ops.h>

extern struct lkl_host_operations lkl_harvey_ops;

extern int	lkl_init(struct lkl_host_operations*);
extern int	lkl_start_kernel(const char *fmt, ...);
extern long	lkl_sys_halt(void);
extern void	lkl_cleanup(void);
extern void	lklhost_setout(int);
extern int	lkl_printf(const char*, ...);

#define say(s) lkl_printf("%s", s)

void
main(void)
{
	int fd;
	/*
	 * Diagnostics go to /dev/lkllog, a synthetic file backed by a ring
	 * buffer.  Writing to /dev/cons from these threads is not safe: the
	 * console path busy-waits on the UART FIFO with interrupts masked,
	 * so a slow reader parks the machine.  Read the log afterwards with
	 * "cat /dev/lkllog".
	 */
	fd = open("/dev/lkllog", OWRITE);
	if(fd < 0){
		char eb[160];
		int m = snprint(eb, sizeof eb, "lklboot: /dev/lkllog: %r; using console\n");
		write(2, eb, m);
		fd = 1;
	}
	lklhost_setout(fd);

	say("lklboot: start\n");
	if(lkl_init(&lkl_harvey_ops) < 0){
		say("lklboot: lkl_init failed\n");
		exits("init");
	}
	say("lklboot: init ok\n");
	if(lkl_start_kernel("mem=128M") < 0){
		say("lklboot: lkl_start_kernel failed\n");
		exits("start");
	}
	say("lklboot: kernel up\n");

	/*
	 * lkl_sys_halt() runs the reboot syscall and unwinds the kernel: it
	 * stops the idle task, joins the kernel threads and frees the arena.
	 */
	if(lkl_sys_halt() < 0)
		say("lklboot: halt failed\n");
	lkl_cleanup();
	say("lklboot: halted\n");
	exits(nil);
}
