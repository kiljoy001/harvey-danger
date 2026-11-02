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

#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* APEX's INT64_C/UINT64_C use the "long" suffix even though its int64_t is
 * "long long".  That makes every UINT64_C() literal a different type from
 * uint64_t and breaks overload resolution (std::max, constructors). */
#undef INT64_C
#undef UINT64_C
#define INT64_C(c)  c##LL
#define UINT64_C(c) c##ULL
#undef INT64_MAX
#undef INT64_MIN
#undef UINT64_MAX
#undef INT64_C
#undef UINT64_C
#define INT64_C(c)  c##LL
#define UINT64_C(c) c##ULL
#define INT64_MAX  9223372036854775807LL
#define INT64_MIN  (-INT64_MAX - 1LL)
#define UINT64_MAX 18446744073709551615ULL

#ifdef __cplusplus
extern "C" {
#endif
int getpagesize(void);
#ifdef __cplusplus
}
#endif

/* APEX's offsetof() takes the address of a null pointer, which is not a
 * constant expression in C++.  __builtin_offsetof is. */
#ifdef offsetof
#undef offsetof
#endif
#define offsetof(type, member) __builtin_offsetof(type, member)

/* APEX's <inttypes.h> has no format macros. */
#ifndef PRIx64
#define PRId8  "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "lld"
#define PRIi8  "i"
#define PRIi16 "i"
#define PRIi32 "i"
#define PRIi64 "lli"
#define PRIu8  "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "llu"
#define PRIo8  "o"
#define PRIo16 "o"
#define PRIo32 "o"
#define PRIo64 "llo"
#define PRIx8  "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 "llx"
#define PRIX8  "X"
#define PRIX16 "X"
#define PRIX32 "X"
#define PRIX64 "llX"
#define SCNd64 "lld"
#define SCNi64 "lli"
#define SCNu64 "llu"
#define SCNx64 "llx"
#define PRIdPTR "ld"
#define PRIiPTR "li"
#define PRIuPTR "lu"
#define PRIoPTR "lo"
#define PRIxPTR "lx"
#define PRIXPTR "lX"
#endif

#ifndef CLOCK_REALTIME
typedef int clockid_t;
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

#ifdef __cplusplus
extern "C" {
#endif
int clock_gettime(clockid_t, struct timespec *);
int nanosleep(const struct timespec *, struct timespec *);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif
float strtof(const char *, char **);
long double strtold(const char *, char **);
char *strerror_r(int, const char *, int);
struct tm *localtime_r(const time_t *, struct tm *);
struct tm *gmtime_r(const time_t *, struct tm *);
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

#ifdef __cplusplus
/*
 * APEX renames the POSIX entry points to __write/__read/__close/... so they
 * do not collide with Plan 9 library names, and defines macros with the
 * POSIX names.  C++ cannot live with those macros: they capture member
 * functions such as raw_ostream::write() and raw_fd_ostream::close().
 *
 * Pull the headers in once (setting their include guards), drop the macros,
 * and expose real POSIX declarations.  compat.cpp forwards them.
 */
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>

#undef write
#undef read
#undef close
#undef pipe
#undef dup
#undef chdir
#undef alarm
#undef atexit
#undef sleep

extern "C" {
ssize_t write(int, const void *, size_t);
ssize_t read(int, void *, size_t);
int close(int);
int pipe(int *);
int dup(int);
int chdir(const char *);
unsigned int alarm(unsigned int);
unsigned int sleep(unsigned int);
int atexit(void (*)(void));

/* libap exports gethostname but APEX declares it nowhere. */
int gethostname(char *, size_t);

/* APEX maps stat()/fstat() to __stat_ap()/__fstat() with function-like
 * macros, so the bare names cannot be used as function pointers.  These
 * inline wrappers give LLVM something to take the address of. */
static inline int __harvey_stat(const char *path, struct stat *sb) {
    return __stat_ap(path, sb);
}
static inline int __harvey_fstat(int fd, struct stat *sb) {
    return __fstat(fd, sb);
}

/* These live behind _BSD_EXTENSION in APEX's <sys/stat.h>. */
int lstat(const char *, struct stat *);
int symlink(const char *, const char *);
ssize_t readlink(const char *, char *, size_t);

/* No session support yet: getsid() reports the current process group. */
int getsid(int);

/* Missing from APEX entirely. */
void _Exit(int);
int wait4(int, int *, int, void *);
char *strsignal(int);
long syscall(long, ...);
int fchmod(int, unsigned int);
int fchown(int, unsigned int, unsigned int);
int msync(void *, size_t, int);
int madvise(void *, size_t, int);
int usleep(unsigned int);
struct timeval;
int utimes(const char *, const struct timeval *);
#ifndef MS_SYNC
#define MS_ASYNC 1
#define MS_INVALIDATE 2
#define MS_SYNC 4
#endif
#ifndef MADV_DONTNEED
#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4
#endif
#define _SC_GETPW_R_SIZE_MAX 20
char *realpath(const char *, char *);
char *strtok_r(char *, const char *, char **);
struct passwd;
int getpwnam_r(const char *, struct passwd *, char *, size_t, struct passwd **);
int getpwuid_r(unsigned int, struct passwd *, char *, size_t, struct passwd **);
}
#endif

#ifdef __cplusplus
/*
 * libc++'s Linux locale backend wants the POSIX *_l entry points.  APEX has
 * the wide ones (declared in <wctype.h> under _GNU_SOURCE) but not the
 * numeric/ctype ones; util/libcxx/locale_stub.cpp implements them.
 */
#include <locale.h>
#include <time.h>
extern "C" {
float strtof_l(const char *, char **, locale_t);
double strtod_l(const char *, char **, locale_t);
long double strtold_l(const char *, char **, locale_t);
long long strtoll_l(const char *, char **, int, locale_t);
unsigned long long strtoull_l(const char *, char **, int, locale_t);
int isdigit_l(int, locale_t);
int isxdigit_l(int, locale_t);
int toupper_l(int, locale_t);
int tolower_l(int, locale_t);
int strcoll_l(const char *, const char *, locale_t);
size_t strxfrm_l(char *, const char *, size_t, locale_t);
size_t strftime_l(char *, size_t, const char *, const struct tm *, locale_t);
}
#endif

#endif
