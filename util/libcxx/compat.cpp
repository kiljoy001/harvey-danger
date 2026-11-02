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
#include <link.h>
#include <dlfcn.h>
#include <exception>
#include <cstdlib>
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

/*
 * libc++abi's cxa_handlers.cpp defines std::__terminate/std::__unexpected,
 * but it also redefines std::terminate and friends, which libc++'s
 * exception.cpp already provides.  Only the two internal entry points are
 * needed, so define just those here.
 */
namespace std {
[[noreturn]] __attribute__((visibility("hidden"))) void
__terminate(void (*func)()) noexcept {
    func();
    abort();
}

[[noreturn]] __attribute__((visibility("hidden"))) void
__unexpected(void (*func)()) {
    func();
    terminate();
}
} // namespace std

/*
 * APEX's libcxxabi stub has no __cxa_thread_atexit().  Harvey has no real
 * threads, so register the destructor and run it at process exit.
 */
namespace {
struct TlsDtor {
    void (*fn)(void *);
    void *obj;
    TlsDtor *next;
};
TlsDtor *tls_dtors;
bool tls_dtor_registered;

void run_tls_dtors() {
    while (TlsDtor *d = tls_dtors) {
        tls_dtors = d->next;
        d->fn(d->obj);
        free(d);
    }
}
} // namespace

extern "C" int __cxa_thread_atexit(void (*fn)(void *), void *obj, void *) {
    TlsDtor *d = (TlsDtor *)malloc(sizeof *d);
    if (!d)
        return -1;
    d->fn = fn;
    d->obj = obj;
    d->next = tls_dtors;
    tls_dtors = d;
    if (!tls_dtor_registered) {
        tls_dtor_registered = true;
        atexit(run_tls_dtors);
    }
    return 0;
}

/* C11 aligned allocation over the same allocator. */
extern "C" void *aligned_alloc(size_t align, size_t n) {
    /* malloc already satisfies alignments up to 16, and its result is
     * free()able; larger alignments use the offset allocator. */
    if (align <= 16)
        return malloc(n);
    return harvey_aligned_alloc(n, align);
}

extern "C" void *memalign(size_t align, size_t n) {
    return aligned_alloc(align, n);
}

extern "C" int posix_memalign(void **out, size_t align, size_t n) {
    void *p = aligned_alloc(align, n);
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

/*
 * POSIX *at() family.  Harvey has no dirfd-relative lookup, so resolve the
 * directory path with fd2path() and join; AT_FDCWD means "relative to cwd",
 * which is what a plain relative path already does.
 */
extern "C" int fd2path(int, char *, int);

namespace {

/*
 * Harvey's fd2path() reports an empty path for descriptors opened with
 * open(), and libc++'s remove_all() threads directory descriptors through
 * openat()/fdopendir().  Record the paths openat() opens so fdopendir() can
 * find them again.
 */
struct FdPath {
    int fd;
    char path[1024];
};
FdPath fdpaths[64];

void record_fd_path(int fd, const char *path) {
    if (fd < 0)
        return;
    for (auto &e : fdpaths) {
        if (e.fd == 0 || e.fd == fd) {
            e.fd = fd;
            size_t n = strlen(path);
            if (n >= sizeof e.path)
                n = sizeof e.path - 1;
            memcpy(e.path, path, n);
            e.path[n] = 0;
            return;
        }
    }
}

const char *lookup_fd_path(int fd) {
    for (auto &e : fdpaths)
        if (e.fd != 0 && e.fd == fd)
            return e.path;
    return nullptr;
}

int harvey_dirfd_path(int dirfd, char *buf, size_t n) {
    int r;
    const char *known;
    if (dirfd == AT_FDCWD)
        return getcwd(buf, n) ? 0 : -1;
    known = lookup_fd_path(dirfd);
    if (known) {
        if (strlen(known) + 1 > n)
            return -1;
        strcpy(buf, known);
        return 0;
    }
    r = fd2path(dirfd, buf, (int)n - 1);
    if (r < 0)
        return -1;
    if (r >= (int)n)
        r = (int)n - 1;
    buf[r] = 0;
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' '))
        buf[--r] = 0;
    return 0;
}

int harvey_join(int dirfd, const char *path, char *out, size_t n) {
    char dir[1024];
    if (path[0] == '/' || path[0] == 0 || dirfd == AT_FDCWD) {
        if (strlen(path) + 1 > n)
            return -1;
        strcpy(out, path);
        return 0;
    }
    if (harvey_dirfd_path(dirfd, dir, sizeof dir) < 0)
        return -1;
    if (strlen(dir) + 1 + strlen(path) + 1 > n)
        return -1;
    strcpy(out, dir);
    strcat(out, "/");
    strcat(out, path);
    return 0;
}

} // namespace

extern "C" int openat(int dirfd, const char *path, int flags, ...) {
    char full[1200];
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (harvey_join(dirfd, path, full, sizeof full) < 0)
        return -1;
    int fd = __open(full, flags, mode);
    if (fd >= 0)
        record_fd_path(fd, full);
    return fd;
}

