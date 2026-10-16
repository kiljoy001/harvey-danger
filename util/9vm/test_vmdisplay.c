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
    uint32_t offset;

    d = calloc(1, sizeof *d);
    assert(d != NULL);
    memset(block, 0xa5, sizeof block);
    assert(command(d, "commit 0\n") < 0);
    assert(command(d, "begin 2\n") < 0);
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
    free(d);
    puts("VMDISPLAY-TEST PASS");
    return 0;
}
