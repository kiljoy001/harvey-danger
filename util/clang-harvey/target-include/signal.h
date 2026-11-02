/*
 * POSIX <signal.h> for the Harvey/APEX target.
 *
 * APEX's <signal.h> models handlers as Plan 9 note handlers (void (*)())
 * because the Plan 9 kernel passes a message string and a Ureg.  POSIX code
 * -- LLVM in particular -- expects `void (*sa_handler)(int)`.  The layouts
 * and the ABI are identical (both are plain function pointers), so this
 * header keeps APEX's numbers but exposes the POSIX types.
 *
 * This file shadows apex/include/signal.h because util/clang-harvey/
 * target-include comes first on the include path.
 */

#ifndef __SIGNAL_H
#define __SIGNAL_H

typedef int sig_atomic_t;

#define SIGHUP   1  /* hangup */
#define SIGINT   2  /* interrupt */
#define SIGQUIT  3  /* quit */
#define SIGILL   4  /* illegal instruction */
#define SIGABRT  5  /* used by abort */
#define SIGFPE   6  /* floating point exception */
#define SIGKILL  7  /* kill (cannot be caught or ignored) */
#define SIGSEGV  8  /* segmentation violation */
#define SIGPIPE  9  /* write on a pipe with no one to read it */
#define SIGALRM  10 /* alarm clock */
#define SIGTERM  11 /* software termination signal from kill */
#define SIGUSR1  12 /* user defined signal 1 */
#define SIGUSR2  13 /* user defined signal 2 */
#define SIGBUS   14 /* bus error */

/* Defined for source compatibility; APEX's sigaction() rejects them. */
#define SIGCHLD  15
#define SIGCONT  16
#define SIGSTOP  17
#define SIGTSTP  18
#define SIGTTIN  19
#define SIGTTOU  20
#define SIGTRAP  21

#define NSIG     22

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*__harvey_sighandler_t)(int);

#define SIG_DFL ((__harvey_sighandler_t)0)
#define SIG_ERR ((__harvey_sighandler_t)-1)
#define SIG_IGN ((__harvey_sighandler_t)1)

extern __harvey_sighandler_t signal(int, __harvey_sighandler_t);
extern int raise(int);
extern int kill(int, int);

typedef unsigned long long sigset_t;

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    int si_pid;
    unsigned int si_uid;
    void *si_addr;
} siginfo_t;

typedef struct {
    void *ss_sp;
    int ss_flags;
    unsigned long ss_size;
} stack_t;

#define SS_ONSTACK 1
#define SS_DISABLE 2

struct sigaction {
    union {
        __harvey_sighandler_t sa_handler;
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t sa_mask;
    int sa_flags;
};

#define SA_NOCLDSTOP 1
#define SA_NODEFER   0x10
#define SA_RESETHAND 0x20
#define SA_ONSTACK   0x40
#define SA_SIGINFO   0x80

#define SIG_BLOCK   1
#define SIG_UNBLOCK 2
#define SIG_SETMASK 3

extern int sigemptyset(sigset_t *);
extern int sigfillset(sigset_t *);
extern int sigaddset(sigset_t *, int);
extern int sigdelset(sigset_t *, int);
extern int sigismember(const sigset_t *, int);
extern int sigaction(int, const struct sigaction *, struct sigaction *);
extern int sigprocmask(int, const sigset_t *, sigset_t *);
extern int sigpending(sigset_t *);
extern int sigsuspend(const sigset_t *);
extern int sigaltstack(const stack_t *, stack_t *);

#ifdef __cplusplus
}
#endif

#endif /* __SIGNAL_H */
