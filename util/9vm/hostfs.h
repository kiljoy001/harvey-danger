#ifndef HOSTFS_H
#define HOSTFS_H

#include <stdint.h>
#include <stddef.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include "vmdisplay.h"

#define HOSTFS_MAX_FIDS 1024

struct hostfs_fid {
    int in_use;
    uint32_t fid;
    char path[PATH_MAX];
    int fd;
    int omode;
    DIR *dir;
    int diroffset;
    int direof;
    struct dirent *dirent;
    struct stat st;
};

struct hostfs {
    char root_dir[PATH_MAX];
    char only_file[PATH_MAX];
    char boot_override[PATH_MAX];
    struct vmdisplay *display;
    uint32_t msize;
    struct hostfs_fid fids[HOSTFS_MAX_FIDS];
};

void hostfs_init(struct hostfs *h, const char *root_dir);
int hostfs_init_image(struct hostfs *h, const char *image_path);
int hostfs_set_boot_override(struct hostfs *h, const char *path);
void hostfs_set_display(struct hostfs *h, struct vmdisplay *display);
void hostfs_free(struct hostfs *h);
uint32_t hostfs_dispatch(struct hostfs *h, const uint8_t *req, uint32_t req_len,
                         uint8_t *resp, uint32_t resp_max);

#endif /* HOSTFS_H */
