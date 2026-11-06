/* blob_support - see blob_support.h.
 *
 * Semantics notes (bring-up grade, not final):
 * - Critical sections disable Xtensa level-1 interrupts via PS
 *   with a nesting counter. This is the ISR-safety layer the
 *   substrate has owed since B11. vPortETSIntrLock maps to the
 *   same section for now; the real ETS lock also gates the WDEV
 *   NMI enable register, which matters once MAC ISRs are live.
 * - heap_caps wraps the NONOS heap. Blobs allocate esf and
 *   control buffers through it; it needs a real malloc/free.
 * - NVS is a RAM shim: values vanish at reboot. Persistence is
 *   a later milestone; init/scan/join do not depend on it.
 * - phy/log/event stubs return success. If init misbehaves,
 *   these are the first suspects, in this order: phy_open_rf,
 *   esp_phy_load_cal_and_init, wifi_timer_init.
 */
#if defined(ICACHE_FLASH) && defined(ESP9_NONOS_IMAGE)
#include "ets_sys.h"
extern void ets_timer_setfn(ETSTimer *t, ETSTimerFunc *fn, void *arg);
extern void ets_timer_disarm(ETSTimer *t);
extern void ets_timer_arm_new(void *ptimer, uint32_t ms, int repeat,
			      int ms_flag);
#endif
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern uint32_t system_get_time(void);
extern uint32_t system_get_free_heap_size(void);
extern void *pvPortMalloc(size_t sz, const char *file, int line);
extern void vPortFree(void *p, const char *file, int line);
extern int os_printf_plus(const char *fmt, ...);
extern unsigned long os_random(void);
extern void p9_yield(void);

/* ---- data globals the blobs expect ---- */
uint32_t g_log_level[32];
uint32_t g_log_mod[32];
uint8_t default_ssid[33];
uint8_t g_misc_nvs[1024];
uint8_t _g_esp_wifi_connect_open_router_when_pwd_is_set;
uint32_t _g_esp_wifi_ppt_task_stk_size = 6144;

/* bit_popcount and os_get_random come from the NONOS libs in
 * a blobtest image; do not redefine them here. */

/* ---- critical sections / ISR mask ---- */
static volatile uint32_t crit_nesting;
static uint32_t crit_saved_ps;

static uint32_t
rsr_ps(void)
{
	uint32_t v;
	__asm__ volatile("rsr %0, ps" : "=r"(v));
	return v;
}

static void
wsr_ps(uint32_t v)
{
	__asm__ volatile("wsr %0, ps\n\trsync" ::"r"(v));
}

void
vPortEnterCritical(void)
{
	uint32_t ps = rsr_ps();
	if (crit_nesting == 0) {
		crit_saved_ps = ps;
		wsr_ps(ps | 0xFu); /* INTLEVEL=15: mask level 1 */
	}
	crit_nesting++;
}

void
vPortExitCritical(void)
{
	if (crit_nesting > 0)
		crit_nesting--;
	if (crit_nesting == 0)
		wsr_ps(crit_saved_ps);
}

void vPortETSIntrLock(void) { vPortEnterCritical(); }
void vPortETSIntrUnlock(void) { vPortExitCritical(); }

void
_xt_isr_mask(uint32_t mask)
{
	uint32_t v;
	__asm__ volatile("rsr %0, intenable" : "=r"(v));
	v &= ~mask;
	__asm__ volatile("wsr %0, intenable" ::"r"(v));
}

void
_xt_isr_unmask(uint32_t mask)
{
	uint32_t v;
	__asm__ volatile("rsr %0, intenable" : "=r"(v));
	v |= mask;
	__asm__ volatile("wsr %0, intenable" ::"r"(v));
}

void
PendSV(int req)
{
	(void)req;
	p9_yield();
}

/* ---- heap_caps over the NONOS heap ---- */
void *
_heap_caps_malloc(size_t size, uint32_t caps, const char *file, int line)
{
	(void)caps;
	return pvPortMalloc(size, file, line);
}

void *
_heap_caps_zalloc(size_t size, uint32_t caps, const char *file, int line)
{
	void *p;
	(void)caps;
	p = pvPortMalloc(size, file, line);
	if (p != NULL)
		memset(p, 0, size);
	return p;
}

