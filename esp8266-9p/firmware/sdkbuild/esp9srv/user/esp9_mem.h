/* esp9_mem - /mem backing for the Phase A firmware.
 *
 * A NONOS timer samples the memwatch ring every 100 ms: system
 * heap free, the 9P server arena used/high, and the current
 * event marker (0 idle, 1 client connected). The timer callback
 * only appends a sample; rendering happens on read. q and stack
 * columns are 0 here: Phase A has no p9 procs (the kerntest
 * firmware fills them on the Phase C substrate).
 */
#ifndef ESP9_MEM_H
#define ESP9_MEM_H

#include <stdint.h>

#include "pp_arena.h"

void esp9_mem_init(pp_arena *arena);
void esp9_mem_start(pp_arena *arena);
void esp9_mem_event(uint32_t ev);
void esp9_mem_sample_at(uint32_t tick_ms, uint32_t free_heap);
const char *esp9_mem_read(uint32_t *len);

#endif
