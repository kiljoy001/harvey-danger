/* esp9: RTOS-generation Wi-Fi blobs on the Plan 9
 * kernel shim, serving the frozen namespace over 9P/TCP 564.
 *
 * app_main runs in the startup's user_init_entry task, which
 * the shim runs as a p9 proc. The Wi-Fi bring-up mirrors the
 * B18 milestone flow (init, pp-task, scan, provisioned join);
 * the 9P server is the rtossrv lineage (srv9p core + lwIP
 * sockets), re-hosted from the real RTOS kernel onto the shim.
 * Milestones are printed as MILESTONE lines for the UART log. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/raw.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"

#include "il9.h"
#include "p9_os_xtensa.h"
#include "pp_arena.h"
#include "rtos_mem.h"
#include "srv9p.h"

extern int8_t esp_wifi_get_ap_rssi(void);

uint32_t lmac_dbg[16];	/* diagnostic stub when blob lmac linked */

static const char *TAG = "esp9";

static Pp9Server *srv;	/* heap, in app_main: BSS is gated */
static pp_arena arena;
static uint8_t arenamem[4096];
static int client_sock = -1;

static char statusbuf[640];
static char wifibuf[512];
static char scanbuf[1024] = "# no scan yet; write scan to /wifi/ctl\n";
static char lastctl[96] = "";
static char lastverdict[64] = "";
/* IL (proto 40) packets accepted by il_raw_recv; in /status
 * as ilrx so a dead transport is visible, not silent. */
static uint32_t il_rx_pkts;
static uint32_t il_tx_pkts;
static uint32_t il_tx_err;
static int il_state_copy;

/* ---- credentials file ----
 * The Wi-Fi credentials live in a file, not in the build:
 * the NVS namespace "esp9wifi" (flash partition) is the
 * backing store, /wifi/creds is its namespace face, and a
 * `connect` write to /wifi/ctl stores there too. Boot loads
 * the file and joins automatically. The key is write-only:
 * no served file ever returns it (rd_creds reports only
 * whether one is set). factoryreset erases the file. */
static char cred_ssid[33] = "";
static char cred_key[65] = "";
static int have_creds;

static int
creds_load(void)
{
	nvs_handle h;
	size_t n;
	int ok = 0;

	if (nvs_open("esp9wifi", NVS_READONLY, &h) != ESP_OK)
		return 0;
	n = sizeof(cred_ssid);
	if (nvs_get_str(h, "ssid", cred_ssid, &n) == ESP_OK) {
		n = sizeof(cred_key);
		if (nvs_get_str(h, "key", cred_key, &n) == ESP_OK)
			ok = cred_ssid[0] != '\0';
	}
	nvs_close(h);
	have_creds = ok;
	if (!ok) {
		cred_ssid[0] = '\0';
		cred_key[0] = '\0';
	}
	return ok;
}

static void
creds_save(const char *ssid, const char *key)
{
	nvs_handle h;

	if (nvs_open("esp9wifi", NVS_READWRITE, &h) != ESP_OK)
		return;
	nvs_set_str(h, "ssid", ssid);
	nvs_set_str(h, "key", key);
	nvs_commit(h);
	nvs_close(h);
	snprintf(cred_ssid, sizeof(cred_ssid), "%s", ssid);
	snprintf(cred_key, sizeof(cred_key), "%s", key);
	have_creds = 1;
}

static void
creds_clear(void)
{
	nvs_handle h;

	if (nvs_open("esp9wifi", NVS_READWRITE, &h) == ESP_OK) {
		nvs_erase_all(h);
		nvs_commit(h);
		nvs_close(h);
	}
	cred_ssid[0] = '\0';
	cred_key[0] = '\0';
	have_creds = 0;
}

/* Store, configure and join. Shared by /wifi/ctl,
 * /wifi/creds and the UART console. */
