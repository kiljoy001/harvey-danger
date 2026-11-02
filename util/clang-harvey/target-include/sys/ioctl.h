/* APEX guards <sys/ioctl.h> behind _BSD_EXTENSION; enable it just for this
 * include instead of turning the whole Plan 9 extension set on. */
#ifndef HARVEY_SYS_IOCTL_H
#define HARVEY_SYS_IOCTL_H

#ifndef _BSD_EXTENSION
#define __HARVEY_TEMP_BSD_EXTENSION
#define _BSD_EXTENSION 1
#endif

#include_next <sys/ioctl.h>

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#ifndef TIOCGWINSZ
#define TIOCGWINSZ 0
#endif
#ifndef TIOCSWINSZ
#define TIOCSWINSZ 1
#endif

#ifdef __cplusplus
}
#endif

#ifdef __HARVEY_TEMP_BSD_EXTENSION
#undef _BSD_EXTENSION
#undef __HARVEY_TEMP_BSD_EXTENSION
#endif

#endif
