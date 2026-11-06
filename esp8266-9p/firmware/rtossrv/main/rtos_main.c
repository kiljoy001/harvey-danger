/* esp9 RTOS-substrate firmware: frozen namespace over 9P/TCP 564.
 *
 * Same namespace and ctl grammar as the NONOS Phase A firmware
 * (firmware/sdkbuild/esp9srv), re-hosted on the ESP8266 RTOS SDK:
 * FreeRTOS tasks, esp_wifi blobs, lwIP sockets. This is the
 * comparison substrate for item 1 (RTOS vs NONOS behaviour).
 */
#include <string.h>
#include <sys/param.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "lwip/sys.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"

#include "rtos_config.h"
#include "rtos_mem.h"
#include "srv9p.h"

static const char *TAG = "esp9-rtos";
static Pp9Server srv;
static pp_arena arena;
static uint8_t arenamem[4096];
static int client_sock = -1;

static char statusbuf[640];
static char wifibuf[512];
static char scanbuf[1024] = "# no scan yet; write scan to /wifi/ctl\n";
static char lastctl[96] = "";
static char lastverdict[64] = "";

static const char *
mode_name(void)
{
	wifi_mode_t mode = WIFI_MODE_NULL;
	esp_wifi_get_mode(&mode);
	switch (mode) {
	case WIFI_MODE_STA:
		return "station";
	case WIFI_MODE_AP:
		return "softap";
	case WIFI_MODE_APSTA:
		return "station+softap";
	default:
		return "off";
	}
}

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
	wifi_sta_list_t sta;

	(void)ctx;
	tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ip);
	esp_wifi_ap_get_sta_list(&sta);
	snprintf(statusbuf, sizeof(statusbuf),
		 "mode %s\nssid ESP9-3968\nip " IPSTR "\nrssi %d\nuptime %u\nheap %u\nclients %u\nlastctl %s\n",
		 mode_name(), IP2STR(&ip.ip), station_rssi(),
		 (unsigned)(esp_timer_get_time() / 1000000),
		 esp_get_free_heap_size(), sta.num, lastctl);
	*len = (uint32_t)strlen(statusbuf);
	return statusbuf;
}

static const char *
rd_version(void *ctx, uint32_t *len)
{
	static char v[160];

	(void)ctx;
	snprintf(v, sizeof(v),
		 "esp9 rtos 0.1\nsrv9p 9P2000 msize 2048\nchip ESP8266EX\nsdk %s\n",
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
	tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ip);
	esp_wifi_get_config(ESP_IF_WIFI_STA, &cfg);
	esp_wifi_get_channel(&ch, &sec);
	snprintf(wifibuf, sizeof(wifibuf),
		 "state %s\nssid ESP9-3968\nstation_ssid %s\nip " IPSTR "\nrssi %d\nchannel %u\n%s",
		 mode_name(), cfg.sta.ssid, IP2STR(&ip.ip), station_rssi(),
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
	tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ip);
	esp_wifi_get_mac(ESP_IF_WIFI_AP, mac);
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
	uint16_t n = 16;
	wifi_ap_record_t rec[16];
	int i;
	char *p = scanbuf;

	if (esp_wifi_scan_get_ap_records(&n, rec) != ESP_OK) {
		snprintf(scanbuf, sizeof(scanbuf), "# scan failed\n");
		return;
	}
	*p = '\0';
	for (i = 0; i < n; i++)
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
}

static const char *
wr_wifi(void *ctx, const char *data, uint32_t len)
{
	char buf[128];
	char *ssid, *key;

	(void)ctx;
	copy_verb(buf, sizeof(buf), data, len);
	snprintf(lastctl, sizeof(lastctl), "wifi:%.80s", buf);
	if (strcmp(buf, "scan") == 0) {
		wifi_scan_config_t sc = {0};
		esp_err_t e = esp_wifi_scan_start(&sc, false);
		if (e != ESP_OK)
			return "scan start failed";
		return NULL;
	}
	if (strcmp(buf, "disconnect") == 0) {
		esp_wifi_disconnect();
		return NULL;
	}
	if (strncmp(buf, "connect ", 8) == 0) {
		wifi_config_t cfg;

		ssid = buf + 8;
		key = strchr(ssid, ' ');
		if (key == NULL)
			return "usage: connect <ssid> <key>";
		*key++ = '\0';
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
	return "bad wifi ctl verb";
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
	if (strcmp(buf, "ap off") == 0) {
		esp_wifi_set_mode(WIFI_MODE_STA);
		return NULL;
	}
	if (strcmp(buf, "ap on") == 0) {
		esp_wifi_set_mode(WIFI_MODE_APSTA);
		return NULL;
	}
	if (strcmp(buf, "factoryreset") == 0) {
		esp_wifi_disconnect();
		esp_wifi_set_mode(WIFI_MODE_APSTA);
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
		vTaskDelete(NULL);
		return;
	}
	ESP_LOGI(TAG, "9P server listening on tcp 564");
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
		pp9_reset(&srv);
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
			if (pp9_input(&srv, &out, (uint8_t *)buf, (uint32_t)r) <
			    0)
				break;
		}
		close(cs);
		client_sock = -1;
		pp9_reset(&srv);
		esp9_mem_event(0);
		ESP_LOGI(TAG, "client disconnected");
	}
}

static void
wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	(void)arg;
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
#ifdef ESP9_PROVISION_SSID
		esp_wifi_connect();
#endif
	} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		ESP_LOGI(TAG, "station disconnected");
#ifdef ESP9_PROVISION_SSID
		esp_wifi_connect();
#endif
	} else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
		scan_done();
	} else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
		ESP_LOGI(TAG, "station ip " IPSTR, IP2STR(&e->ip_info.ip));
	}
}

void
app_main(void)
{
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	wifi_config_t ap;

	ESP_ERROR_CHECK(nvs_flash_init());
	tcpip_adapter_init();
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
						   &wifi_event, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
						   IP_EVENT_STA_GOT_IP,
						   &wifi_event, NULL));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
	memset(&ap, 0, sizeof(ap));
	strncpy((char *)ap.ap.ssid, "ESP9-3968", sizeof(ap.ap.ssid));
	ap.ap.ssid_len = 9;
	ap.ap.channel = 1;
	ap.ap.authmode = WIFI_AUTH_OPEN;
	ap.ap.max_connection = 1;
	ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap));
#ifdef ESP9_PROVISION_SSID
	{
		wifi_config_t sta;
		memset(&sta, 0, sizeof(sta));
		strncpy((char *)sta.sta.ssid, ESP9_PROVISION_SSID,
			sizeof(sta.sta.ssid) - 1);
#ifdef ESP9_PROVISION_KEY
		strncpy((char *)sta.sta.password, ESP9_PROVISION_KEY,
			sizeof(sta.sta.password) - 1);
#endif
		ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta));
		ESP_LOGI(TAG, "joining provisioned ssid %s", ESP9_PROVISION_SSID);
	}
#endif
	ESP_ERROR_CHECK(esp_wifi_start());

	pp_arena_init(&arena, arenamem, sizeof(arenamem));
	pp9_init(&srv, esp9_files,
		 (int)(sizeof(esp9_files) / sizeof(esp9_files[0])), NULL,
		 &arena);
	esp9_mem_start(&arena);
	xTaskCreate(server_task, "srv9p", 4096, NULL, 5, NULL);
}
