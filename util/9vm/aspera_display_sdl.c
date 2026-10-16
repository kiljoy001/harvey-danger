/* SDL presentation for the 9vm display 9P backend. UI policy stays in Aspera. */
#define _POSIX_C_SOURCE 200809L
#include <SDL2/SDL.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "vmdisplay_client.h"

static int
clockpublish(const char *dir)
{
    static char previous[6];
    char path[PATH_MAX], temporary[PATH_MAX], value[8];
    time_t now;
    struct tm local;
    FILE *file;
    int written, closed;

    now = time(NULL);
    if(localtime_r(&now, &local) == NULL ||
       strftime(value, sizeof value, "%H:%M", &local) != 5)
        return -1;
    if(strcmp(value, previous) == 0)
        return 0;
    if(snprintf(path, sizeof path, "%s/clock", dir) >= (int)sizeof path ||
       snprintf(temporary, sizeof temporary, "%s/clock.tmp", dir) >= (int)sizeof temporary)
        return -1;
    file = fopen(temporary, "w");
    if(file == NULL)
        return -1;
    written = fprintf(file, "%s\n", value);
    closed = fclose(file);
    if(written < 0 || closed != 0 || rename(temporary, path) < 0){
        unlink(temporary);
        return -1;
    }
    memcpy(previous, value, sizeof previous);
    return 0;
}

