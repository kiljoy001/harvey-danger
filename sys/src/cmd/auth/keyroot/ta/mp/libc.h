#ifndef KEYROOT_MP_LIBC_H
#define KEYROOT_MP_LIBC_H

#include <tee_internal_api.h>
#include "u.h"

void	*malloc(usize n);
void	*mallocz(usize n, int clr);
void	*realloc(void *p, usize n);
void	free(void *p);
void	*memmove(void *d, const void *s, usize n);
void	*memset(void *d, int c, usize n);
void	*memcpy(void *d, const void *s, usize n);
int	memcmp(const void *a, const void *b, usize n);
usize	strlen(const char *s);
int	dec64(u8*, int, char*, int);
int	dec32(u8*, int, char*, int);

void	sysfatal(char *fmt, ...);
void	abort(void);
void	setmalloctag(void *p, usize t);
usize	getcallerpc(void);

#define assert(x) do { if(!(x)) TEE_Panic(0x1000); } while(0)

#endif
