/*
 * 9ring.h - Duplex lockless SPSC ring buffer for tagged 9P IPC
 */

#ifndef _9RING_H_
#define _9RING_H_

#ifndef nil
#include <stdint.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

#define P9_RING_SIZE    256
#define P9_RING_MASK    (P9_RING_SIZE - 1)
#define P9_CACHELINE    64

/* Fixed-size 9P submission descriptor */
typedef struct P9Slot P9Slot;
struct P9Slot {
	u16 tag;       /* 9P tag (0..65534, NOTAG=0xFFFF) */
	u8  type;      /* 9P message type (Tread, Twrite, Tflush, etc.) */
	u8  flags;     /* Descriptor flags (e.g. DMA, shared page) */
	u32 fid;       /* File identifier */
	u64 offset;    /* Byte offset for Tread / Twrite */
	u32 count;     /* Requested byte count */
	u64 buf_addr;  /* Buffer address or physical frame address */
};

/* Cache-aligned Submission Ring (T-Channel) */
typedef struct P9TRing P9TRing;
struct P9TRing {
	/* Cache line 0: Client producer index */
	volatile u32 head __attribute__((aligned(P9_CACHELINE)));

	/* Cache line 1: Server consumer index */
	volatile u32 tail __attribute__((aligned(P9_CACHELINE)));

	/* Cache line 2+: Descriptors */
	P9Slot slots[P9_RING_SIZE] __attribute__((aligned(P9_CACHELINE)));
};

/* Fixed-size 9P completion descriptor */
typedef struct P9Comp P9Comp;
struct P9Comp {
	u16 tag;       /* Matches submission tag */
	u8  type;      /* 9P message type (Rread, Rwrite, Rflush, Rerror) */
	u8  status;    /* Status code (0 = success, >0 = error) */
	u32 count;     /* Completed byte count */
	u64 result;    /* Qid path or error string offset */
};

/* Cache-aligned Completion Ring (R-Channel) */
typedef struct P9RRing P9RRing;
struct P9RRing {
	/* Cache line 0: Server producer index */
	volatile u32 head __attribute__((aligned(P9_CACHELINE)));

	/* Cache line 1: Client consumer index */
	volatile u32 tail __attribute__((aligned(P9_CACHELINE)));

	/* Cache line 2+: Completions */
	P9Comp comps[P9_RING_SIZE] __attribute__((aligned(P9_CACHELINE)));
};

/* Duplex 9P IPC channel */
typedef struct P9Channel P9Channel;
struct P9Channel {
	P9TRing t_ring;
	P9RRing r_ring;
};

/* 9vm's guest/host transport. The descriptor's buf_addr is a slot index,
 * never a host virtual pointer or an unchecked guest physical address. */
#define P9_HOST_MAGIC 0x39524853U
#define P9_HOST_VERSION 1
#define P9_HOST_SLOTS 8
#define P9_HOST_MSIZE 32768
#define P9_HOST_REGION_SIZE (2 * 1024 * 1024)
#define P9_HOST_DOORBELL_PORT 0x9009
typedef struct P9HostBuffer P9HostBuffer;
struct P9HostBuffer {
	u8 request[P9_HOST_MSIZE];
	u8 response[P9_HOST_MSIZE];
};
typedef struct P9HostShared P9HostShared;
struct P9HostShared {
	u32 magic;
	u32 version;
	P9Channel channel;
	P9HostBuffer buffer[P9_HOST_SLOTS];
};

/* T-Ring Operations */
static inline int
p9_tring_submit(P9TRing *ring, const P9Slot *slot)
{
	u32 head = __atomic_load_n(&ring->head, __ATOMIC_RELAXED);
	u32 tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);

	if ((head - tail) >= P9_RING_SIZE)
		return -1;

	ring->slots[head & P9_RING_MASK] = *slot;
	__atomic_store_n(&ring->head, head + 1, __ATOMIC_RELEASE);
	return 0;
}

static inline int
p9_tring_consume(P9TRing *ring, P9Slot *out)
{
	u32 tail = __atomic_load_n(&ring->tail, __ATOMIC_RELAXED);
	u32 head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);

	if (tail == head)
		return 0;

	*out = ring->slots[tail & P9_RING_MASK];
	__atomic_store_n(&ring->tail, tail + 1, __ATOMIC_RELEASE);
	return 1;
}

/* R-Ring Operations */
static inline int
p9_rring_complete(P9RRing *ring, const P9Comp *comp)
{
	u32 head = __atomic_load_n(&ring->head, __ATOMIC_RELAXED);
	u32 tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);

	if ((head - tail) >= P9_RING_SIZE)
		return -1;

	ring->comps[head & P9_RING_MASK] = *comp;
	__atomic_store_n(&ring->head, head + 1, __ATOMIC_RELEASE);
	return 0;
}

static inline int
p9_rring_poll(P9RRing *ring, P9Comp *out)
{
	u32 tail = __atomic_load_n(&ring->tail, __ATOMIC_RELAXED);
	u32 head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);

	if (tail == head)
		return 0;

	*out = ring->comps[tail & P9_RING_MASK];
	__atomic_store_n(&ring->tail, tail + 1, __ATOMIC_RELEASE);
	return 1;
}

#endif /* _9RING_H_ */
