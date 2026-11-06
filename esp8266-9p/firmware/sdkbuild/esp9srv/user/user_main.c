/* esp9 Phase A firmware: frozen namespace over 9P/TCP 564.
 *
 * Substrate is the NONOS blob init (Phase A pragmatics; RTOS
 * phases are in doc/PLAN9-OS.md). SoftAP+station bootstrap:
 * joins ESP9-3968 (open) or stays reachable while connecting to
 * a real network via /wifi/ctl. Do not flash without approval.
 */
#include "ets_sys.h"
#include "osapi.h"
#include "user_interface.h"
#include "espconn.h"

#include "srv9p.h"
#include "esp9_mem.h"
#include "user_config.h"

extern int os_printf_plus(const char *fmt, ...);
extern void uart_div_modify(uint8 uart_no, uint32 div);
extern void *ets_memset(void *dst, int c, unsigned int n);
extern void *ets_memcpy(void *dst, const void *src, unsigned int n);
extern void ets_timer_disarm(ETSTimer *ptimer);
extern void ets_timer_setfn(ETSTimer *ptimer, ETSTimerFunc *fn, void *arg);
extern void ets_timer_arm_new(ETSTimer *ptimer, uint32_t ms, bool repeat,
			      int ms_flag);
extern char *ets_strcpy(char *dst, const char *src);
const Pp9File *esp9_file_table(int *nfiles);

static struct espconn server_conn;
static esp_tcp server_tcp;
static struct espconn *client_conn;
static Pp9Server srv;
static pp_arena arena;
static uint8_t arenamem[4096];

/* espconn allows one outstanding send; a second espconn_send
 * before the sent callback fails with ESPCONN_MAXNUM. So this
 * transport queues bytes and pumps one chunk at a time from the
 * sent callback. srv9p writes header and body as separate calls;
 * both just append here. */
static uint8_t sendbuf[4200];
static uint32_t sendlen, sendoff, sendcur;
static int sendbusy;
static ETSTimer send_timer;

static int ICACHE_FLASH_ATTR
pump_send(struct espconn *c)
{
	uint32_t chunk;

	if (c == NULL || sendbusy || sendoff >= sendlen)
		return 0;
	chunk = sendlen - sendoff;
	if (chunk > 1024)
		chunk = 1024;
	if (espconn_send(c, sendbuf + sendoff, (uint16)chunk) != ESPCONN_OK)
		return 0;
	sendcur = chunk;
	sendbusy = 1;
	return 1;
}

static void ICACHE_FLASH_ATTR
schedule_pump(uint32_t ms)
{
	os_timer_disarm(&send_timer);
	os_timer_arm(&send_timer, ms, 0);
}

static void ICACHE_FLASH_ATTR
send_retry(void *arg)
{
	(void)arg;
	if (client_conn != NULL && sendlen > sendoff &&
	    !pump_send(client_conn))
		schedule_pump(20);
}

static void ICACHE_FLASH_ATTR
client_sent(void *arg)
{
	(void)arg;
	sendoff += sendcur;
	sendcur = 0;
	sendbusy = 0;
	if (sendoff >= sendlen)
		sendoff = sendlen = 0;
	if (sendlen > sendoff)
		pump_send(client_conn);
}

static int ICACHE_FLASH_ATTR
esp9_send(void *ctx, const void *buf, uint32_t n)
{
	struct espconn *c = (struct espconn *)ctx;
	if (c == NULL)
		return -1;
	if (sendlen + n > sizeof(sendbuf))
		return -1;
	ets_memcpy(sendbuf + sendlen, buf, n);
	sendlen += n;
	/* Coalesce: srv9p writes a reply as header then body. Wait
	 * 1 ms so both appends land, then send the reply as one
	 * espconn chunk instead of two back-to-back sends. */
	if (!sendbusy)
		schedule_pump(1);
	return (int)n;
}

static void ICACHE_FLASH_ATTR
send_reset(void)
{
	sendlen = sendoff = sendcur = 0;
	sendbusy = 0;
}

static void ICACHE_FLASH_ATTR
client_recv(void *arg, char *pdata, unsigned short length)
{
	Pp9Transport out;
	out.ctx = arg;
	out.read = NULL;
	out.write = esp9_send;
	if (pp9_input(&srv, &out, (uint8_t *)pdata, length) < 0)
		espconn_disconnect((struct espconn *)arg);
}

static void ICACHE_FLASH_ATTR
client_discon(void *arg)
{
	(void)arg;
	client_conn = NULL;
	pp9_reset(&srv);
	send_reset();
	esp9_mem_event(0);
	os_printf("ESP9: client disconnected\n");
}

