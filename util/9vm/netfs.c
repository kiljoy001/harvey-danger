/*
 * netfs.c - User-space 9P2000 /net service for the Harvey/9vm microVM.
 *
 * The microVM kernel has no network stack.  This process is the network
 * service: it exposes the Plan 9 /net file interface and carries TCP and
 * UDP conversations over host sockets.  The guest sees an ordinary 9P
 * tree and uses its normal dial(2), cs(2), webfs and IRC clients.
 *
 * Design notes:
 *   - Every conversation is a host socket, but the guest-visible protocol
 *     is the Plan 9 /net protocol (clone, ctl, data, local, remote, status,
 *     err), not a Linux socket ABI.
 *   - A connect is performed by a short-lived worker thread.  The 9P event
 *     loop never blocks on DNS or TCP handshakes.
 *   - Reads and writes that cannot complete immediately are parked.  The
 *     9P connection can therefore carry out-of-order replies, which the
 *     Harvey devmnt client already supports.
 *   - /net/cs translates names with the host resolver.  A later revision
 *     can route .coin through emercoin9p or a guest-side resolver without
 *     changing the /net protocol.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>

#define MAX_CLIENTS       64
#define MAX_FIDS         256
#define MAX_CONVS         64
#define MAX_PENDING      256
#define BUFFER_SIZE  (65536 + 24)
#define DEFAULT_SOCK_PATH "/tmp/netfs.sock"
#define CONNECT_TIMEOUT_MS 10000

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

#define QTDIR  0x80
#define QTFILE 0x00
#define DMDIR  0x80000000

/* Protocol tree */
enum {
    P_ROOT = 0,
    P_TCP,
    P_UDP,
    P_IL,
    P_COUNT
};

/* File types within a protocol tree */
enum {
    FT_DIR = 0,
    FT_CLONE,
    FT_STATS,
    FT_CTL,
    FT_DATA,
    FT_LOCAL,
    FT_REMOTE,
    FT_STATUS,
    FT_ERR,
    FT_LISTEN,
    FT_CS,
};

enum {
    CS_IDLE = 0,
    CS_CONNECTING,
    CS_CONNECTED,
    CS_LISTEN,
    CS_ERROR,
};

struct conv {
    int in_use;
    int proto;
    int state;
    int fd;
    uint64_t gen;
    int cancel;
    char host[256];
    char serv[64];
    char err[256];
    char status[128];
    char remote[128];
    char local[128];
};

struct fid_entry {
    uint32_t fid;
    int in_use;
    uint64_t path;
    uint8_t qid_type;
    int open_mode;
    int conv;             /* conversation referred to by this fid, or -1 */
    int clone_conv;       /* conversation allocated by opening clone */
    int clone_read_done;
    char cs_buf[8192];
    size_t cs_len;
};

struct client {
    int fd;
    uint8_t in_buf[BUFFER_SIZE];
    size_t in_len;
    uint32_t msize;
    struct fid_entry fids[MAX_FIDS];
    struct conv convs[P_COUNT][MAX_CONVS];
};

struct pending_read {
    struct pending_read *next;
    struct client *client;
    uint32_t tag;
    uint32_t fid;
    int proto;
    int conv;
    uint32_t count;
};

struct pending_write {
    struct pending_write *next;
    struct client *client;
    uint32_t tag;
    uint32_t fid;
    int proto;
    int conv;
    uint8_t *data;
    size_t len;
    size_t sent;
    uint32_t total;
};

struct pending_listen {
    struct pending_listen *next;
    struct client *client;
    uint32_t tag;
    uint32_t fid;
    int proto;
    int conv;
};

struct connect_job {
    int client_index;
    int proto;
    int conv;
    uint64_t gen;
    char host[256];
    char serv[64];
};

static struct client clients[MAX_CLIENTS];
static int listen_fd = -1;
static volatile sig_atomic_t running = 1;
static int wake_pipe[2] = {-1, -1};
static pthread_mutex_t worker_lock = PTHREAD_MUTEX_INITIALIZER;
static struct pending_read *pending_reads;
static struct pending_write *pending_writes;
static struct pending_listen *pending_listens;

/* Little-endian 9P field helpers. */
static uint16_t g16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t g32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t g64(const uint8_t *p) {
    return (uint64_t)g32(p) | ((uint64_t)g32(p + 4) << 32);
}

