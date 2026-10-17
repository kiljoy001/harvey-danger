/* Exercise the 9vm display control state machine independently of a guest. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hostfs.c"

static int
command(struct vmdisplay *d, const char *text)
{
    return display_control(d, (const uint8_t *)text, strlen(text), 0);
}

int
main(void)
{
    struct vmdisplay *d;
    uint8_t block[4096];
    uint8_t tile[8 + VMDISPLAY_TILE_WIDTH * VMDISPLAY_TILE_HEIGHT * 4];
    uint32_t offset;

    d = calloc(1, sizeof *d);
    assert(d != NULL);
    memset(block, 0xa5, sizeof block);
    assert(command(d, "commit 0\n") < 0);
    assert(command(d, "begin 2\n") < 0);
    assert(command(d, "begin-delta 0\n") < 0);
    assert(command(d, "begin 0 extra\n") < 0);
    assert(command(d, "begin 0\n") > 0);
    assert(command(d, "begin 0\n") < 0);
    assert(command(d, "commit 0\n") < 0);
    assert(display_write(d, 0, block, sizeof block, 1) < 0);
    assert(display_write(d, 0, block, sizeof block, 0) == sizeof block);
    assert(display_write(d, 0, block, sizeof block, 0) < 0);
    assert(command(d, "abort 0\n") > 0);
    assert(d->state[0] == VMDISPLAY_FREE);
    assert(d->written[0] == 0);
    assert(command(d, "begin 0\n") > 0);
    for(offset = 0; offset < VMDISPLAY_FRAME_BYTES; offset += sizeof block)
        assert(display_write(d, 0, block, sizeof block, offset) == sizeof block);
    assert(display_write(d, 0, block, 1, VMDISPLAY_FRAME_BYTES) < 0);
    assert(command(d, "commit 0\n") > 0);
    assert(d->state[0] == VMDISPLAY_READY);
    assert(d->slot_serial[0] == 1 && d->latest_serial == 1);
    assert(display_write(d, 0, block, sizeof block, 0) < 0);
    assert(command(d, "abort 0\n") < 0);
    assert(command(d, "begin-delta 1\n") > 0);
    assert(d->written[1] == VMDISPLAY_DELTA_WRITES);
    assert(memcmp(d->pixels[1], d->pixels[0], VMDISPLAY_FRAME_BYTES) == 0);
    memset(tile, 0x5a, sizeof tile);
    tile[0] = 32;
    tile[1] = 0;
    tile[2] = 16;
    tile[3] = 0;
    tile[4] = VMDISPLAY_TILE_WIDTH;
    tile[5] = 0;
    tile[6] = VMDISPLAY_TILE_HEIGHT;
    tile[7] = 0;
    assert(display_write(d, 1, tile, sizeof tile - 1, 0) < 0);
    assert(display_write(d, 1, tile, sizeof tile, 0) == sizeof tile);
    assert(display_write(d, 1, tile, sizeof tile, sizeof tile) == sizeof tile);
    assert(d->pixels[1][16 * VMDISPLAY_WIDTH + 32] == 0x5a5a5a5a);
    assert(d->pixels[1][15 * VMDISPLAY_WIDTH + 32] == 0xa5a5a5a5);
    assert(d->pixels[1][16 * VMDISPLAY_WIDTH + 31] == 0xa5a5a5a5);
    tile[0] = 33;
    assert(display_write(d, 1, tile, sizeof tile, 0) < 0);
    tile[0] = 32;
    tile[2] = 0xff;
    tile[3] = 0xff;
    assert(display_write(d, 1, tile, sizeof tile, 0) < 0);
    assert(command(d, "commit 1\n") > 0);
    assert(d->slot_serial[1] == 2 && d->latest_serial == 2);
    assert(d->pixels[1][16 * VMDISPLAY_WIDTH + 32] == 0x5a5a5a5a);
    assert(d->pixels[1][0] == d->pixels[0][0]);
    d->state[0] = VMDISPLAY_FREE;
    assert(command(d, "begin-delta 0\n") > 0);
    assert(memcmp(d->pixels[0], d->pixels[1], VMDISPLAY_FRAME_BYTES) == 0);
    assert(command(d, "abort 0\n") > 0);
    free(d);
    puts("VMDISPLAY-TEST PASS");
    return 0;
}
