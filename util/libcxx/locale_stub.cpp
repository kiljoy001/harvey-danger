/*
 * Minimal locale surface for libc++ on Harvey/APEX.
 *
 * APEX is musl-derived and already provides setlocale, localeconv,
 * newlocale, duplocale, freelocale, uselocale, the plain multibyte and wide
 * character functions, and the __*-prefixed _l variants of the wide ctype
 * functions (__iswdigit_l, __towupper_l, ...).
 *
 * libc++'s Linux locale backend calls the POSIX _l names directly, and APEX
 * has no numeric/ctype _l wrappers at all.  Harvey has a single C locale, so
 * every wrapper ignores the locale argument and forwards to the plain call.
 */

#include <ctype.h>
#include <nl_types.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

extern "C" {

/* APEX exports the wide _l family with a double-underscore prefix. */
int __iswctype_l(wint_t, wctype_t, locale_t);
int __iswspace_l(wint_t, locale_t);
int __iswprint_l(wint_t, locale_t);
int __iswcntrl_l(wint_t, locale_t);
int __iswupper_l(wint_t, locale_t);
int __iswlower_l(wint_t, locale_t);
int __iswalpha_l(wint_t, locale_t);
int __iswblank_l(wint_t, locale_t);
int __iswdigit_l(wint_t, locale_t);
int __iswpunct_l(wint_t, locale_t);
int __iswxdigit_l(wint_t, locale_t);
wint_t __towupper_l(wint_t, locale_t);
wint_t __towlower_l(wint_t, locale_t);
int __wcscoll_l(const wchar_t *, const wchar_t *, locale_t);
size_t __wcsxfrm_l(wchar_t *, const wchar_t *, size_t, locale_t);

int iswctype_l(wint_t c, wctype_t t, locale_t l) { return __iswctype_l(c, t, l); }
int iswspace_l(wint_t c, locale_t l) { return __iswspace_l(c, l); }
int iswprint_l(wint_t c, locale_t l) { return __iswprint_l(c, l); }
int iswcntrl_l(wint_t c, locale_t l) { return __iswcntrl_l(c, l); }
int iswupper_l(wint_t c, locale_t l) { return __iswupper_l(c, l); }
int iswlower_l(wint_t c, locale_t l) { return __iswlower_l(c, l); }
int iswalpha_l(wint_t c, locale_t l) { return __iswalpha_l(c, l); }
int iswblank_l(wint_t c, locale_t l) { return __iswblank_l(c, l); }
int iswdigit_l(wint_t c, locale_t l) { return __iswdigit_l(c, l); }
int iswpunct_l(wint_t c, locale_t l) { return __iswpunct_l(c, l); }
int iswxdigit_l(wint_t c, locale_t l) { return __iswxdigit_l(c, l); }
wint_t towupper_l(wint_t c, locale_t l) { return __towupper_l(c, l); }
wint_t towlower_l(wint_t c, locale_t l) { return __towlower_l(c, l); }
int wcscoll_l(const wchar_t *a, const wchar_t *b, locale_t l) { return __wcscoll_l(a, b, l); }
size_t wcsxfrm_l(wchar_t *d, const wchar_t *s, size_t n, locale_t l) { return __wcsxfrm_l(d, s, n, l); }
/* APEX's __wcsftime_l() calls a musl-internal helper that libap does not
 * export, so widen/narrow around the working strftime() instead. */
size_t wcsftime_l(wchar_t *s, size_t n, const wchar_t *f, const struct tm *tm, locale_t l) {
    (void)l;
    if (!s || n == 0) return 0;
    char fmt[256];
    size_t i;
    for (i = 0; f[i] && i + 1 < sizeof fmt; i++)
        fmt[i] = (char)f[i];
    fmt[i] = 0;

    char out[512];
    size_t r = strftime(out, sizeof out, fmt, tm);
    if (r == 0) return 0;
    size_t j;
    for (j = 0; j < r && j + 1 < n; j++)
        s[j] = (wchar_t)(unsigned char)out[j];
    s[j] = 0;
    return j;
}

/* Harvey has exactly one locale.  These are the locale objects; every
 * operation ignores them. */
struct __locale_struct {
    int __unused;
};

static struct __locale_struct harvey_c_locale;
static locale_t harvey_current_locale;

locale_t newlocale(int mask, const char *name, locale_t base) {
    (void)mask; (void)name; (void)base;
    return &harvey_c_locale;
}

void freelocale(locale_t loc) {
    (void)loc;
}

locale_t duplocale(locale_t loc) {
    return loc ? loc : &harvey_c_locale;
}

locale_t uselocale(locale_t loc) {
    locale_t old = harvey_current_locale ? harvey_current_locale : &harvey_c_locale;
    if (loc == LC_GLOBAL_LOCALE)
        harvey_current_locale = 0;
    else if (loc)
        harvey_current_locale = loc;
    return old;
}

/* Message catalogues do not exist; every lookup returns the default. */
nl_catd catopen(const char *name, int flag) {
    (void)name; (void)flag;
    return (nl_catd)-1;
}

char *catgets(nl_catd cat, int set, int msg, const char *def) {
    (void)cat; (void)set; (void)msg;
    return (char *)def;
}

int catclose(nl_catd cat) {
    (void)cat;
    return -1;
}

/* Bounded multibyte conversion.  APEX is single-byte, but drive it through
 * mbrtowc()/wcrtomb() so it keeps working if a real encoding appears. */
size_t mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t len, mbstate_t *ps) {
    const char *s = *src;
    size_t i = 0;
    mbstate_t st = ps ? *ps : mbstate_t{};
    while (i < nms && (len == (size_t)-1 || i < len)) {
        wchar_t wc;
        size_t n = mbrtowc(&wc, s, nms - i, &st);
        if (n == (size_t)-1 || n == (size_t)-2)
            return (size_t)-1;
        if (n == 0) {
            if (dst) dst[i] = 0;
            s = 0;
            *src = s;
            if (ps) *ps = st;
            return i;
        }
        if (dst) dst[i] = wc;
        s += n;
        i++;
    }
    *src = s;
    if (ps) *ps = st;
    return i;
}

