#ifndef HARVEY_LIBCXX_COMPAT_H
#define HARVEY_LIBCXX_COMPAT_H

/* APEX's C headers predate C11 and carry a few Plan 9-isms.  These
 * definitions have to exist before libc++'s <cstddef>/<cstdlib>/<cstdint>
 * wrappers look for them. */

#ifndef __lldiv_t_defined
#define __lldiv_t_defined
typedef struct { long long quot, rem; } lldiv_t;
#ifdef __cplusplus
extern "C" {
#endif
static inline lldiv_t lldiv(long long a, long long b) {
    lldiv_t r; r.quot = a / b; r.rem = a % b; return r;
}
#ifdef __cplusplus
}
#endif
#endif

#ifndef __max_align_t_defined
#define __max_align_t_defined
typedef struct { long long __ll; long double __ld; } max_align_t;
#endif

#ifndef __harvey_fast_int_types
#define __harvey_fast_int_types
typedef int int_fast16_t;
typedef int int_fast32_t;
typedef unsigned int uint_fast16_t;
typedef unsigned int uint_fast32_t;
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif
float strtof(const char *, char **);
long double strtold(const char *, char **);
#ifdef __cplusplus
}
#endif

/* APEX maps remove() to Plan 9's __remove() with a macro, which breaks
 * libc++'s std::ranges::remove.  Pull in the C header, drop the macro, and
 * provide a real declaration for <cstdio>. */
#include <stdio.h>
#ifdef remove
#undef remove
#endif
#ifdef __cplusplus
extern "C" {
#endif
int remove(const char *);
#ifdef __cplusplus
}
#endif

#endif
