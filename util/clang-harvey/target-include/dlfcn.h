/* Harvey has no dynamic loader.  LLVM's Unix headers include <dlfcn.h>
 * unconditionally; these stubs let the static build link, and
 * DynamicLibrary::getPermanentLibrary() fails at runtime as expected. */
#ifndef HARVEY_DLFCN_H
#define HARVEY_DLFCN_H

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY   1
#define RTLD_NOW    2
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL  0
#define RTLD_DEFAULT ((void *)0)

void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int dlclose(void *);
char *dlerror(void);

#ifdef __cplusplus
}
#endif

#endif
