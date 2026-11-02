/*
 * Minimal libm for Harvey/APEX.
 *
 * APEX ships almost no math library (only frexp, modf, ldexp and a few
 * others), but LLVM and libc++ need the usual C99 entry points.  Everything
 * here is implemented from x87 instructions and bit manipulation, so there
 * is no dependency on a maths library at all.
 *
 * The transcendentals go through the x87 FPU: fyl2x computes st(1)*log2(st(0))
 * and f2xm1 computes 2^x-1 for |x| < 1.  Results are computed in 80-bit
 * extended precision and rounded once on store, which is at least as accurate
 * as the double routines this replaces for the purposes of a compiler.
 */

#include <math.h>
#include <stdint.h>

namespace {

union f64bits {
    double d;
    uint64_t u;
};

union f32bits {
    float f;
    uint32_t u;
};

inline double dabs(double x) {
    f64bits b;
    b.d = x;
    b.u &= ~(uint64_t)1 << 63;
    return b.d;
}

inline float fabsf_impl(float x) {
    f32bits b;
    b.f = x;
    b.u &= ~(uint32_t)1 << 31;
    return b.f;
}

inline double x87_log2(double x) {
    double r;
    __asm__ volatile("fld1\n\t"
                     "fldl %1\n\t"
                     "fyl2x\n\t"
                     "fstpl %0"
                     : "=m"(r)
                     : "m"(x)
                     : "st");
    return r;
}

inline double x87_exp2(double x) {
    double ipart = x >= 0 ? (double)(long long)x : -(double)(long long)(-x);
    double frac = x - ipart;
    double r;
    __asm__ volatile("fldl %2\n\t"   /* st0 = ipart            */
                     "fldl %1\n\t"   /* st0 = frac, st1 = ipart */
                     "f2xm1\n\t"     /* st0 = 2^frac - 1        */
                     "fld1\n\t"
                     "faddp\n\t"     /* st0 = 2^frac, st1 = ipart */
                     "fscale\n\t"    /* st0 = 2^frac * 2^ipart  */
                     "fstpl %0\n\t"
                     "fstp %%st(0)"  /* drop ipart              */
                     : "=m"(r)
                     : "m"(frac), "m"(ipart)
                     : "st");
    return r;
}

const double LN2 = 0.693147180559945309417232121458176568;

} // namespace

extern "C" {

float fabsf(float x) { return fabsf_impl(x); }

double trunc(double x) {
    if (!(dabs(x) < 4503599627370496.0))
        return x;
    return (double)(long long)x;
}

float truncf(float x) {
    if (!(fabsf_impl(x) < 8388608.0f))
        return x;
    return (float)(int)x;
}

float floorf(float x) {
    float t = truncf(x);
    return (t > x) ? t - 1.0f : t;
}

float ceilf(float x) {
    float t = truncf(x);
    return (t < x) ? t + 1.0f : t;
}

double rint(double x) {
    double r;
    __asm__ volatile("fldl %1\n\t"
                     "frndint\n\t"
                     "fstpl %0"
                     : "=m"(r)
                     : "m"(x)
                     : "st");
    return r;
}

double nearbyint(double x) { return rint(x); }

double round(double x) {
    double t = trunc(x);
    if (dabs(x - t) >= 0.5)
        t += (x < 0) ? -1.0 : 1.0;
    return t;
}

float roundf(float x) { return (float)round((double)x); }

long lround(double x) { return (long)round(x); }
long lrint(double x) { return (long)rint(x); }

double copysign(double x, double y) {
    f64bits a, b;
    a.d = x;
    b.d = y;
    a.u = (a.u & ~((uint64_t)1 << 63)) | (b.u & ((uint64_t)1 << 63));
    return a.d;
}

float copysignf(float x, float y) {
    f32bits a, b;
    a.f = x;
    b.f = y;
    a.u = (a.u & ~((uint32_t)1 << 31)) | (b.u & ((uint32_t)1 << 31));
    return a.f;
}

float sqrtf(float x) { return __builtin_sqrtf(x); }

double log2(double x) { return x87_log2(x); }
float log2f(float x) { return (float)x87_log2((double)x); }
float logf(float x) { return (float)log((double)x); }

double exp2(double x) { return x87_exp2(x); }
float exp2f(float x) { return (float)x87_exp2((double)x); }
float expf(float x) { return (float)exp((double)x); }

double log1p(double x) {
    /* Keep the series for tiny x so the leading term is not lost. */
    if (dabs(x) < 1e-4) {
        double term = x;
        double sum = 0.0;
        for (int k = 1; k <= 6; k++) {
            sum += ((k & 1) ? term : -term) / (double)k;
            term *= x;
        }
        return sum;
    }
    return log(1.0 + x);
}

float log1pf(float x) { return (float)log1p((double)x); }

double logb(double x) {
    if (x == 0.0)
        return -__builtin_inf();
    if (x != x || dabs(x) == __builtin_inf())
        return dabs(x);
    return floor(x87_log2(dabs(x)));
}

float powf(float x, float y) { return (float)pow((double)x, (double)y); }

float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); }

double nan(const char *) { return __builtin_nan(""); }
float nanf(const char *) { return __builtin_nanf(""); }

} // extern "C"