static void ICACHE_FLASH_ATTR
client_recon(void *arg, sint8 err)
{
	(void)arg;
	(void)err;
	client_conn = NULL;
	pp9_reset(&srv);
	send_reset();
	esp9_mem_event(0);
}

static void ICACHE_FLASH_ATTR
server_connect(void *arg)
{
	struct espconn *c = (struct espconn *)arg;
	client_conn = c;
	pp9_reset(&srv);
	send_reset();
	esp9_mem_event(1);
	espconn_regist_sentcb(c, client_sent);
	espconn_regist_recvcb(c, client_recv);
	espconn_regist_disconcb(c, client_discon);
	espconn_regist_reconcb(c, client_recon);
	os_printf("ESP9: client connected\n");
}

static void ICACHE_FLASH_ATTR
start_server(void)
{
	int n;
	const Pp9File *files = esp9_file_table(&n);
	pp_arena_init(&arena, arenamem, sizeof(arenamem));
	pp9_init(&srv, files, n, NULL, &arena);
	esp9_mem_start(&arena);
	os_timer_disarm(&send_timer);
	os_timer_setfn(&send_timer, send_retry, NULL);

	server_conn.type = ESPCONN_TCP;
	server_conn.state = ESPCONN_NONE;
	server_tcp.local_port = 564;
	server_conn.proto.tcp = &server_tcp;
	espconn_regist_connectcb(&server_conn, server_connect);
	espconn_accept(&server_conn);
	os_printf("ESP9: 9P server listening on tcp 564\n");
}

static void ICACHE_FLASH_ATTR
wifi_event(System_Event_t *e)
{
	switch (e->event) {
	case EVENT_STAMODE_GOT_IP:
		os_printf("ESP9: station ip " IPSTR "\n",
			  IP2STR(&e->event_info.got_ip.ip));
		break;
	case EVENT_STAMODE_CONNECTED:
		os_printf("ESP9: station connected\n");
		break;
	case EVENT_STAMODE_DISCONNECTED:
		os_printf("ESP9: station disconnected reason %u\n",
			  e->event_info.disconnected.reason);
		break;
	default:
		break;
	}
}

static void ICACHE_FLASH_ATTR
init_done(void)
{
	struct softap_config ap;
	os_memset(&ap, 0, sizeof(ap));
	os_strcpy((char *)ap.ssid, "ESP9-3968");
	ap.ssid_len = 9;
	ap.channel = 1;
	ap.authmode = AUTH_OPEN;
	ap.max_connection = 1;
	ap.beacon_interval = 100;
	wifi_set_opmode(STATIONAP_MODE);
	wifi_softap_set_config(&ap);
	wifi_set_event_handler_cb(wifi_event);
#ifdef ESP9_PROVISION_SSID
	{
		struct station_config sc;
		os_memset(&sc, 0, sizeof(sc));
		os_strcpy((char *)sc.ssid, ESP9_PROVISION_SSID);
#ifdef ESP9_PROVISION_KEY
		os_strcpy((char *)sc.password, ESP9_PROVISION_KEY);
#endif
		wifi_station_set_config_current(&sc);
		wifi_station_connect();
		os_printf("ESP9: joining provisioned ssid %s\n",
			  ESP9_PROVISION_SSID);
	}
#endif
	start_server();
}

void ICACHE_FLASH_ATTR
user_init(void)
{
	uart_div_modify(0, UART_CLK_FREQ / 115200);
	os_printf("\nESP9: phaseA boot sdk=%s\n", system_get_sdk_version());
	system_init_done_cb(init_done);
}

uint32 ICACHE_FLASH_ATTR
user_rf_cal_sector_set(void)
{
	enum flash_size_map size_map = system_get_flash_size_map();
	uint32 rf_cal_sec = 0;

	switch (size_map) {
	case FLASH_SIZE_4M_MAP_256_256:
		rf_cal_sec = 128 - 5;
		break;
	case FLASH_SIZE_8M_MAP_512_512:
		rf_cal_sec = 256 - 5;
		break;
	case FLASH_SIZE_16M_MAP_512_512:
	case FLASH_SIZE_16M_MAP_1024_1024:
		rf_cal_sec = 512 - 5;
		break;
	case FLASH_SIZE_32M_MAP_512_512:
	case FLASH_SIZE_32M_MAP_1024_1024:
		rf_cal_sec = 1024 - 5;
		break;
	default:
		rf_cal_sec = 0;
		break;
	}
	return rf_cal_sec;
}
