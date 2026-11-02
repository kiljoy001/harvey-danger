/* APEX guards <sys/socket.h> behind _BSD_EXTENSION. */
#ifndef HARVEY_SYS_SOCKET_H
#define HARVEY_SYS_SOCKET_H

#ifndef _BSD_EXTENSION
#define __HARVEY_TEMP_BSD_EXTENSION
#define _BSD_EXTENSION 1
#endif

#include_next <sys/socket.h>

#ifdef __HARVEY_TEMP_BSD_EXTENSION
#undef _BSD_EXTENSION
#undef __HARVEY_TEMP_BSD_EXTENSION
#endif

#endif