static const char *
provision(const char *ssid, const char *key)
{
	wifi_config_t cfg;

	creds_save(ssid, key);
	memset(&cfg, 0, sizeof(cfg));
	strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
	strncpy((char *)cfg.sta.password, key,
		sizeof(cfg.sta.password) - 1);
	esp_wifi_set_config(ESP_IF_WIFI_STA, &cfg);
	esp_wifi_connect();
	snprintf(lastverdict, sizeof(lastverdict), "connecting to %s\n",
		 ssid);
	return NULL;
}

/* ---- frozen namespace (DESIGN section 5), station flavour ---- */

static int
station_rssi(void)
{
	wifi_ap_record_t ap;
	if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
		return ap.rssi;
	return 0;
}

static const char *
rd_status(void *ctx, uint32_t *len)
{
	tcpip_adapter_ip_info_t ip;

	(void)ctx;
	tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip);
	snprintf(statusbuf, sizeof(statusbuf),
		 "mode station\nip " IPSTR "\nrssi %d\nuptime %u\nheap %u\nclients %u\nilrx %u\niltx %u\niltxerr %u\nilstate %d\nlastctl %s\n",
		 IP2STR(&ip.ip), station_rssi(),
		 (unsigned)(esp_timer_get_time() / 1000000),
		 esp_get_free_heap_size(),
		 client_sock >= 0 ? 1u : 0u, (unsigned)il_rx_pkts,
		 (unsigned)il_tx_pkts, (unsigned)il_tx_err,
		 il_state_copy, lastctl);
	*len = (uint32_t)strlen(statusbuf);
	return statusbuf;
}

static const char *
rd_version(void *ctx, uint32_t *len)
{
	static char v[160];

	(void)ctx;
	snprintf(v, sizeof(v),
		 "esp9 0.1\nsrv9p 9P2000 msize 2048\nchip ESP8266EX\nsdk %s\n",
		 esp_get_idf_version());
	*len = (uint32_t)strlen(v);
	return v;
}

static const char *
rd_wifi(void *ctx, uint32_t *len)
{
	tcpip_adapter_ip_info_t ip;
	wifi_config_t cfg;
	uint8_t ch = 0;
	wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;

	(void)ctx;
	tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip);
	esp_wifi_get_config(ESP_IF_WIFI_STA, &cfg);
	esp_wifi_get_channel(&ch, &sec);
	snprintf(wifibuf, sizeof(wifibuf),
		 "state station\nstation_ssid %s\nip " IPSTR "\nrssi %d\nchannel %u\n%s",
		 cfg.sta.ssid, IP2STR(&ip.ip), station_rssi(),
		 ch, lastverdict);
	*len = (uint32_t)strlen(wifibuf);
	return wifibuf;
}

static const char *
rd_scan(void *ctx, uint32_t *len)
{
	(void)ctx;
	*len = (uint32_t)strlen(scanbuf);
	return scanbuf;
}

static const char *
rd_mem(void *ctx, uint32_t *len)
{
	(void)ctx;
	return esp9_mem_read(len);
}

static const char *
rd_net(void *ctx, uint32_t *len)
{
	static char v[256];
	tcpip_adapter_ip_info_t ip;
	uint8_t mac[6];

	(void)ctx;
	tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip);
	esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);
	snprintf(v, sizeof(v),
		 "ip " IPSTR "\nnetmask " IPSTR "\ngateway " IPSTR
		 "\ndns 0.0.0.0\nmac %02x:%02x:%02x:%02x:%02x:%02x\n",
		 IP2STR(&ip.ip), IP2STR(&ip.netmask), IP2STR(&ip.gw), mac[0],
		 mac[1], mac[2], mac[3], mac[4], mac[5]);
	*len = (uint32_t)strlen(v);
	return v;
}

static void
scan_done(void)
{
	/* static: 16 records do not fit the sys_evt proc stack */
	static wifi_ap_record_t rec[16];
	uint16_t n = 16;
	int i;
	char *p = scanbuf;

	if (esp_wifi_scan_get_ap_records(&n, rec) != ESP_OK) {
		snprintf(scanbuf, sizeof(scanbuf), "# scan failed\n");
		return;
	}
	*p = '\0';
	for (i = 0; i < (int)n; i++)
		p += snprintf(p, (size_t)(scanbuf + sizeof(scanbuf) - p),
			      "%s\t%u\t%d\t%u\n", rec[i].ssid, rec[i].primary,
			      rec[i].rssi, rec[i].authmode);
}

