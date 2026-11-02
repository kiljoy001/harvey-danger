#include <stdlib.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <utime.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <poll.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <pwd.h>
#include <signal.h>
#ifdef PATH_MAX
#define HARVEY_PATH_MAX PATH_MAX
#else
#define HARVEY_PATH_MAX 4096
#endif

extern "C" long long nsec(void);

extern "C" int clock_gettime(clockid_t clk, struct timespec *ts) {
    if (!ts) return -1;
    if (clk == CLOCK_REALTIME) {
        ts->tv_sec = time(0);
        ts->tv_nsec = 0;
    } else {
        long long ns = nsec();
        ts->tv_sec = (time_t)(ns / 1000000000LL);
        ts->tv_nsec = (long)(ns % 1000000000LL);
    }
    return 0;
}

extern "C" int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (req && req->tv_sec > 0)
        __sleep((unsigned int)req->tv_sec);
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

/* POSIX names for APEX's __write/__read/... entry points.  See compat.h. */
/* APEX has no getpagesize(); Harvey maps 4 KiB pages. */
static void *harvey_aligned_alloc(size_t n, size_t align);

/* C11 aligned allocation over the same allocator. */
extern "C" void *aligned_alloc(size_t align, size_t n) {
    return harvey_aligned_alloc(n, align);
}

extern "C" void *memalign(size_t align, size_t n) {
    return harvey_aligned_alloc(n, align);
}

extern "C" int posix_memalign(void **out, size_t align, size_t n) {
    void *p = harvey_aligned_alloc(n, align);
    if (!p) return 12 /* ENOMEM */;
    *out = p;
    return 0;
}

extern "C" int getpagesize(void) {
    return 4096;
}

extern "C" int statfs(const char *path, struct statfs *fs) {
    (void)path; (void)fs;
    errno = 38 /* ENOSYS */;
    return -1;
}

extern "C" int fstatfs(int fd, struct statfs *fs) {
    (void)fd; (void)fs;
    errno = 38 /* ENOSYS */;
    return -1;
}

extern "C" int poll(struct pollfd *fds, nfds_t n, int timeout) {
    (void)fds; (void)n; (void)timeout;
    errno = 38 /* ENOSYS */;
    return -1;
}

/* Harvey has no resource limits; report an 8 MiB stack like POSIX defaults. */
extern "C" int getrlimit(int resource, struct rlimit *rl) {
    if (!rl) { errno = 22 /* EINVAL */; return -1; }
    if (resource == RLIMIT_STACK) {
        rl->rlim_cur = 8u * 1024u * 1024u;
        rl->rlim_max = RLIM_INFINITY;
        return 0;
    }
    rl->rlim_cur = RLIM_INFINITY;
    rl->rlim_max = RLIM_INFINITY;
    return 0;
}

extern "C" int setrlimit(int resource, const struct rlimit *rl) {
    (void)resource; (void)rl;
    return 0;
}

extern "C" int getrusage(int who, struct rusage *ru) {
    (void)who;
    if (!ru) { errno = 22 /* EINVAL */; return -1; }
    return -1;
}

/* APEX has no realpath().  Normalise the path lexically; symbolic links are
 * not resolved, which is enough for clang's own-executable lookup. */
extern "C" char *realpath(const char *path, char *resolved) {
    char work[HARVEY_PATH_MAX];
    char out[HARVEY_PATH_MAX];
    size_t olen = 0;

    if (!path) { errno = 22 /* EINVAL */; return 0; }
    if (path[0] == '/') {
        work[0] = 0;
        strncpy(work, path, sizeof work - 1);
        work[sizeof work - 1] = 0;
    } else {
        if (!getcwd(work, sizeof work)) return 0;
        size_t n = strlen(work);
        if (n + 1 < sizeof work) {
            work[n] = '/';
            strncpy(work + n + 1, path, sizeof work - n - 2);
            work[sizeof work - 1] = 0;
        }
    }

    out[olen++] = '/';
    char *p = work;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *q = p;
        while (*q && *q != '/') q++;
        size_t n = (size_t)(q - p);
        if (n == 1 && p[0] == '.') {
            /* skip */
        } else if (n == 2 && p[0] == '.' && p[1] == '.') {
            while (olen > 1 && out[olen - 1] != '/') olen--;
            if (olen > 1) olen--;
            if (olen == 0) out[olen++] = '/';
        } else {
            if (olen != 1) out[olen++] = '/';
            if (olen + n + 1 >= sizeof out) { errno = 36 /* ENAMETOOLONG */; return 0; }
            memcpy(out + olen, p, n);
            olen += n;
        }
        p = q;
    }
    out[olen] = 0;

    if (resolved) {
        strcpy(resolved, out);
        return resolved;
    }
    char *m = (char *)malloc(olen + 1);
    if (!m) return 0;
    memcpy(m, out, olen + 1);
    return m;
}

extern "C" char *strtok_r(char *s, const char *delim, char **save) {
    if (!s) s = *save;
    if (!s) return 0;
    s += strspn(s, delim);
    if (!*s) { *save = s; return 0; }
    char *end = s + strcspn(s, delim);
    if (*end) { *end = 0; *save = end + 1; }
    else *save = end;
    return s;
}

/* Single-threaded password database shim: reuse libap's static entries. */
extern "C" int getpwnam_r(const char *name, struct passwd *pwd, char *buf,
                          size_t len, struct passwd **result) {
    (void)buf; (void)len;
    struct passwd *p = getpwnam(name);
    if (!p) { if (result) *result = 0; return 0; }
    *pwd = *p;
    if (result) *result = pwd;
    return 0;
}

