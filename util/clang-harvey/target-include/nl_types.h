/* APEX has no message catalogues.  libc++'s <locale> only needs the types. */
#ifndef HARVEY_NL_TYPES_H
#define HARVEY_NL_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int nl_catd;
typedef int nl_item;

#define NL_SETD 1
#define NL_CAT_LOCALE 1

nl_catd catopen(const char *, int);
char *catgets(nl_catd, int, int, const char *);
int catclose(nl_catd);

#ifdef __cplusplus
}
#endif

#endif
