/* esp9 libpp test firmware (minimal, NOT the plan9ized OS).
 *
 * Runs behind the stock NONOS blob init: user_init exercises the
 * clean-room libpp (firmware/libpp) descriptor and pool code and
 * prints PASS/FAIL over UART0, which the CP2102 on /dev/ttyUSB1
 * carries. Build only - do not flash without approval (see
 * ../../../../re/B6 report and doc/DESIGN.md approval gate).
 */
#include "ets_sys.h"
#include "osapi.h"
#include "user_interface.h"

#include "pp_lldesc.h"
#include "pp_esf.h"

/* ROM/SDK functions used without a public header in this SDK. */
extern int os_printf_plus(const char *fmt, ...);
extern void uart_div_modify(uint8 uart_no, uint32 div);

static int failures;

static void
check(int cond, const char *name)
{
	if (cond) {
		os_printf("ESP9-LIBPP-TEST pass %s\n", name);
	} else {
		os_printf("ESP9-LIBPP-TEST FAIL %s\n", name);
		failures++;
	}
}

static void
run_libpp_tests(void)
{
	static pp_desc descs[4];
	static uint8_t descbufs[4 * 256];
	static pp_buf bufs[3];
	static uint8_t storage[3 * 512];
	pp_pool pool;
	pp_buf *a, *b;
	uint32_t i;
	int ok;

	pp_desc_chain(descs, 4, descbufs, 256);
	ok = 1;
	for (i = 0; i < 4; i++) {
		if (descs[i].buf != (uint32_t)(uintptr_t)(descbufs + i * 256))
			ok = 0;
		if (pp_desc_size(&descs[i]) != 256)
			ok = 0;
		if (descs[i].next !=
		    (uint32_t)(uintptr_t)(i < 3 ? &descs[i + 1] : NULL))
			ok = 0;
	}
	check(ok, "desc-chain");

	pp_desc_offer_rx(&descs[0], 1);
	check((descs[0].ctrl & PP_DESC_OWNER) != 0, "offer-owner");
	check(pp_desc_len(&descs[0]) == 256 - PP_RX_FCS_RESERVE,
	      "offer-len-fcs");
	check(!pp_desc_done(&descs[0]), "offer-not-done");
	pp_desc_reclaim(&descs[0]);
	check((descs[0].ctrl & PP_DESC_OWNER) == 0, "reclaim");

	pp_pool_init(&pool, bufs, storage, 3, 512);
	a = pp_buf_alloc(&pool);
	b = pp_buf_alloc(&pool);
	check(a != NULL && b != NULL && a != b, "pool-alloc");
	pp_buf_offer_rx(&pool, a, 1);
	check(pp_desc_len(&a->desc) == 512 - PP_RX_FCS_RESERVE,
	      "pool-offer-len");
	pp_buf_free(&pool, a);
	pp_buf_free(&pool, b);
	check(pool.nfree == 3, "pool-free");

	if (failures == 0)
		os_printf("ESP9-LIBPP-TEST ALL-PASS heap=%u\n",
			  system_get_free_heap_size());
	else
		os_printf("ESP9-LIBPP-TEST FAILURES=%d\n", failures);
}

void ICACHE_FLASH_ATTR
user_init(void)
{
	uart_div_modify(0, UART_CLK_FREQ / 115200);
	os_printf("\nESP9-LIBPP-TEST boot sdk=%s\n", system_get_sdk_version());
	run_libpp_tests();
}

/* Required by NONOS SDK 2.x: RF calibration sector selection. */
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