extern "C" int unlinkat(int dirfd, const char *path, int flags) {
    char full[1200];
    if (harvey_join(dirfd, path, full, sizeof full) < 0)
        return -1;
    return (flags & AT_REMOVEDIR) ? rmdir(full) : unlink(full);
}

extern "C" int mkdirat(int dirfd, const char *path, mode_t mode) {
    char full[1200];
    if (harvey_join(dirfd, path, full, sizeof full) < 0)
        return -1;
    return mkdir(full, mode);
}

extern "C" int renameat(int oldfd, const char *oldpath, int newfd,
                        const char *newpath) {
    char a[1200], b[1200];
    if (harvey_join(oldfd, oldpath, a, sizeof a) < 0 ||
        harvey_join(newfd, newpath, b, sizeof b) < 0)
        return -1;
    return rename(a, b);
}

extern "C" int symlinkat(const char *target, int dirfd, const char *path) {
    char full[1200];
    if (harvey_join(dirfd, path, full, sizeof full) < 0)
        return -1;
    return symlink(target, full);
}

extern "C" ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t n) {
    char full[1200];
    if (harvey_join(dirfd, path, full, sizeof full) < 0)
        return -1;
    return readlink(full, buf, n);
}

extern "C" int fstatat(int dirfd, const char *path, struct stat *sb, int flags) {
    char full[1200];
    if (harvey_join(dirfd, path, full, sizeof full) < 0)
        return -1;
    return (flags & AT_SYMLINK_NOFOLLOW) ? lstat(full, sb) : stat(full, sb);
}

extern "C" int fchmodat(int dirfd, const char *path, mode_t mode, int flags) {
    char full[1200];
    (void)flags;
    if (harvey_join(dirfd, path, full, sizeof full) < 0)
        return -1;
    return chmod(full, mode);
}

extern "C" int utimensat(int dirfd, const char *path,
                         const struct timespec ts[2], int flags) {
    char full[1200];
    struct timeval tv[2];
    (void)flags;
    if (harvey_join(dirfd, path, full, sizeof full) < 0)
        return -1;
    if (!ts)
        return utimes(full, 0);
    tv[0].tv_sec = ts[0].tv_sec;
    tv[0].tv_usec = ts[0].tv_nsec / 1000;
    tv[1].tv_sec = ts[1].tv_sec;
    tv[1].tv_usec = ts[1].tv_nsec / 1000;
    return utimes(full, tv);
}

extern "C" DIR *fdopendir(int fd) {
    char path[1024];
    if (harvey_dirfd_path(fd, path, sizeof path) < 0)
        return 0;
    return opendir(path);
}

extern "C" int truncate(const char *path, off_t length) {
    int fd = __open(path, O_WRONLY);
    int r;
    if (fd < 0)
        return -1;
    r = ftruncate(fd, length);
    __close(fd);
    return r;
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

/* Harvey has no dynamic loader: no shared objects, no symbol lookup. */
extern "C" void *dlopen(const char *name, int mode) {
    (void)name; (void)mode;
    return 0;
}

extern "C" void *dlsym(void *handle, const char *name) {
    (void)handle; (void)name;
    return 0;
}

extern "C" int dlclose(void *handle) {
    (void)handle;
    return -1;
}

extern "C" char *dlerror(void) {
    return (char *)"no dynamic loader";
}

extern "C" int dladdr(const void *addr, Dl_info *info) {
    (void)addr; (void)info;
    return 0;
}

/*
 * Harvey has no dynamic loader.  libunwind asks where the unwind tables are
 * through dl_iterate_phdr; report the one loaded image, taken from the ELF
 * header that lld exposes as __ehdr_start.  The executable is linked
 * static and non-PIE, so the program headers' addresses are absolute and
 * there is no load bias.
 */
extern "C" int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *, size_t, void *),
                              void *data) {
    extern char __ehdr_start;
    const unsigned char *eh = (const unsigned char *)&__ehdr_start;
    struct dl_phdr_info info;
    uint64_t phoff;
    uint16_t phentsize, phnum;

    if (!(eh[0] == 0x7f && eh[1] == 'E' && eh[2] == 'L' && eh[3] == 'F'))
        return 0;

    phoff = *(const uint64_t *)(eh + 32);
    phentsize = *(const uint16_t *)(eh + 54);
    phnum = *(const uint16_t *)(eh + 56);
    if (phentsize != sizeof(Elf64_Phdr))
        return 0;

    memset(&info, 0, sizeof info);
    info.dlpi_addr = 0;
    info.dlpi_name = "";
    info.dlpi_phdr = (const Elf64_Phdr *)(eh + phoff);
    info.dlpi_phnum = phnum;
    return callback(&info, sizeof info, data);
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
/* The nothrow forms pair with the ordinary operator delete, so they must
 * return pointers that free() accepts.  APEX's malloc is 16-byte aligned. */
void *operator new(size_t n, const std::nothrow_t &) noexcept { return malloc(n); }
void *operator new[](size_t n, const std::nothrow_t &) noexcept { return malloc(n); }
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