static void p16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void p32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void p64(uint8_t *p, uint64_t v) {
    p32(p, (uint32_t)v);
    p32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t make_path(uint8_t proto, uint8_t conv, uint8_t ftype) {
    return ((uint64_t)proto << 24) | ((uint64_t)conv << 8) | ftype;
}

static uint8_t path_proto(uint64_t path) { return (uint8_t)((path >> 24) & 0xff); }
static uint8_t path_conv(uint64_t path)  { return (uint8_t)((path >> 8) & 0xff); }
static uint8_t path_ftype(uint64_t path) { return (uint8_t)(path & 0xff); }

static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                int prc;
                do {
                    prc = poll(&pfd, 1, 1000);
                } while (prc < 0 && errno == EINTR);
                if (prc > 0)
                    continue;
            }
            return -1;
        }
        if (n == 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void send_rerror(struct client *c, uint16_t tag, const char *ename) {
    uint8_t resp[512];
    size_t elen = strlen(ename);
    if (elen > sizeof(resp) - 11)
        elen = sizeof(resp) - 11;
    uint32_t size = (uint32_t)(4 + 1 + 2 + 2 + elen);
    p32(resp, size);
    resp[4] = Rerror;
    p16(resp + 5, tag);
    p16(resp + 7, (uint16_t)elen);
    memcpy(resp + 9, ename, elen);
    write_all(c->fd, resp, size);
}

static void send_rread(struct client *c, uint16_t tag, const void *data, uint32_t n) {
    uint8_t *resp = malloc(11 + n);
    if (resp == NULL) {
        send_rerror(c, tag, "out of memory");
        return;
    }
    p32(resp, 11 + n);
    resp[4] = Rread;
    p16(resp + 5, tag);
    p32(resp + 7, n);
    if (n > 0)
        memcpy(resp + 11, data, n);
    write_all(c->fd, resp, 11 + n);
    free(resp);
}

static void send_rwrite(struct client *c, uint16_t tag, uint32_t n) {
    uint8_t resp[16];
    p32(resp, 4 + 1 + 2 + 4);
    resp[4] = Rwrite;
    p16(resp + 5, tag);
    p32(resp + 7, n);
    write_all(c->fd, resp, 11);
}

static struct fid_entry *find_fid(struct client *c, uint32_t fid) {
    for (int i = 0; i < MAX_FIDS; i++)
        if (c->fids[i].in_use && c->fids[i].fid == fid)
            return &c->fids[i];
    return NULL;
}

static struct fid_entry *alloc_fid(struct client *c, uint32_t fid) {
    struct fid_entry *f = find_fid(c, fid);
    if (f != NULL)
        return f;
    for (int i = 0; i < MAX_FIDS; i++) {
        if (!c->fids[i].in_use) {
            memset(&c->fids[i], 0, sizeof(c->fids[i]));
            c->fids[i].in_use = 1;
            c->fids[i].fid = fid;
            c->fids[i].open_mode = -1;
            c->fids[i].conv = -1;
            c->fids[i].clone_conv = -1;
            return &c->fids[i];
        }
    }
    return NULL;
}

static void free_fid(struct client *c, uint32_t fid) {
    struct fid_entry *f = find_fid(c, fid);
    if (f != NULL)
        memset(f, 0, sizeof(*f));
}

static int encode_stat(uint8_t *buf, int maxlen, uint64_t path, uint8_t type,
                       uint32_t mode, uint64_t length, const char *name) {
    int namelen = (int)strlen(name);
    const char *user = "netfs";
    int userlen = (int)strlen(user);
    int stat_body_size = 2 + 4 + 13 + 4 + 4 + 4 + 8 +
                         (2 + namelen) + (2 + userlen) * 3;
    int total_size = 2 + stat_body_size;
    if (total_size > maxlen)
        return -1;

    uint8_t *p = buf;
    p16(p, (uint16_t)stat_body_size); p += 2;
    p16(p, 0); p += 2;                 /* type */
    p32(p, 0); p += 4;                 /* dev */
    *p++ = type;                       /* qid.type */
    p32(p, 0); p += 4;                 /* qid.vers */
    p64(p, path); p += 8;              /* qid.path */
    p32(p, mode); p += 4;
    p32(p, 1700000000); p += 4;
    p32(p, 1700000000); p += 4;
    p64(p, length); p += 8;
    p16(p, (uint16_t)namelen); p += 2;
    memcpy(p, name, (size_t)namelen); p += namelen;
    for (int i = 0; i < 3; i++) {
        p16(p, (uint16_t)userlen); p += 2;
        memcpy(p, user, (size_t)userlen); p += userlen;
    }
    return total_size;
}

static const char *proto_name(int proto) {
    switch (proto) {
    case P_TCP: return "tcp";
    case P_UDP: return "udp";
    case P_IL:  return "il";
    default:    return "net";
    }
}

static int proto_from_name(const char *name, int *proto) {
    if (strcmp(name, "tcp") == 0) { *proto = P_TCP; return 0; }
    if (strcmp(name, "udp") == 0) { *proto = P_UDP; return 0; }
    if (strcmp(name, "il") == 0)  { *proto = P_IL; return 0; }
    return -1;
}

static void conv_reset(struct conv *cv, int proto) {
    if (cv->fd >= 0)
        close(cv->fd);
    memset(cv, 0, sizeof(*cv));
    cv->fd = -1;
    cv->proto = proto;
    cv->state = CS_IDLE;
    snprintf(cv->status, sizeof(cv->status), "Closed\n");
}

static int alloc_conv(struct client *c, int proto) {
    for (int i = 0; i < MAX_CONVS; i++) {
        struct conv *cv = &c->convs[proto][i];
        if (!cv->in_use) {
            conv_reset(cv, proto);
            cv->in_use = 1;
            cv->gen++;
            return i;
        }
    }
    return -1;
}

static struct conv *get_conv(struct client *c, int proto, int idx) {
    if (proto < 0 || proto >= P_COUNT || idx < 0 || idx >= MAX_CONVS)
        return NULL;
    return &c->convs[proto][idx];
}

static void set_conv_error(struct conv *cv, const char *err) {
    cv->state = CS_ERROR;
    snprintf(cv->err, sizeof(cv->err), "%s", err);
    snprintf(cv->status, sizeof(cv->status), "Error: %s\n", err);
    if (cv->fd >= 0) {
        close(cv->fd);
        cv->fd = -1;
    }
}

static void cancel_listens_for_fid(struct client *c, uint32_t fid) {
    struct pending_listen **pp = &pending_listens;
    while (*pp != NULL) {
        struct pending_listen *p = *pp;
        if (p->client == c && p->fid == fid) {
            *pp = p->next;
            free(p);
        } else {
            pp = &p->next;
        }
    }
}

static void cancel_listens_for_tag(struct client *c, uint16_t tag) {
    struct pending_listen **pp = &pending_listens;
    while (*pp != NULL) {
        struct pending_listen *p = *pp;
        if (p->client == c && p->tag == tag) {
            *pp = p->next;
            free(p);
        } else {
            pp = &p->next;
        }
    }
}

static void cancel_pending_for_fid(struct client *c, uint32_t fid) {
    struct pending_read **pr = &pending_reads;
    while (*pr != NULL) {
        struct pending_read *p = *pr;
        if (p->client == c && p->fid == fid) {
            *pr = p->next;
            free(p);
        } else {
            pr = &p->next;
        }
    }
    struct pending_write **pw = &pending_writes;
    while (*pw != NULL) {
        struct pending_write *p = *pw;
        if (p->client == c && p->fid == fid) {
            *pw = p->next;
            free(p->data);
            free(p);
        } else {
            pw = &p->next;
        }
    }
    cancel_listens_for_fid(c, fid);
}

static void fail_pending_for_conv(struct client *c, int proto, int conv, const char *err) {
    struct pending_read **pr = &pending_reads;
    while (*pr != NULL) {
        struct pending_read *p = *pr;
        if (p->client == c && p->proto == proto && p->conv == conv) {
            *pr = p->next;
            send_rerror(c, p->tag, err);
            free(p);
        } else {
            pr = &p->next;
        }
    }
    struct pending_write **pw = &pending_writes;
    while (*pw != NULL) {
        struct pending_write *p = *pw;
        if (p->client == c && p->proto == proto && p->conv == conv) {
            *pw = p->next;
            send_rerror(c, p->tag, err);
            free(p->data);
            free(p);
        } else {
            pw = &p->next;
        }
    }
    struct pending_listen **pl = &pending_listens;
    while (*pl != NULL) {
        struct pending_listen *p = *pl;
        if (p->client == c && p->proto == proto && p->conv == conv) {
            *pl = p->next;
            send_rerror(c, p->tag, err);
            free(p);
        } else {
            pl = &p->next;
        }
    }
}

static void cancel_pending_tag(struct client *c, uint16_t tag) {
    struct pending_read **pr = &pending_reads;
    while (*pr != NULL) {
        struct pending_read *p = *pr;
        if (p->client == c && p->tag == tag) {
            *pr = p->next;
            free(p);
        } else {
            pr = &p->next;
        }
    }
    struct pending_write **pw = &pending_writes;
    while (*pw != NULL) {
        struct pending_write *p = *pw;
        if (p->client == c && p->tag == tag) {
            *pw = p->next;
            free(p->data);
            free(p);
        } else {
            pw = &p->next;
        }
    }
    cancel_listens_for_tag(c, tag);
}

/* ---- name translation (/net/cs) ------------------------------------- */

static int service_port(const char *service) {
    static const struct { const char *name; int port; } map[] = {
        {"http", 80}, {"https", 443}, {"ssh", 22}, {"smtp", 25},
        {"submission", 587}, {"imap", 143}, {"imaps", 993},
        {"pop3", 110}, {"pop3s", 995}, {"irc", 6667}, {"ircs", 6697},
        {"gemini", 1965}, {"gopher", 70}, {"ntp", 123},
        {"dns", 53}, {"domain", 53}, {"ftp", 21}, {"telnet", 23},
        {NULL, 0}
    };
    char *end = NULL;
    long n = strtol(service, &end, 10);
    if (end != service && *end == '\0' && n > 0 && n < 65536)
        return (int)n;
    for (int i = 0; map[i].name != NULL; i++)
        if (strcasecmp(map[i].name, service) == 0)
            return map[i].port;
    return -1;
}

static int addr_to_plan9(const struct sockaddr *sa, socklen_t salen,
                         char *addr, size_t addrlen, int *port) {
    char host[INET6_ADDRSTRLEN];
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        if (inet_ntop(AF_INET, &sin->sin_addr, host, sizeof(host)) == NULL)
            return -1;
        *port = ntohs(sin->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
        if (inet_ntop(AF_INET6, &sin6->sin6_addr, host, sizeof(host)) == NULL)
            return -1;
        *port = ntohs(sin6->sin6_port);
    } else {
        return -1;
    }
    (void)salen;
    snprintf(addr, addrlen, "%s", host);
    return 0;
}

static int append_cs_line(char *out, size_t outsz, size_t *used,
                          const char *proto, const char *addr, int port) {
    char line[512];
    int n = snprintf(line, sizeof(line), "/net/%s/clone %s!%d\n", proto, addr, port);
    if (n < 0 || *used + (size_t)n >= outsz)
        return -1;
    if (strstr(out, line) != NULL)
        return 0;
    memcpy(out + *used, line, (size_t)n);
    *used += (size_t)n;
    out[*used] = '\0';
    return 0;
}

static int resolve_one(const char *proto, const char *host, const char *service,
                       char *out, size_t outsz, size_t *used) {
    size_t before = *used;
    int socktype = strcmp(proto, "udp") == 0 ? SOCK_DGRAM : SOCK_STREAM;
    int port = service_port(service);
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socktype;
    hints.ai_flags = AI_NUMERICSERV;
    char servbuf[64];
    if (port > 0)
        snprintf(servbuf, sizeof(servbuf), "%d", port);
    else
        snprintf(servbuf, sizeof(servbuf), "%s", service);

    int rc = getaddrinfo(host, servbuf, &hints, &res);
    if (rc != 0) {
        /* Retry with the service name so the host's services database can
         * supply it. */
        hints.ai_flags = 0;
        rc = getaddrinfo(host, service, &hints, &res);
    }
    if (rc != 0)
        return -1;

    uint64_t seen[16];
    int nseen = 0;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        char addr[INET6_ADDRSTRLEN];
        int p = 0;
        if (addr_to_plan9(ai->ai_addr, ai->ai_addrlen, addr, sizeof(addr), &p) < 0)
            continue;
        uint64_t key = ((uint64_t)p << 32) ^ (uint64_t)ai->ai_family;
        if (ai->ai_family == AF_INET) {
            const struct sockaddr_in *sin = (const struct sockaddr_in *)ai->ai_addr;
            key ^= (uint64_t)sin->sin_addr.s_addr << 8;
        } else {
            const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)ai->ai_addr;
            for (int i = 0; i < 16; i++)
                key = key * 1099511628211ULL ^ sin6->sin6_addr.s6_addr[i];
        }
        int duplicate = 0;
        for (int i = 0; i < nseen; i++)
            if (seen[i] == key) { duplicate = 1; break; }
        if (duplicate)
            continue;
        if (nseen < (int)(sizeof(seen) / sizeof(seen[0])))
            seen[nseen++] = key;
        if (append_cs_line(out, outsz, used, proto, addr, p) < 0)
            break;
    }
    freeaddrinfo(res);
    return *used > before ? 0 : -1;
}

