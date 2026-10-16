#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vmdisplay_client.h"

int
main(void)
{
    struct vmdisplay_client client;
    struct vmdisplay *d;
    uint32_t *pixels;
    uint64_t serial;
    int i;

    d = calloc(1, sizeof *d);
    pixels = calloc(VMDISPLAY_WIDTH * VMDISPLAY_HEIGHT, sizeof *pixels);
    assert(d != NULL && pixels != NULL);
    memset(&client, 0, sizeof client);
    client.display = d;
    client.fd = -1;

    assert(vmdisplay_client_frame(&client, pixels,
           VMDISPLAY_WIDTH * VMDISPLAY_HEIGHT - 1, &serial) < 0);
    assert(vmdisplay_client_frame(&client, pixels,
           VMDISPLAY_WIDTH * VMDISPLAY_HEIGHT, &serial) == 0);
    d->pixels[0][0] = 0x123456;
    d->state[0] = VMDISPLAY_READY;
    d->slot_serial[0] = 1;
    d->latest_slot = 0;
    d->latest_serial = 1;
    assert(vmdisplay_client_frame(&client, pixels,
           VMDISPLAY_WIDTH * VMDISPLAY_HEIGHT, &serial) == 1);
    assert(serial == 1 && pixels[0] == 0x123456 &&
           d->state[0] == VMDISPLAY_FREE);
    assert(vmdisplay_client_frame(&client, pixels,
           VMDISPLAY_WIDTH * VMDISPLAY_HEIGHT, &serial) == 0);

    d->state[0] = VMDISPLAY_READY;
    d->slot_serial[0] = 2;
    d->pixels[0][0] = 0xabcdef;
    d->latest_serial = 2;
    assert(vmdisplay_client_frame(&client, pixels,
           VMDISPLAY_WIDTH * VMDISPLAY_HEIGHT, &serial) == 1);
    assert(serial == 2 && pixels[0] == 0xabcdef);
    d->state[1] = VMDISPLAY_READY;
    d->slot_serial[1] = 3;
    d->state[0] = VMDISPLAY_READY;
    d->slot_serial[0] = 4;
    d->pixels[0][0] = 0x445566;
    d->latest_serial = 4;
    assert(vmdisplay_client_frame(&client, pixels,
           VMDISPLAY_WIDTH * VMDISPLAY_HEIGHT, &serial) == 1);
    assert(serial == 4 && pixels[0] == 0x445566 &&
           d->state[1] == VMDISPLAY_FREE);
    assert(vmdisplay_client_input(&client, "key 65 0") < 0);
    assert(vmdisplay_client_input(&client, "key 65 0\n") == 0);
    assert(strcmp(d->input[0], "key 65 0\n") == 0);
    for(i = 1; i < VMDISPLAY_EVENTS; i++)
        assert(vmdisplay_client_input(&client, "move 1 2\n") == 0);
    assert(vmdisplay_client_input(&client, "down 1 2\n") < 0);
    assert(vmdisplay_client_input_wait(&client, "move 2 3\n", 0) == 0);
    d->input_tail = 1;
    assert(vmdisplay_client_input(&client, "down 1 2\n") == 0);

    free(pixels);
    free(d);
    puts("VMDISPLAY-CLIENT-TEST PASS");
    return 0;
}
