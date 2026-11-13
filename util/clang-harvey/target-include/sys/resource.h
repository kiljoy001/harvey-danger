/*
 * POSIX <sys/resource.h> for the Harvey/APEX target.
 *
 * APEX only has rusage (behind _BSD_EXTENSION) and no rlimit at all; LLVM
 * needs RLIMIT_STACK for getDefaultStackSize().
 */

#ifndef __RESOURCE_H__
#define __RESOURCE_H__

#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss;
    long ru_ixrss;
    long ru_idrss;
    long ru_isrss;
    long ru_minflt;
    long ru_majflt;
    long ru_nswap;
    long ru_inblock;
    long ru_oublock;
    long ru_msgsnd;
    long ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw;
    long ru_nivcsw;
};

typedef unsigned long rlim_t;

#define RLIM_INFINITY (~(rlim_t)0)
#define RLIM_SAVED_MAX RLIM_INFINITY
#define RLIM_SAVED_CUR RLIM_INFINITY

struct rlimit {
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_RSS     5
#define RLIMIT_NPROC   6
#define RLIMIT_NOFILE  7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS      9

#ifndef PRIO_PROCESS
#define PRIO_PROCESS 0
#define PRIO_PGRP 1
#define PRIO_USER 2
#endif

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)

int getrlimit(int, struct rlimit *);
int setrlimit(int, const struct rlimit *);
int getrusage(int, struct rusage *);
int getpriority(int, unsigned);
int setpriority(int, unsigned, int);

#ifdef __cplusplus
}
#endif

#endif
