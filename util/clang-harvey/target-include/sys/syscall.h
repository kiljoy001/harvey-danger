/* APEX has no <sys/syscall.h>.  LLVM's Linux signal code only needs these
 * two numbers; the syscall() wrapper itself returns failure. */
#ifndef HARVEY_SYS_SYSCALL_H
#define HARVEY_SYS_SYSCALL_H

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_read              63
#define SYS_write             64
#define SYS_getpid            172
#define SYS_gettid            186
#define SYS_getrandom         278
#define SYS_mmap              222
#define SYS_futex             98
#define SYS_munmap            215
#define SYS_munmap            215
#define SYS_rt_tgsigqueueinfo 297
#define __NR_read             63
#define __NR_write            64
#define __NR_getpid           172
#define __NR_gettid           186
#define __NR_getrandom        278
#define __NR_mmap             222
#define __NR_munmap           215
#define __NR_rt_tgsigqueueinfo 297

long syscall(long, ...);

#ifdef __cplusplus
}
#endif

#endif
