/*
 * Start a Harvey thread on a stack chosen by the host layer.
 * rfork(RFPROC|RFMEM) returns with the child sharing the address space;
 * the child switches to the new stack before calling f(arg), so the
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
	mov	x29, xzr
	mov	x30, xzr
	blr	x2
	mov	x0, xzr
	bl	_exits
	b	.
	.size harvey_thread_tramp, .-harvey_thread_tramp