static void
copy_verb(char *dst, uint32_t dstlen, const char *data, uint32_t len)
{
	if (len >= dstlen)
		len = dstlen - 1;
	memcpy(dst, data, len);
	dst[len] = '\0';
	/* Shell writes carry a trailing newline (echo x > ctl);
	 * Plan 9 ctl parsers accept it, so strip CR/LF here.
	 * Without this every verb from rc/echo was rejected
	 * (found B32 via Harvey: echo scan > /n/esp9/wifi/ctl). */
	while (len > 0 && (dst[len - 1] == '\n' || dst[len - 1] == '\r'))
		dst[--len] = '\0';
}

static const char *
wr_wifi(void *ctx, const char *data, uint32_t len)
{
	char buf[128];
	char *ssid, *key;

	(void)ctx;
	copy_verb(buf, sizeof(buf), data, len);
	/* Never record the raw line: the connect verb carries
	 * the AP key, and lastctl is world-readable in /status.
	 * (Found B33: provisioning via console put the key in
	 * /status for anyone on the LAN to read.) connect is
	 * special-cased below once the SSID is split out. */
	if (strncmp(buf, "connect ", 8) != 0)
		snprintf(lastctl, sizeof(lastctl), "wifi:%.80s", buf);
	if (strcmp(buf, "scan") == 0) {
		wifi_scan_config_t sc;
		esp_err_t e;
		memset(&sc, 0, sizeof(sc));
		e = esp_wifi_scan_start(&sc, false);
		if (e != ESP_OK)
			return "scan start failed";
		return NULL;
	}
	if (strcmp(buf, "disconnect") == 0) {
		esp_wifi_disconnect();
		return NULL;
	}
	if (strncmp(buf, "connect ", 8) == 0) {
		ssid = buf + 8;
		key = strchr(ssid, ' ');
		if (key == NULL)
			return "usage: connect <ssid> <key>";
		*key++ = '\0';
		snprintf(lastctl, sizeof(lastctl), "wifi:connect %.60s", ssid);
		return provision(ssid, key);
	}
	if (strcmp(buf, "forget") == 0) {
		esp_wifi_disconnect();
		creds_clear();
		snprintf(lastverdict, sizeof(lastverdict),
			 "credentials forgotten\n");
		return NULL;
	}
	return "bad wifi ctl verb";
}

/* /wifi/creds: read reports the SSID and whether a key is
 * stored, never the key. Write is "<ssid> <key>" and means
 * store-and-join, exactly like ctl `connect`. */
static const char *
rd_creds(void *ctx, uint32_t *len)
{
	static char v[96];

	(void)ctx;
	snprintf(v, sizeof(v), "ssid %s\nkey %s\n",
		 have_creds ? cred_ssid : "",
		 have_creds ? "set" : "unset");
	*len = (uint32_t)strlen(v);
	return v;
}

static const char *
wr_creds(void *ctx, const char *data, uint32_t len)
{
	char buf[128];
	char *ssid, *key;

	(void)ctx;
	copy_verb(buf, sizeof(buf), data, len);
	ssid = buf;
	key = strchr(ssid, ' ');
	if (key == NULL)
		return "usage: write is <ssid> <key>";
	*key++ = '\0';
	return provision(ssid, key);
}

static const char *
wr_ctl(void *ctx, const char *data, uint32_t len)
{
	char buf[64];

	(void)ctx;
	copy_verb(buf, sizeof(buf), data, len);
	snprintf(lastctl, sizeof(lastctl), "%s", buf);
	if (strcmp(buf, "reboot") == 0) {
		esp_restart();
		return NULL;
	}
	if (strcmp(buf, "factoryreset") == 0) {
		creds_clear();
		esp_restart();
		return NULL;
	}
	return "bad ctl verb";
}

