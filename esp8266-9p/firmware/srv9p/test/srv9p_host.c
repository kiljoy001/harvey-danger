/* Host harness for srv9p: serves the frozen namespace with mock
 * state over a POSIX TCP socket. Same content model as mock9p,
 * different implementation; both are tested by one client. */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../srv9p.h"

static char statusbuf[512];
static char wifibuf[256];
static char scanbuf[256] = "# no scan yet; write 'scan' to /wifi/ctl\n";
static char lastctl[64] = "";

static const char *rd_status(void *ctx, uint32_t *len)
{
	(void)ctx;
	snprintf(statusbuf, sizeof(statusbuf),
		 "mode softap\nssid ESP9-3968\nip 192.168.4.1\nrssi 0\nuptime 0\nheap 52328\nclients 0\nlastctl %s\n",
		 lastctl);
	*len = (uint32_t)strlen(statusbuf);
	return statusbuf;
}

static const char *rd_version(void *ctx, uint32_t *len)
{
	static const char v[] =
		"esp9-srv9p 0.1\nchip ESP8266EX (host test)\nmac 48:3f:da:7e:39:68\n";
	(void)ctx;
	*len = (uint32_t)strlen(v);
	return v;
}

static const char *rd_wifi(void *ctx, uint32_t *len)
{
	(void)ctx;
	snprintf(wifibuf, sizeof(wifibuf),
		 "state softap\nssid ESP9-3968\nip 192.168.4.1\nrssi 0\nchannel 1\n");
	*len = (uint32_t)strlen(wifibuf);
	return wifibuf;
}

static const char *rd_scan(void *ctx, uint32_t *len)
{
	(void)ctx;
	*len = (uint32_t)strlen(scanbuf);
	return scanbuf;
}

static const char *rd_net(void *ctx, uint32_t *len)
{
	static const char v[] =
		"ip 192.168.4.1\nnetmask 255.255.255.0\ngateway 192.168.4.1\ndns 192.168.4.1\nmac 48:3f:da:7e:39:68\n";
	(void)ctx;
	*len = (uint32_t)strlen(v);
	return v;
}

static const char *rd_console(void *ctx, uint32_t *len)
{
	static const char v[] = "esp9 console (srv9p host test)\n";
	(void)ctx;
	*len = (uint32_t)strlen(v);
	return v;
}

static const char *wr_wifi(void *ctx, const char *data, uint32_t len)
{
	char buf[64];
	(void)ctx;
	if (len >= sizeof(buf))
		return "ctl too long";
	memcpy(buf, data, len);
	buf[len] = '\0';
	if (strncmp(buf, "scan", 4) == 0) {
		snprintf(scanbuf, sizeof(scanbuf),
			 "# scan (srv9p host test)\nExampleAP\t6\t-61\twpa2\n");
		return NULL;
	}
	if (strncmp(buf, "connect ", 8) == 0 || strncmp(buf, "disconnect", 10) == 0)
		return NULL;
	return "bad wifi ctl verb";
}

static const char *wr_ctl(void *ctx, const char *data, uint32_t len)
{
	char buf[64];
	(void)ctx;
	if (len >= sizeof(buf))
		return "ctl too long";
	memcpy(buf, data, len);
	buf[len] = '\0';
	if (strncmp(buf, "reboot", 6) == 0 || strncmp(buf, "ap ", 3) == 0 ||
	    strncmp(buf, "factoryreset", 12) == 0) {
		snprintf(lastctl, sizeof(lastctl), "%s", buf);
		return NULL;
	}
	return "bad ctl verb";
}

static const Pp9File files[] = {
	{ "/", 1, 1, 0, NULL, NULL },
	{ "/ctl", 2, 0, 1, rd_status, wr_ctl },
	{ "/status", 3, 0, 0, rd_status, NULL },
	{ "/version", 4, 0, 0, rd_version, NULL },
	{ "/wifi", 5, 1, 0, NULL, NULL },
	{ "/wifi/ctl", 6, 0, 1, rd_wifi, wr_wifi },
	{ "/wifi/status", 7, 0, 0, rd_wifi, NULL },
	{ "/wifi/scan", 8, 0, 0, rd_scan, NULL },
	{ "/net", 9, 1, 0, NULL, NULL },
	{ "/net/status", 10, 0, 0, rd_net, NULL },
	{ "/console", 11, 0, 1, rd_console, wr_ctl },
};

static int sock_read(void *ctx, void *buf, uint32_t n)
{
	ssize_t r = recv((int)(intptr_t)ctx, buf, n, 0);
	if (r == 0)
		return 0;
	if (r < 0)
		return -1;
	return (int)r;
}

static int sock_write(void *ctx, const void *buf, uint32_t n)
{
	ssize_t r = send((int)(intptr_t)ctx, buf, n, 0);
	if (r < 0)
		return -1;
	return (int)r;
}

int
main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : 1565;
	int ls, cs;
	struct sockaddr_in addr;
	static uint8_t arenamem[4096];
	static pp_arena arena;
	static Pp9Server srv;
	Pp9Transport t;

	ls = socket(AF_INET, SOCK_STREAM, 0);
	{
		int one = 1;
		setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons((uint16_t)port);
	if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(ls, 1) < 0) {
		perror("bind/listen");
		return 1;
	}
	fprintf(stderr, "srv9p_host on 127.0.0.1:%d\n", port);
	cs = accept(ls, NULL, NULL);
	if (cs < 0)
		return 1;
	{
		/* srv9p sends each reply as header+body writes;
		 * Nagle + delayed ACK would add ~40 ms per reply. */
		int one = 1;
		setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	}
	pp_arena_init(&arena, arenamem, sizeof(arenamem));
	pp9_init(&srv, files, (int)(sizeof(files) / sizeof(files[0])), NULL, &arena);
	t.ctx = (void *)(intptr_t)cs;
	t.read = sock_read;
	t.write = sock_write;
	pp9_serve(&srv, &t);
	close(cs);
	close(ls);
	return 0;
}