static int resolve_cs(const char *query, char *out, size_t outsz, char *err, size_t errsz) {
    char q[1024];
    char *bang1, *bang2, *host, *service, *proto;
    size_t used = 0;
    out[0] = '\0';
    snprintf(q, sizeof(q), "%s", query);
    /* Strip leading/trailing whitespace. */
    char *s = q;
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    if (*s == '\0') {
        snprintf(err, errsz, "empty cs query");
        return -1;
    }
    bang1 = strchr(s, '!');
    bang2 = strrchr(s, '!');
    if (bang1 == NULL || bang2 == NULL || bang1 == bang2) {
        snprintf(err, errsz, "malformed cs query `%s'", s);
        return -1;
    }
    *bang1 = '\0';
    proto = s;
    host = bang1 + 1;
    *bang2 = '\0';
    service = bang2 + 1;
    if (*host == '\0' || *service == '\0') {
        snprintf(err, errsz, "malformed cs query `%s'", query);
        return -1;
    }
    if (host[0] == '[' && host[strlen(host) - 1] == ']') {
        host[strlen(host) - 1] = '\0';
        host++;
    }
    if (strcmp(proto, "net") == 0) {
        int tcp_ok = resolve_one("tcp", host, service, out, outsz, &used);
        int udp_ok = resolve_one("udp", host, service, out, outsz, &used);
        if (tcp_ok < 0 && udp_ok < 0) {
            snprintf(err, errsz, "no address for %s", query);
            return -1;
        }
    } else if (strcmp(proto, "tcp") == 0 || strcmp(proto, "udp") == 0) {
        if (resolve_one(proto, host, service, out, outsz, &used) < 0) {
            snprintf(err, errsz, "no address for %s", query);
            return -1;
        }
    } else if (strcmp(proto, "il") == 0) {
        snprintf(err, errsz, "il is not provided by the host netfs yet");
        return -1;
    } else {
        snprintf(err, errsz, "unknown network `%s'", proto);
        return -1;
    }
    return 0;
}

/* ---- connection workers --------------------------------------------- */

static void wake_worker(void) {
    uint8_t b = 1;
    if (wake_pipe[1] >= 0) {
        ssize_t n = write(wake_pipe[1], &b, 1);
        (void)n;
    }
}

static int connect_socket(const char *host, const char *service, int socktype,
                          char *remote, size_t remote_sz,
                          char *local, size_t local_sz,
                          char *err, size_t errsz) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socktype;
    hints.ai_flags = AI_NUMERICSERV;
    int port = service_port(service);
    char servbuf[64];
    if (port > 0)
        snprintf(servbuf, sizeof(servbuf), "%d", port);
    else
        snprintf(servbuf, sizeof(servbuf), "%s", service);
    int rc = getaddrinfo(host, servbuf, &hints, &res);
    if (rc != 0) {
        hints.ai_flags = 0;
        rc = getaddrinfo(host, service, &hints, &res);
    }
    if (rc != 0) {
        snprintf(err, errsz, "cannot resolve %s: %s", host, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    char last[256] = "connection failed";
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) {
            snprintf(last, sizeof(last), "socket: %s", strerror(errno));
            continue;
        }
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0)
            fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        if (socktype == SOCK_STREAM) {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        }
        rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno != EINPROGRESS) {
            snprintf(last, sizeof(last), "connect: %s", strerror(errno));
            close(fd);
            fd = -1;
            continue;
        }
        if (rc < 0) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            int prc;
            do {
                prc = poll(&pfd, 1, CONNECT_TIMEOUT_MS);
            } while (prc < 0 && errno == EINTR);
            if (prc <= 0) {
                snprintf(last, sizeof(last), "connect: %s",
                         prc == 0 ? "timed out" : strerror(errno));
                close(fd);
                fd = -1;
                continue;
            }
            int soerr = 0;
            socklen_t slen = sizeof(soerr);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0 || soerr != 0) {
                snprintf(last, sizeof(last), "connect: %s",
                         strerror(soerr != 0 ? soerr : errno));
                close(fd);
                fd = -1;
                continue;
            }
        }
        break;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        snprintf(err, errsz, "%s", last);
        return -1;
    }

    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    char a[INET6_ADDRSTRLEN];
    int p;
    if (getpeername(fd, (struct sockaddr *)&ss, &slen) == 0 &&
        addr_to_plan9((struct sockaddr *)&ss, slen, a, sizeof(a), &p) == 0)
        snprintf(remote, remote_sz, "%s!%d", a, p);
    else
        snprintf(remote, remote_sz, "%s!%s", host, service);
    slen = sizeof(ss);
    if (getsockname(fd, (struct sockaddr *)&ss, &slen) == 0 &&
        addr_to_plan9((struct sockaddr *)&ss, slen, a, sizeof(a), &p) == 0)
        snprintf(local, local_sz, "%s!%d", a, p);
    else
        local[0] = '\0';
    return fd;
}

static void *connect_worker(void *arg) {
    struct connect_job *job = arg;
    struct client *c = &clients[job->client_index];
    struct conv *cv = get_conv(c, job->proto, job->conv);
    char remote[128] = "";
    char local[128] = "";
    char err[256] = "";
    int fd = connect_socket(job->host, job->serv,
                            job->proto == P_UDP ? SOCK_DGRAM : SOCK_STREAM,
                            remote, sizeof(remote), local, sizeof(local),
                            err, sizeof(err));
    pthread_mutex_lock(&worker_lock);
    if (cv != NULL && cv->in_use && cv->gen == job->gen && !cv->cancel) {
        if (fd >= 0) {
            cv->fd = fd;
            cv->state = CS_CONNECTED;
            snprintf(cv->remote, sizeof(cv->remote), "%s", remote);
            snprintf(cv->local, sizeof(cv->local), "%s", local);
            snprintf(cv->status, sizeof(cv->status), "Established\n");
            cv->err[0] = '\0';
        } else {
            if (fd >= 0)
                close(fd);
            set_conv_error(cv, err);
        }
    } else if (fd >= 0) {
        close(fd);
    }
    pthread_mutex_unlock(&worker_lock);
    wake_worker();
    free(job);
    return NULL;
}

static int start_connect(struct client *c, struct conv *cv,
                         const char *host, const char *service) {
    if (cv->state == CS_CONNECTING)
        return -1;
    if (cv->fd >= 0) {
        close(cv->fd);
        cv->fd = -1;
    }
    pthread_mutex_lock(&worker_lock);
    cv->gen++;
    cv->state = CS_CONNECTING;
    cv->cancel = 0;
    cv->err[0] = '\0';
    snprintf(cv->host, sizeof(cv->host), "%s", host);
    snprintf(cv->serv, sizeof(cv->serv), "%s", service);
    snprintf(cv->status, sizeof(cv->status), "Connect\n");
    snprintf(cv->remote, sizeof(cv->remote), "%s!%s", host, service);
    cv->local[0] = '\0';
    uint64_t gen = cv->gen;
    pthread_mutex_unlock(&worker_lock);

    struct connect_job *job = calloc(1, sizeof(*job));
    if (job == NULL)
        return -1;
    job->client_index = (int)(c - clients);
    job->proto = cv->proto;
    job->conv = (int)(cv - c->convs[cv->proto]);
    job->gen = gen;
    snprintf(job->host, sizeof(job->host), "%s", host);
    snprintf(job->serv, sizeof(job->serv), "%s", service);
    pthread_t tid;
    if (pthread_create(&tid, NULL, connect_worker, job) != 0) {
        free(job);
        cv->state = CS_ERROR;
        snprintf(cv->err, sizeof(cv->err), "cannot start connector");
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

static int start_announce(struct conv *cv, const char *host, const char *service,
                          char *err, size_t errsz) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    int port = service_port(service);
    char servbuf[64];
    if (port > 0)
        snprintf(servbuf, sizeof(servbuf), "%d", port);
    else
        snprintf(servbuf, sizeof(servbuf), "%s", service);
    int rc = getaddrinfo(host[0] == '*' ? NULL : host, servbuf, &hints, &res);
    if (rc != 0) {
        hints.ai_flags = AI_PASSIVE;
        rc = getaddrinfo(host[0] == '*' ? NULL : host, service, &hints, &res);
    }
    if (rc != 0) {
        snprintf(err, errsz, "cannot resolve listen address: %s", gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    char last[256] = "cannot listen";
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) {
            snprintf(last, sizeof(last), "socket: %s", strerror(errno));
            continue;
        }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
            snprintf(last, sizeof(last), "bind: %s", strerror(errno));
            close(fd);
            fd = -1;
            continue;
        }
        if (listen(fd, 16) < 0) {
            snprintf(last, sizeof(last), "listen: %s", strerror(errno));
            close(fd);
            fd = -1;
            continue;
        }
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0)
            fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        break;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        snprintf(err, errsz, "%s", last);
        return -1;
    }
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    char a[INET6_ADDRSTRLEN];
    int p;
    if (getsockname(fd, (struct sockaddr *)&ss, &slen) == 0 &&
        addr_to_plan9((struct sockaddr *)&ss, slen, a, sizeof(a), &p) == 0)
        snprintf(cv->local, sizeof(cv->local), "%s!%d", a, p);
    else
        snprintf(cv->local, sizeof(cv->local), "%s!%s", host, service);
    cv->fd = fd;
    cv->state = CS_LISTEN;
    cv->err[0] = '\0';
    cv->remote[0] = '\0';
    snprintf(cv->status, sizeof(cv->status), "Listen\n");
    return 0;
}

static void queue_listen(struct client *c, uint32_t tag, struct fid_entry *f,
                         int proto, int conv) {
    struct pending_listen *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        send_rerror(c, tag, "out of memory");
        return;
    }
    p->client = c;
    p->tag = tag;
    p->fid = f->fid;
    p->proto = proto;
    p->conv = conv;
    p->next = NULL;
    if (pending_listens == NULL) {
        pending_listens = p;
    } else {
        struct pending_listen *tail = pending_listens;
        while (tail->next != NULL)
            tail = tail->next;
        tail->next = p;
    }
}