/* UART console: the same verb grammar as the files, so a
 * cabled device with no credentials can be provisioned
 * (`connect <ssid> <key>`) before it has any network.
 * Wi-Fi verbs route to wr_wifi, the rest to wr_ctl. */
static void
cons_task(void *arg)
{
	char line[128];
	int n = 0;
	int c;

	(void)arg;
	ESP_LOGI(TAG, "console ready: connect <ssid> <key>");
	for (;;) {
		c = getchar();
		if (c == EOF) {
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}
		if (c == '\r' || c == '\n') {
			const char *err;
			if (n == 0)
				continue;
			line[n] = '\0';
			n = 0;
			if (strncmp(line, "connect ", 8) == 0 ||
			    strcmp(line, "scan") == 0 ||
			    strcmp(line, "disconnect") == 0 ||
			    strcmp(line, "forget") == 0)
				err = wr_wifi(NULL, line,
					      (uint32_t)strlen(line));
			else
				err = wr_ctl(NULL, line,
					     (uint32_t)strlen(line));
			ESP_LOGI(TAG, "console: %s -> %s", line,
				 err != NULL ? err : "ok");
			continue;
		}
		if (n < (int)sizeof(line) - 1)
			line[n++] = (char)c;
	}
}

static const Pp9File esp9_files[] = {
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
	{ "/console", 11, 0, 1, rd_status, wr_ctl },
	{ "/mem", 12, 0, 0, rd_mem, NULL },
	{ "/wifi/creds", 13, 0, 1, rd_creds, wr_creds },
};

/* ---- 9P server task (lwIP TCP 564) ---- */

static int
sock_send(void *ctx, const void *buf, uint32_t n)
{
	int s = *(int *)ctx;
	const uint8_t *p = (const uint8_t *)buf;
	uint32_t sent = 0;

	while (sent < n) {
		int r = send(s, p + sent, n - sent, 0);
		if (r <= 0)
			return -1;
		sent += (uint32_t)r;
	}
	return (int)n;
}

static void
server_task(void *arg)
{
	int ls;
	struct sockaddr_in addr;

	(void)arg;
	ls = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	memset(&addr, 0, sizeof(addr));
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_family = AF_INET;
	addr.sin_port = htons(564);
	if (ls < 0 || bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(ls, 1) != 0) {
		ESP_LOGE(TAG, "listen 564 failed");
		if (ls >= 0)
			close(ls);
		return; /* supervisor retries */
	}
	ESP_LOGI(TAG, "MILESTONE 9p-listening tcp 564");
	for (;;) {
		int cs = accept(ls, NULL, NULL);
		char buf[1024];

		if (cs < 0)
			continue;
		client_sock = cs;
		{
			int one = 1;
			setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &one,
				   sizeof(one));
		}
		pp9_reset(srv);
		esp9_mem_event(1);
		ESP_LOGI(TAG, "client connected");
		for (;;) {
			int r = recv(cs, buf, sizeof(buf), 0);
			Pp9Transport out;

			if (r <= 0)
				break;
			out.ctx = &client_sock;
			out.read = NULL;
			out.write = sock_send;
			if (pp9_input(srv, &out, (uint8_t *)buf, (uint32_t)r) <
			    0)
				break;
		}
		close(cs);
		client_sock = -1;
		pp9_reset(srv);
		esp9_mem_event(0);
		ESP_LOGI(TAG, "client disconnected");
	}
}

/* The server proc is disposable: if the listener ever exits
 * (listen failure is the only return path), recreate it
 * instead of serving nothing forever. Faults inside a
 * session reboot the machine via panicHandler (see
 * shim_xtensa.c fatal_reboot). */
static void
server_supervised(void *arg)
{
	(void)arg;
	for (;;) {
		server_task(NULL);
		ESP_LOGE(TAG, "9p server exited; restarting in 2s");
		vTaskDelay(pdMS_TO_TICKS(2000));
	}
}