size_t wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len, mbstate_t *ps) {
    const wchar_t *s = *src;
    size_t i = 0, out = 0;
    mbstate_t st = ps ? *ps : mbstate_t{};
    while (i < nwc && (len == (size_t)-1 || out < len)) {
        char buf[8];
        size_t n = wcrtomb(buf, s[i], &st);
        if (n == (size_t)-1)
            return (size_t)-1;
        if (len != (size_t)-1 && out + n > len)
            break;
        if (dst) {
            for (size_t j = 0; j < n; j++)
                dst[out + j] = buf[j];
        }
        out += n;
        i++;
        if (s[i - 1] == 0) {
            *src = 0;
            if (ps) *ps = st;
            return out;
        }
    }
    *src = s + i;
    if (ps) *ps = st;
    return out;
}

/* Numeric and narrow ctype conversions have no locale in Harvey. */
float strtof_l(const char *s, char **end, locale_t l) { (void)l; return strtof(s, end); }
double strtod_l(const char *s, char **end, locale_t l) { (void)l; return strtod(s, end); }
long double strtold_l(const char *s, char **end, locale_t l) { (void)l; return strtold(s, end); }
long long strtoll_l(const char *s, char **end, int base, locale_t l) { (void)l; return strtoll(s, end, base); }
unsigned long long strtoull_l(const char *s, char **end, int base, locale_t l) { (void)l; return strtoull(s, end, base); }

int isdigit_l(int c, locale_t l) { (void)l; return isdigit(c); }
int isxdigit_l(int c, locale_t l) { (void)l; return isxdigit(c); }
int toupper_l(int c, locale_t l) { (void)l; return toupper(c); }
int tolower_l(int c, locale_t l) { (void)l; return tolower(c); }
int strcoll_l(const char *a, const char *b, locale_t l) { (void)l; return strcoll(a, b); }
size_t strxfrm_l(char *d, const char *s, size_t n, locale_t l) { (void)l; return strxfrm(d, s, n); }
size_t strftime_l(char *s, size_t n, const char *f, const struct tm *tm, locale_t l) {
    (void)l;
    return strftime(s, n, f, tm);
}

} // extern "C"
