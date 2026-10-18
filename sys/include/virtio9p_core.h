/* Host-testable validation shared by the virtio-9P kernel driver. */
#ifndef _VIRTIO9P_CORE_H_
#define _VIRTIO9P_CORE_H_

#if !defined(nil) && !defined(_9RING_H_)
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif

enum {
	V9pminmsg = 7,
	V9pdescnext = 1,
};

static inline u32
v9pget32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 |
		(u32)p[3] << 24;
}

static inline int
v9pmessagevalid(const u8 *message, u32 length, u32 capacity)
{
	if(message == 0 || length < V9pminmsg || length > capacity)
		return 0;
	return v9pget32(message) == length;
}

static inline u32
v9pqueuesize(u32 offered, u32 limit)
{
	u32 size;

	if(offered > limit)
		offered = limit;
	size = 1;
	while(size <= offered / 2)
		size *= 2;
	return size >= 2 ? size : 0;
}

static inline int
v9pindexvalid(u32 index, u32 size)
{
	return size >= 2 && (size & (size - 1)) == 0 && index < size;
}

static inline int
v9pchain2valid(u32 head, u16 firstflags, u16 next, u16 secondflags,
	u32 size)
{
	return v9pindexvalid(head, size) && (firstflags & V9pdescnext) != 0 &&
		v9pindexvalid(next, size) && next != head &&
		(secondflags & V9pdescnext) == 0;
}

#endif
