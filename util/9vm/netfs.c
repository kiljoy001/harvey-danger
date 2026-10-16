/*
 * netfs.c - User-space 9P2000 network file server for Prism microkernel
 * Exposes Plan 9 /net hierarchy over UNIX domain sockets for multi-VM setups.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_CLIENTS 64
#define MAX_FIDS 256
#define MAX_CONVS 32
#define BUFFER_SIZE (65536 + 24)
#define DEFAULT_SOCK_PATH "/tmp/netfs.sock"

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
#define QTDIR  0x80
#define QTFILE 0x00

#define DMDIR  0x80000000

/* Protocol identifiers */
enum {
    PROTO_ROOT = 0,
    PROTO_ETHER,
    PROTO_IPIFC,
    PROTO_UDP,
    PROTO_TCP,
    PROTO_IL,
    PROTO_COUNT
};

/* File types */
enum {
    FT_DIR = 0,
    FT_CLONE,
    FT_STATS,
    FT_CTL,
    FT_DATA,
    FT_STATUS,
};

/* Build 64-bit QID path */
static inline uint64_t make_path(uint8_t proto, uint8_t conv, uint8_t ftype) {
    return ((uint64_t)proto << 16) | ((uint64_t)conv << 8) | ftype;
}

static inline uint8_t path_proto(uint64_t path) { return (path >> 16) & 0xFF; }
static inline uint8_t path_conv(uint64_t path)  { return (path >> 8) & 0xFF; }
static inline uint8_t path_ftype(uint64_t path) { return path & 0xFF; }

struct conv {
    int in_use;
    char remote_addr[128];
    char status[128];
    uint8_t data_buf[4096];
    size_t data_len;
};

struct fid_entry {
    uint32_t fid;
    int in_use;
    uint64_t path;
    uint8_t qid_type;
    int open_mode;
    int clone_conv; /* If opened via clone, the allocated conv index */
    int clone_read_done;
};

struct client {
    int fd;
    uint8_t in_buf[BUFFER_SIZE];
    size_t in_len;
    uint32_t msize;
    struct fid_entry fids[MAX_FIDS];
    struct conv convs[PROTO_COUNT][MAX_CONVS];
    int next_conv[PROTO_COUNT];
};

static struct client clients[MAX_CLIENTS];
static int listen_fd = -1;
static volatile sig_atomic_t running = 1;

static void sigint_handler(int sig) {
    (void)sig;
    running = 0;
}

static void init_convs(struct client *c) {
    for (int p = 0; p < PROTO_COUNT; p++) {
        c->next_conv[p] = 0;
        for (int i = 0; i < MAX_CONVS; i++) {
            memset(&c->convs[p][i], 0, sizeof(struct conv));
        }
        /* Ensure conv 0 is active by default */
        c->convs[p][0].in_use = 1;
        snprintf(c->convs[p][0].status, sizeof(c->convs[p][0].status), "Idle\n");
    }
}

static struct fid_entry *find_fid(struct client *c, uint32_t fid) {
    for (int i = 0; i < MAX_FIDS; i++) {
        if (c->fids[i].in_use && c->fids[i].fid == fid)
            return &c->fids[i];
    }
    return NULL;
}

static struct fid_entry *alloc_fid(struct client *c, uint32_t fid) {
    struct fid_entry *f = find_fid(c, fid);
    if (f) return f;
    for (int i = 0; i < MAX_FIDS; i++) {
        if (!c->fids[i].in_use) {
            c->fids[i].in_use = 1;
            c->fids[i].fid = fid;
            c->fids[i].open_mode = -1;
            c->fids[i].clone_conv = -1;
            c->fids[i].clone_read_done = 0;
            return &c->fids[i];
        }
    }
    return NULL;
}

static void free_fid(struct client *c, uint32_t fid) {
    struct fid_entry *f = find_fid(c, fid);
    if (f) {
        memset(f, 0, sizeof(*f));
    }
}