static void try_pending_listens(void) {
    struct pending_listen **pp = &pending_listens;
    while (*pp != NULL) {
        struct pending_listen *p = *pp;
        struct conv *cv = get_conv(p->client, p->proto, p->conv);
        if (cv == NULL || !cv->in_use) {
            *pp = p->next;
            send_rerror(p->client, p->tag, "invalid conversation");
            free(p);
            continue;
        }
        int fd;
        int state;
        pthread_mutex_lock(&worker_lock);
        fd = cv->fd;
        state = cv->state;
        pthread_mutex_unlock(&worker_lock);
        if (state == CS_ERROR) {
            *pp = p->next;
            send_rerror(p->client, p->tag, cv->err[0] ? cv->err : "listen failed");
            free(p);
            continue;
        }
        if (fd < 0 || state != CS_LISTEN) {
            pp = &p->next;
            continue;
        }
        struct sockaddr_storage ss;
        socklen_t slen = sizeof(ss);
        int nfd = accept4(fd, (struct sockaddr *)&ss, &slen, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (nfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                pp = &p->next;
                continue;
            }
            *pp = p->next;
            send_rerror(p->client, p->tag, strerror(errno));
            free(p);
            continue;
        }
        int idx = alloc_conv(p->client, p->proto);
        if (idx < 0) {
            close(nfd);
            *pp = p->next;
            send_rerror(p->client, p->tag, "out of conversations");
            free(p);
            continue;
        }
        struct conv *nc = &p->client->convs[p->proto][idx];
        pthread_mutex_lock(&worker_lock);
        nc->fd = nfd;
        nc->state = CS_CONNECTED;
        snprintf(nc->status, sizeof(nc->status), "Established\n");
        char a[INET6_ADDRSTRLEN];
        int port;
        if (addr_to_plan9((struct sockaddr *)&ss, slen, a, sizeof(a), &port) == 0)
            snprintf(nc->remote, sizeof(nc->remote), "%s!%d", a, port);
        slen = sizeof(ss);
        if (getsockname(nfd, (struct sockaddr *)&ss, &slen) == 0 &&
            addr_to_plan9((struct sockaddr *)&ss, slen, a, sizeof(a), &port) == 0)
            snprintf(nc->local, sizeof(nc->local), "%s!%d", a, port);
        pthread_mutex_unlock(&worker_lock);
        char num[32];
        int len = snprintf(num, sizeof(num), "%d", idx);
        *pp = p->next;
        send_rread(p->client, p->tag, num, (uint32_t)len);
        free(p);
    }
}

/* ---- pending I/O ----------------------------------------------------- */

static void try_pending_reads(void) {
    struct pending_read **pp = &pending_reads;
    while (*pp != NULL) {
        struct pending_read *p = *pp;
        struct conv *cv = get_conv(p->client, p->proto, p->conv);
        if (cv == NULL) {
            *pp = p->next;
            send_rerror(p->client, p->tag, "invalid conversation");
            free(p);
            continue;
        }
        int fd;
        int state;
        pthread_mutex_lock(&worker_lock);
        fd = cv->fd;
        state = cv->state;
        pthread_mutex_unlock(&worker_lock);
        if (state == CS_ERROR) {
            char err[256];
            snprintf(err, sizeof(err), "%s", cv->err[0] ? cv->err : "connection failed");
            *pp = p->next;
            send_rerror(p->client, p->tag, err);
            free(p);
            continue;
        }
        if (fd < 0 || state != CS_CONNECTED) {
            pp = &p->next;
            continue;
        }
        uint32_t want = p->count;
        if (want == 0) {
            *pp = p->next;
            send_rread(p->client, p->tag, "", 0);
            free(p);
            continue;
        }
        uint8_t *buf = malloc(want);
        if (buf == NULL) {
            *pp = p->next;
            send_rerror(p->client, p->tag, "out of memory");
            free(p);
            continue;
        }
        ssize_t n = recv(fd, buf, want, MSG_DONTWAIT);
        if (n > 0) {
            *pp = p->next;
                send_rread(p->client, p->tag, buf, (uint32_t)n);
            free(buf);
            free(p);
            continue;
        }
        free(buf);
        if (n == 0) {
            *pp = p->next;
            send_rread(p->client, p->tag, "", 0);
            free(p);
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            pp = &p->next;
            continue;
        }
        char err[256];
        snprintf(err, sizeof(err), "read: %s", strerror(errno));
        *pp = p->next;
        send_rerror(p->client, p->tag, err);
        free(p);
    }
}

static void try_pending_writes(void) {
    struct pending_write **pp = &pending_writes;
    while (*pp != NULL) {
        struct pending_write *p = *pp;
        struct conv *cv = get_conv(p->client, p->proto, p->conv);
        if (cv == NULL) {
            *pp = p->next;
            send_rerror(p->client, p->tag, "invalid conversation");
            free(p->data);
            free(p);
            continue;
        }
        int fd;
        int state;
        pthread_mutex_lock(&worker_lock);
        fd = cv->fd;
        state = cv->state;
        pthread_mutex_unlock(&worker_lock);
        if (state == CS_ERROR) {
            char err[256];
            snprintf(err, sizeof(err), "%s", cv->err[0] ? cv->err : "connection failed");
            *pp = p->next;
            send_rerror(p->client, p->tag, err);
            free(p->data);
            free(p);
            continue;
        }
        if (fd < 0 || state != CS_CONNECTED) {
            pp = &p->next;
            continue;
        }
        ssize_t n = send(fd, p->data + p->sent, p->len - p->sent, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            p->sent += (size_t)n;
            if (p->sent >= p->len) {
                *pp = p->next;
                send_rwrite(p->client, p->tag, p->total);
                free(p->data);
                free(p);
                continue;
            }
            pp = &p->next;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            pp = &p->next;
            continue;
        }
        char err[256];
        snprintf(err, sizeof(err), "write: %s", n == 0 ? "peer closed" : strerror(errno));
        *pp = p->next;
        send_rerror(p->client, p->tag, err);
        free(p->data);
        free(p);
    }
}

static void queue_read(struct client *c, uint32_t tag, struct fid_entry *f,
                       int proto, int conv, uint32_t count) {
    struct pending_read *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        send_rerror(c, tag, "out of memory");
        return;
    }
    p->client = c;
    p->tag = tag;
    p->fid = f->fid;
    p->proto = proto;
    p->conv = conv;
    p->count = count;
    p->next = NULL;
    if (pending_reads == NULL) {
        pending_reads = p;
    } else {
        struct pending_read *tail = pending_reads;
        while (tail->next != NULL)
            tail = tail->next;
        tail->next = p;
    }
}

static void append_pending_write(struct pending_write *p) {
    p->next = NULL;
    if (pending_writes == NULL) {
        pending_writes = p;
    } else {
        struct pending_write *tail = pending_writes;
        while (tail->next != NULL)
            tail = tail->next;
        tail->next = p;
    }
}

static void queue_write(struct client *c, uint32_t tag, struct fid_entry *f,
                        int proto, int conv, const uint8_t *data, size_t len) {
    struct pending_write *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        send_rerror(c, tag, "out of memory");
        return;
    }
    p->data = malloc(len);
    if (p->data == NULL) {
        free(p);
        send_rerror(c, tag, "out of memory");
        return;
    }
    memcpy(p->data, data, len);
    p->client = c;
    p->tag = tag;
    p->fid = f->fid;
    p->proto = proto;
    p->conv = conv;
    p->len = len;
    p->total = (uint32_t)len;
    append_pending_write(p);
}

/* ---- directory and stat generation ---------------------------------- */

static int build_root_dir(struct client *c, uint8_t *buf, int max) {
    int pos = 0;
    const struct { const char *name; int path; int dir; uint32_t mode; } entries[] = {
        {"tcp",    P_TCP,   1, DMDIR | 0555},
        {"udp",    P_UDP,   1, DMDIR | 0555},
        {"il",     P_IL,    1, DMDIR | 0555},
        {"cs",     FT_CS,   0, 0666},
        {"stats",  FT_STATS, 0, 0444},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        uint64_t path;
        uint8_t type = entries[i].dir ? QTDIR : QTFILE;
        if (entries[i].path == FT_CS || entries[i].path == FT_STATS)
            path = make_path(P_ROOT, 0xff, (uint8_t)entries[i].path);
        else
            path = make_path((uint8_t)entries[i].path, 0xff, FT_DIR);
        int n = encode_stat(buf + pos, max - pos, path, type, entries[i].mode, 0, entries[i].name);
        if (n < 0)
            break;
        pos += n;
    }
    (void)c;
    return pos;
}

