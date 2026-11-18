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
#define	USE_FPdbleword

/* Harvey's libc has coherence() as a function; emu/port/fns.h declares a
 * hook with the same name. */
#define	coherence	emu_coherence

/* u.h makes getcallerpc a compiler builtin; emu declares and implements
 * its own (used by the Dis stack traces). */
#undef	getcallerpc
