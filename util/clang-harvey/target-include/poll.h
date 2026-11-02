/* APEX has no <poll.h>.  LLVM's raw_socket_stream is the only user and is
 * not reachable from the clang driver, so poll() fails at runtime. */
#ifndef HARVEY_POLL_H
#define HARVEY_POLL_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int nfds_t;

struct pollfd {
    int   fd;
    short events;
    short revents;
};

#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

int poll(struct pollfd *, nfds_t, int);

#ifdef __cplusplus
}
#endif

#endif
