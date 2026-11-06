/* pp_arena - Goose-style data stack for the plan9ized firmware.
 *
 * One statically sized bump arena per proc/server (doc/PLAN9-OS.md
 * section 4.5): no heap, nothing moves, scope exit frees by
 * rewinding to a mark in one store. At most one growable buffer,
 * always the topmost allocation, so references into it survive
 * growth. Allocation failure returns NULL; it never relocates.
 *
 * DMA memory is NOT allocated here (see pp_esf.h): hardware-owned
 * buffers must outlive any arena rewind.
 */
#ifndef PP_ARENA_H
#define PP_ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint8_t *base;
	uint32_t cap;
	uint32_t used;
	uint32_t high; /* high-water mark of used */
	uint8_t *grow; /* current growable buffer, or NULL */
	uint32_t growsize;
} pp_arena;

void pp_arena_init(pp_arena *a, void *mem, uint32_t cap);
uint32_t pp_arena_mark(const pp_arena *a);
void pp_arena_release(pp_arena *a, uint32_t mark);
void pp_arena_reset(pp_arena *a);
void *pp_arena_alloc(pp_arena *a, uint32_t size, uint32_t align);
/* Allocate the arena's single growable buffer (at the top). */
void *pp_arena_growable(pp_arena *a, uint32_t size);
/* Grow the growable buffer in place. Returns the same pointer,
 * or NULL if it is not the growable buffer or does not fit. */
void *pp_arena_grow(pp_arena *a, void *p, uint32_t newsize);
uint32_t pp_arena_highwater(const pp_arena *a);

#endif