/* ---- improved IL transport (IP protocol 40, port 564) ----
 * Second Pp9Server instance over the il9 endpoint. The
 * lwIP raw callback runs in the tcpip proc and only copies
 * the packet into a one-slot mailbox; all IL and 9P work
 * happens in the il9p proc. Buffers come from the SDK heap
 * once at startup (the app layer already lives there);
 * the kernel's static budgets are untouched. */
/* IL shares the TCP server's Pp9Server (srv, set up in
 * app_main): DESIGN allows one client per transport, and
 * a second server instance cost 4.4 KB of heap the blob
 * Wi-Fi stack demonstrably misses (assoc-expire cycles
 * at 18 KB free heap, B30). One client total in
 * practice; if both transports ever deliver at once the
 * core simply sees one interleaved stream. */
static Il9 il;
static struct raw_pcb *il_pcb;
static uint8_t *il_rxbuf;	/* one received packet, heap */
static volatile uint32_t il_rxlen;
static uint32_t il_rx_ip;
static Pp9Transport il_transport;
static uint8_t il_frame[PP9_MSIZE];
static uint32_t il_framelen;
static int il_used;		/* IL side has live 9P state */

static uint32_t
il_now(void)
{
	return (uint32_t)(esp_timer_get_time() / 1000);
}

static void
il_sendpkt(void *ctx, uint32_t dst_ip, const uint8_t *pkt, uint32_t len)
{
	struct pbuf *p;
	ip_addr_t dst;

	(void)ctx;
	p = pbuf_alloc(PBUF_IP, (uint16_t)len, PBUF_RAM);
	if (p == NULL)
		return;
	pbuf_take(p, pkt, (uint16_t)len);
	IP_ADDR4(&dst, (dst_ip >> 24) & 0xFF, (dst_ip >> 16) & 0xFF,
		 (dst_ip >> 8) & 0xFF, dst_ip & 0xFF);
	il_tx_pkts++;
	if (raw_sendto(il_pcb, p, &dst) != ERR_OK)
		il_tx_err++;
	pbuf_free(p);
}

static void il_pump_rx(void);

static int
il_write(void *ctx, const void *buf, uint32_t n)
{
	const uint8_t *p = (const uint8_t *)buf;
	uint32_t sent = 0;

	(void)ctx;
	/* frame complete 9P messages (4-byte little-endian
	 * length prefix) and hand each to the IL engine;
	 * if its two slots are busy, pump and wait - our
	 * peer's ACK is what frees a slot */
	while (sent < n) {
		uint32_t want, take, tries;
		if (il_framelen < 4) {
			take = 4 - il_framelen;
			if (take > n - sent)
				take = n - sent;
			memcpy(il_frame + il_framelen, p + sent, take);
			il_framelen += take;
			sent += take;
			if (il_framelen < 4)
				break;
		}
		want = (uint32_t)il_frame[0] |
		       ((uint32_t)il_frame[1] << 8) |
		       ((uint32_t)il_frame[2] << 16) |
		       ((uint32_t)il_frame[3] << 24);
		if (want < 4 || want > PP9_MSIZE)
			return -1;
		take = want - il_framelen;
		if (take > n - sent)
			take = n - sent;
		memcpy(il_frame + il_framelen, p + sent, take);
		il_framelen += take;
		sent += take;
		if (il_framelen < want)
			break;
		for (tries = 0;; tries++) {
			if (il9_sendmsg(&il, il_frame, want, il_now()) == 0)
				break;
			if (tries > 500)
				return -1;
			il_pump_rx();
			il9_poll(&il, il_now());
			vTaskDelay(pdMS_TO_TICKS(10));
		}
		il_framelen = 0;
	}
	return (int)n;
}

static void
il_deliver(void *ctx, const uint8_t *msg, uint32_t len)
{
	(void)ctx;
	il_used = 1;
	if (pp9_input(srv, &il_transport, msg, len) < 0) {
		pp9_reset(srv);
		il9_close(&il, il_now());
	}
}