int
main(int argc, char **argv)
{
    struct vmdisplay_client display;
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    SDL_Event event;
    uint32_t *argb = NULL;
    uint64_t smoke_start = 0, serial;
    uint64_t terminal_frame = 0;
    uint32_t deadline;
    uint32_t terminal_start = 0;
    size_t terminal_sent = 0;
    int running = 1, failed = 0, dragging = 0, smoke, stage = 0, changed;
    char line[96];
    size_t i;

    if(argc < 3 || argc > 4){
        fprintf(stderr, "usage: %s directory shm-name [--smoke]\n", argv[0]);
        return 2;
    }
    smoke = argc == 4 && strcmp(argv[3], "--smoke") == 0;
    if(argc == 4 && !smoke)
        return 2;
    if(vmdisplay_client_open(&display, argv[2], 35000) < 0){
        fprintf(stderr, "Aspera: cannot attach display %s: %s\n", argv[2], strerror(errno));
        failed = 1;
        goto done;
    }
    if(SDL_Init(SDL_INIT_VIDEO) < 0){
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        failed = 1;
        goto done;
    }
    SDL_StartTextInput();
    window = SDL_CreateWindow("Aspera on Aspera", SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, VMDISPLAY_WIDTH, VMDISPLAY_HEIGHT, 0);
    if(window == NULL)
        goto sdlfail;
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED |
                                 SDL_RENDERER_PRESENTVSYNC);
    if(renderer == NULL)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if(renderer == NULL)
        goto sdlfail;
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING,
                                VMDISPLAY_WIDTH, VMDISPLAY_HEIGHT);
    if(texture == NULL)
        goto sdlfail;
    argb = malloc(VMDISPLAY_FRAME_BYTES);
    if(argb == NULL){
        failed = 1;
        goto done;
    }
    deadline = SDL_GetTicks() + 35000;
    while(running){
        if(clockpublish(argv[1]) < 0){
            fprintf(stderr, "Aspera: cannot publish local clock\n");
            failed = 1;
            break;
        }
        while(SDL_PollEvent(&event)){
            line[0] = '\0';
            if(event.type == SDL_QUIT){
                snprintf(line, sizeof line, "quit\n");
                running = 0;
            }else if(event.type == SDL_MOUSEBUTTONDOWN &&
                     event.button.button == SDL_BUTTON_LEFT){
                dragging = 1;
                snprintf(line, sizeof line, "down %d %d\n", event.button.x, event.button.y);
            }else if(event.type == SDL_MOUSEBUTTONUP &&
                     event.button.button == SDL_BUTTON_LEFT){
                dragging = 0;
                snprintf(line, sizeof line, "up %d %d\n", event.button.x, event.button.y);
            }else if(event.type == SDL_MOUSEMOTION && dragging){
                snprintf(line, sizeof line, "move %d %d\n", event.motion.x, event.motion.y);
            }else if(event.type == SDL_KEYDOWN){
                if(event.key.keysym.sym == SDLK_ESCAPE){
                    snprintf(line, sizeof line, "quit\n");
                    running = 0;
                }else if((event.key.keysym.mod & (KMOD_ALT | KMOD_CTRL)) ||
                         event.key.keysym.sym == SDLK_BACKSPACE ||
                         event.key.keysym.sym == SDLK_RETURN){
                    int modifier = 0;
                    if(event.key.keysym.mod & KMOD_ALT) modifier |= 1;
                    if(event.key.keysym.mod & KMOD_CTRL) modifier |= 2;
                    if(event.key.keysym.mod & KMOD_SHIFT) modifier |= 4;
                    snprintf(line, sizeof line, "key %d %d\n", event.key.keysym.sym,
                             modifier);
                }
            }else if(event.type == SDL_TEXTINPUT){
                const unsigned char *p = (const unsigned char *)event.text.text;
                while(*p != 0){
                    if(*p >= 32 && *p <= 126){
                        snprintf(line, sizeof line, "key %u 0\n", *p);
                        if(vmdisplay_client_input_wait(&display, line, 1000) < 0){
                            fprintf(stderr, "Aspera: input queue full\n");
                            failed = 1;
                            running = 0;
                            break;
                        }
                    }
                    p++;
                }
                line[0] = '\0';
            }
            if(line[0] != '\0' &&
               vmdisplay_client_input_wait(&display, line, 1000) < 0){
                fprintf(stderr, "Aspera: input queue full\n");
                failed = 1;
                running = 0;
            }
        }
        changed = vmdisplay_client_frame(&display, argb,
                      VMDISPLAY_WIDTH * VMDISPLAY_HEIGHT, &serial);
        if(changed < 0){
            failed = 1;
            break;
        }
        if(changed > 0){
            for(i = 0; i < VMDISPLAY_WIDTH * VMDISPLAY_HEIGHT; i++)
                argb[i] |= 0xff000000U;
            SDL_UpdateTexture(texture, NULL, argb, VMDISPLAY_WIDTH * sizeof *argb);
        }
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        if(smoke && stage == 0 && display.seen > 0){
            smoke_start = display.seen;
            if(vmdisplay_client_input(&display, "down 100 95\n") < 0 ||
               vmdisplay_client_input(&display, "move 155 145\n") < 0 ||
               vmdisplay_client_input(&display, "up 155 145\n") < 0)
                failed = 1;
            stage = 1;
        }
        if(smoke && stage == 1 && display.seen >= smoke_start + 2){
            if(vmdisplay_client_input(&display, "down 400 14\n") < 0 ||
               vmdisplay_client_input(&display, "up 400 14\n") < 0)
                failed = 1;
            stage = 2;
        }
        if(smoke && stage == 2 && display.seen >= smoke_start + 3){
            if(vmdisplay_client_input(&display, "key 72 0\n") < 0 ||
               vmdisplay_client_input(&display, "key 105 0\n") < 0 ||
               vmdisplay_client_input(&display, "key 115 2\n") < 0)
                failed = 1;
            stage = 3;
        }
        if(smoke && stage == 3 && display.seen >= smoke_start + 6){
            if(vmdisplay_client_input(&display, "down 660 143\n") < 0 ||
               vmdisplay_client_input(&display, "up 660 143\n") < 0)
                failed = 1;
            stage = 4;
        }
        if(smoke && stage == 4 && display.seen >= smoke_start + 7){
            if(vmdisplay_client_input(&display, "down 20 10\n") < 0 ||
               vmdisplay_client_input(&display, "up 20 10\n") < 0)
                failed = 1;
            stage = 5;
        }
        if(smoke && stage == 5 && display.seen >= smoke_start + 8){
            if(vmdisplay_client_input(&display, "down 20 44\n") < 0 ||
               vmdisplay_client_input(&display, "up 20 44\n") < 0)
                failed = 1;
            stage = 6;
        }
        if(smoke && stage == 6 && display.seen >= smoke_start + 9){
            if(vmdisplay_client_input(&display, "key 33 0\n") < 0 ||
               vmdisplay_client_input(&display, "key 115 2\n") < 0)
                failed = 1;
            stage = 7;
        }
        if(smoke && stage == 7 && display.seen >= smoke_start + 11){
            if(vmdisplay_client_input(&display, "down 260 14\n") < 0 ||
               vmdisplay_client_input(&display, "up 260 14\n") < 0)
                failed = 1;
            terminal_frame = display.seen;
            stage = 8;
        }
        if(smoke && stage == 8 && display.seen > terminal_frame){
            const char *command_text = "echo VMTERMOK";
            if(command_text[terminal_sent] != '\0'){
                snprintf(line, sizeof line, "key %u 0\n",
                         (unsigned char)command_text[terminal_sent++]);
                if(vmdisplay_client_input_wait(&display, line, 1000) < 0)
                    failed = 1;
                terminal_frame = display.seen;
            }else{
                if(vmdisplay_client_input_wait(&display, "key 13 0\n", 1000) < 0)
                    failed = 1;
                terminal_start = SDL_GetTicks();
                stage = 9;
            }
        }
        if(smoke && stage == 9 && SDL_GetTicks() - terminal_start >= 750){
            if(vmdisplay_client_input(&display, "down 684 128\n") < 0 ||
               vmdisplay_client_input(&display, "up 684 128\n") < 0)
                failed = 1;
            terminal_frame = display.seen;
            stage = 10;
        }
        if(smoke && stage == 10 && display.seen > terminal_frame){
            if(vmdisplay_client_input(&display, "down 20 10\n") < 0 ||
               vmdisplay_client_input(&display, "up 20 10\n") < 0)
                failed = 1;
            terminal_frame = display.seen;
            stage = 11;
        }
        if(smoke && stage == 11 && display.seen > terminal_frame){
            if(vmdisplay_client_input(&display, "down 20 78\n") < 0 ||
               vmdisplay_client_input(&display, "up 20 78\n") < 0)
                failed = 1;
            terminal_frame = display.seen;
            terminal_sent = 0;
            stage = 12;
        }
        if(smoke && stage == 12 && display.seen > terminal_frame){
            const char *command_text = "echo REOPENOK";
            if(command_text[terminal_sent] != '\0'){
                snprintf(line, sizeof line, "key %u 0\n",
                         (unsigned char)command_text[terminal_sent++]);
                if(vmdisplay_client_input_wait(&display, line, 1000) < 0)
                    failed = 1;
                terminal_frame = display.seen;
            }else{
                if(vmdisplay_client_input_wait(&display, "key 13 0\n", 1000) < 0)
                    failed = 1;
                terminal_start = SDL_GetTicks();
                stage = 13;
            }
        }
        if(smoke && stage == 13 && SDL_GetTicks() - terminal_start >= 750){
            puts("ASPERA-DISPLAY PASS committed frames and input round trip");
            running = 0;
        }
        if(smoke && (int32_t)(SDL_GetTicks() - deadline) >= 0){
            fprintf(stderr, "ASPERA-DISPLAY FAIL timed out at frame %llu\n",
                    (unsigned long long)display.seen);
            failed = 1;
            running = 0;
        }
        if(failed)
            running = 0;
        SDL_Delay(16);
    }
    goto done;
sdlfail:
    fprintf(stderr, "Aspera SDL: %s\n", SDL_GetError());
    failed = 1;
done:
    if(display.display != NULL)
        vmdisplay_client_input(&display, "quit\n");
    vmdisplay_client_close(&display);
    free(argb);
    if(texture != NULL) SDL_DestroyTexture(texture);
    if(renderer != NULL) SDL_DestroyRenderer(renderer);
    if(window != NULL) SDL_DestroyWindow(window);
    SDL_StopTextInput();
    SDL_Quit();
    return failed ? 1 : 0;
}
