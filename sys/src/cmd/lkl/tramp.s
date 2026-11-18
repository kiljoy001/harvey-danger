/*
 * Start a Harvey thread on a stack chosen by the host layer.
 * rfork(RFPROC|RFMEM) returns with the child sharing the address space;
 * the child switches to the new stack before calling hthread_entry, so the
 * parent's stack is never used again.
 *
 *	void harvey_thread_tramp(void *sp, void *arg, void (*f)(void*));
 */
	.text
	.globl harvey_thread_tramp
	.type harvey_thread_tramp, %function
harvey_thread_tramp:
	mov	sp, x0
	mov	x0, x1
	mov	x1, x2
	mov	x29, xzr
	mov	x30, xzr
	b	hthread_entry
	.size harvey_thread_tramp, .-harvey_thread_tramp

/*
 * int harvey_rfork_thread(void *newsp, void *arg, void (*f)(void*), int flags);
 *
 * Performs rfork(flags) without the child ever touching the parent's stack.
 * Both parent and child keep newsp/arg/f in caller-saved scratch registers
 * across svc #0 (syscall preserves x1..x30); the child switches sp to newsp
 * and jumps to hthread_entry(arg, f).
 */
	.globl harvey_rfork_thread
	.type harvey_rfork_thread, %function
harvey_rfork_thread:
	mov	x9, x0
	mov	x10, x1
	mov	x11, x2
	mov	x0, x3
	mov	x8, #19		/* RFORK */
	svc	#0
	cbz	x0, 1f
	ret
1:
	mov	sp, x9
	mov	x0, x10
	mov	x1, x11
	mov	x29, xzr
	mov	x30, xzr
	b	hthread_entry
	.size harvey_rfork_thread, .-harvey_rfork_thread

/*
 * int harvey_rfork_call(void *newsp, void *arg, void (*fn)(void*), int flags);
 *
 * Same as harvey_rfork_thread, but the child calls fn(arg) directly.
 */
	.globl harvey_rfork_call
	.type harvey_rfork_call, %function
harvey_rfork_call:
	mov	x9, x0
	mov	x10, x1
	mov	x11, x2
	mov	x0, x3
	mov	x8, #19		/* RFORK */
	svc	#0
	cbz	x0, 1f
	ret
1:
	mov	sp, x9
	mov	x0, x10
	mov	x29, xzr
	mov	x30, xzr
	br	x11
	.size harvey_rfork_call, .-harvey_rfork_call
