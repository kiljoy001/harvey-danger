/* esp9 file table: real device state behind the frozen namespace.
 * Phase A (DESIGN 9): SoftAP+station, 9P over TCP. Ctl verbs act;
 * see doc/DESIGN.md section 5 for the contract. */
#include "ets_sys.h"
#include "osapi.h"
#include "user_interface.h"

#include "srv9p.h"
#include "esp9_mem.h"

extern int os_printf_plus(const char *fmt, ...);
/* ets_* ROM functions have no prototypes in this SDK's headers. */
extern void *ets_memcpy(void *dst, const void *src, unsigned int n);
extern void *ets_memset(void *dst, int c, unsigned int n);
extern int ets_strcmp(const char *a, const char *b);
extern int ets_strncmp(const char *a, const char *b, unsigned int n);
extern char *ets_strncpy(char *dst, const char *src, unsigned int n);
extern unsigned int ets_strlen(const char *s);
extern int ets_sprintf(char *buf, const char *fmt, ...);

static char statusbuf[640];
static char wifibuf[512];
static char scanbuf[1024] = "# no scan yet; write scan to /wifi/ctl\n";
static char lastctl[96] = "";
static char lastverdict[64] = "";

static const char *ICACHE_FLASH_ATTR
mode_name(void)
{
	switch (wifi_get_opmode()) {
	case STATION_MODE:
		return "station";
	case SOFTAP_MODE:
		return "softap";
	case STATIONAP_MODE:
		return "station+softap";
	default:
		return "off";
	}
}

static const char *ICACHE_FLASH_ATTR
rd_status(void *ctx, uint32_t *len)
{
	struct ip_info ip;
	(void)ctx;
	wifi_get_ip_info(SOFTAP_IF, &ip);
	os_sprintf(statusbuf,
		   "mode %s\nssid ESP9-3968\nip " IPSTR "\nrssi %d\nuptime %u\nheap %u\nclients %u\nlastctl %s\n",
		   mode_name(), IP2STR(&ip.ip), wifi_station_get_rssi(),
		   system_get_time() / 1000000, system_get_free_heap_size(),
		   wifi_softap_get_station_num(), lastctl);
	*len = (uint32_t)os_strlen(statusbuf);
	return statusbuf;
}

static const char *ICACHE_FLASH_ATTR
rd_version(void *ctx, uint32_t *len)
{
	static char v[160];
	(void)ctx;
	os_sprintf(v, "esp9 phaseA 0.1\nsrv9p 9P2000 msize 2048\nchip ESP8266EX\nsdk %s\n",
		   system_get_sdk_version());
	*len = (uint32_t)os_strlen(v);
	return v;
}

static const char *ICACHE_FLASH_ATTR
rd_wifi(void *ctx, uint32_t *len)
{
	struct ip_info ip;
	struct station_config cfg;
	(void)ctx;
	wifi_get_ip_info(SOFTAP_IF, &ip);
	wifi_station_get_config(&cfg);
	os_sprintf(wifibuf,
		   "state %s\nssid ESP9-3968\nstation_ssid %s\nip " IPSTR "\nrssi %d\nchannel %u\n%s",
		   mode_name(), cfg.ssid, IP2STR(&ip.ip),
		   wifi_station_get_rssi(), wifi_get_channel(), lastverdict);
	*len = (uint32_t)os_strlen(wifibuf);
	return wifibuf;
}

static const char *ICACHE_FLASH_ATTR
rd_mem(void *ctx, uint32_t *len)
{
	(void)ctx;
	return esp9_mem_read(len);
}

static const char *ICACHE_FLASH_ATTR
rd_scan(void *ctx, uint32_t *len)
{
	(void)ctx;
	*len = (uint32_t)os_strlen(scanbuf);
	return scanbuf;
}