void
_heap_caps_free(void *p, const char *file, int line)
{
	vPortFree(p, file, line);
}

size_t
heap_caps_get_dram_free_size(uint32_t caps)
{
	(void)caps;
	return system_get_free_heap_size();
}

uint32_t
esp_get_free_heap_size(void)
{
	return system_get_free_heap_size();
}

/* ---- log / event / timer / misc esp_* ---- */
void
esp_log_write(int level, const char *tag, const char *fmt, ...)
{
	static char buf[256];
	va_list ap;

	(void)level;
	(void)tag;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	os_printf_plus("%s", buf);
}

uint32_t
esp_log_early_timestamp(void)
{
	return system_get_time() / 1000;
}

int64_t
esp_timer_get_time(void)
{
	return (int64_t)system_get_time();
}

int esp_event_send(void *event) { (void)event; return 0; }
uint32_t esp_random(void) { return (uint32_t)os_random(); }
int esp_phy_load_cal_and_init(void) { return 0; }
int esp_set_cpu_freq(void) { return 0; }
void esp_sleep_lock(void) {}
void esp_sleep_unlock(void) {}
int esp_now_deinit(void) { return 0; }
uint16_t esp_wifi_scan_get_ap_num_max(void) { return 16; }
int esp_wifi_try_rate_from_high(void) { return 0; }
void phy_open_rf(void) {}
void wifi_timer_init(void) {}
void pp_soft_wdt_restart(void) {}
void pp_soft_wdt_stop(void) {}
int misc_nvs_init(void) { return 0; }
int misc_nvs_restore(void) { return 0; }

/* Crypto stubs: replaced by the SDK's open wpa/crypto objects
 * before any WPA2/CCMP traffic. Open-AP scan/join milestones do
 * not call these; returning error is the honest placeholder. */
int ccmp_encrypt(void) { return -1; }
int ccmp_decrypt(void) { return -1; }
int omac1_aes_128(void) { return -1; }
int pbkdf2_sha1(void) { return -1; }

/* ---- RAM NVS shim ---- */
#define NVS_ENTRIES 32
#define NVS_KEYMAX 16
#define NVS_BLOBMAX 64

typedef struct {
	char key[NVS_KEYMAX];
	uint8_t used;
	uint8_t isblob;
	uint32_t val;
	uint8_t blob[NVS_BLOBMAX];
	size_t bloblen;
} NvsEntry;

static NvsEntry nvs_tab[NVS_ENTRIES];

static NvsEntry *
nvs_find(const char *key, int create)
{
	int i, freei = -1;

	for (i = 0; i < NVS_ENTRIES; i++) {
		if (nvs_tab[i].used && strcmp(nvs_tab[i].key, key) == 0)
			return &nvs_tab[i];
		if (!nvs_tab[i].used && freei < 0)
			freei = i;
	}
	if (!create || freei < 0)
		return NULL;
	memset(&nvs_tab[freei], 0, sizeof(nvs_tab[freei]));
	strncpy(nvs_tab[freei].key, key, NVS_KEYMAX - 1);
	nvs_tab[freei].used = 1;
	return &nvs_tab[freei];
}

int
nvs_open(const char *name, int mode, uint32_t *out_handle)
{
	(void)name;
	(void)mode;
	if (out_handle != NULL)
		*out_handle = 1;
	return 0;
}

void nvs_close(uint32_t handle) { (void)handle; }
int nvs_commit(uint32_t handle) { (void)handle; return 0; }

static int
nvs_get_int(uint32_t handle, const char *key, uint32_t *out)
{
	NvsEntry *e;
	(void)handle;
	e = nvs_find(key, 0);
	if (e == NULL || e->isblob)
		return 0x1102; /* ESP_ERR_NVS_NOT_FOUND */
	*out = e->val;
	return 0;
}

static int
nvs_set_int(uint32_t handle, const char *key, uint32_t v)
{
	NvsEntry *e;
	(void)handle;
	e = nvs_find(key, 1);
	if (e == NULL)
		return 0x1101; /* ESP_ERR_NVS_NO_FREE_PAGES-ish */
	e->val = v;
	e->isblob = 0;
	return 0;
}