static int build_proto_dir(struct client *c, int proto, uint8_t *buf, int max) {
    int pos = 0;
    int n = encode_stat(buf, max, make_path((uint8_t)proto, 0xff, FT_CLONE),
                        QTFILE, 0666, 0, "clone");
    if (n < 0) return 0;
    pos += n;
    n = encode_stat(buf + pos, max - pos, make_path((uint8_t)proto, 0xff, FT_STATS),
                    QTFILE, 0444, 0, "stats");
    if (n < 0) return pos;
    pos += n;
    for (int i = 0; i < MAX_CONVS; i++) {
        if (!c->convs[proto][i].in_use)
            continue;
        char name[16];
        snprintf(name, sizeof(name), "%d", i);
        n = encode_stat(buf + pos, max - pos, make_path((uint8_t)proto, (uint8_t)i, FT_DIR),
                        QTDIR, DMDIR | 0555, 0, name);
        if (n < 0)
            break;
        pos += n;
    }
    return pos;
}

static int build_conv_dir(int proto, int conv, uint8_t *buf, int max) {
    static const struct { const char *name; int ft; uint32_t mode; } entries[] = {
        {"ctl",    FT_CTL,    0666},
        {"data",   FT_DATA,   0666},
        {"local",  FT_LOCAL,  0444},
        {"remote", FT_REMOTE, 0444},
        {"status", FT_STATUS, 0444},
        {"err",    FT_ERR,    0444},
        {"listen", FT_LISTEN, 0666},
    };
    int pos = 0;
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        int n = encode_stat(buf + pos, max - pos,
                            make_path((uint8_t)proto, (uint8_t)conv, (uint8_t)entries[i].ft),
                            QTFILE, entries[i].mode, 0, entries[i].name);
        if (n < 0)
            break;
        pos += n;
    }
    return pos;
}

static const char *fid_name(struct fid_entry *f, char *buf, size_t bufsz) {
    int proto = path_proto(f->path);
    int conv = path_conv(f->path);
    int ft = path_ftype(f->path);
    if (f->path == make_path(P_ROOT, 0xff, FT_DIR))
        snprintf(buf, bufsz, "net");
    else if (ft == FT_CS)
        snprintf(buf, bufsz, "cs");
    else if (ft == FT_STATS && proto == P_ROOT)
        snprintf(buf, bufsz, "stats");
    else if (conv == 0xff && ft == FT_DIR)
        snprintf(buf, bufsz, "%s", proto_name(proto));
    else if (conv == 0xff && ft == FT_CLONE)
        snprintf(buf, bufsz, "clone");
    else if (conv == 0xff && ft == FT_STATS)
        snprintf(buf, bufsz, "stats");
    else {
        switch (ft) {
        case FT_CLONE:  snprintf(buf, bufsz, "clone"); break;
        case FT_STATS:  snprintf(buf, bufsz, "stats"); break;
        case FT_CTL:    snprintf(buf, bufsz, "ctl"); break;
        case FT_DATA:   snprintf(buf, bufsz, "data"); break;
        case FT_LOCAL:  snprintf(buf, bufsz, "local"); break;
        case FT_REMOTE: snprintf(buf, bufsz, "remote"); break;
        case FT_STATUS: snprintf(buf, bufsz, "status"); break;
        case FT_ERR:    snprintf(buf, bufsz, "err"); break;
        case FT_LISTEN: snprintf(buf, bufsz, "listen"); break;
        case FT_DIR:    snprintf(buf, bufsz, "%d", conv); break;
        default:        snprintf(buf, bufsz, "net"); break;
        }
    }
    return buf;
}

static uint32_t fid_mode(struct fid_entry *f) {
    if (f->qid_type & QTDIR)
        return DMDIR | 0555;
    return 0666;
}

static void get_conv_text(struct conv *cv, int ft, char *buf, size_t bufsz) {
    switch (ft) {
    case FT_STATUS:
        if (cv->state == CS_CONNECTED)
            snprintf(buf, bufsz, "Established\n");
        else if (cv->state == CS_CONNECTING)
            snprintf(buf, bufsz, "Connect\n");
        else if (cv->state == CS_LISTEN)
            snprintf(buf, bufsz, "Listen\n");
        else if (cv->state == CS_ERROR)
            snprintf(buf, bufsz, "%s\n", cv->err[0] ? cv->err : "Error");
        else
            snprintf(buf, bufsz, "Closed\n");
        break;
    case FT_LOCAL:
        snprintf(buf, bufsz, "%s%s", cv->local[0] ? cv->local : "", cv->local[0] ? "\n" : "");
        break;
    case FT_REMOTE:
        snprintf(buf, bufsz, "%s%s", cv->remote[0] ? cv->remote : "", cv->remote[0] ? "\n" : "");
        break;
    case FT_ERR:
        snprintf(buf, bufsz, "%s%s", cv->err[0] ? cv->err : "", cv->err[0] ? "\n" : "");
        break;
    default:
        snprintf(buf, bufsz, "ok\n");
        break;
    }
}

/* ---- 9P request handling -------------------------------------------- */

static void handle_version(struct client *c, uint8_t *msg, uint32_t size, uint16_t tag) {
    if (size < 13) {
        send_rerror(c, tag, "short Tversion");
        return;
    }
    uint32_t msize = g32(msg + 7);
    uint16_t vlen = g16(msg + 11);
    if ((uint32_t)13 + vlen > size) {
        send_rerror(c, tag, "short Tversion string");
        return;
    }
    char version[64];
    uint16_t n = vlen < sizeof(version) - 1 ? vlen : sizeof(version) - 1;
    memcpy(version, msg + 13, n);
    version[n] = '\0';
    if (strncmp(version, "9P2000", 6) != 0) {
        send_rerror(c, tag, "unsupported 9P version");
        return;
    }
    c->msize = msize < BUFFER_SIZE ? msize : BUFFER_SIZE;
    if (c->msize < 256)
        c->msize = 256;
    uint8_t resp[64];
    const char *ver = "9P2000";
    uint32_t rsize = 4 + 1 + 2 + 4 + 2 + 6;
    p32(resp, rsize);
    resp[4] = Rversion;
    p16(resp + 5, tag);
    p32(resp + 7, c->msize);
    p16(resp + 11, 6);
    memcpy(resp + 13, ver, 6);
    write_all(c->fd, resp, rsize);
}

static void handle_attach(struct client *c, uint8_t *msg, uint32_t size, uint16_t tag) {
    if (size < 11) {
        send_rerror(c, tag, "short Tattach");
        return;
    }
    uint32_t fid = g32(msg + 7);
    struct fid_entry *f = alloc_fid(c, fid);
    if (f == NULL) {
        send_rerror(c, tag, "out of fids");
        return;
    }
    memset(f, 0, sizeof(*f));
    f->in_use = 1;
    f->fid = fid;
    f->path = make_path(P_ROOT, 0xff, FT_DIR);
    f->qid_type = QTDIR;
    f->open_mode = -1;
    f->conv = -1;
    f->clone_conv = -1;
    uint8_t resp[32];
    p32(resp, 4 + 1 + 2 + 13);
    resp[4] = Rattach;
    p16(resp + 5, tag);
    resp[7] = QTDIR;
    p32(resp + 8, 0);
    p64(resp + 12, f->path);
    write_all(c->fd, resp, 20);
}

struct walk_result {
    uint64_t path;
    uint8_t qid_type;
    int conv;
};

static int walk_one(struct client *c, uint64_t cur, const char *name,
                    struct walk_result *out) {
    uint8_t proto = path_proto(cur);
    uint8_t conv = path_conv(cur);
    out->path = cur;
    out->qid_type = QTFILE;
    out->conv = -1;

    if (strcmp(name, ".") == 0 || strcmp(name, "") == 0)
        return 0;

    if (strcmp(name, "..") == 0) {
        if (proto == P_ROOT)
            return 0;
        if (conv != 0xff)
            out->path = make_path(proto, 0xff, FT_DIR);
        else
            out->path = make_path(P_ROOT, 0xff, FT_DIR);
        out->qid_type = QTDIR;
        return 0;
    }

    if (proto == P_ROOT) {
        if (strcmp(name, "tcp") == 0 || strcmp(name, "udp") == 0 || strcmp(name, "il") == 0) {
            int p;
            if (proto_from_name(name, &p) < 0)
                return -1;
            out->path = make_path((uint8_t)p, 0xff, FT_DIR);
            out->qid_type = QTDIR;
            return 0;
        }
        if (strcmp(name, "cs") == 0) {
            out->path = make_path(P_ROOT, 0xff, FT_CS);
            out->qid_type = QTFILE;
            return 0;
        }
        if (strcmp(name, "stats") == 0) {
            out->path = make_path(P_ROOT, 0xff, FT_STATS);
            out->qid_type = QTFILE;
            return 0;
        }
        return -1;
    }

    if (conv == 0xff) {
        /* Inside /net/tcp, /net/udp, /net/il. */
        if (strcmp(name, "clone") == 0) {
            out->path = make_path(proto, 0xff, FT_CLONE);
            out->qid_type = QTFILE;
            return 0;
        }
        if (strcmp(name, "stats") == 0) {
            out->path = make_path(proto, 0xff, FT_STATS);
            out->qid_type = QTFILE;
            return 0;
        }
        char *end = NULL;
        long n = strtol(name, &end, 10);
        if (end == name || *end != '\0' || n < 0 || n >= MAX_CONVS)
            return -1;
        if (!c->convs[proto][n].in_use) {
            struct conv *cv = &c->convs[proto][n];
            conv_reset(cv, proto);
            cv->in_use = 1;
            cv->gen++;
        }
        out->path = make_path(proto, (uint8_t)n, FT_DIR);
        out->qid_type = QTDIR;
        out->conv = (int)n;
        return 0;
    }

    /* Inside a conversation directory. */
    if (strcmp(name, "ctl") == 0) {
        out->path = make_path(proto, conv, FT_CTL);
        out->conv = conv;
        return 0;
    }
    if (strcmp(name, "data") == 0) {
        out->path = make_path(proto, conv, FT_DATA);
        out->conv = conv;
        return 0;
    }
    if (strcmp(name, "local") == 0) {
        out->path = make_path(proto, conv, FT_LOCAL);
        out->conv = conv;
        return 0;
    }
    if (strcmp(name, "remote") == 0) {
        out->path = make_path(proto, conv, FT_REMOTE);
        out->conv = conv;
        return 0;
    }
    if (strcmp(name, "status") == 0) {
        out->path = make_path(proto, conv, FT_STATUS);
        out->conv = conv;
        return 0;
    }
    if (strcmp(name, "err") == 0) {
        out->path = make_path(proto, conv, FT_ERR);
        out->conv = conv;
        return 0;
    }
    if (strcmp(name, "listen") == 0) {
        out->path = make_path(proto, conv, FT_LISTEN);
        out->conv = conv;
        return 0;
    }
    return -1;
}

