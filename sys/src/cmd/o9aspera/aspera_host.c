// aspera_host.c - Native Interactive Host Driver for Aspera Window System
// Opens a 60 FPS graphical window on the host desktop (X11 / Wayland via SDL2)
// Captures live mouse, clicks, dragging, resizing, scrollbars, wheel, and keyboard keystrokes.

#include "aspera_engine.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[aspera] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow(
        "Aspera - Window System [Interactive]",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        ASPERA_SCREEN_W, ASPERA_SCREEN_H,
        SDL_WINDOW_SHOWN
    );
    if (!win) {
        fprintf(stderr, "[aspera] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *rend = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!rend) {
        rend = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!rend) {
        fprintf(stderr, "[aspera] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    SDL_Texture *tex = SDL_CreateTexture(
        rend,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        ASPERA_SCREEN_W, ASPERA_SCREEN_H
    );
    if (!tex) {
        fprintf(stderr, "[aspera] SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(rend);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    AsperaDesktop desk;
    aspera_init(&desk);

    uint8_t blank_data[1] = {0};
    uint8_t blank_mask[1] = {0};
    SDL_Cursor *blank_cursor = SDL_CreateCursor(blank_data, blank_mask, 8, 1, 0, 0);
    if (blank_cursor) {
        SDL_SetCursor(blank_cursor);
    }
    SDL_ShowCursor(SDL_DISABLE);

    SDL_StartTextInput();
    bool running = true;
    uint32_t last_time = SDL_GetTicks();
    int frames = 0;
    int fps = 60;
    char title_buf[256];

    printf("==================================================\n");
    printf("  Aspera Window System [LIVE]\n");
    printf("  Interactive Session Online (1024x768 @ 60 FPS)\n");
    printf("--------------------------------------------------\n");
    printf("  Mouse Controls:\n");
    printf("    Click Titlebar:     Drag window (with magnetic snap & edge flip)\n");
    printf("    Borders / Corners:  8-way live window resize\n");
    printf("    Scrollbars:         Drag thumb / Click track to page / Mouse wheel\n");
    printf("    Middle Click (Rio): Jump scrollbar thumb directly\n");
    printf("    Button [o]:         Pin window (sticky across workspaces)\n");
    printf("    Button [^]:         Accordion shade (collapses to titlebar)\n");
    printf("    Button [+]:         Maximize / Restore window\n");
    printf("    Button [x]:         Close window\n");
    printf("    Right-Click Title:  Window context menu\n");
    printf("    Right-Click Root:   Desktop root context menu\n");
    printf("    Click 2D Pager:     Switch virtual workspace\n");
    printf("  Keyboard Controls:\n");
    printf("    Type into Terminal: Interactive shell (help, theme, desk, etc.)\n");
    printf("    PageUp / PageDown:  Scroll terminal buffer\n");
    printf("    Alt + 1..4:         Jump to workspace 0..3\n");
    printf("    Alt + Tab:          Cycle window focus\n");
    printf("    Alt + T:            Cycle themes (E16, Win95, NeXT, Plan9)\n");
    printf("    Alt + S:            Toggle shade on active window\n");
    printf("    Alt + P:            Toggle sticky pin on active window\n");
    printf("    Alt + Enter:        Spawn new Terminal\n");
    printf("==================================================\n");

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_ENTER ||
                    ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    SDL_ShowCursor(SDL_DISABLE);
                }
                break;
            case SDL_MOUSEMOTION:
                aspera_handle_mouse_move(&desk, ev.motion.x, ev.motion.y);
                break;
            case SDL_MOUSEBUTTONDOWN:
                aspera_handle_mouse_down(&desk, ev.button.x, ev.button.y, ev.button.button);
                break;
            case SDL_MOUSEBUTTONUP:
                aspera_handle_mouse_up(&desk, ev.button.x, ev.button.y, ev.button.button);
                break;
            case SDL_MOUSEWHEEL:
                aspera_handle_mouse_wheel(&desk, ev.wheel.y);
                break;
            case SDL_KEYDOWN: {
                bool alt = (ev.key.keysym.mod & KMOD_ALT) != 0;
                bool ctrl = (ev.key.keysym.mod & KMOD_CTRL) != 0;
                int key = ev.key.keysym.sym;
                // Pass ch=0: SDL_TEXTINPUT handles printable characters without duplication
                aspera_handle_key_down(&desk, key, 0, alt, ctrl);
                break;
            }
            case SDL_TEXTINPUT:
                aspera_handle_text_input(&desk, ev.text.text);
                break;
            }
        }

        // Render scene
        aspera_render(&desk);

        // Upload to texture
        SDL_UpdateTexture(tex, NULL, desk.pixels, ASPERA_SCREEN_W * sizeof(uint32_t));
        SDL_RenderClear(rend);
        SDL_RenderCopy(rend, tex, NULL, NULL);
        SDL_RenderPresent(rend);

        frames++;
        uint32_t now = SDL_GetTicks();
        if (now - last_time >= 1000) {
            fps = frames;
            frames = 0;
            last_time = now;
            snprintf(title_buf, sizeof(title_buf),
                     "Aspera - Window System [%s | Desk (%d,%d) | %d FPS]",
                     desk.theme.name, desk.cur_x, desk.cur_y, fps);
            SDL_SetWindowTitle(win, title_buf);
        }

        SDL_Delay(16); // ~60 FPS
    }

    SDL_StopTextInput();
    if (blank_cursor) {
        SDL_FreeCursor(blank_cursor);
    }
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(rend);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