int nvs_get_u8(uint32_t h, const char *k, uint8_t *o)
{
	uint32_t v;
	int r = nvs_get_int(h, k, &v);
	*o = (uint8_t)v;
	return r;
}
int nvs_get_i8(uint32_t h, const char *k, int8_t *o)
{
	uint32_t v;
	int r = nvs_get_int(h, k, &v);
	*o = (int8_t)v;
	return r;
}
int nvs_get_u16(uint32_t h, const char *k, uint16_t *o)
{
	uint32_t v;
	int r = nvs_get_int(h, k, &v);
	*o = (uint16_t)v;
	return r;
}
int nvs_set_u8(uint32_t h, const char *k, uint8_t v)
{
	return nvs_set_int(h, k, v);
}

/* ---- newlib reent/syscall layer (blobs call printf family) ---- */
#include <sys/reent.h>
#include <sys/stat.h>

static struct _reent bs_reent;
static uint8_t bs_heap[4096];
static uint32_t bs_heap_used;

struct _reent *__getreent(void) { return &bs_reent; }

int pthread_setcancelstate(int state, int *old)
{
	(void)state;
	if (old != NULL)
		*old = 0;
	return 0;
}

void *
_sbrk_r(struct _reent *r, ptrdiff_t incr)
{
	uint8_t *prev;
	(void)r;
	if (incr < 0 || bs_heap_used + (uint32_t)incr > sizeof(bs_heap))
		return (void *)-1;
	prev = bs_heap + bs_heap_used;
	bs_heap_used += (uint32_t)incr;
	return prev;
}

int _close_r(struct _reent *r, int fd) { (void)r; (void)fd; return 0; }
int _read_r(struct _reent *r, int fd, void *buf, size_t n)
{
	(void)r; (void)fd; (void)buf; (void)n;
	return 0;
}
int _write_r(struct _reent *r, int fd, const void *buf, size_t n)
{
	(void)r; (void)fd; (void)buf;
	return (int)n;
}
int _lseek_r(struct _reent *r, int fd, int off, int whence)
{
	(void)r; (void)fd; (void)off; (void)whence;
	return 0;
}
int _fstat_r(struct _reent *r, int fd, struct stat *st)
{
	(void)r; (void)fd;
	st->st_mode = S_IFCHR;
	return 0;
}

#if defined(ICACHE_FLASH) && defined(ESP9_NONOS_IMAGE)
/* ---- os_timer over NONOS ets timers (blobtest images) ---- */

void os_timer_setfn(void *ptimer, void *fn, void *arg)
{
	ets_timer_setfn((ETSTimer *)ptimer, (ETSTimerFunc *)fn, arg);
}
void os_timer_disarm(void *ptimer) { ets_timer_disarm((ETSTimer *)ptimer); }
void os_timer_arm(void *ptimer, uint32_t ms, int repeat)
{
	ets_timer_arm_new(ptimer, ms, repeat, 1);
}
void os_timer_arm_us(void *ptimer, uint32_t us, int repeat)
{
	ets_timer_arm_new(ptimer, us, repeat, 0);
}
void ets_timer_arm_us(void *ptimer, uint32_t us, int repeat)
{
	ets_timer_arm_new(ptimer, us, repeat, 0);
}
void ets_timer_proc(void) {}
#endif
int nvs_set_i8(uint32_t h, const char *k, int8_t v)
{
	return nvs_set_int(h, k, (uint32_t)v);
}
int nvs_set_u16(uint32_t h, const char *k, uint16_t v)
{
	return nvs_set_int(h, k, v);
}

int
nvs_set_blob(uint32_t handle, const char *key, const void *value,
	     size_t length)
{
	NvsEntry *e;
	(void)handle;
	e = nvs_find(key, 1);
	if (e == NULL || length > NVS_BLOBMAX)
		return 0x1101;
	memcpy(e->blob, value, length);
	e->bloblen = length;
	e->isblob = 1;
	return 0;
}

int
nvs_get_blob(uint32_t handle, const char *key, void *out, size_t *length)
{
	NvsEntry *e;
	(void)handle;
	e = nvs_find(key, 0);
	if (e == NULL || !e->isblob)
		return 0x1102;
	if (out != NULL) {
		if (*length < e->bloblen)
			return 0x110a; /* ESP_ERR_NVS_INVALID_LENGTH */
		memcpy(out, e->blob, e->bloblen);
	}
	*length = e->bloblen;
	return 0;
}