static void handle_walk(struct client *c, uint8_t *msg, uint32_t size, uint16_t tag) {
    if (size < 17) {
        send_rerror(c, tag, "short Twalk");
        return;
    }
    uint32_t fid = g32(msg + 7);
    uint32_t newfid = g32(msg + 11);
    uint16_t nwname = g16(msg + 15);
    if (nwname > 256) {
        send_rerror(c, tag, "too many walk names");
        return;
    }
    struct fid_entry *f = find_fid(c, fid);
    if (f == NULL) {
        send_rerror(c, tag, "invalid fid");
        return;
    }
    if (newfid != fid) {
        if (find_fid(c, newfid) != NULL) {
            send_rerror(c, tag, "newfid already in use");
            return;
        }
    }
    struct fid_entry *nf = (newfid == fid) ? f : alloc_fid(c, newfid);
    if (nf == NULL) {
        send_rerror(c, tag, "out of fids");
        return;
    }
    uint64_t cur = f->path;
    uint8_t curq = f->qid_type;
    int curconv = f->conv;
    if (nf != f) {
        nf->path = f->path;
        nf->qid_type = f->qid_type;
        nf->conv = f->conv;
        nf->clone_conv = f->clone_conv;
        nf->clone_read_done = f->clone_read_done;
    }

    uint8_t *resp = malloc(9 + (size_t)nwname * 13 + 64);
    if (resp == NULL) {
        send_rerror(c, tag, "out of memory");
        return;
    }
    uint8_t *out = resp + 9;
    uint16_t nwqid = 0;
    uint32_t off = 17;
    for (int i = 0; i < nwname; i++) {
        if (off + 2 > size) {
            free(resp);
            send_rerror(c, tag, "short Twalk name");
            return;
        }
        uint16_t nl = g16(msg + off);
        off += 2;
        if (off + nl > size) {
            free(resp);
            send_rerror(c, tag, "short Twalk name");
            return;
        }
        char name[256];
        size_t cn = nl < sizeof(name) - 1 ? nl : sizeof(name) - 1;
        memcpy(name, msg + off, cn);
        name[cn] = '\0';
        off += nl;
        struct walk_result wr;
        if (walk_one(c, cur, name, &wr) < 0)
            break;
        cur = wr.path;
        curq = wr.qid_type;
        if (wr.conv >= 0)
            curconv = wr.conv;
        else if (path_conv(cur) != 0xff)
            curconv = path_conv(cur);
        *out++ = curq;
        p32(out, 0); out += 4;
        p64(out, cur); out += 8;
        nwqid++;
    }
    if (nwname > 0 && nwqid == 0) {
        free(resp);
        send_rerror(c, tag, "file not found");
        return;
    }
    nf->path = cur;
    nf->qid_type = curq;
    nf->conv = curconv;
    uint32_t rsize = 4 + 1 + 2 + 2 + (uint32_t)nwqid * 13;
    p32(resp, rsize);
    resp[4] = Rwalk;
    p16(resp + 5, tag);
    p16(resp + 7, nwqid);
    write_all(c->fd, resp, rsize);
    free(resp);
}

static void handle_open(struct client *c, uint8_t *msg, uint32_t size, uint16_t tag) {
    if (size < 12) {
        send_rerror(c, tag, "short Topen");
        return;
    }
    uint32_t fid = g32(msg + 7);
    uint8_t mode = msg[11];
    struct fid_entry *f = find_fid(c, fid);
    if (f == NULL) {
        send_rerror(c, tag, "invalid fid");
        return;
    }
    int ft = path_ftype(f->path);
    if (ft == FT_CLONE) {
        int proto = path_proto(f->path);
        int idx = alloc_conv(c, proto);
        if (idx < 0) {
            send_rerror(c, tag, "out of conversations");
            return;
        }
        f->conv = idx;
        f->clone_conv = idx;
        f->clone_read_done = 0;
    }
    f->open_mode = mode;
    uint8_t resp[32];
    p32(resp, 4 + 1 + 2 + 13 + 4);
    resp[4] = Ropen;
    p16(resp + 5, tag);
    resp[7] = f->qid_type;
    p32(resp + 8, 0);
    p64(resp + 12, f->path);
    p32(resp + 20, c->msize > 24 ? c->msize - 24 : 0);
    write_all(c->fd, resp, 24);
}

static void handle_read(struct client *c, uint8_t *msg, uint32_t size, uint16_t tag) {
    if (size < 23) {
        send_rerror(c, tag, "short Tread");
        return;
    }
    uint32_t fid = g32(msg + 7);
    uint64_t offset = g64(msg + 11);
    uint32_t count = g32(msg + 19);
    struct fid_entry *f = find_fid(c, fid);
    if (f == NULL) {
        send_rerror(c, tag, "invalid fid");
        return;
    }
    if (count > c->msize - 11)
        count = c->msize - 11;

    if (f->qid_type & QTDIR) {
        uint8_t *dirbuf = malloc(32768);
        if (dirbuf == NULL) {
            send_rerror(c, tag, "out of memory");
            return;
        }
        int pos = 0;
        int proto = path_proto(f->path);
        int conv = path_conv(f->path);
        if (f->path == make_path(P_ROOT, 0xff, FT_DIR)) {
            pos = build_root_dir(c, dirbuf, 32768);
        } else if (conv == 0xff) {
            pos = build_proto_dir(c, proto, dirbuf, 32768);
        } else {
            pos = build_conv_dir(proto, conv, dirbuf, 32768);
        }
        uint32_t n = 0;
        if (offset < (uint64_t)pos) {
            uint64_t avail = (uint64_t)pos - offset;
            n = (uint32_t)(avail < count ? avail : count);
            memmove(dirbuf, dirbuf + offset, n);
        }
        send_rread(c, tag, dirbuf, n);
        free(dirbuf);
        return;
    }

    int ft = path_ftype(f->path);
    int proto = path_proto(f->path);
    int convidx = f->conv >= 0 ? f->conv : path_conv(f->path);
    struct conv *cv = get_conv(c, proto, convidx);

    if (ft == FT_CLONE) {
        char num[32];
        int n = snprintf(num, sizeof(num), "%d", f->clone_conv);
        uint32_t rn = 0;
        if (offset < (uint64_t)n) {
            uint64_t avail = (uint64_t)n - offset;
            rn = (uint32_t)(avail < count ? avail : count);
            send_rread(c, tag, num + offset, rn);
        } else {
            send_rread(c, tag, "", 0);
        }
        f->clone_read_done = 1;
        return;
    }
    if (ft == FT_CS) {
        uint32_t rn = 0;
        if (offset < f->cs_len) {
            size_t avail = f->cs_len - (size_t)offset;
            rn = (uint32_t)(avail < count ? avail : count);
            send_rread(c, tag, f->cs_buf + offset, rn);
        } else {
            send_rread(c, tag, "", 0);
        }
        return;
    }
    if (ft == FT_STATS) {
        const char *s;
        if (proto == P_ROOT) {
            s = "netfs: host socket service\n";
        } else {
            s = "in: 0 packets\nout: 0 packets\n";
        }
        size_t sl = strlen(s);
        uint32_t rn = 0;
        if (offset < sl) {
            size_t avail = sl - (size_t)offset;
            rn = (uint32_t)(avail < count ? avail : count);
            send_rread(c, tag, s + offset, rn);
        } else {
            send_rread(c, tag, "", 0);
        }
        return;
    }
    if (ft == FT_LISTEN) {
        if (cv == NULL || !cv->in_use || cv->state != CS_LISTEN) {
            send_rerror(c, tag, "not listening");
            return;
        }
        queue_listen(c, tag, f, proto, convidx);
        try_pending_listens();
        return;
    }
    if (ft == FT_DATA) {
        if (cv == NULL || !cv->in_use) {
            send_rerror(c, tag, "invalid conversation");
            return;
        }
        if (cv->state == CS_LISTEN) {
            send_rerror(c, tag, "listening socket has no data");
            return;
        }
        queue_read(c, tag, f, proto, convidx, count);
        try_pending_reads();
        return;
    }
    if (cv != NULL && (ft == FT_STATUS || ft == FT_LOCAL || ft == FT_REMOTE || ft == FT_ERR)) {
        char text[512];
        get_conv_text(cv, ft, text, sizeof(text));
        size_t sl = strlen(text);
        uint32_t rn = 0;
        if (offset < sl) {
            size_t avail = sl - (size_t)offset;
            rn = (uint32_t)(avail < count ? avail : count);
            send_rread(c, tag, text + offset, rn);
        } else {
            send_rread(c, tag, "", 0);
        }
        return;
    }
    if (ft == FT_CTL) {
        const char *s = "ok\n";
        size_t sl = strlen(s);
        uint32_t rn = 0;
        if (offset < sl) {
            size_t avail = sl - (size_t)offset;
            rn = (uint32_t)(avail < count ? avail : count);
            send_rread(c, tag, s + offset, rn);
        } else {
            send_rread(c, tag, "", 0);
        }
        return;
    }
    send_rerror(c, tag, "read not supported");
}

