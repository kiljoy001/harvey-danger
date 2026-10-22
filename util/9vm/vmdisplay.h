#ifndef VMDISPLAY_H
#define VMDISPLAY_H

#include <stdint.h>

enum {
    VMDISPLAY_WIDTH = 1024,
    VMDISPLAY_HEIGHT = 768,
    VMDISPLAY_SLOTS = 2,
    VMDISPLAY_TILE_WIDTH = 32,
    VMDISPLAY_TILE_HEIGHT = 16,
    VMDISPLAY_EVENTS = 256,
    VMDISPLAY_EVENT_BYTES = 96,
    VMDISPLAY_FREE = 0,
    VMDISPLAY_WRITING = 1,
    VMDISPLAY_READY = 2,
    VMDISPLAY_READING = 3,
};

#define VMDISPLAY_MAGIC 0x56444c39U
#define VMDISPLAY_FRAME_BYTES (VMDISPLAY_WIDTH * VMDISPLAY_HEIGHT * 4)
/* Guest physical address where 9vm maps the display shared memory when the
 * guest mapping extension is enabled.  Kept below the virtio MMIO windows. */
#define VMDISPLAY_GUEST_ADDR 0xF8000000ULL
#define VMDISPLAY_GUEST_SIZE (((sizeof(struct vmdisplay)) + 4095U) & ~4095U)
/* written[] uses this sentinel while a slot holds a copy of the last frame. */
#define VMDISPLAY_DELTA_WRITES 0x80000000U

struct vmdisplay {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint64_t latest_serial;
    uint32_t latest_slot;
    uint32_t state[VMDISPLAY_SLOTS];
    uint32_t written[VMDISPLAY_SLOTS];
    uint64_t slot_serial[VMDISPLAY_SLOTS];
    uint32_t input_head;
    uint32_t input_tail;
    char input[VMDISPLAY_EVENTS][VMDISPLAY_EVENT_BYTES];
    uint32_t pixels[VMDISPLAY_SLOTS][VMDISPLAY_WIDTH * VMDISPLAY_HEIGHT];
};

#endif
