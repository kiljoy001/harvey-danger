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
