#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

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
extern "C" int remove(const char *path) {
    return __remove(path);
}

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

void *operator new(size_t n, std::align_val_t a) { return harvey_aligned_alloc(n, (size_t)a); }
void *operator new[](size_t n, std::align_val_t a) { return harvey_aligned_alloc(n, (size_t)a); }
void operator delete(void *p, std::align_val_t) { harvey_aligned_free(p); }
void operator delete[](void *p, std::align_val_t) { harvey_aligned_free(p); }
void operator delete(void *p, size_t, std::align_val_t) { harvey_aligned_free(p); }
void operator delete[](void *p, size_t, std::align_val_t) { harvey_aligned_free(p); }
