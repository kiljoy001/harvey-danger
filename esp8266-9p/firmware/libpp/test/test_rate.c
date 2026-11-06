/* Host test for clean-room rate_control (B31). Expected
 * values were computed independently in Python from the
 * blob's arithmetic (extracted tables + getter formulas),
 * not from this implementation. */
#include <stdio.h>
#include <string.h>

#include "../rate_control.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); failures++; } } while (0)

int
main(void)
{
	/* before any setup, table is zero (BSS), as blob */
	CHECK(RC_GetAckRate(0) == 0);

	RC_SetBasicRate(0x15F, 0);
	/* spot-check records reconstructed from the blob
	 * store sequence: CCK head, OFDM, HT tail */
	CHECK(RC_GetAckRate(0) == 0 && RC_GetRtsRate(0) == 0);
	CHECK(RC_GetAckRate(2) == 2);
	CHECK(RC_GetAckRate(8) == 9 && RC_GetRtsRate(8) == 9);
	CHECK(RC_GetAckRate(10) == 10 && RC_GetRtsRate(11) == 11);
	CHECK(RC_GetAckRate(31) == 9);

	/* ACK times from the blob's 32-entry table */
	CHECK(RC_GetAckTime(0) == 314);
	CHECK(RC_GetAckTime(8) == 44);	/* ack index 9 */
	CHECK(RC_GetAckTime(10) == 48);	/* ack index 10 */

	/* CTS airtime, Python-computed vectors */
	CHECK(RC_GetCtsTime(0, 100, 0) == 206);
	CHECK(RC_GetCtsTime(6, 500, 10) == 844);
	CHECK(RC_GetCtsTime(11, 1500, 0) == 2040);
	CHECK(RC_GetCtsTime(20, 64, 0) == 68);
	CHECK(RC_GetCtsTime(3, 1500, 20) == 1313);

	/* BlockAck airtime, Python-computed vectors */
	CHECK(RC_GetBlockAckTime(0) == 203);
	CHECK(RC_GetBlockAckTime(9) == 48);
	CHECK(RC_GetBlockAckTime(24) == 84);
	CHECK(RC_GetBlockAckTime(11) == 84);
	CHECK(RC_GetBlockAckTime(31) == 48);

	/* partial-mask branch: only OFDM RTS bytes move,
	 * from the ERP flag, rest of table untouched */
	RC_SetBasicRate(0, 1);
	CHECK(RC_GetRtsRate(11) == 5);	/* 4|1 */
	CHECK(RC_GetRtsRate(8) == 7);	/* 4|3 */
	CHECK(RC_GetAckRate(11) == 11);	/* ack byte unchanged */
	RC_SetBasicRate(0, 0);
	CHECK(RC_GetRtsRate(11) == 1);
	CHECK(RC_GetRtsRate(8) == 3);

	/* full setup restores defaults after retune */
	RC_SetBasicRate(0xFFF, 0);
	CHECK(RC_GetRtsRate(11) == 11);

	if (failures) {
		printf("test_rate: %d failures\n", failures);
		return 1;
	}
	puts("test_rate: all tests passed");
	return 0;
}
