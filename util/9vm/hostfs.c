#define _GNU_SOURCE
#include "hostfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>

static pthread_mutex_t display_lock = PTHREAD_MUTEX_INITIALIZER;

enum { DisplayNone, DisplayCtl, DisplayScreen0, DisplayScreen1, DisplayInput };

static int display_file(struct hostfs *h, const char *path) {
    char prefix[PATH_MAX];
    const char *name;
    size_t length;

    if (h->display == NULL)
        return DisplayNone;
    if (snprintf(prefix, sizeof prefix, "%s/build/aspera-live/dev/", h->root_dir) >=
        (int)sizeof prefix)
        return DisplayNone;
    length = strlen(prefix);
    if (strncmp(path, prefix, length) != 0) {
        if (snprintf(prefix, sizeof prefix, "%s/build/lola-live/dev/", h->root_dir) >=
            (int)sizeof prefix)
            return DisplayNone;
        length = strlen(prefix);
        if (strncmp(path, prefix, length) != 0)
            return DisplayNone;
    }
    name = path + length;
    if (strcmp(name, "ctl") == 0) return DisplayCtl;
    if (strcmp(name, "screen0") == 0) return DisplayScreen0;
    if (strcmp(name, "screen1") == 0) return DisplayScreen1;
    if (strcmp(name, "input") == 0) return DisplayInput;
    return DisplayNone;
}

/* 9P2000 message types */
enum {
    Tversion = 100, Rversion,
    Tauth    = 102, Rauth,
    Tattach  = 104, Rattach,
    Terror   = 106, Rerror,
    Tflush   = 108, Rflush,
    Twalk    = 110, Rwalk,
    Topen    = 112, Ropen,
    Tcreate  = 114, Rcreate,
    Tread    = 116, Rread,
    Twrite   = 118, Rwrite,
    Tclunk   = 120, Rclunk,
    Tremove  = 122, Rremove,
    Tstat    = 124, Rstat,
    Twstat   = 126, Rwstat,
};

/* QID types */
enum {
    QTDIR  = 0x80,
    QTFILE = 0x00,
};

static struct hostfs_fid *find_fid(struct hostfs *h, uint32_t fid) {
    for (int i = 0; i < HOSTFS_MAX_FIDS; i++) {
        if (h->fids[i].in_use && h->fids[i].fid == fid)
            return &h->fids[i];
    }
    return NULL;
}

static struct hostfs_fid *alloc_fid(struct hostfs *h, uint32_t fid) {
    struct hostfs_fid *f = find_fid(h, fid);
    if (f) return f;
    for (int i = 0; i < HOSTFS_MAX_FIDS; i++) {
        if (!h->fids[i].in_use) {
            memset(&h->fids[i], 0, sizeof(h->fids[i]));
            h->fids[i].in_use = 1;
            h->fids[i].fid = fid;
            h->fids[i].fd = -1;
            h->fids[i].omode = -1;
            return &h->fids[i];
        }
    }
    return NULL;
}

static void free_fid(struct hostfs *h, uint32_t fid) {
    struct hostfs_fid *f = find_fid(h, fid);
    if (f) {
        if (f->fd >= 0) close(f->fd);
        if (f->dir) closedir(f->dir);
        memset(f, 0, sizeof(*f));
    }
}

void hostfs_init(struct hostfs *h, const char *root_dir) {
    memset(h, 0, sizeof(*h));
    h->msize = 65560;
    if (realpath(root_dir, h->root_dir) == NULL) {
        strncpy(h->root_dir, root_dir, sizeof(h->root_dir) - 1);
    }
}

int hostfs_init_image(struct hostfs *h, const char *image_path) {
    char resolved[PATH_MAX];
    char *slash;
    struct stat st;

    if (realpath(image_path, resolved) == NULL ||
        stat(resolved, &st) < 0 || !S_ISREG(st.st_mode) ||
        st.st_size < 128 * 1024 * 1024 + 16 * 1024)
        return -1;
    slash = strrchr(resolved, '/');
    if (slash == NULL || slash == resolved)
        return -1;
    *slash = '\0';
    hostfs_init(h, resolved);
    snprintf(h->only_file, sizeof h->only_file, "%s/%s", resolved, slash + 1);
    return 0;
}

static int image_path_allowed(struct hostfs *h, const char *path) {
    return h->only_file[0] == '\0' ||
        strcmp(path, h->root_dir) == 0 || strcmp(path, h->only_file) == 0;
}

