#ifndef HARVEY_ARM64_O9_COMPAT_H
#define HARVEY_ARM64_O9_COMPAT_H

#include <u.h>

/* Generated Objective-9C calls this fast path directly, but the upstream
 * public header currently exposes only its checked wrapper. */
extern void *o9_dispatch_call(void *, ulong, void *);

#endif
