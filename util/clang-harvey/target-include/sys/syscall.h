/* APEX has no <sys/syscall.h>.  LLVM's Linux signal code only needs these
 * two numbers; the syscall() wrapper itself returns failure. */
#ifndef HARVEY_SYS_SYSCALL_H
#define HARVEY_SYS_SYSCALL_H

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_gettid            186
#define SYS_rt_tgsigqueueinfo 297
#define __NR_gettid           186
#define __NR_rt_tgsigqueueinfo 297

long syscall(long, ...);

#ifdef __cplusplus
}
#endif

#endif