static void
il_hangup(void *ctx)
{
	(void)ctx;
	if (il_used) {
		pp9_reset(srv);
		il_used = 0;
	}
	ESP_LOGI(TAG, "il: connection closed");
}

/* process the mailbox slot, if filled */
static void
il_pump_rx(void)
{
	uint32_t len, ip;
	uint16_t sport;

	if (il_rxlen == 0)
		return;
	len = il_rxlen;
	ip = il_rx_ip;
	sport = (uint16_t)((il_rxbuf[6] << 8) | il_rxbuf[7]);
	il_rxlen = 0;
	il9_input(&il, ip, sport, il_rxbuf, len, il_now());
}

static uint8_t
il_raw_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p,
	    const ip_addr_t *addr)
{
	uint32_t ihl, len;
	uint32_t ip;

	(void)arg;
	(void)pcb;
	if (p == NULL || !IP_IS_V4(addr))
		return 0;
	if (il_rxlen != 0 || p->tot_len < 20)
		return 0;	/* busy: drop, IL retransmits */
	ihl = (uint32_t)(p->payload ? (((uint8_t *)p->payload)[0] & 0x0F) : 0) * 4;
	/* Was <= until B33: a header-only packet (Sync, Close,
	 * bare Ack) is exactly ihl + IL9_HDRSIZE long (38 bytes
	 * for IPv4), so the handshake itself was dropped here
	 * before the engine ever saw it. Data packets are
	 * longer, which is why engine host-tests passed and
	 * only a real kernel peer exposed it. < is correct:
	 * a full header with zero payload is a legal packet. */
	if (ihl < 20 || p->tot_len < ihl + IL9_HDRSIZE)
		return 0;
	len = p->tot_len - ihl;
	if (len > IL9_BUFMAX)
		return 0;
	if (pbuf_copy_partial(p, il_rxbuf, (uint16_t)len, (uint16_t)ihl) != len)
		return 0;
	il_rx_pkts++;
	ip = ip4_addr_get_u32(ip_2_ip4(addr));
	il_rx_ip = ((ip & 0xFF) << 24) | ((ip >> 8) & 0xFF) << 16 |
		   ((ip >> 16) & 0xFF) << 8 | ((ip >> 24) & 0xFF);
	il_rxlen = len;
	pbuf_free(p);
	return 1;
}

static void
il_task(void *arg)
{
	uint8_t *unacked, *pending;
	uint8_t mac[6];
	uint32_t seed;

	(void)arg;
	unacked = (uint8_t *)malloc(IL9_BUFMAX);
	il_rxbuf = (uint8_t *)malloc(IL9_BUFMAX);
	if (unacked == NULL || il_rxbuf == NULL) {
		ESP_LOGE(TAG, "FATAL: il buffer alloc failed");
		abort();
	}
	pending = NULL;	/* no queue slot: sendmsg refuses while busy */
	esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);
	seed = (uint32_t)esp_timer_get_time() ^ 25001u ^
	       ((uint32_t)mac[4] << 8) ^ mac[5] ^ (il_now() << 7);
	il9_init(&il, seed & 0xFFFFFFu, unacked, pending);
	il.ctx = NULL;
	il.sendpkt = il_sendpkt;
	il.deliver = il_deliver;
	il.hangup = il_hangup;
	il.lport = 564;
	il_transport.ctx = NULL;
	il_transport.read = NULL;
	il_transport.write = il_write;
	il_pcb = raw_new(IL9_PROTO);
	if (il_pcb == NULL) {
		ESP_LOGE(TAG, "FATAL: il raw pcb failed");
		abort();
	}
	raw_recv(il_pcb, il_raw_recv, NULL);
	raw_bind(il_pcb, IP_ADDR_ANY);
	ESP_LOGI(TAG, "MILESTONE il-listening proto 40 port 564");
	for (;;) {
		il_pump_rx();
		il9_poll(&il, il_now());
		il_state_copy = il9_state(&il);
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

/* ---- Wi-Fi bring-up ---- */

static void
on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	(void)arg;
	(void)base;
	if (id == WIFI_EVENT_STA_DISCONNECTED && data != NULL) {
		wifi_event_sta_disconnected_t *d = data;
		ESP_LOGI(TAG, "EVT wifi id=%d reason=%u", id, d->reason);
		/* With stored credentials, retry like the SDK
		 * station example: the auth/assoc exchange over
		 * the shim loses a frame now and then. */
		if (have_creds) {
			static int retries;
			if (retries < 10) {
				retries++;
				ESP_LOGI(TAG, "reconnect %d rc=%d", retries,
					 esp_wifi_connect());
			}
		}
	} else if (id == WIFI_EVENT_SCAN_DONE) {
		scan_done();
		ESP_LOGI(TAG, "EVT wifi id=%d", id);
	} else {
		ESP_LOGI(TAG, "EVT wifi id=%d", id);
	}
}