int hostfs_set_boot_override(struct hostfs *h, const char *path) {
    char resolved[PATH_MAX];
    struct stat st;
    size_t rootlen;

    if (realpath(path, resolved) == NULL || stat(resolved, &st) < 0 ||
        !S_ISREG(st.st_mode))
        return -1;
    rootlen = strlen(h->root_dir);
    if (strncmp(resolved, h->root_dir, rootlen) != 0 ||
        resolved[rootlen] != '/')
        return -1;
    snprintf(h->boot_override, sizeof h->boot_override, "%s", resolved);
    return 0;
}

void hostfs_set_display(struct hostfs *h, struct vmdisplay *display) {
    h->display = display;
}

static int display_control(struct vmdisplay *d, const uint8_t *data, uint32_t count,
                           uint64_t offset) {
    char text[33], verb[16], extra;
    int slot, matched, ok = -1;
    uint32_t expected;
    uint64_t serial;

    if (offset != 0 || count == 0 || count > 32)
        return -1;
    memcpy(text, data, count);
    text[count] = '\0';
    matched = sscanf(text, "%15s %d %c", verb, &slot, &extra);
    if (matched != 2 || slot < 0 || slot >= VMDISPLAY_SLOTS)
        return -1;
    pthread_mutex_lock(&display_lock);
    if (strcmp(verb, "begin") == 0) {
        expected = VMDISPLAY_FREE;
        if (__atomic_compare_exchange_n(&d->state[slot], &expected,
                VMDISPLAY_WRITING, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            d->written[slot] = 0;
            ok = count;
        }
    } else if (strcmp(verb, "abort") == 0) {
        if (__atomic_load_n(&d->state[slot], __ATOMIC_ACQUIRE) == VMDISPLAY_WRITING) {
            d->written[slot] = 0;
            __atomic_store_n(&d->state[slot], VMDISPLAY_FREE, __ATOMIC_RELEASE);
            ok = count;
        }
    } else if (strcmp(verb, "commit") == 0) {
        if (__atomic_load_n(&d->state[slot], __ATOMIC_ACQUIRE) == VMDISPLAY_WRITING &&
            d->written[slot] == VMDISPLAY_FRAME_BYTES) {
            serial = __atomic_load_n(&d->latest_serial, __ATOMIC_RELAXED) + 1;
            __atomic_store_n(&d->slot_serial[slot], serial, __ATOMIC_RELEASE);
            __atomic_store_n(&d->state[slot], VMDISPLAY_READY, __ATOMIC_RELEASE);
            __atomic_store_n(&d->latest_slot, slot, __ATOMIC_RELEASE);
            __atomic_store_n(&d->latest_serial, serial, __ATOMIC_RELEASE);
            ok = count;
        }
    }
    pthread_mutex_unlock(&display_lock);
    return ok;
}

static int display_write(struct vmdisplay *d, int slot, const uint8_t *data,
                         uint32_t count, uint64_t offset) {
    int ok = -1;

    pthread_mutex_lock(&display_lock);
    if (__atomic_load_n(&d->state[slot], __ATOMIC_ACQUIRE) == VMDISPLAY_WRITING &&
        offset == d->written[slot] && offset <= VMDISPLAY_FRAME_BYTES &&
        count <= VMDISPLAY_FRAME_BYTES - offset) {
        memcpy((uint8_t *)d->pixels[slot] + offset, data, count);
        d->written[slot] += count;
        ok = count;
    }
    pthread_mutex_unlock(&display_lock);
    return ok;
}

void hostfs_free(struct hostfs *h) {
    for (int i = 0; i < HOSTFS_MAX_FIDS; i++) {
        if (h->fids[i].in_use) {
            free_fid(h, h->fids[i].fid);
        }
    }
}

static uint32_t send_rerror(uint16_t tag, const char *ename, uint8_t *resp, uint32_t resp_max) {
    uint16_t elen = (uint16_t)strlen(ename);
    uint32_t size = 4 + 1 + 2 + 2 + elen;
    if (size > resp_max) return 0;
    *(uint32_t *)(resp + 0) = size;
    resp[4] = Rerror;
    *(uint16_t *)(resp + 5) = tag;
    *(uint16_t *)(resp + 7) = elen;
    memcpy(resp + 9, ename, elen);
    return size;
}

static int encode_stat(uint8_t *buf, int maxlen, const struct stat *st, const char *name) {
    int namelen = (int)strlen(name);
    const char *user = "scott";
    int userlen = (int)strlen(user);
    uint8_t qid_type = S_ISDIR(st->st_mode) ? QTDIR : QTFILE;
    uint32_t mode = ((uint32_t)qid_type << 24) | (st->st_mode & 0777);

    int stat_body_size = 2 + 4 + 13 + 4 + 4 + 4 + 8 + (2 + namelen) + (2 + userlen) * 3;
    int total_size = 2 + stat_body_size;
    if (total_size > maxlen) return -1;

    uint8_t *p = buf;
    *(uint16_t *)p = (uint16_t)stat_body_size; p += 2;
    *(uint16_t *)p = 0; p += 2;
    *(uint32_t *)p = 0; p += 4;
    *p++ = qid_type;
    *(uint32_t *)p = (uint32_t)st->st_mtime; p += 4;
    *(uint64_t *)p = (uint64_t)st->st_ino; p += 8;
    *(uint32_t *)p = mode; p += 4;
    *(uint32_t *)p = (uint32_t)st->st_atime; p += 4;
    *(uint32_t *)p = (uint32_t)st->st_mtime; p += 4;
    *(uint64_t *)p = S_ISDIR(st->st_mode) ? 0 : (uint64_t)st->st_size; p += 8;
    *(uint16_t *)p = (uint16_t)namelen; p += 2;
    memcpy(p, name, namelen); p += namelen;
    *(uint16_t *)p = (uint16_t)userlen; p += 2;
    memcpy(p, user, userlen); p += userlen;
    *(uint16_t *)p = (uint16_t)userlen; p += 2;
    memcpy(p, user, userlen); p += userlen;
    *(uint16_t *)p = (uint16_t)userlen; p += 2;
    memcpy(p, user, userlen); p += userlen;

    return total_size;
}

uint32_t hostfs_dispatch(struct hostfs *h, const uint8_t *req, uint32_t req_len,
                         uint8_t *resp, uint32_t resp_max) {
    if (req_len < 7) return 0;

    uint32_t msg_len = *(uint32_t *)(req + 0);
    if (msg_len > req_len) return 0;

    uint8_t type = req[4];
    uint16_t tag = *(uint16_t *)(req + 5);

    switch (type) {
    case Tversion: {
        uint32_t client_msize = *(uint32_t *)(req + 7);
        h->msize = client_msize < 65560 ? client_msize : 65560;
        const char *ver = "9P2000";
        uint16_t vlen = (uint16_t)strlen(ver);
        uint32_t rsize = 4 + 1 + 2 + 4 + 2 + vlen;
        if (rsize > resp_max) return 0;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Rversion;
        *(uint16_t *)(resp + 5) = tag;
        *(uint32_t *)(resp + 7) = h->msize;
        *(uint16_t *)(resp + 11) = vlen;
        memcpy(resp + 13, ver, vlen);
        return rsize;
    }
    case Tauth: {
        return send_rerror(tag, "authentication not required", resp, resp_max);
    }
    case Tflush: {
        uint32_t rsize = 4 + 1 + 2;
        if (rsize > resp_max) return 0;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Rflush;
        *(uint16_t *)(resp + 5) = tag;
        return rsize;
    }
    case Tattach: {
        uint32_t fid = *(uint32_t *)(req + 7);
        struct hostfs_fid *f = alloc_fid(h, fid);
        if (!f) return send_rerror(tag, "out of fids", resp, resp_max);

        strncpy(f->path, h->root_dir, sizeof(f->path) - 1);
        if (stat(f->path, &f->st) < 0) {
            free_fid(h, fid);
            return send_rerror(tag, strerror(errno), resp, resp_max);
        }

        uint32_t rsize = 4 + 1 + 2 + 13;
        if (rsize > resp_max) return 0;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Rattach;
        *(uint16_t *)(resp + 5) = tag;
        resp[7] = S_ISDIR(f->st.st_mode) ? QTDIR : QTFILE;
        *(uint32_t *)(resp + 8) = (uint32_t)f->st.st_mtime;
        *(uint64_t *)(resp + 12) = (uint64_t)f->st.st_ino;
        return rsize;
    }
    case Twalk: {
        uint32_t fid = *(uint32_t *)(req + 7);
        uint32_t newfid = *(uint32_t *)(req + 11);
        uint16_t nwname = *(uint16_t *)(req + 15);

        struct hostfs_fid *oldf = find_fid(h, fid);
        if (!oldf) return send_rerror(tag, "unknown fid", resp, resp_max);

        struct hostfs_fid *nf = alloc_fid(h, newfid);
        if (!nf) return send_rerror(tag, "out of fids", resp, resp_max);

        char cur_path[PATH_MAX];
        strncpy(cur_path, oldf->path, sizeof(cur_path) - 1);

        uint16_t nwqid = 0;
        const uint8_t *p = req + 17;
        uint8_t qid_bytes[16 * 13];

        for (int i = 0; i < nwname; i++) {
            if (p + 2 > req + msg_len) break;
            uint16_t wlen = *(uint16_t *)p; p += 2;
            if (p + wlen > req + msg_len) break;

            char elem[256];
            int copylen = wlen < 255 ? wlen : 255;
            memcpy(elem, p, copylen);
            elem[copylen] = '\0';
            p += wlen;

            char next_path[PATH_MAX];
            if (h->only_file[0] != '\0' &&
                strcmp(elem, ".") != 0 && strcmp(elem, "..") != 0 &&
                (strcmp(elem, "image") != 0 || strcmp(cur_path, h->root_dir) != 0))
                break;
            if (h->only_file[0] != '\0' && strcmp(elem, "image") == 0) {
                snprintf(next_path, sizeof next_path, "%s", h->only_file);
            } else if (strcmp(elem, ".") == 0) {
                strncpy(next_path, cur_path, sizeof(next_path) - 1);
            } else if (strcmp(elem, "..") == 0) {
                /* Strip last path component unless already root */
                if (strcmp(cur_path, h->root_dir) == 0) {
                    strncpy(next_path, cur_path, sizeof(next_path) - 1);
                } else {
                    strncpy(next_path, cur_path, sizeof(next_path) - 1);
                    char *slash = strrchr(next_path, '/');
                    if (slash && slash != next_path) {
                        *slash = '\0';
                    } else if (slash == next_path) {
                        next_path[1] = '\0';
                    }
                }
            } else {
                snprintf(next_path, sizeof(next_path), "%s/%s", cur_path, elem);
            }
            if (h->boot_override[0] != '\0' && strcmp(elem, "boot") == 0 &&
                strlen(cur_path) == strlen(h->root_dir) + 5 &&
                strncmp(cur_path, h->root_dir, strlen(h->root_dir)) == 0 &&
                strcmp(cur_path + strlen(h->root_dir), "/boot") == 0)
                snprintf(next_path, sizeof next_path, "%s", h->boot_override);

            if (!image_path_allowed(h, next_path))
                break;

            struct stat st;
            if (stat(next_path, &st) < 0) {
                break;
            }

            qid_bytes[nwqid * 13 + 0] = S_ISDIR(st.st_mode) ? QTDIR : QTFILE;
            *(uint32_t *)&qid_bytes[nwqid * 13 + 1] = (uint32_t)st.st_mtime;
            *(uint64_t *)&qid_bytes[nwqid * 13 + 5] = (uint64_t)st.st_ino;
            nwqid++;
            strncpy(cur_path, next_path, sizeof(cur_path) - 1);
        }

        if (nwname > 0 && nwqid == 0) {
            if (newfid != fid) free_fid(h, newfid);
            return send_rerror(tag, "file does not exist", resp, resp_max);
        }

        if (nwqid == nwname) {
            strncpy(nf->path, cur_path, sizeof(nf->path) - 1);
            stat(nf->path, &nf->st);
        }

        uint32_t rsize = 4 + 1 + 2 + 2 + nwqid * 13;
        if (rsize > resp_max) return 0;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Rwalk;
        *(uint16_t *)(resp + 5) = tag;
        *(uint16_t *)(resp + 7) = nwqid;
        if (nwqid > 0) {
            memcpy(resp + 9, qid_bytes, nwqid * 13);
        }
        return rsize;
    }
    case Topen: {
        uint32_t fid = *(uint32_t *)(req + 7);
        uint8_t mode = req[11];

        struct hostfs_fid *f = find_fid(h, fid);
        if (!f) return send_rerror(tag, "unknown fid", resp, resp_max);

        int device = display_file(h, f->path);
        if (device != DisplayNone &&
            ((device == DisplayInput && (mode & 3) != 0) ||
             (device != DisplayInput && (mode & 3) != 1) ||
             (mode & 16) != 0))
            return send_rerror(tag, "bad display device mode", resp, resp_max);

        if (stat(f->path, &f->st) < 0)
            return send_rerror(tag, strerror(errno), resp, resp_max);

        if (!image_path_allowed(h, f->path) ||
            (h->only_file[0] != '\0' && (mode & 16) != 0))
            return send_rerror(tag, "image access denied", resp, resp_max);

        int flags = O_RDONLY;
        switch (mode & 3) {
        case 0: flags = O_RDONLY; break;
        case 1: flags = O_WRONLY; break;
        case 2: flags = O_RDWR; break;
        case 3: flags = O_RDONLY; break;
        }
        if (mode & 16) flags |= O_TRUNC;

        if (S_ISDIR(f->st.st_mode)) {
            f->dir = opendir(f->path);
            if (!f->dir) return send_rerror(tag, strerror(errno), resp, resp_max);
            f->diroffset = 0;
            f->direof = 0;
            f->dirent = NULL;
        } else {
            if (h->only_file[0] != '\0')
                flags |= O_DSYNC;
            f->fd = open(f->path, flags);
            if (f->fd < 0) return send_rerror(tag, strerror(errno), resp, resp_max);
        }
        f->omode = mode;

        uint32_t rsize = 4 + 1 + 2 + 13 + 4;
        if (rsize > resp_max) return 0;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Ropen;
        *(uint16_t *)(resp + 5) = tag;
        resp[7] = S_ISDIR(f->st.st_mode) ? QTDIR : QTFILE;
        *(uint32_t *)(resp + 8) = (uint32_t)f->st.st_mtime;
        *(uint64_t *)(resp + 12) = (uint64_t)f->st.st_ino;
        *(uint32_t *)(resp + 20) = h->msize - 24;
        return rsize;
    }
    case Tcreate: {
        if (h->only_file[0] != '\0')
            return send_rerror(tag, "image export is fixed", resp, resp_max);
        uint32_t fid = *(uint32_t *)(req + 7);
        uint16_t namelen = *(uint16_t *)(req + 11);
        char name[256];
        int copylen = namelen < 255 ? namelen : 255;
        memcpy(name, req + 13, copylen);
        name[copylen] = '\0';

        uint32_t perm = *(uint32_t *)(req + 13 + namelen);
        uint8_t mode = req[13 + namelen + 4];

        struct hostfs_fid *f = find_fid(h, fid);
        if (!f) return send_rerror(tag, "unknown fid", resp, resp_max);

        char new_path[PATH_MAX];
        snprintf(new_path, sizeof(new_path), "%s/%s", f->path, name);
        if (display_file(h, new_path) != DisplayNone)
            return send_rerror(tag, "display device is fixed", resp, resp_max);

        if (perm & 0x80000000) { /* DMDIR */
            if (mkdir(new_path, perm & 0777) < 0)
                return send_rerror(tag, strerror(errno), resp, resp_max);
            f->dir = opendir(new_path);
            if (!f->dir) return send_rerror(tag, strerror(errno), resp, resp_max);
            f->diroffset = 0;
            f->direof = 0;
            f->dirent = NULL;
        } else {
            int flags = O_CREAT;
            switch (mode & 3) {
            case 0: flags |= O_RDONLY; break;
            case 1: flags |= O_WRONLY; break;
            case 2: flags |= O_RDWR; break;
            }
            if (mode & 16) flags |= O_TRUNC;
            f->fd = open(new_path, flags, perm & 0777);
            if (f->fd < 0) return send_rerror(tag, strerror(errno), resp, resp_max);
        }

        strncpy(f->path, new_path, sizeof(f->path) - 1);
        stat(f->path, &f->st);
        f->omode = mode;

        uint32_t rsize = 4 + 1 + 2 + 13 + 4;
        if (rsize > resp_max) return 0;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Rcreate;
        *(uint16_t *)(resp + 5) = tag;
        resp[7] = S_ISDIR(f->st.st_mode) ? QTDIR : QTFILE;
        *(uint32_t *)(resp + 8) = (uint32_t)f->st.st_mtime;
        *(uint64_t *)(resp + 12) = (uint64_t)f->st.st_ino;
        *(uint32_t *)(resp + 20) = h->msize - 24;
        return rsize;
    }
    case Tread: {
        uint32_t fid = *(uint32_t *)(req + 7);
        uint64_t offset = *(uint64_t *)(req + 11);
        uint32_t count = *(uint32_t *)(req + 19);

        struct hostfs_fid *f = find_fid(h, fid);
        if (!f) return send_rerror(tag, "unknown fid", resp, resp_max);
        if (display_file(h, f->path) != DisplayNone &&
            display_file(h, f->path) != DisplayInput)
            return send_rerror(tag, "display device is write only", resp, resp_max);

        uint32_t max_read = resp_max - 11;
        if (count > max_read) count = max_read;

        uint32_t actual_read = 0;

        if (f->dir) {
            if (h->only_file[0] != '\0') {
                struct stat st;
                int encoded;
                if (stat(h->only_file, &st) < 0)
                    return send_rerror(tag, strerror(errno), resp, resp_max);
                encoded = encode_stat(resp + 11, count, &st, "image");
                if (encoded < 0)
                    return send_rerror(tag, "directory read too small", resp, resp_max);
                if (offset == 0)
                    actual_read = (uint32_t)encoded;
                else if (offset != (uint64_t)encoded)
                    return send_rerror(tag, "bad directory offset", resp, resp_max);
            } else {
                if ((int)offset != f->diroffset) {
                    if (offset == 0) {
                        rewinddir(f->dir);
                        f->diroffset = 0;
                        f->direof = 0;
                        f->dirent = NULL;
                    } else {
                        return send_rerror(tag, "bad directory offset", resp, resp_max);
                    }
                }

                uint8_t *p = resp + 11;
                uint8_t *ep = resp + 11 + count;

                while (!f->direof) {
                    if (!f->dirent) {
                        f->dirent = readdir(f->dir);
                        if (!f->dirent) {
                            f->direof = 1;
                            break;
                        }
                    }
                    if (strcmp(f->dirent->d_name, ".") == 0 || strcmp(f->dirent->d_name, "..") == 0) {
                        f->dirent = NULL;
                        continue;
                    }

                    char entpath[PATH_MAX];
                    snprintf(entpath, sizeof(entpath), "%s/%s", f->path, f->dirent->d_name);
                    if (!image_path_allowed(h, entpath)) {
                        f->dirent = NULL;
                        continue;
                    }
                    struct stat entst;
                    if (stat(entpath, &entst) < 0) {
                        f->dirent = NULL;
                        continue;
                    }

                    int encoded = encode_stat(p, (int)(ep - p), &entst, f->dirent->d_name);
                    if (encoded < 0)
                        break;
                    p += encoded;
                    f->dirent = NULL;
                }

                actual_read = (uint32_t)(p - (resp + 11));
                f->diroffset += actual_read;
            }
        } else if (display_file(h, f->path) == DisplayInput) {
            struct vmdisplay *d = h->display;
            uint32_t tail = __atomic_load_n(&d->input_tail, __ATOMIC_RELAXED);
            uint32_t head = __atomic_load_n(&d->input_head, __ATOMIC_ACQUIRE);
            if (tail != head) {
                while (tail + 1 != head &&
                       strncmp(d->input[tail % VMDISPLAY_EVENTS], "move ", 5) == 0 &&
                       strncmp(d->input[(tail + 1) % VMDISPLAY_EVENTS], "move ", 5) == 0) {
                    tail++;
                }
                const char *event = d->input[tail % VMDISPLAY_EVENTS];
                size_t length = strnlen(event, VMDISPLAY_EVENT_BYTES);
                if (length <= count) {
                    memcpy(resp + 11, event, length);
                    actual_read = length;
                    __atomic_store_n(&d->input_tail, tail + 1, __ATOMIC_RELEASE);
                }
            }
        } else {
            if (f->fd >= 0) {
                ssize_t nr = pread(f->fd, resp + 11, count, (off_t)offset);
                actual_read = (nr > 0) ? (uint32_t)nr : 0;
            }
        }

        uint32_t rsize = 4 + 1 + 2 + 4 + actual_read;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Rread;
        *(uint16_t *)(resp + 5) = tag;
        *(uint32_t *)(resp + 7) = actual_read;
        return rsize;
    }
    case Twrite: {
        uint32_t fid = *(uint32_t *)(req + 7);
        uint64_t offset = *(uint64_t *)(req + 11);
        uint32_t count = *(uint32_t *)(req + 19);

        struct hostfs_fid *f = find_fid(h, fid);
        if (!f) return send_rerror(tag, "unknown fid", resp, resp_max);
        if (f->fd < 0) return send_rerror(tag, "bad fid for write", resp, resp_max);

        uint32_t actual_write = 0;
        if (req_len >= 23 + count) {
            int kind = display_file(h, f->path);
            if (kind == DisplayCtl) {
                int nw = display_control(h->display, req + 23, count, offset);
                if (nw < 0) return send_rerror(tag, "display control rejected", resp, resp_max);
                actual_write = nw;
            } else if (kind == DisplayScreen0 || kind == DisplayScreen1) {
                int nw = display_write(h->display, kind - DisplayScreen0,
                                       req + 23, count, offset);
                if (nw < 0) return send_rerror(tag, "display write rejected", resp, resp_max);
                actual_write = nw;
            } else if (kind == DisplayInput) {
                return send_rerror(tag, "display input is read only", resp, resp_max);
            } else {
                ssize_t nw = pwrite(f->fd, req + 23, count, (off_t)offset);
                if (nw < 0 && h->only_file[0] != '\0')
                    return send_rerror(tag, strerror(errno), resp, resp_max);
                actual_write = (nw > 0) ? (uint32_t)nw : 0;
            }
        }

        uint32_t rsize = 4 + 1 + 2 + 4;
        if (rsize > resp_max) return 0;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Rwrite;
        *(uint16_t *)(resp + 5) = tag;
        *(uint32_t *)(resp + 7) = actual_write;
        return rsize;
    }
    case Tclunk: {
        uint32_t fid = *(uint32_t *)(req + 7);
        free_fid(h, fid);

        uint32_t rsize = 4 + 1 + 2;
        if (rsize > resp_max) return 0;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Rclunk;
        *(uint16_t *)(resp + 5) = tag;
        return rsize;
    }
    case Tremove: {
        if (h->only_file[0] != '\0')
            return send_rerror(tag, "image export is fixed", resp, resp_max);
        uint32_t fid = *(uint32_t *)(req + 7);
        struct hostfs_fid *f = find_fid(h, fid);
        if (!f) return send_rerror(tag, "unknown fid", resp, resp_max);
        if (display_file(h, f->path) != DisplayNone)
            return send_rerror(tag, "display device cannot be removed", resp, resp_max);

        if (S_ISDIR(f->st.st_mode)) {
            rmdir(f->path);
        } else {
            unlink(f->path);
        }
        free_fid(h, fid);

        uint32_t rsize = 4 + 1 + 2;
        if (rsize > resp_max) return 0;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Rremove;
        *(uint16_t *)(resp + 5) = tag;
        return rsize;
    }
    case Tstat: {
        uint32_t fid = *(uint32_t *)(req + 7);
        struct hostfs_fid *f = find_fid(h, fid);
        if (!f) return send_rerror(tag, "unknown fid", resp, resp_max);

        if (stat(f->path, &f->st) < 0)
            return send_rerror(tag, strerror(errno), resp, resp_max);

        const char *basename = strrchr(f->path, '/');
        basename = (basename && basename[1]) ? basename + 1 : f->path;
        if (h->only_file[0] != '\0' && strcmp(f->path, h->only_file) == 0)
            basename = "image";

        uint8_t stat_buf[512];
        int stat_len = encode_stat(stat_buf, sizeof(stat_buf), &f->st, basename);
        if (stat_len < 0) return send_rerror(tag, "stat encode error", resp, resp_max);

        uint32_t rsize = 4 + 1 + 2 + 2 + stat_len;
        if (rsize > resp_max) return 0;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Rstat;
        *(uint16_t *)(resp + 5) = tag;
        *(uint16_t *)(resp + 7) = (uint16_t)stat_len;
        memcpy(resp + 9, stat_buf, stat_len);
        return rsize;
    }
    case Twstat: {
        if (h->only_file[0] != '\0')
            return send_rerror(tag, "image export is fixed", resp, resp_max);
        /* Accept stat update */
        uint32_t rsize = 4 + 1 + 2;
        if (rsize > resp_max) return 0;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Rwstat;
        *(uint16_t *)(resp + 5) = tag;
        return rsize;
    }
    default:
        return send_rerror(tag, "unsupported 9P operation", resp, resp_max);
    }
}
