#ifndef HARVEY_SCHED_H
#define HARVEY_SCHED_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int sched_yield(void);

/*
 * CPU affinity: Harvey has no scheduler affinity, but LLVM's threading code
 * asks for it, so provide a fixed single-CPU set that always reports the
 * caller.  The allocation helpers are here as well because LLVM includes
 * <sched.h> (not <pthread.h>) and uses CPU_ALLOC(CPU_ALLOC_SIZE(...)).
 */
typedef struct {
    unsigned long __bits[16];
} cpu_set_t;

#define CPU_SETSIZE 1024
#define CPU_ZERO(set) __builtin_memset((set), 0, sizeof(cpu_set_t))
#define CPU_SET(cpu, set) \
    ((set)->__bits[(cpu) / (8 * sizeof(unsigned long))] |= \
     1ul << ((cpu) % (8 * sizeof(unsigned long))))
#define CPU_ISSET(cpu, set) \
    (((set)->__bits[(cpu) / (8 * sizeof(unsigned long))] >> \
      ((cpu) % (8 * sizeof(unsigned long)))) & 1ul)
#define CPU_COUNT(set) 1
#define CPU_ALLOC_SIZE(ncpus) (sizeof(cpu_set_t))
#define CPU_ALLOC(ncpus) \
    ((cpu_set_t *)__builtin_calloc(1, CPU_ALLOC_SIZE(ncpus)))
#define CPU_FREE(set) __builtin_free(set)

int sched_setaffinity(int, size_t, const cpu_set_t *);
int sched_getaffinity(int, size_t, cpu_set_t *);

#ifdef __cplusplus
}
#endif

#endif