static void handle_write(struct client *c, uint8_t *msg, uint32_t size, uint16_t tag) {
    if (size < 23) {
        send_rerror(c, tag, "short Twrite");
        return;
    }
    uint32_t fid = g32(msg + 7);
    uint64_t offset = g64(msg + 11);
    uint32_t count = g32(msg + 19);
    if (count > size - 23) {
        send_rerror(c, tag, "short Twrite data");
        return;
    }
    uint8_t *data = msg + 23;
    (void)offset;
    struct fid_entry *f = find_fid(c, fid);
    if (f == NULL) {
        send_rerror(c, tag, "invalid fid");
        return;
    }
    int ft = path_ftype(f->path);
    int proto = path_proto(f->path);
    int convidx = f->conv >= 0 ? f->conv : path_conv(f->path);

    if (ft == FT_CS) {
        char query[1024];
        size_t n = count < sizeof(query) - 1 ? count : sizeof(query) - 1;
        memcpy(query, data, n);
        query[n] = '\0';
        char err[256];
        if (resolve_cs(query, f->cs_buf, sizeof(f->cs_buf), err, sizeof(err)) < 0) {
            send_rerror(c, tag, err);
            return;
        }
        f->cs_len = strlen(f->cs_buf);
        send_rwrite(c, tag, count);
        return;
    }

    if (ft == FT_LISTEN) {
        char cmd[256];
        size_t n = count < sizeof(cmd) - 1 ? count : sizeof(cmd) - 1;
        memcpy(cmd, data, n);
        cmd[n] = '\0';
        if (strncmp(cmd, "accept ", 7) == 0 || strncmp(cmd, "reject ", 7) == 0) {
            send_rwrite(c, tag, count);
        } else if (strcmp(cmd, "hangup") == 0 || strcmp(cmd, "close") == 0) {
            struct conv *cv = get_conv(c, proto, convidx);
            if (cv != NULL && cv->in_use) {
                pthread_mutex_lock(&worker_lock);
                if (cv->fd >= 0)
                    close(cv->fd);
                cv->fd = -1;
                cv->state = CS_IDLE;
                snprintf(cv->status, sizeof(cv->status), "Closed\n");
                pthread_mutex_unlock(&worker_lock);
                fail_pending_for_conv(c, proto, convidx, "hangup");
            }
            send_rwrite(c, tag, count);
        } else {
            send_rerror(c, tag, "unsupported listen command");
        }
        return;
    }

    if (ft == FT_CTL || ft == FT_CLONE) {
        char cmd[1024];
        size_t n = count < sizeof(cmd) - 1 ? count : sizeof(cmd) - 1;
        memcpy(cmd, data, n);
        cmd[n] = '\0';
        while (n > 0 && isspace((unsigned char)cmd[n - 1]))
            cmd[--n] = '\0';
        char *s = cmd;
        while (*s && isspace((unsigned char)*s)) s++;
        struct conv *cv = get_conv(c, proto, convidx);
        if (cv == NULL || !cv->in_use) {
            send_rerror(c, tag, "invalid conversation");
            return;
        }
        if (strncmp(s, "connect ", 8) == 0) {
            if (proto == P_IL) {
                send_rerror(c, tag, "il is not provided by the host netfs yet");
                return;
            }
            char *dest = s + 8;
            while (*dest == ' ')
                dest++;
            char *local = strchr(dest, ' ');
            if (local != NULL)
                *local++ = '\0';
            char *bang = strchr(dest, '!');
            if (bang == NULL) {
                send_rerror(c, tag, "malformed connect address");
                return;
            }
            *bang = '\0';
            char *host = dest;
            char *service = bang + 1;
            if (*host == '\0' || *service == '\0') {
                send_rerror(c, tag, "malformed connect address");
                return;
            }
            if (start_connect(c, cv, host, service) < 0) {
                send_rerror(c, tag, "cannot start connection");
                return;
            }
            send_rwrite(c, tag, count);
            return;
        }
        if (strncmp(s, "announce ", 9) == 0) {
            if (proto != P_TCP) {
                send_rerror(c, tag, "announce is only implemented for tcp");
                return;
            }
            char *dest = s + 9;
            while (*dest == ' ')
                dest++;
            char *bang = strrchr(dest, '!');
            if (bang == NULL) {
                send_rerror(c, tag, "malformed announce address");
                return;
            }
            *bang = '\0';
            char *host = dest;
            char *service = bang + 1;
            if (*host == '\0' || *service == '\0') {
                send_rerror(c, tag, "malformed announce address");
                return;
            }
            pthread_mutex_lock(&worker_lock);
            cv->cancel = 1;
            if (cv->fd >= 0)
                close(cv->fd);
            cv->fd = -1;
            cv->state = CS_IDLE;
            pthread_mutex_unlock(&worker_lock);
            char err[256];
            if (start_announce(cv, host, service, err, sizeof(err)) < 0) {
                cv->state = CS_ERROR;
                snprintf(cv->err, sizeof(cv->err), "%s", err);
                send_rerror(c, tag, err);
                return;
            }
            send_rwrite(c, tag, count);
            return;
        }
        if (strcmp(s, "hangup") == 0 || strcmp(s, "close") == 0) {
            pthread_mutex_lock(&worker_lock);
            cv->cancel = 1;
            if (cv->fd >= 0)
                close(cv->fd);
            cv->fd = -1;
            cv->state = CS_IDLE;
            cv->status[0] = '\0';
            snprintf(cv->status, sizeof(cv->status), "Closed\n");
            pthread_mutex_unlock(&worker_lock);
            fail_pending_for_conv(c, proto, convidx, "hangup");
            send_rwrite(c, tag, count);
            return;
        }
        if (strncmp(s, "timeout ", 8) == 0 || strcmp(s, "keepalive") == 0 ||
            strcmp(s, "reuseport") == 0 || strncmp(s, "headers", 7) == 0) {
            send_rwrite(c, tag, count);
            return;
        }
        send_rerror(c, tag, "unsupported ctl command");
        return;
    }

    if (ft == FT_DATA) {
        struct conv *cv = get_conv(c, proto, convidx);
        if (cv == NULL || !cv->in_use) {
            send_rerror(c, tag, "invalid conversation");
            return;
        }
        int fd;
        int state;
        pthread_mutex_lock(&worker_lock);
        fd = cv->fd;
        state = cv->state;
        pthread_mutex_unlock(&worker_lock);
        if (state == CS_ERROR) {
            send_rerror(c, tag, cv->err[0] ? cv->err : "connection failed");
            return;
        }
        if (state == CS_LISTEN) {
            send_rerror(c, tag, "listening socket has no data");
            return;
        }
        if (fd < 0 || state != CS_CONNECTED || pending_writes != NULL) {
            queue_write(c, tag, f, proto, convidx, data, count);
            try_pending_writes();
            return;
        }
        ssize_t n = send(fd, data, count, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n == (ssize_t)count) {
            send_rwrite(c, tag, count);
            return;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            send_rerror(c, tag, strerror(errno));
            return;
        }
        size_t sent = n > 0 ? (size_t)n : 0;
        if (sent < count) {
            /* Park the unsent tail. */
            struct pending_write *p = calloc(1, sizeof(*p));
            if (p == NULL) {
                send_rerror(c, tag, "out of memory");
                return;
            }
            p->data = malloc(count - sent);
            if (p->data == NULL) {
                free(p);
                send_rerror(c, tag, "out of memory");
                return;
            }
            memcpy(p->data, data + sent, count - sent);
            p->client = c;
            p->tag = tag;
            p->fid = f->fid;
            p->proto = proto;
            p->conv = convidx;
            p->len = count - sent;
            p->total = count;
            append_pending_write(p);
            try_pending_writes();
        }
        return;
    }

    send_rerror(c, tag, "write not supported");
}

static void handle_clunk(struct client *c, uint8_t *msg, uint32_t size, uint16_t tag) {
    if (size < 11) {
        send_rerror(c, tag, "short Tclunk");
        return;
    }
    uint32_t fid = g32(msg + 7);
    cancel_pending_for_fid(c, fid);
    free_fid(c, fid);
    uint8_t resp[16];
    p32(resp, 4 + 1 + 2);
    resp[4] = Rclunk;
    p16(resp + 5, tag);
    write_all(c->fd, resp, 7);
}

