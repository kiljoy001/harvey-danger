/* Transport-neutral 9vm display consumer. No SDL or network dependencies. */
#define _POSIX_C_SOURCE 200809L
#include "vmdisplay_client.h"
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void
pause_ms(int ms)
{
    struct timespec pause;

    pause.tv_sec = ms / 1000;
    pause.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&pause, NULL);
}

int
vmdisplay_client_open(struct vmdisplay_client *client, const char *name,
                      int timeout_ms)
{
    struct vmdisplay *display;
    struct stat st;
    int elapsed;

    if(client == NULL || name == NULL || timeout_ms < 0)
        return -1;
    memset(client, 0, sizeof *client);
    client->fd = -1;
    display = NULL;
    for(elapsed = 0; elapsed <= timeout_ms; elapsed += 50){
        if(client->fd < 0)
            client->fd = shm_open(name, O_RDWR, 0);
        if(client->fd >= 0 && fstat(client->fd, &st) == 0 &&
           st.st_size >= (off_t)sizeof *display){
            display = mmap(NULL, sizeof *display, PROT_READ | PROT_WRITE,
                           MAP_SHARED, client->fd, 0);
            if(display == MAP_FAILED)
                break;
            client->display = display;
        }
        if(client->display != NULL &&
           __atomic_load_n(&client->display->magic, __ATOMIC_ACQUIRE) ==
               VMDISPLAY_MAGIC && client->display->version == 1 &&
           client->display->width == VMDISPLAY_WIDTH &&
           client->display->height == VMDISPLAY_HEIGHT)
            return 0;
        if(client->display != NULL){
            munmap(client->display, sizeof *client->display);
            client->display = NULL;
        }
        pause_ms(50);
    }
    vmdisplay_client_close(client);
    return -1;
}

void
vmdisplay_client_close(struct vmdisplay_client *client)
{
    if(client == NULL)
        return;
    if(client->display != NULL){
        munmap(client->display, sizeof *client->display);
        client->display = NULL;
    }
    if(client->fd >= 0){
        close(client->fd);
        client->fd = -1;
    }
}

int
vmdisplay_client_frame(struct vmdisplay_client *client, uint32_t *pixels,
                       size_t pixel_count, uint64_t *serial)
{
    struct vmdisplay *d;
    uint64_t latest;
    uint32_t slot, expected;
    int copied;

    if(client == NULL || client->display == NULL || pixels == NULL ||
       pixel_count < VMDISPLAY_WIDTH * VMDISPLAY_HEIGHT)
        return -1;
    d = client->display;
    copied = 0;
    latest = __atomic_load_n(&d->latest_serial, __ATOMIC_ACQUIRE);
    slot = __atomic_load_n(&d->latest_slot, __ATOMIC_ACQUIRE);
    if(latest > client->seen && slot < VMDISPLAY_SLOTS &&
       __atomic_load_n(&d->slot_serial[slot], __ATOMIC_ACQUIRE) == latest){
        expected = VMDISPLAY_READY;
        if(__atomic_compare_exchange_n(&d->state[slot], &expected,
               VMDISPLAY_READING, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)){
            memcpy(pixels, d->pixels[slot], VMDISPLAY_FRAME_BYTES);
            client->seen = latest;
            if(serial != NULL)
                *serial = latest;
            copied = 1;
            __atomic_store_n(&d->state[slot], VMDISPLAY_FREE, __ATOMIC_RELEASE);
        }
    }
    for(slot = 0; slot < VMDISPLAY_SLOTS; slot++){
        if(__atomic_load_n(&d->slot_serial[slot], __ATOMIC_ACQUIRE) <
           client->seen){
            expected = VMDISPLAY_READY;
            __atomic_compare_exchange_n(&d->state[slot], &expected,
                VMDISPLAY_FREE, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        }
    }
    return copied;
}

int
vmdisplay_client_input(struct vmdisplay_client *client, const char *record)
{
    struct vmdisplay *d;
    uint32_t head, tail;
    size_t length;

    if(client == NULL || client->display == NULL || record == NULL)
        return -1;
    length = strlen(record);
    if(length == 0 || length >= VMDISPLAY_EVENT_BYTES ||
       record[length - 1] != '\n')
        return -1;
    d = client->display;
    head = __atomic_load_n(&d->input_head, __ATOMIC_RELAXED);
    tail = __atomic_load_n(&d->input_tail, __ATOMIC_ACQUIRE);
    if(head - tail >= VMDISPLAY_EVENTS)
        return -1;
    memcpy(d->input[head % VMDISPLAY_EVENTS], record, length + 1);
    __atomic_store_n(&d->input_head, head + 1, __ATOMIC_RELEASE);
    return 0;
}

int
vmdisplay_client_input_wait(struct vmdisplay_client *client,
                            const char *record, int timeout_ms)
{
    int elapsed;

    if(record == NULL || timeout_ms < 0)
        return -1;
    if(strncmp(record, "move ", 5) == 0){
        vmdisplay_client_input(client, record);
        return 0;
    }
    for(elapsed = 0; elapsed <= timeout_ms; elapsed += 10){
        if(vmdisplay_client_input(client, record) == 0)
            return 0;
        pause_ms(10);
    }
    return -1;
}