static int encode_stat(uint8_t *buf, int maxlen, uint64_t path, uint8_t type, uint32_t mode, uint64_t length, const char *name) {
    int namelen = strlen(name);
    const char *user = "scott";
    int userlen = strlen(user);
    int stat_body_size = 2 + 4 + 13 + 4 + 4 + 4 + 8 + (2 + namelen) + (2 + userlen)*3;
    int total_size = 2 + stat_body_size;
    if (total_size > maxlen) return -1;

    uint8_t *p = buf;
    *(uint16_t *)p = (uint16_t)stat_body_size; p += 2;
    *(uint16_t *)p = 0; p += 2;
    *(uint32_t *)p = 0; p += 4;
    *p++ = type;
    *(uint32_t *)p = 0; p += 4;
    *(uint64_t *)p = path; p += 8;
    *(uint32_t *)p = mode; p += 4;
    *(uint32_t *)p = 1700000000; p += 4;
    *(uint32_t *)p = 1700000000; p += 4;
    *(uint64_t *)p = length; p += 8;
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

static void send_rerror(struct client *c, uint16_t tag, const char *ename) {
    uint8_t resp[256];
    uint16_t elen = strlen(ename);
    uint32_t size = 4 + 1 + 2 + 2 + elen;
    *(uint32_t *)(resp + 0) = size;
    resp[4] = Terror + 1;
    *(uint16_t *)(resp + 5) = tag;
    *(uint16_t *)(resp + 7) = elen;
    memcpy(resp + 9, ename, elen);
    write(c->fd, resp, size);
}

static void handle_9p_message(struct client *c, uint8_t *msg, uint32_t size) {
    uint8_t type = msg[4];
    uint16_t tag = *(uint16_t *)(msg + 5);

    switch (type) {
    case Tversion: {
        uint32_t client_msize = *(uint32_t *)(msg + 7);
        c->msize = client_msize < BUFFER_SIZE ? client_msize : BUFFER_SIZE;
        uint8_t resp[32];
        const char *ver = "9P2000";
        uint16_t vlen = strlen(ver);
        uint32_t rsize = 4 + 1 + 2 + 4 + 2 + vlen;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Rversion;
        *(uint16_t *)(resp + 5) = tag;
        *(uint32_t *)(resp + 7) = c->msize;
        *(uint16_t *)(resp + 11) = vlen;
        memcpy(resp + 13, ver, vlen);
        write(c->fd, resp, rsize);
        break;
    }
    case Tauth: {
        send_rerror(c, tag, "authentication not required");
        break;
    }
    case Tattach: {
        uint32_t fid = *(uint32_t *)(msg + 7);
        struct fid_entry *f = alloc_fid(c, fid);
        if (!f) {
            send_rerror(c, tag, "out of fids");
            return;
        }
        f->path = make_path(PROTO_ROOT, 0xFF, FT_DIR);
        f->qid_type = QTDIR;

        uint8_t resp[32];
        uint32_t rsize = 4 + 1 + 2 + 13;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Rattach;
        *(uint16_t *)(resp + 5) = tag;
        resp[7] = QTDIR;
        *(uint32_t *)(resp + 8) = 0;
        *(uint64_t *)(resp + 12) = f->path;
        write(c->fd, resp, rsize);
        break;
    }
    case Tflush: {
        uint8_t resp[16];
        uint32_t rsize = 4 + 1 + 2;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Rflush;
        *(uint16_t *)(resp + 5) = tag;
        write(c->fd, resp, rsize);
        break;
    }
    case Twalk: {
        uint32_t fid = *(uint32_t *)(msg + 7);
        uint32_t newfid = *(uint32_t *)(msg + 11);
        uint16_t nwname = *(uint16_t *)(msg + 15);

        struct fid_entry *f = find_fid(c, fid);
        if (!f) {
            send_rerror(c, tag, "invalid fid");
            return;
        }

        struct fid_entry *nf = (newfid == fid) ? f : alloc_fid(c, newfid);
        if (!nf) {
            send_rerror(c, tag, "out of fids");
            return;
        }

        uint64_t cur_path = f->path;
        uint8_t cur_qid_type = f->qid_type;

        uint8_t resp[BUFFER_SIZE];
        uint8_t *p = resp + 9;
        uint16_t nwqid = 0;

        uint8_t *in = msg + 17;
        for (int i = 0; i < nwname; i++) {
            uint16_t namelen = *(uint16_t *)in; in += 2;
            char name[64];
            int clen = namelen < 63 ? namelen : 63;
            memcpy(name, in, clen);
            name[clen] = '\0';
            in += namelen;

            uint8_t proto = path_proto(cur_path);
            uint8_t conv  = path_conv(cur_path);
            uint8_t ftype = path_ftype(cur_path);

            uint64_t next_path = 0;
            uint8_t next_qid_type = QTFILE;

            if (strcmp(name, "..") == 0) {
                if (conv != 0xFF) {
                    next_path = make_path(proto, 0xFF, FT_DIR);
                    next_qid_type = QTDIR;
                } else if (proto != PROTO_ROOT) {
                    next_path = make_path(PROTO_ROOT, 0xFF, FT_DIR);
                    next_qid_type = QTDIR;
                } else {
                    next_path = make_path(PROTO_ROOT, 0xFF, FT_DIR);
                    next_qid_type = QTDIR;
                }
            } else if (proto == PROTO_ROOT) {
                if (strcmp(name, "ether0") == 0) {
                    next_path = make_path(PROTO_ETHER, 0xFF, FT_DIR);
                    next_qid_type = QTDIR;
                } else if (strcmp(name, "ipifc") == 0) {
                    next_path = make_path(PROTO_IPIFC, 0xFF, FT_DIR);
                    next_qid_type = QTDIR;
                } else if (strcmp(name, "udp") == 0) {
                    next_path = make_path(PROTO_UDP, 0xFF, FT_DIR);
                    next_qid_type = QTDIR;
                } else if (strcmp(name, "tcp") == 0) {
                    next_path = make_path(PROTO_TCP, 0xFF, FT_DIR);
                    next_qid_type = QTDIR;
                } else if (strcmp(name, "il") == 0) {
                    next_path = make_path(PROTO_IL, 0xFF, FT_DIR);
                    next_qid_type = QTDIR;
                } else {
                    break;
                }
            } else if (proto == PROTO_ETHER) {
                if (strcmp(name, "stats") == 0) {
                    next_path = make_path(PROTO_ETHER, 0xFF, FT_STATS);
                    next_qid_type = QTFILE;
                } else {
                    break;
                }
            } else if (conv == 0xFF) {
                if (strcmp(name, "clone") == 0) {
                    next_path = make_path(proto, 0xFF, FT_CLONE);
                    next_qid_type = QTFILE;
                } else if (strcmp(name, "stats") == 0) {
                    next_path = make_path(proto, 0xFF, FT_STATS);
                    next_qid_type = QTFILE;
                } else {
                    int cnum = atoi(name);
                    if (cnum >= 0 && cnum < MAX_CONVS) {
                        c->convs[proto][cnum].in_use = 1;
                        next_path = make_path(proto, cnum, FT_DIR);
                        next_qid_type = QTDIR;
                    } else {
                        break;
                    }
                }
            } else {
                /* Inside conv dir */
                if (strcmp(name, "ctl") == 0) {
                    next_path = make_path(proto, conv, FT_CTL);
                    next_qid_type = QTFILE;
                } else if (strcmp(name, "data") == 0) {
                    next_path = make_path(proto, conv, FT_DATA);
                    next_qid_type = QTFILE;
                } else if (strcmp(name, "status") == 0) {
                    next_path = make_path(proto, conv, FT_STATUS);
                    next_qid_type = QTFILE;
                } else {
                    break;
                }
            }

            cur_path = next_path;
            cur_qid_type = next_qid_type;

            *p++ = cur_qid_type;
            *(uint32_t *)p = 0; p += 4;
            *(uint64_t *)p = cur_path; p += 8;
            nwqid++;
        }

        if (nwname > 0 && nwqid == 0) {
            send_rerror(c, tag, "file not found");
            return;
        }

        nf->path = cur_path;
        nf->qid_type = cur_qid_type;

        uint32_t rsize = 4 + 1 + 2 + 2 + nwqid * 13;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Rwalk;
        *(uint16_t *)(resp + 5) = tag;
        *(uint16_t *)(resp + 7) = nwqid;
        write(c->fd, resp, rsize);
        break;
    }
    case Topen: {
        uint32_t fid = *(uint32_t *)(msg + 7);
        uint8_t mode = msg[11];
        struct fid_entry *f = find_fid(c, fid);
        if (!f) {
            send_rerror(c, tag, "invalid fid");
            return;
        }

        f->open_mode = mode;
        uint8_t proto = path_proto(f->path);
        uint8_t ftype = path_ftype(f->path);

        if (ftype == FT_CLONE) {
            /* Allocate conversation slot */
            int conv_idx = c->next_conv[proto]++;
            if (conv_idx >= MAX_CONVS) conv_idx = 0;
            c->convs[proto][conv_idx].in_use = 1;
            f->clone_conv = conv_idx;
            f->clone_read_done = 0;
        }

        uint8_t resp[32];
        uint32_t rsize = 4 + 1 + 2 + 13 + 4;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Ropen;
        *(uint16_t *)(resp + 5) = tag;
        resp[7] = f->qid_type;
        *(uint32_t *)(resp + 8) = 0;
        *(uint64_t *)(resp + 12) = f->path;
        *(uint32_t *)(resp + 20) = 8192; /* iounit */
        write(c->fd, resp, rsize);
        break;
    }
    case Tread: {
        uint32_t fid = *(uint32_t *)(msg + 7);
        uint64_t offset = *(uint64_t *)(msg + 11);
        uint32_t count = *(uint32_t *)(msg + 19);

        struct fid_entry *f = find_fid(c, fid);
        if (!f) {
            send_rerror(c, tag, "invalid fid");
            return;
        }

        uint8_t resp[BUFFER_SIZE];
        uint32_t read_bytes = 0;

        uint8_t proto = path_proto(f->path);
        uint8_t conv  = path_conv(f->path);
        uint8_t ftype = path_ftype(f->path);

        if (f->qid_type & QTDIR) {
            uint8_t stat_buf[4096];
            int stat_pos = 0;

            if (proto == PROTO_ROOT) {
                stat_pos += encode_stat(stat_buf + stat_pos, sizeof(stat_buf) - stat_pos, make_path(PROTO_ETHER, 0xFF, FT_DIR), QTDIR, DMDIR|0555, 0, "ether0");
                stat_pos += encode_stat(stat_buf + stat_pos, sizeof(stat_buf) - stat_pos, make_path(PROTO_IPIFC, 0xFF, FT_DIR), QTDIR, DMDIR|0555, 0, "ipifc");
                stat_pos += encode_stat(stat_buf + stat_pos, sizeof(stat_buf) - stat_pos, make_path(PROTO_UDP, 0xFF, FT_DIR), QTDIR, DMDIR|0555, 0, "udp");
                stat_pos += encode_stat(stat_buf + stat_pos, sizeof(stat_buf) - stat_pos, make_path(PROTO_TCP, 0xFF, FT_DIR), QTDIR, DMDIR|0555, 0, "tcp");
                stat_pos += encode_stat(stat_buf + stat_pos, sizeof(stat_buf) - stat_pos, make_path(PROTO_IL, 0xFF, FT_DIR), QTDIR, DMDIR|0555, 0, "il");
            } else if (proto == PROTO_ETHER) {
                stat_pos += encode_stat(stat_buf + stat_pos, sizeof(stat_buf) - stat_pos, make_path(PROTO_ETHER, 0xFF, FT_STATS), QTFILE, 0444, 128, "stats");
            } else if (conv == 0xFF) {
                stat_pos += encode_stat(stat_buf + stat_pos, sizeof(stat_buf) - stat_pos, make_path(proto, 0xFF, FT_CLONE), QTFILE, 0666, 0, "clone");
                stat_pos += encode_stat(stat_buf + stat_pos, sizeof(stat_buf) - stat_pos, make_path(proto, 0xFF, FT_STATS), QTFILE, 0444, 64, "stats");
                for (int i = 0; i < MAX_CONVS; i++) {
                    if (c->convs[proto][i].in_use) {
                        char cname[16];
                        snprintf(cname, sizeof(cname), "%d", i);
                        stat_pos += encode_stat(stat_buf + stat_pos, sizeof(stat_buf) - stat_pos, make_path(proto, i, FT_DIR), QTDIR, DMDIR|0555, 0, cname);
                    }
                }
            } else {
                stat_pos += encode_stat(stat_buf + stat_pos, sizeof(stat_buf) - stat_pos, make_path(proto, conv, FT_CTL), QTFILE, 0666, 0, "ctl");
                stat_pos += encode_stat(stat_buf + stat_pos, sizeof(stat_buf) - stat_pos, make_path(proto, conv, FT_DATA), QTFILE, 0666, 0, "data");
                stat_pos += encode_stat(stat_buf + stat_pos, sizeof(stat_buf) - stat_pos, make_path(proto, conv, FT_STATUS), QTFILE, 0444, 64, "status");
            }

            if (offset < (uint64_t)stat_pos) {
                size_t avail = stat_pos - offset;
                read_bytes = count < avail ? count : avail;
                memcpy(resp + 11, stat_buf + offset, read_bytes);
            }
        } else {
            /* Regular file read */
            char content[1024];
            content[0] = '\0';

            if (ftype == FT_CLONE) {
                if (!f->clone_read_done && f->clone_conv >= 0) {
                    snprintf(content, sizeof(content), "%d\n", f->clone_conv);
                    f->clone_read_done = 1;
                }
            } else if (ftype == FT_STATS) {
                if (proto == PROTO_ETHER) {
                    snprintf(content, sizeof(content), "addr: 525400123456\nin: 42 packets, 2048 bytes\nout: 42 packets, 2048 bytes\n");
                } else {
                    snprintf(content, sizeof(content), "in: 10 packets\nout: 10 packets\n");
                }
            } else if (ftype == FT_STATUS) {
                if (proto == PROTO_IPIFC) {
                    snprintf(content, sizeof(content), "10.0.2.15 255.255.255.0 10.0.2.0 /net/ether0\n");
                } else if (conv < MAX_CONVS && c->convs[proto][conv].remote_addr[0]) {
                    snprintf(content, sizeof(content), "Connected to %s\n", c->convs[proto][conv].remote_addr);
                } else {
                    snprintf(content, sizeof(content), "Idle\n");
                }
            } else if (ftype == FT_CTL) {
                int cidx = (f->clone_conv >= 0) ? f->clone_conv : conv;
                if (cidx < MAX_CONVS && c->convs[proto][cidx].remote_addr[0]) {
                    snprintf(content, sizeof(content), "connect %s\n", c->convs[proto][cidx].remote_addr);
                } else {
                    snprintf(content, sizeof(content), "ok\n");
                }
            } else if (ftype == FT_DATA) {
                /* Return data if any */
                int cidx = (f->clone_conv >= 0) ? f->clone_conv : conv;
                if (cidx < MAX_CONVS && c->convs[proto][cidx].data_len > 0) {
                    size_t dlen = c->convs[proto][cidx].data_len;
                    read_bytes = count < dlen ? count : dlen;
                    memcpy(resp + 11, c->convs[proto][cidx].data_buf, read_bytes);
                    c->convs[proto][cidx].data_len = 0;
                }
            }

            if (content[0] != '\0') {
                size_t clen = strlen(content);
                if (offset < clen) {
                    size_t avail = clen - offset;
                    read_bytes = count < avail ? count : avail;
                    memcpy(resp + 11, content + offset, read_bytes);
                }
            }
        }

        uint32_t rsize = 4 + 1 + 2 + 4 + read_bytes;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Rread;
        *(uint16_t *)(resp + 5) = tag;
        *(uint32_t *)(resp + 7) = read_bytes;
        write(c->fd, resp, rsize);
        break;
    }
    case Twrite: {
        uint32_t fid = *(uint32_t *)(msg + 7);
        uint64_t offset = *(uint64_t *)(msg + 11);
        uint32_t count = *(uint32_t *)(msg + 19);
        uint8_t *data = msg + 23;
        (void)offset;

        struct fid_entry *f = find_fid(c, fid);
        if (!f) {
            send_rerror(c, tag, "invalid fid");
            return;
        }

        uint8_t proto = path_proto(f->path);
        uint8_t conv  = path_conv(f->path);
        uint8_t ftype = path_ftype(f->path);

        int cidx = (f->clone_conv >= 0) ? f->clone_conv : conv;
        if (cidx >= MAX_CONVS) cidx = 0;

        if (ftype == FT_CTL || ftype == FT_CLONE) {
            char cmd[256];
            int clen = count < 255 ? count : 255;
            memcpy(cmd, data, clen);
            cmd[clen] = '\0';
            while (clen > 0 && (cmd[clen-1] == '\n' || cmd[clen-1] == '\r')) {
                cmd[--clen] = '\0';
            }

            if (strncmp(cmd, "connect ", 8) == 0) {
                strncpy(c->convs[proto][cidx].remote_addr, cmd + 8, sizeof(c->convs[proto][cidx].remote_addr) - 1);
                snprintf(c->convs[proto][cidx].status, sizeof(c->convs[proto][cidx].status), "Connected to %s\n", cmd + 8);
            }
        } else if (ftype == FT_DATA) {
            size_t copy_len = count < sizeof(c->convs[proto][cidx].data_buf) ? count : sizeof(c->convs[proto][cidx].data_buf);
            memcpy(c->convs[proto][cidx].data_buf, data, copy_len);
            c->convs[proto][cidx].data_len = copy_len;
        }

        uint8_t resp[16];
        uint32_t rsize = 4 + 1 + 2 + 4;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Rwrite;
        *(uint16_t *)(resp + 5) = tag;
        *(uint32_t *)(resp + 7) = count;
        write(c->fd, resp, rsize);
        break;
    }
    case Tclunk: {
        uint32_t fid = *(uint32_t *)(msg + 7);
        free_fid(c, fid);

        uint8_t resp[16];
        uint32_t rsize = 4 + 1 + 2;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Rclunk;
        *(uint16_t *)(resp + 5) = tag;
        write(c->fd, resp, rsize);
        break;
    }
    case Tstat: {
        uint32_t fid = *(uint32_t *)(msg + 7);
        struct fid_entry *f = find_fid(c, fid);
        if (!f) {
            send_rerror(c, tag, "invalid fid");
            return;
        }

        uint8_t stat_buf[512];
        const char *name = "net";
        uint8_t proto = path_proto(f->path);
        uint8_t conv  = path_conv(f->path);
        uint8_t ftype = path_ftype(f->path);

        if (proto == PROTO_ETHER) name = (ftype == FT_DIR) ? "ether0" : "stats";
        else if (proto == PROTO_IPIFC) name = (ftype == FT_DIR) ? "ipifc" : ((ftype == FT_CLONE) ? "clone" : "stats");
        else if (proto == PROTO_UDP) name = (ftype == FT_DIR) ? ((conv == 0xFF) ? "udp" : "0") : "clone";
        else if (proto == PROTO_TCP) name = (ftype == FT_DIR) ? ((conv == 0xFF) ? "tcp" : "0") : "clone";
        else if (proto == PROTO_IL)  name = (ftype == FT_DIR) ? ((conv == 0xFF) ? "il" : "0") : "clone";

        uint32_t mode = (f->qid_type & QTDIR) ? (DMDIR | 0555) : 0666;
        int slen = encode_stat(stat_buf, sizeof(stat_buf), f->path, f->qid_type, mode, 0, name);

        uint8_t resp[1024];
        uint32_t rsize = 4 + 1 + 2 + 2 + slen;
        *(uint32_t *)(resp + 0) = rsize;
        resp[4] = Rstat;
        *(uint16_t *)(resp + 5) = tag;
        *(uint16_t *)(resp + 7) = (uint16_t)slen;
        memcpy(resp + 9, stat_buf, slen);
        write(c->fd, resp, rsize);
        break;
    }
    default:
        send_rerror(c, tag, "unsupported 9P operation");
        break;
    }
}

static void process_client(struct client *c) {
    while (c->in_len >= 4) {
        uint32_t msg_len = *(uint32_t *)c->in_buf;
        if (msg_len < 7 || msg_len > BUFFER_SIZE) {
            /* Corrupted message, drop client */
            close(c->fd);
            c->fd = -1;
            return;
        }
        if (c->in_len < msg_len)
            break; /* Wait for more bytes */

        handle_9p_message(c, c->in_buf, msg_len);

        c->in_len -= msg_len;
        if (c->in_len > 0) {
            memmove(c->in_buf, c->in_buf + msg_len, c->in_len);
        }
    }
}

int main(int argc, char **argv) {
    const char *sock_path = DEFAULT_SOCK_PATH;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            sock_path = argv[++i];
        } else if (argv[i][0] != '-') {
            sock_path = argv[i];
        }
    }

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    unlink(sock_path);

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("[netfs] socket");
        return 1;
    }

    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[netfs] bind");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 16) < 0) {
        perror("[netfs] listen");
        close(listen_fd);
        return 1;
    }

    fprintf(stderr, "[netfs] Central network driver file server listening on %s\n", sock_path);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
    }

    struct pollfd pfd[MAX_CLIENTS + 1];

    while (running) {
        int nfds = 0;
        pfd[nfds].fd = listen_fd;
        pfd[nfds].events = POLLIN;
        nfds++;

        int client_idx[MAX_CLIENTS + 1];
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0) {
                pfd[nfds].fd = clients[i].fd;
                pfd[nfds].events = POLLIN;
                client_idx[nfds] = i;
                nfds++;
            }
        }

        int ret = poll(pfd, nfds, 1000);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("[netfs] poll");
            break;
        }

        if (pfd[0].revents & POLLIN) {
            int cfd = accept(listen_fd, NULL, NULL);
            if (cfd >= 0) {
                int assigned = 0;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd < 0) {
                        clients[i].fd = cfd;
                        clients[i].in_len = 0;
                        clients[i].msize = 8192;
                        memset(clients[i].fids, 0, sizeof(clients[i].fids));
                        init_convs(&clients[i]);
                        fprintf(stderr, "[netfs] Accepted client on slot %d (fd %d)\n", i, cfd);
                        assigned = 1;
                        break;
                    }
                }
                if (!assigned) {
                    fprintf(stderr, "[netfs] Max clients reached, rejecting fd %d\n", cfd);
                    close(cfd);
                }
            }
        }

        for (int i = 1; i < nfds; i++) {
            if (pfd[i].revents & (POLLIN | POLLERR | POLLHUP)) {
                int idx = client_idx[i];
                struct client *c = &clients[idx];
                ssize_t nr = read(c->fd, c->in_buf + c->in_len, sizeof(c->in_buf) - c->in_len);
                if (nr <= 0) {
                    fprintf(stderr, "[netfs] Client on slot %d disconnected\n", idx);
                    close(c->fd);
                    c->fd = -1;
                } else {
                    c->in_len += nr;
                    process_client(c);
                }
            }
        }
    }

    fprintf(stderr, "[netfs] Shutting down...\n");
    close(listen_fd);
    unlink(sock_path);
    return 0;
}