static const char *ICACHE_FLASH_ATTR
rd_net(void *ctx, uint32_t *len)
{
	static char v[256];
	struct ip_info ip;
	uint8 mac[6];
	(void)ctx;
	wifi_get_ip_info(SOFTAP_IF, &ip);
	wifi_get_macaddr(SOFTAP_IF, mac);
	os_sprintf(v, "ip " IPSTR "\nnetmask " IPSTR "\ngateway " IPSTR
		   "\ndns 0.0.0.0\nmac %02x:%02x:%02x:%02x:%02x:%02x\n",
		   IP2STR(&ip.ip), IP2STR(&ip.netmask), IP2STR(&ip.gw),
		   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	*len = (uint32_t)os_strlen(v);
	return v;
}

static void ICACHE_FLASH_ATTR
scan_done(void *arg, STATUS status)
{
	struct bss_info *info = (struct bss_info *)arg;
	char *p = scanbuf;
	if (status != OK) {
		os_sprintf(scanbuf, "# scan failed\n");
		return;
	}
	*p = '\0';
	while (info != NULL) {
		p += os_sprintf(p, "%s\t%u\t%d\t%u\n", info->ssid,
				info->channel, info->rssi, info->authmode);
		info = STAILQ_NEXT(info, next);
	}
}

static void ICACHE_FLASH_ATTR
copy_verb(char *dst, uint32_t dstlen, const char *data, uint32_t len)
{
	if (len >= dstlen)
		len = dstlen - 1;
	os_memcpy(dst, data, len);
	dst[len] = '\0';
}

static const char *ICACHE_FLASH_ATTR
wr_wifi(void *ctx, const char *data, uint32_t len)
{
	char buf[128];
	char *ssid, *key;
	(void)ctx;
	copy_verb(buf, sizeof(buf), data, len);
	os_sprintf(lastctl, "wifi:%s", buf);
	if (os_strcmp(buf, "scan") == 0) {
		if (!wifi_station_scan(NULL, scan_done))
			return "scan start failed";
		return NULL;
	}
	if (os_strcmp(buf, "disconnect") == 0) {
		wifi_station_disconnect();
		return NULL;
	}
	if (os_strncmp(buf, "connect ", 8) == 0) {
		struct station_config cfg;
		ssid = buf + 8;
		key = os_strchr(ssid, ' ');
		if (key == NULL)
			return "usage: connect <ssid> <key>";
		*key++ = '\0';
		os_memset(&cfg, 0, sizeof(cfg));
		os_strncpy((char *)cfg.ssid, ssid, sizeof(cfg.ssid) - 1);
		os_strncpy((char *)cfg.password, key, sizeof(cfg.password) - 1);
		wifi_station_set_config_current(&cfg);
		wifi_station_connect();
		os_sprintf(lastverdict, "connecting to %s\n", ssid);
		return NULL;
	}
	return "bad wifi ctl verb";
}

static const char *ICACHE_FLASH_ATTR
wr_ctl(void *ctx, const char *data, uint32_t len)
{
	char buf[64];
	(void)ctx;
	copy_verb(buf, sizeof(buf), data, len);
	os_sprintf(lastctl, "%s", buf);
	if (os_strcmp(buf, "reboot") == 0) {
		system_restart();
		return NULL;
	}
	if (os_strcmp(buf, "ap off") == 0) {
		wifi_set_opmode_current(STATION_MODE);
		return NULL;
	}
	if (os_strcmp(buf, "ap on") == 0) {
		wifi_set_opmode_current(STATIONAP_MODE);
		return NULL;
	}
	if (os_strcmp(buf, "factoryreset") == 0) {
		wifi_station_disconnect();
		wifi_set_opmode_current(STATIONAP_MODE);
		return NULL;
	}
	return "bad ctl verb";
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
};

const Pp9File *ICACHE_FLASH_ATTR
esp9_file_table(int *nfiles)
{
	*nfiles = (int)(sizeof(esp9_files) / sizeof(esp9_files[0]));
	return esp9_files;
}
