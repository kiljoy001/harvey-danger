/* Host tests for the /mem backing (esp9_mem + memwatch + arena). */
#include <stdio.h>
#include <string.h>

#include "esp9_mem.h"
#include "pp_arena.h"

static int failures;

static void
check(int cond, const char *name)
{
	if (cond)
		printf("pass %s\n", name);
	else {
		printf("FAIL %s\n", name);
		failures++;
	}
}

int
main(void)
{
	static uint8_t mem[2048];
	pp_arena arena;
	uint32_t len = 0;
	const char *dump;

	pp_arena_init(&arena, mem, sizeof(mem));
	esp9_mem_init(&arena);
	esp9_mem_event(0);
	esp9_mem_sample_at(100, 20000);
	(void)pp_arena_alloc(&arena, 512, 8);
	esp9_mem_event(1);
	esp9_mem_sample_at(200, 18000);
	esp9_mem_sample_at(300, 19500);

	dump = esp9_mem_read(&len);
	check(dump != NULL && len == strlen(dump), "dump-length");
	check(strstr(dump, "# memwatch samples 3 total 3") != NULL,
	      "dump-header");
	check(strstr(dump, "minfree 18000") != NULL, "dump-minfree");
	check(strstr(dump, "maxarena 512") != NULL, "dump-maxarena");
	/* seq tick ev free arena arenahigh q stack */
	check(strstr(dump, "1 200 1 18000 512 512 0 0\n") != NULL,
	      "dump-sample-line");
	check(strstr(dump, "2 300 1 19500 512 512 0 0\n") != NULL,
	      "dump-last-line");

	if (failures == 0)
		printf("test_esp9_mem: all tests passed\n");
	return failures != 0;
}
