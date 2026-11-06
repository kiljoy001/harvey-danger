/* pp_arena - clean-room data-stack allocator; see pp_arena.h. */
#include "pp_arena.h"

static uint32_t
align_up(uint32_t v, uint32_t align)
{
	if (align < 2)
		return v;
	return (v + align - 1) & ~(align - 1);
}

void
pp_arena_init(pp_arena *a, void *mem, uint32_t cap)
{
	a->base = (uint8_t *)mem;
	a->cap = cap;
	a->used = 0;
	a->high = 0;
	a->grow = NULL;
	a->growsize = 0;
}

uint32_t
pp_arena_mark(const pp_arena *a)
{
	return a->used;
}

void
pp_arena_release(pp_arena *a, uint32_t mark)
{
	if (mark > a->used)
		return;
	if (a->grow != NULL && a->grow >= a->base + mark) {
		a->grow = NULL;
		a->growsize = 0;
	}
	a->used = mark;
}

void
pp_arena_reset(pp_arena *a)
{
	a->used = 0;
	a->grow = NULL;
	a->growsize = 0;
}

void *
pp_arena_alloc(pp_arena *a, uint32_t size, uint32_t align)
{
	uint32_t at = align_up(a->used, align);

	if (at + size > a->cap)
		return NULL;
	a->used = at + size;
	if (a->used > a->high)
		a->high = a->used;
	return a->base + at;
}

void *
pp_arena_growable(pp_arena *a, uint32_t size)
{
	void *p;

	if (a->grow != NULL)
		return NULL;
	p = pp_arena_alloc(a, size, 8);
	if (p == NULL)
		return NULL;
	a->grow = (uint8_t *)p;
	a->growsize = size;
	return p;
}

void *
pp_arena_grow(pp_arena *a, void *p, uint32_t newsize)
{
	uint32_t at;

	if (p == NULL || (uint8_t *)p != a->grow)
		return NULL;
	at = (uint32_t)(a->grow - a->base);
	if (at + newsize > a->cap)
		return NULL;
	a->used = at + newsize;
	a->growsize = newsize;
	if (a->used > a->high)
		a->high = a->used;
	return p;
}

uint32_t
pp_arena_highwater(const pp_arena *a)
{
	return a->high;
}