static void handle_stat(struct client *c, uint8_t *msg, uint32_t size, uint16_t tag) {
    if (size < 11) {
        send_rerror(c, tag, "short Tstat");
        return;
    }
    uint32_t fid = g32(msg + 7);
    struct fid_entry *f = find_fid(c, fid);
    if (f == NULL) {
        send_rerror(c, tag, "invalid fid");
        return;
    }
    char name[64];
    fid_name(f, name, sizeof(name));
    uint8_t stat_buf[1024];
    int slen = encode_stat(stat_buf, sizeof(stat_buf), f->path, f->qid_type,
                           fid_mode(f), 0, name);
    if (slen < 0) {
        send_rerror(c, tag, "stat too large");
        return;
    }
    uint8_t *resp = malloc(4 + 1 + 2 + 2 + (size_t)slen);
    if (resp == NULL) {
        send_rerror(c, tag, "out of memory");
        return;
    }
    p32(resp, 4 + 1 + 2 + 2 + (uint32_t)slen);
    resp[4] = Rstat;
    p16(resp + 5, tag);
    p16(resp + 7, (uint16_t)slen);
    memcpy(resp + 9, stat_buf, (size_t)slen);
    write_all(c->fd, resp, 9 + (size_t)slen);
    free(resp);
}

static void handle_message(struct client *c, uint8_t *msg, uint32_t size) {
    if (size < 7)
        return;
    uint8_t type = msg[4];
    uint16_t tag = g16(msg + 5);
    if (getenv("NETFS_DEBUG") != NULL) {
        unsigned fid = size >= 11 ? g32(msg + 7) : 0;
        struct fid_entry *df = size >= 11 ? find_fid(c, fid) : NULL;
        fprintf(stderr, "[netfs] T%d tag=%u fid=%u path=%llx ft=%d size=%u\n",
                type, tag, fid, (unsigned long long)(df ? df->path : 0),
                df ? path_ftype(df->path) : -1, size);
    }
    switch (type) {
    case Tversion:
        handle_version(c, msg, size, tag);
        break;
    case Tauth:
        send_rerror(c, tag, "authentication not required");
        break;
    case Tattach:
        handle_attach(c, msg, size, tag);
        break;
    case Tflush: {
        if (size < 9) {
            send_rerror(c, tag, "short Tflush");
            break;
        }
        uint16_t oldtag = g16(msg + 7);
        cancel_pending_tag(c, oldtag);
        uint8_t resp[16];
        p32(resp, 4 + 1 + 2);
        resp[4] = Rflush;
        p16(resp + 5, tag);
        write_all(c->fd, resp, 7);
        break;
    }
    case Twalk:
        handle_walk(c, msg, size, tag);
        break;
    case Topen:
        handle_open(c, msg, size, tag);
        break;
    case Tread:
        handle_read(c, msg, size, tag);
        break;
    case Twrite:
        handle_write(c, msg, size, tag);
        break;
    case Tclunk:
        handle_clunk(c, msg, size, tag);
        break;
    case Tstat:
        handle_stat(c, msg, size, tag);
        break;
    case Tcreate:
    case Tremove:
    case Twstat:
        send_rerror(c, tag, "operation not supported by netfs");
        break;
    default:
        send_rerror(c, tag, "unsupported 9P operation");
        break;
    }
}

/* ---- server lifecycle ----------------------------------------------- */

static void close_client(struct client *c) {
    struct pending_read **pr = &pending_reads;
    while (*pr != NULL) {
        struct pending_read *p = *pr;
        if (p->client == c) {
            *pr = p->next;
            free(p);
        } else {
            pr = &p->next;
        }
    }
    struct pending_write **pw = &pending_writes;
    while (*pw != NULL) {
        struct pending_write *p = *pw;
        if (p->client == c) {
            *pw = p->next;
            free(p->data);
            free(p);
        } else {
            pw = &p->next;
        }
    }
    struct pending_listen **pl = &pending_listens;
    while (*pl != NULL) {
        struct pending_listen *p = *pl;
        if (p->client == c) {
            *pl = p->next;
            free(p);
        } else {
            pl = &p->next;
        }
    }
    for (int p = 0; p < P_COUNT; p++) {
        for (int i = 0; i < MAX_CONVS; i++) {
            if (c->convs[p][i].in_use) {
                if (c->convs[p][i].fd >= 0)
                    close(c->convs[p][i].fd);
                c->convs[p][i].in_use = 0;
            }
        }
    }
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    c->in_len = 0;
    memset(c->fids, 0, sizeof(c->fids));
}

static void init_client(struct client *c, int fd) {
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->msize = 8192;
}

static void process_client(struct client *c) {
    while (c->in_len >= 4) {
        uint32_t msg_len = g32(c->in_buf);
        if (msg_len < 7 || msg_len > BUFFER_SIZE) {
            close_client(c);
            return;
        }
        if (c->in_len < msg_len)
            break;
        handle_message(c, c->in_buf, msg_len);
        if (c->fd < 0)
            return;
        c->in_len -= msg_len;
        if (c->in_len > 0)
            memmove(c->in_buf, c->in_buf + msg_len, c->in_len);
    }
}

static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char **argv) {
    const char *sock_path = DEFAULT_SOCK_PATH;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            sock_path = argv[++i];
        else if (argv[i][0] != '-')
            sock_path = argv[i];
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    if (pipe2(wake_pipe, O_NONBLOCK | O_CLOEXEC) < 0) {
        perror("[netfs] pipe");
        return 1;
    }

    unlink(sock_path);
    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
        perror("[netfs] socket");
        return 1;
    }
    int flags = fcntl(listen_fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[netfs] bind");
        return 1;
    }
    /* The service has the host's network access.  Do not let other local
     * users attach to it. */
    chmod(sock_path, 0600);
    if (listen(listen_fd, 16) < 0) {
        perror("[netfs] listen");
        return 1;
    }
    fprintf(stderr, "[netfs] host /net service listening on %s\n", sock_path);

    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i].fd = -1;

    while (running) {
        int nfds = 0;
        struct pollfd pfds[MAX_CLIENTS + 1 + MAX_CLIENTS * P_COUNT * MAX_CONVS];
        int pclient[MAX_CLIENTS + 1 + MAX_CLIENTS * P_COUNT * MAX_CONVS];
        int pproto[MAX_CLIENTS + 1 + MAX_CLIENTS * P_COUNT * MAX_CONVS];

        pfds[nfds].fd = listen_fd;
        pfds[nfds].events = POLLIN;
        pclient[nfds] = -1;
        nfds++;
        pfds[nfds].fd = wake_pipe[0];
        pfds[nfds].events = POLLIN;
        pclient[nfds] = -2;
        nfds++;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0) {
                pfds[nfds].fd = clients[i].fd;
                pfds[nfds].events = POLLIN;
                pclient[nfds] = i;
                pproto[nfds] = -1;
                nfds++;
            }
            for (int p = 0; p < P_COUNT; p++) {
                for (int j = 0; j < MAX_CONVS; j++) {
                    struct conv *cv = &clients[i].convs[p][j];
                    if (cv->in_use && cv->fd >= 0) {
                        pfds[nfds].fd = cv->fd;
                        pfds[nfds].events = POLLIN | POLLOUT;
                        pclient[nfds] = i;
                        pproto[nfds] = p;
                        nfds++;
                    }
                }
            }
        }

        int ret = poll(pfds, (nfds_t)nfds, 500);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            perror("[netfs] poll");
            break;
        }

        if (pfds[0].revents & POLLIN) {
            int cfd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (cfd >= 0) {
                int assigned = 0;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd < 0) {
                        init_client(&clients[i], cfd);
                        fprintf(stderr, "[netfs] client %d connected\n", i);
                        assigned = 1;
                        break;
                    }
                }
                if (!assigned) {
                    close(cfd);
                    fprintf(stderr, "[netfs] too many clients\n");
                }
            }
        }
        if (pfds[1].revents & POLLIN) {
            uint8_t drain[64];
            while (read(wake_pipe[0], drain, sizeof(drain)) > 0)
                ;
        }

        for (int i = 2; i < nfds; i++) {
            if (pfds[i].revents == 0)
                continue;
            int ci = pclient[i];
            if (ci < 0)
                continue;
            struct client *c = &clients[ci];
            if (c->fd < 0)
                continue;
            if (pproto[i] >= 0) {
                /* Socket activity is handled by the pending queues. */
                continue;
            }
            if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t nr;
                do {
                    nr = read(c->fd, c->in_buf + c->in_len, sizeof(c->in_buf) - c->in_len);
                } while (nr < 0 && errno == EINTR);
                if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                    continue;
                if (nr <= 0) {
                    fprintf(stderr, "[netfs] client %d disconnected\n", ci);
                    close_client(c);
                } else {
                    c->in_len += (size_t)nr;
                    process_client(c);
                }
            }
        }

        try_pending_reads();
        try_pending_writes();
        try_pending_listens();
    }

    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0)
            close_client(&clients[i]);
    close(listen_fd);
    unlink(sock_path);
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    fprintf(stderr, "[netfs] stopped\n");
    return 0;
}
