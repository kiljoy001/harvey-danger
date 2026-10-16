#ifndef VMDISPLAY_CLIENT_H
#define VMDISPLAY_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include "vmdisplay.h"

/* One host presenter owns the ring. Host-side screen mirroring can fan out
 * copied frames; portable remote sessions belong above the output backend. */
struct vmdisplay_client {
    struct vmdisplay *display;
    int fd;
    uint64_t seen;
};

int vmdisplay_client_open(struct vmdisplay_client *client, const char *name,
                          int timeout_ms);
void vmdisplay_client_close(struct vmdisplay_client *client);
int vmdisplay_client_frame(struct vmdisplay_client *client, uint32_t *pixels,
                           size_t pixel_count, uint64_t *serial);
int vmdisplay_client_input(struct vmdisplay_client *client, const char *record);
int vmdisplay_client_input_wait(struct vmdisplay_client *client,
                                const char *record, int timeout_ms);

#endif
