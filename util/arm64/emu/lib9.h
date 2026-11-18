/*
 * lib9.h for building Inferno's emu sources against Harvey's libc.
 *
 * Harvey is Plan 9 enough that emu's Plan 9 host layer compiles against
 * it almost unchanged; this shim only resolves the names the two trees
 * disagree about.
 */
#include <u.h>
typedef usize size_t;

#define	Rendez	xRendez
#include <libc.h>
#undef	Rendez

#define	setbinmode()

/* interp.h declares int segflush(void*, ulong); Harvey's libc has
 * void segflush(void*, usize). */
#define	segflush	emu_segflush

/* Harvey's libc sizes are 32-bit; emu's allocator is 64-bit.  Renaming
 * emu's symbols keeps libc's allocator intact for libc internals (and
 * for anything that frees what libc allocated, e.g. dirstat). */
#define	malloc		emu_malloc
#define	free		emu_free
#define	realloc		emu_realloc
#define	mallocz		emu_mallocz
#define	msize		emu_msize
#define	calloc		emu_calloc

extern	void*	emu_malloc(usize);
extern	void	emu_free(void*);
extern	void*	emu_realloc(void*, usize);
extern	void*	emu_mallocz(usize, int);
extern	usize	emu_msize(void*);
extern	void*	emu_calloc(usize, usize);

/* Harvey's libc has coherence() as a function; emu/port/fns.h declares a
 * hook with the same name. */
#define	coherence	emu_coherence

/* u.h makes getcallerpc a compiler builtin; emu declares and implements
 * its own (used by the Dis stack traces). */
#undef	getcallerpc
extern	ulong	getcallerpc(void*);	/* asm-arm64.S */