extern "C" int getpwuid_r(unsigned int uid, struct passwd *pwd, char *buf,
                          size_t len, struct passwd **result) {
    (void)buf; (void)len;
    struct passwd *p = getpwuid((uid_t)uid);
    if (!p) { if (result) *result = 0; return 0; }
    *pwd = *p;
    if (result) *result = pwd;
    return 0;
}

extern "C" char *strsignal(int sig) {
    static char buf[32];
    switch (sig) {
    case 1:  return (char *)"hangup";
    case 2:  return (char *)"interrupt";
    case 3:  return (char *)"quit";
    case 4:  return (char *)"illegal instruction";
    case 5:  return (char *)"abort";
    case 6:  return (char *)"floating point exception";
    case 7:  return (char *)"killed";
    case 8:  return (char *)"segmentation violation";
    case 9:  return (char *)"write on closed pipe";
    case 10: return (char *)"alarm";
    case 11: return (char *)"terminated";
    case 12: return (char *)"user defined signal 1";
    case 13: return (char *)"user defined signal 2";
    case 14: return (char *)"bus error";
    default: break;
    }
    buf[0] = 's'; buf[1] = 'i'; buf[2] = 'g'; buf[3] = 'n'; buf[4] = 'a';
    buf[5] = 'l'; buf[6] = ' '; buf[7] = 0;
    return buf;
}

extern "C" long syscall(long n, ...) {
    (void)n;
    errno = 38 /* ENOSYS */;
    return -1;
}

extern "C" void _Exit(int status) {
    extern void _exit(int);
    _exit(status);
}

extern "C" int fchown(int fd, unsigned int uid, unsigned int gid) {
    (void)fd; (void)uid; (void)gid;
    return 0;
}

extern "C" int msync(void *addr, size_t len, int flags) {
    (void)addr; (void)len; (void)flags;
    return 0;
}

extern "C" int madvise(void *addr, size_t len, int advice) {
    (void)addr; (void)len; (void)advice;
    return 0;
}

extern "C" int sigaltstack(const stack_t *ss, stack_t *old) {
    if (old) {
        old->ss_sp = 0;
        old->ss_flags = SS_DISABLE;
        old->ss_size = 0;
    }
    (void)ss;
    return 0;
}

extern "C" int statvfs(const char *path, struct statvfs *vfs) {
    (void)path; (void)vfs;
    errno = 38 /* ENOSYS */;
    return -1;
}

extern "C" int fstatvfs(int fd, struct statvfs *vfs) {
    (void)fd; (void)vfs;
    errno = 38 /* ENOSYS */;
    return -1;
}

extern "C" int utimes(const char *path, const struct timeval *tv) {
    if (!tv) return utime(path, 0);
    struct utimbuf ut;
    ut.actime = tv[0].tv_sec;
    ut.modtime = tv[1].tv_sec;
    return utime(path, &ut);
}

extern "C" int usleep(unsigned int usec) {
    unsigned int sec = usec / 1000000u;
    if (sec)
        __sleep(sec);
    return 0;
}

extern "C" int getsid(int pid) {
    (void)pid;
    return 1;
}

extern "C" float strtof(const char *s, char **end) {
    return (float)strtod(s, end);
}

extern "C" long double strtold(const char *s, char **end) {
    return (long double)strtod(s, end);
}

#include <stdio.h>
#ifdef remove
#undef remove
#endif
/* C++17 aligned allocation, which APEX's libcxxabi stub does not provide. */
#include <new>
#include <stdint.h>

extern "C" void *malloc(size_t);
extern "C" void free(void *);

static void *harvey_aligned_alloc(size_t n, size_t align) {
    if (align < sizeof(void *)) align = sizeof(void *);
    void *base = malloc(n + align + sizeof(void *));
    if (!base) return nullptr;
    uintptr_t p = (uintptr_t)base + sizeof(void *);
    p = (p + align - 1) & ~(uintptr_t)(align - 1);
    ((void **)p)[-1] = base;
    return (void *)p;
}

static void harvey_aligned_free(void *p) {
    if (p) free(((void **)p)[-1]);
}

/* APEX's malloc only guarantees 8-byte alignment, but x86-64 code compiled
 * by clang emits 16-byte aligned SSE stores (movaps) into objects from
 * ordinary new.  Route every allocation through the aligned allocator. */
/* APEX's malloc is now 16-byte aligned, so the plain operator new/delete
 * from libcxxabi is usable; only the nothrow and aligned forms are ours. */
void *operator new(size_t n, const std::nothrow_t &) noexcept {
    return harvey_aligned_alloc(n, 16);
}
void *operator new[](size_t n, const std::nothrow_t &) noexcept {
    return harvey_aligned_alloc(n, 16);
}
void *operator new(size_t n, std::align_val_t a, const std::nothrow_t &) noexcept {
    return harvey_aligned_alloc(n, (size_t)a);
}
void *operator new[](size_t n, std::align_val_t a, const std::nothrow_t &) noexcept {
    return harvey_aligned_alloc(n, (size_t)a);
}

void *operator new(size_t n, std::align_val_t a) { return harvey_aligned_alloc(n, (size_t)a); }
void *operator new[](size_t n, std::align_val_t a) { return harvey_aligned_alloc(n, (size_t)a); }
void operator delete(void *p, std::align_val_t) { harvey_aligned_free(p); }
void operator delete[](void *p, std::align_val_t) { harvey_aligned_free(p); }
void operator delete(void *p, size_t, std::align_val_t) { harvey_aligned_free(p); }
void operator delete[](void *p, size_t, std::align_val_t) { harvey_aligned_free(p); }
