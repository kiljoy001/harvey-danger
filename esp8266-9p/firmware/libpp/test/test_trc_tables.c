/* Host test for trc schedule tables (B31 stage 1).
 * Expected bytes are the blob's .data/.rodata section
 * dumps in re/inventory/b31 - ground truth independent
 * of this transcription path (objdump of libpp.a). */
#include <stdio.h>
#include <string.h>

#include "../trc_tables.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); failures++; } } while (0)

int
main(void)
{
	/* schedule sizes from the archive sections */
	CHECK(sizeof(rc11b_sched) == 48);
	CHECK(sizeof(rc11g_sched) == 132);
	CHECK(sizeof(rc11n_sched) == 144);
	CHECK(sizeof(basic_ofdm_sched) == 12);

	/* head entries, byte-exact vs b31 dump */
	CHECK(rc11b_sched[0][0] == 0x07 && rc11b_sched[0][9] == 0x40);
	CHECK(rc11g_sched[0][0] == 0x0c && rc11g_sched[0][9] == 0x72);
	CHECK(rc11n_sched[0][0] == 0x1f && rc11n_sched[0][2] == 0x16);
	CHECK(rc11n_sched[11][8] == 0x20 && rc11n_sched[11][9] == 0x00);
	CHECK(basic_ofdm_sched[0][0] == 0x0b &&
	      basic_ofdm_sched[0][4] == 0x0b);
	CHECK(user_rc_limit[0] == 0 && user_rc_limit[1] == 3 &&
	      user_rc_limit[11] == 0x0b);

	/* rate->schedule index, blob switch tables */
	CHECK(rc11b_rate2schedidx(0) == 3);
	CHECK(rc11b_rate2schedidx(3) == 0);
	CHECK(rc11b_rate2schedidx(4) == 0xFF);
	CHECK(rc11b_rate2schedidx(8) == 0xFF);
	CHECK(rc11g_rate2schedidx(0) == 0x0a);
	CHECK(rc11g_rate2schedidx(3) == 0xFF);
	CHECK(rc11g_rate2schedidx(8) == 1);
	CHECK(rc11g_rate2schedidx(15) == 6);
	CHECK(rc11g_rate2schedidx(16) == 0xFF);

	if (failures) {
		printf("test_trc_tables: %d failures\n", failures);
		return 1;
	}
	puts("test_trc_tables: all tests passed");
	return 0;
}