void
app_main(void)
{
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	esp_err_t rc;
	int i;

	esp_log_level_set("*", ESP_LOG_INFO);
	esp_log_level_set("wifi", ESP_LOG_VERBOSE);
	esp_log_level_set("phy_init", ESP_LOG_VERBOSE);
	esp_log_level_set("esp_adapter", ESP_LOG_VERBOSE);
	ESP_LOGI(TAG, "app_main entered (shim scheduler)");
	ESP_ERROR_CHECK(nvs_flash_init());
	tcpip_adapter_init();
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
						   ESP_EVENT_ANY_ID,
						   on_wifi_event, NULL));
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_LOGI(TAG, "MILESTONE init-returns heap=%u",
		 esp_get_free_heap_size());

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	/* Credentials come from the file, never the build. */
	if (creds_load()) {
		wifi_config_t sta;
		memset(&sta, 0, sizeof(sta));
		strncpy((char *)sta.sta.ssid, cred_ssid,
			sizeof(sta.sta.ssid) - 1);
		strncpy((char *)sta.sta.password, cred_key,
			sizeof(sta.sta.password) - 1);
		ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta));
		ESP_LOGI(TAG, "creds: loaded ssid %s from file", cred_ssid);
	} else {
		ESP_LOGI(TAG, "creds: none stored; provision via "
			 "/wifi/creds, /wifi/ctl connect, or console");
	}
	rc = esp_wifi_start();
	ESP_LOGI(TAG, "wifi_start rc=%d", rc);
	/* Give the pp task (a shim proc) a moment to run. */
	vTaskDelay(pdMS_TO_TICKS(1000));
	ESP_LOGI(TAG, "MILESTONE pp-task heap=%u", esp_get_free_heap_size());

	{
		wifi_scan_config_t sc;
		uint16_t n = 0, show;
		wifi_ap_record_t rec[8];

		memset(&sc, 0, sizeof(sc));
		rc = esp_wifi_scan_start(&sc, true);
		esp_wifi_scan_get_ap_num(&n);
		scan_done();
		ESP_LOGI(TAG, "scan rc=%d ap_num=%u", rc, n);
		show = n > 8 ? 8 : n;
		if (esp_wifi_scan_get_ap_records(&show, rec) == ESP_OK) {
			for (i = 0; i < show; i++)
				ESP_LOGI(TAG, "AP %s ch=%u rssi=%d auth=%u",
					 rec[i].ssid, rec[i].primary,
					 rec[i].rssi, rec[i].authmode);
		}
		if (n > 0)
			ESP_LOGI(TAG, "MILESTONE scan-ok");
	}

	srv = (Pp9Server *)malloc(sizeof(Pp9Server));
	if (srv == NULL) {
		ESP_LOGE(TAG, "FATAL: srv alloc failed");
		abort();
	}
	pp_arena_init(&arena, arenamem, sizeof(arenamem));
	pp9_init(srv, esp9_files,
		 (int)(sizeof(esp9_files) / sizeof(esp9_files[0])), NULL,
		 &arena);
	esp9_mem_init(&arena);


	if (have_creds) {
		rc = esp_wifi_connect();
		ESP_LOGI(TAG, "connect rc=%d", rc);
		for (i = 0; i < 40; i++) {
			vTaskDelay(pdMS_TO_TICKS(500));
			if (esp_wifi_get_ap_rssi() != 0) {
				ESP_LOGI(TAG, "MILESTONE join-ok rssi=%d",
					 esp_wifi_get_ap_rssi());
				break;
			}
		}
		ESP_LOGI(TAG, "join state rssi=%d", esp_wifi_get_ap_rssi());
	}
	/* Required procs are fatal if they cannot start:
	 * B26 lost srv9p silently to a full proc table and
	 * the only symptom was a missing milestone. */
	if (xTaskCreate(cons_task, "cons", 3072, NULL, 5, NULL) != pdPASS) {
		ESP_LOGE(TAG, "FATAL: cons proc create failed");
		abort();
	}
	if (xTaskCreate(server_supervised, "srv9p", 4096, NULL, 5,
			NULL) != pdPASS) {
		ESP_LOGE(TAG, "FATAL: srv9p proc create failed");
		abort();
	}
	if (xTaskCreate(il_task, "il9p", 4096, NULL, 5, NULL) != pdPASS) {
		ESP_LOGE(TAG, "FATAL: il9p proc create failed");
		abort();
	}
	{
		char line[128];
		int off = 0;
		uint32_t s;
		off += snprintf(line + off, sizeof(line) - off,
				"procs %u/%u:", p9_os_proc_used(),
				p9_os_proc_slots());
		for (s = 0; s < p9_os_proc_slots(); s++) {
			const char *nm = p9_os_proc_name(s);
			if (nm != NULL && off > 0 &&
			    off < (int)sizeof(line))
				off += snprintf(line + off,
						sizeof(line) - off,
						" %s", nm);
		}
		ESP_LOGI(TAG, "%s", line);
	}
	ESP_LOGI(TAG, "MILESTONE done heap=%u", esp_get_free_heap_size());
	if (have_creds) {
		for (;;) {
		extern uint32_t g_pp_posts;
		vTaskDelay(pdMS_TO_TICKS(5000));
		esp9_mem_sample_at((uint32_t)(esp_timer_get_time() / 1000),
				   esp_get_free_heap_size());
		/* No blocking Wi-Fi query on this line (B21):
		 * if the RX path dies, esp_wifi_get_ap_rssi can
		 * block forever and take liveness logging with
		 * it. rx counts pp-queue RX posts (sig 5). */
			/* esf pool levels (B24/B25 probe): the
			 * blob kept these unreadable; if a pool
			 * drains to 0 and stays, that is the
			 * RX-stall mechanism. */
			{
				extern uint32_t pp_esf_rx_free_count(void);
				extern uint32_t pp_esf_general_free_count(void);
				extern void *GetAccess(uint32_t ac);
				{
					extern uint32_t lmac_dbg[16];

					ESP_LOGI(TAG, "alive rx=%u heap=%u cli=%d "
						 "esfrx=%u esfgen=%u acst=%d,%d,%d,%d",
						 g_pp_posts, esp_get_free_heap_size(),
						 client_sock >= 0 ? 1 : 0,
						 pp_esf_rx_free_count(),
						 pp_esf_general_free_count(),
						 ((uint8_t *)GetAccess(0))[0x11],
						 ((uint8_t *)GetAccess(1))[0x11],
						 ((uint8_t *)GetAccess(2))[0x11],
						 ((uint8_t *)GetAccess(3))[0x11]);
					ESP_LOGI(TAG, "dbg tf=%u ts=%u at=%u srf=%u "
						 "ef1=%u ef5=%u ef6=%u pq=%u rc=%u pp=%u",
						 lmac_dbg[0], lmac_dbg[1], lmac_dbg[2],
						 lmac_dbg[3], lmac_dbg[4], lmac_dbg[5],
						 lmac_dbg[6], lmac_dbg[8], lmac_dbg[9],
						 lmac_dbg[10]);
				}
			}
		}
	}
}
