// aspera_engine.c - Enlightenment-inspired Window Manager Engine
// Implements part-tree hit testing, live dragging, magnetic snapping,
// edge desktop flipping, 2D workspace pager, and interactive terminal.

#include "aspera_engine.h"
#include "font8x14.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

static const ThemeMetrics themes[THEME_COUNT] = {
    [THEME_E16] = {
        .name = "e16-slate",
        .border_thickness = 4,
        .titlebar_height = 24,
        .btn_size = 14,
        .btn_spacing = 3,
        .bg_desktop = 0x0c1014,
        .title_active_bg = 0x2e4054,
        .title_active_fg = 0xf0c040,
        .title_inactive_bg = 0x182028,
        .title_inactive_fg = 0x8898a8,
        .frame_active = 0x486480,
        .frame_inactive = 0x222c36,
        .bevel_light = 0x6888a8,
        .bevel_dark = 0x101820,
        .content_bg = 0x080a0e,
        .content_fg = 0xd0e0f0,
        .btn_close_col = 0xcc3322,
        .btn_max_col = 0x22aa44,
        .btn_shade_col = 0xee8800,
        .btn_stick_col = 0x2288cc,
    },
    [THEME_WIN95] = {
        .name = "win95-classic",
        .border_thickness = 4,
        .titlebar_height = 20,
        .btn_size = 14,
        .btn_spacing = 2,
        .bg_desktop = 0x008080,
        .title_active_bg = 0x000080,
        .title_active_fg = 0xffffff,
        .title_inactive_bg = 0x808080,
        .title_inactive_fg = 0xc0c0c0,
        .frame_active = 0xc0c0c0,
        .frame_inactive = 0xc0c0c0,
        .bevel_light = 0xffffff,
        .bevel_dark = 0x404040,
        .content_bg = 0xffffff,
        .content_fg = 0x000000,
        .btn_close_col = 0xc0c0c0,
        .btn_max_col = 0xc0c0c0,
        .btn_shade_col = 0xc0c0c0,
        .btn_stick_col = 0xc0c0c0,
    },
    [THEME_NEXT] = {
        .name = "nextstep-dark",
        .border_thickness = 3,
        .titlebar_height = 22,
        .btn_size = 14,
        .btn_spacing = 2,
        .bg_desktop = 0x202224,
        .title_active_bg = 0x000000,
        .title_active_fg = 0xffffff,
        .title_inactive_bg = 0x383a3c,
        .title_inactive_fg = 0xaaaaaa,
        .frame_active = 0x505458,
        .frame_inactive = 0x282a2c,
        .bevel_light = 0x707478,
        .bevel_dark = 0x141618,
        .content_bg = 0x181a1c,
        .content_fg = 0xe0e4e8,
        .btn_close_col = 0x303030,
        .btn_max_col = 0x303030,
        .btn_shade_col = 0x303030,
        .btn_stick_col = 0x303030,
    },
    [THEME_PLAN9] = {
        .name = "plan9-rio",
        .border_thickness = 4,        // Authentic Selborder = 4
        .titlebar_height = 18,        // Clean Plan 9 header tag
        .btn_size = 12,
        .btn_spacing = 3,
        .bg_desktop = 0x777777,       // Authentic Rio solid grey (0x777777)
        .title_active_bg = 0x55aaaa,  // DGreygreen
        .title_active_fg = 0x000000,
        .title_inactive_bg = 0x9eeeee,// DPalegreygreen
        .title_inactive_fg = 0x333333,
        .frame_active = 0x55aaaa,     // DGreygreen
        .frame_inactive = 0x9eeeee,   // DPalegreygreen
        .bevel_light = 0x55aaaa,      // Flat border
        .bevel_dark = 0x55aaaa,       // Flat border
        .content_bg = 0xffffff,       // Pure white window interior
        .content_fg = 0x000000,       // Crisp black text
        .btn_close_col = 0x55aaaa,
        .btn_max_col = 0x55aaaa,
        .btn_shade_col = 0x55aaaa,
        .btn_stick_col = 0x55aaaa,
    }
};

static inline int win_visible_h(const Window *w, const ThemeMetrics *t) {
    if (w->is_shaded) {
        return t->titlebar_height + t->border_thickness * 2;
    }
    return w->h;
}

// Low-level software rasterizer helpers
static void draw_fill_rect(uint32_t *pixels, int x0, int y0, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    int x1 = x0 + w;
    int y1 = y0 + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > ASPERA_SCREEN_W) x1 = ASPERA_SCREEN_W;
    if (y1 > ASPERA_SCREEN_H) y1 = ASPERA_SCREEN_H;

    for (int j = y0; j < y1; j++) {
        uint32_t *row = pixels + j * ASPERA_SCREEN_W;
        for (int i = x0; i < x1; i++) {
            row[i] = color;
        }
    }
}

static void draw_border_rect(uint32_t *pixels, int x, int y, int w, int h, int thick, uint32_t color) {
    if (w <= 0 || h <= 0 || thick <= 0) return;
    draw_fill_rect(pixels, x, y, w, thick, color);                     // Top
    draw_fill_rect(pixels, x, y + h - thick, w, thick, color);         // Bottom
    draw_fill_rect(pixels, x, y, thick, h, color);                     // Left
    draw_fill_rect(pixels, x + w - thick, y, thick, h, color);         // Right
}

static void draw_bevel_rect(uint32_t *pixels, int x, int y, int w, int h, uint32_t bg, uint32_t light, uint32_t dark, bool sunken) {
    draw_fill_rect(pixels, x, y, w, h, bg);
    uint32_t tl = sunken ? dark : light;
    uint32_t br = sunken ? light : dark;
    draw_fill_rect(pixels, x, y, w, 1, tl);
    draw_fill_rect(pixels, x, y, 1, h, tl);
    draw_fill_rect(pixels, x, y + h - 1, w, 1, br);
    draw_fill_rect(pixels, x + w - 1, y, 1, h, br);
}

// Scrollbar calculations and layout (modeled from Objective-9C DrawScrollbar in stdlib/draw.o9)
static int scrollbar_inner_length(const AsperaScrollbar *sb) {
    int n = sb->is_vertical ? (sb->h - 4) : (sb->w - 4);
    return n < 0 ? 0 : n;
}

static int scrollbar_track_start(const AsperaScrollbar *sb) {
    return sb->is_vertical ? (sb->y + 2) : (sb->x + 2);
}

int scrollbar_thumb_length(const AsperaScrollbar *sb) {
    int track = scrollbar_inner_length(sb);
    if (track <= 0) return 0;
    int range = sb->max - sb->min;
    if (range <= 0) return track;
    int n = (sb->page * track) / (range + sb->page);
    if (n < 12) n = 12;
    if (n > track) n = track;
    return n;
}

int scrollbar_thumb_start(const AsperaScrollbar *sb) {
    int track = scrollbar_inner_length(sb);
    int thumb = scrollbar_thumb_length(sb);
    int span = track - thumb;
    int range = sb->max - sb->min;
    if (track <= 0 || thumb <= 0 || span <= 0 || range <= 0) {
        return scrollbar_track_start(sb);
    }
    return scrollbar_track_start(sb) + ((sb->val - sb->min) * span) / range;
}

int scrollbar_thumb_end(const AsperaScrollbar *sb) {
    return scrollbar_thumb_start(sb) + scrollbar_thumb_length(sb);
}

bool scrollbar_thumb_contains(const AsperaScrollbar *sb, int px, int py) {
    if (!sb->visible) return false;
    if (px < sb->x || px >= sb->x + sb->w || py < sb->y || py >= sb->y + sb->h) return false;
    int axis = sb->is_vertical ? py : px;
    int t_start = scrollbar_thumb_start(sb);
    int t_end = scrollbar_thumb_end(sb);
    return (axis >= t_start && axis < t_end);
}

int scrollbar_val_for_start(const AsperaScrollbar *sb, int start) {
    int track = scrollbar_inner_length(sb);
    int thumb = scrollbar_thumb_length(sb);
    int span = track - thumb;
    int range = sb->max - sb->min;
    if (track <= 0 || thumb <= 0 || span <= 0 || range <= 0) {
        return sb->min;
    }
    int t_start = scrollbar_track_start(sb);
    int rel = start - t_start;
    if (rel < 0) rel = 0;
    if (rel > span) rel = span;
    return sb->min + (rel * range) / span;
}

void win_layout_scrollbar(Window *w, const AsperaDesktop *desk) {
    if (w->is_shaded) {
        w->vscroll.visible = false;
        return;
    }
    w->vscroll.visible = true;
    w->vscroll.is_vertical = true;
    int bw = desk->theme.border_thickness;
    int th = desk->theme.titlebar_height;
    int cx = w->x + bw;
    int cy = w->y + bw + th;
    int cw = w->w - 2 * bw;
    int ch = w->h - 2 * bw - th;
    if (ch < 20) ch = 20;

    if (desk->theme_idx == THEME_PLAN9) {
        // Rio signature left-hand scrollbar (Scrollwid = 12)
        w->vscroll.x = cx;
        w->vscroll.y = cy;
        w->vscroll.w = 12;
        w->vscroll.h = ch;
    } else {
        // E16 / Win95 / NeXT right-hand scrollbar: width = 14
        w->vscroll.w = 14;
        w->vscroll.x = cx + cw - w->vscroll.w;
        w->vscroll.y = cy;
        w->vscroll.h = ch;
    }

    int page = (ch - 24) / 16;
    if (page < 1) page = 1;
    w->vscroll.page = page;
    w->vscroll.min = 0;
    int total_lines = (w->client_type == CLIENT_TERMINAL) ? w->term.num_lines : 16;
    if (total_lines <= page) {
        w->vscroll.max = 0;
    } else {
        w->vscroll.max = total_lines - page;
    }
    if (w->vscroll.val < w->vscroll.min) w->vscroll.val = w->vscroll.min;
    if (w->vscroll.val > w->vscroll.max) w->vscroll.val = w->vscroll.max;
}

static void draw_scrollbar(AsperaDesktop *desk, Window *w) {
    if (!w->vscroll.visible) return;
    const AsperaScrollbar *sb = &w->vscroll;
    const ThemeMetrics *t = &desk->theme;
    bool is_p9 = (desk->theme_idx == THEME_PLAN9);

    int t_len = scrollbar_thumb_length(sb);
    int t_start = scrollbar_thumb_start(sb);

    if (is_p9) {
        // Authentic Plan 9 Rio Scrollbar (left-hand side)
        // Background track: Rio grey (#999999)
        draw_fill_rect(desk->pixels, sb->x, sb->y, sb->w, sb->h, 0x999999);
        // Right divider line (1px black)
        draw_fill_rect(desk->pixels, sb->x + sb->w - 1, sb->y, 1, sb->h, 0x000000);
        // Thumb: Rio pure white (#ffffff) with 1px black border
        if (t_len > 0) {
            draw_fill_rect(desk->pixels, sb->x, t_start, sb->w - 1, t_len, 0xffffff);
            draw_border_rect(desk->pixels, sb->x, t_start, sb->w - 1, t_len, 1, 0x000000);
        }
    } else {
        // Enlightenment / Win95 / NeXT Scrollbar (right-hand side)
        uint32_t track_col = (desk->theme_idx == THEME_WIN95) ? 0xdfdfdf :
                             (desk->theme_idx == THEME_NEXT)  ? 0x444444 : 0x182430;
        uint32_t thumb_col = (desk->theme_idx == THEME_WIN95) ? 0xc0c0c0 :
                             (desk->theme_idx == THEME_NEXT)  ? 0x777777 : 0x3a5068;
        if (sb->dragging) {
            thumb_col = (desk->theme_idx == THEME_WIN95) ? 0xa0a0a0 : 0xe0c040;
        } else if (desk->hover_win == w && (desk->hover_part == PART_SCROLLBAR_THUMB || desk->hover_part == PART_SCROLLBAR_UP || desk->hover_part == PART_SCROLLBAR_DOWN)) {
            thumb_col = (desk->theme_idx == THEME_WIN95) ? 0xd0d0d0 : 0x4a6a8c;
        }

        draw_fill_rect(desk->pixels, sb->x, sb->y, sb->w, sb->h, track_col);
        draw_border_rect(desk->pixels, sb->x, sb->y, sb->w, sb->h, 1, t->bevel_dark);

        // Draw thumb
        if (t_len > 0) {
            draw_bevel_rect(desk->pixels, sb->x + 1, t_start, sb->w - 2, t_len,
                            thumb_col, t->bevel_light, t->bevel_dark, sb->dragging);
        }
    }
}

static void draw_char_clipped(uint32_t *pixels, int x, int y, char c, uint32_t color,
                              int min_x, int min_y, int max_x, int max_y) {
    if (x + 8 <= min_x || x >= max_x || y + 14 <= min_y || y >= max_y) return;
    unsigned char ch = (unsigned char)c;
    if (ch < 32 || ch > 126) ch = '?';
    int glyph_idx = ch - 32;

    for (int row = 0; row < 14; row++) {
        int py = y + row;
        if (py >= min_y && py < max_y && py >= 0 && py < ASPERA_SCREEN_H) {
            unsigned char bits = font8x14_data[glyph_idx][row];
            uint32_t *scan = pixels + py * ASPERA_SCREEN_W;
            for (int col = 0; col < 8; col++) {
                int px = x + col;
                if (px >= min_x && px < max_x && px >= 0 && px < ASPERA_SCREEN_W) {
                    if (bits & (0x80 >> col)) {
                        scan[px] = color;
                    }
                }
            }
        }
    }
}

static void draw_text_clipped(uint32_t *pixels, int x, int y, const char *str, uint32_t color,
                              int min_x, int min_y, int max_x, int max_y) {
    if (!str || y + 14 <= min_y || y >= max_y) return;
    int cur_x = x;
    while (*str && cur_x < max_x) {
        if (cur_x + 8 > min_x) {
            draw_char_clipped(pixels, cur_x, y, *str, color, min_x, min_y, max_x, max_y);
        }
        cur_x += 8;
        str++;
    }
}

static void draw_fill_rect_clipped(uint32_t *pixels, int x0, int y0, int w, int h, uint32_t color,
                                   int min_x, int min_y, int max_x, int max_y) {
    if (w <= 0 || h <= 0) return;
    int x1 = x0 + w;
    int y1 = y0 + h;
    if (x0 < min_x) x0 = min_x;
    if (y0 < min_y) y0 = min_y;
    if (x1 > max_x) x1 = max_x;
    if (y1 > max_y) y1 = max_y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > ASPERA_SCREEN_W) x1 = ASPERA_SCREEN_W;
    if (y1 > ASPERA_SCREEN_H) y1 = ASPERA_SCREEN_H;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = pixels + j * ASPERA_SCREEN_W;
        for (int i = x0; i < x1; i++) {
            row[i] = color;
        }
    }
}

static void draw_char(uint32_t *pixels, int x, int y, char c, uint32_t color) {
    draw_char_clipped(pixels, x, y, c, color, 0, 0, ASPERA_SCREEN_W, ASPERA_SCREEN_H);
}

static void draw_text(uint32_t *pixels, int x, int y, const char *str, uint32_t color) {
    draw_text_clipped(pixels, x, y, str, color, 0, 0, ASPERA_SCREEN_W, ASPERA_SCREEN_H);
}

static void draw_cursor(const AsperaDesktop *desk, uint32_t *pixels, int mx, int my) {
    if (desk->theme_idx == THEME_PLAN9) {
        // Authentic Plan 9 Cursors from xcursor-plan9 repo
        static const uint16_t p9_arrow_mask[16] = {
            0xFFFF, 0xFFFF, 0xFFFE, 0xFFFC, 0xFFF0, 0xFFF0, 0xFFF8, 0xFFFC,
            0xFFFE, 0xFFFF, 0xFFFE, 0xFFFC, 0xF3F8, 0xF1F0, 0xE0E0, 0xC040
        };
        static const uint16_t p9_arrow_body[16] = {
            0x0000, 0x7FFE, 0x7FFC, 0x7FF0, 0x7FE0, 0x7FE0, 0x7FF0, 0x7FF8,
            0x7FFC, 0x7FFE, 0x7FFC, 0x73F8, 0x61F0, 0x60E0, 0x4040, 0x0000
        };
        static const uint16_t p9_box_mask[16] = {
            0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xF81F, 0xF81F, 0xF81F,
            0xF81F, 0xF81F, 0xF81F, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF
        };
        static const uint16_t p9_box_body[16] = {
            0x0000, 0x7FFE, 0x7FFE, 0x7FFE, 0x700E, 0x700E, 0x700E, 0x700E,
            0x700E, 0x700E, 0x700E, 0x700E, 0x7FFE, 0x7FFE, 0x7FFE, 0x0000
        };
        static const uint16_t p9_sight_mask[16] = {
            0x1FF8, 0x3FFC, 0x7FFE, 0xFBDF, 0xF3CF, 0xE3C7, 0xFFFF, 0xFFFF,
            0xFFFF, 0xFFFF, 0xE3C7, 0xF3CF, 0x7BDF, 0x7FFE, 0x3FFC, 0x1FF8
        };
        static const uint16_t p9_sight_body[16] = {
            0x0000, 0x0FF0, 0x318C, 0x2184, 0x4182, 0x4182, 0x4182, 0x7FFE,
            0x7FFE, 0x4182, 0x4182, 0x4182, 0x2184, 0x318C, 0x0FF0, 0x0000
        };

        const uint16_t *mask = p9_arrow_mask;
        const uint16_t *body = p9_arrow_body;
        if (desk->is_dragging) {
            mask = p9_box_mask;
            body = p9_box_body;
        } else if (desk->is_resizing) {
            mask = p9_sight_mask;
            body = p9_sight_body;
        }

        for (int r = 0; r < 16; r++) {
            int py = my + r;
            if (py >= 0 && py < ASPERA_SCREEN_H) {
                uint32_t *row = pixels + py * ASPERA_SCREEN_W;
                for (int c = 0; c < 16; c++) {
                    int px = mx + c;
                    if (px >= 0 && px < ASPERA_SCREEN_W) {
                        uint16_t bit = 0x8000 >> c;
                        if (mask[r] & bit) {
                            if (body[r] & bit) {
                                row[px] = 0xffffff; // White body
                            } else {
                                row[px] = 0x000000; // Black outline
                            }
                        }
                    }
                }
            }
        }
        return;
    }

    static const uint16_t cursor_mask[16] = {
        0x8000, 0xC000, 0xE000, 0xF000,
        0xF800, 0xFC00, 0xFE00, 0xFF00,
        0xF800, 0xD800, 0x8C00, 0x0C00,
        0x0600, 0x0600, 0x0300, 0x0000
    };
    static const uint16_t cursor_fill[16] = {
        0x0000, 0x4000, 0x6000, 0x7000,
        0x7800, 0x7C00, 0x7E00, 0x7800,
        0x6800, 0x4800, 0x0400, 0x0400,
        0x0200, 0x0200, 0x0000, 0x0000
    };

    for (int r = 0; r < 16; r++) {
        int py = my + r;
        if (py >= 0 && py < ASPERA_SCREEN_H) {
            uint32_t *row = pixels + py * ASPERA_SCREEN_W;
            for (int c = 0; c < 16; c++) {
                int px = mx + c;
                if (px >= 0 && px < ASPERA_SCREEN_W) {
                    if (cursor_mask[r] & (0x8000 >> c)) {
                        if (cursor_fill[r] & (0x8000 >> c)) {
                            row[px] = 0x000000;
                        } else {
                            row[px] = 0xffffff;
                        }
                    }
                }
            }
        }
    }
}

// Terminal commands
static void terminal_append_line(TerminalState *term, const char *line) {
    if (term->num_lines >= ASPERA_MAX_TERMINAL_LINES - 1) {
        // Scroll up
        memmove(&term->lines[0], &term->lines[1], sizeof(term->lines[0]) * (ASPERA_MAX_TERMINAL_LINES - 1));
        term->num_lines = ASPERA_MAX_TERMINAL_LINES - 2;
    }
    strncpy(term->lines[term->num_lines], line, ASPERA_MAX_LINE_LEN - 1);
    term->lines[term->num_lines][ASPERA_MAX_LINE_LEN - 1] = '\0';
    term->num_lines++;
}

static void terminal_start_rc(Window *w) {
    w->term.rc_pid = -1;
    w->term.rc_in_fd = -1;
    w->term.rc_out_fd = -1;
    w->term.partial_len = 0;
    w->term.partial[0] = '\0';

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0) return;
    if (pipe(out_pipe) < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        setenv("PLAN9", "/usr/local/plan9", 1);
        const char *old_path = getenv("PATH");
        char new_path[1024];
        snprintf(new_path, sizeof(new_path), "/usr/local/plan9/bin:%s", old_path ? old_path : "/usr/bin:/bin");
        setenv("PATH", new_path, 1);
        execl("/usr/local/plan9/bin/rc", "rc", NULL);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (pid > 0) {
        int flags = fcntl(out_pipe[0], F_GETFL, 0);
        fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK);
        w->term.rc_pid = (int)pid;
        w->term.rc_in_fd = in_pipe[1];
        w->term.rc_out_fd = out_pipe[0];
    } else {
        close(in_pipe[1]);
        close(out_pipe[0]);
    }
}

static bool terminal_poll_rc(AsperaDesktop *desk, Window *w) {
    TerminalState *term = &w->term;
    if (term->rc_out_fd < 0) return false;
    char buf[512];
    bool added = false;
    for (;;) {
        ssize_t n = read(term->rc_out_fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                term->partial[term->partial_len] = '\0';
                terminal_append_line(term, term->partial);
                term->partial_len = 0;
                term->partial[0] = '\0';
                added = true;
            } else if (c >= 32 && c <= 126) {
                if (term->partial_len < ASPERA_MAX_LINE_LEN - 2) {
                    term->partial[term->partial_len++] = c;
                    term->partial[term->partial_len] = '\0';
                } else {
                    term->partial[term->partial_len] = '\0';
                    terminal_append_line(term, term->partial);
                    term->partial_len = 0;
                    term->partial[term->partial_len++] = c;
                    term->partial[term->partial_len] = '\0';
                    added = true;
                }
            } else if (c == '\t') {
                for (int s = 0; s < 4 && term->partial_len < ASPERA_MAX_LINE_LEN - 2; s++) {
                    term->partial[term->partial_len++] = ' ';
                }
                term->partial[term->partial_len] = '\0';
            }
        }
    }
    if (added) {
        win_layout_scrollbar(w, desk);
        w->vscroll.val = w->vscroll.max;
    }
    return added;
}

static void sam_append_cmd_line(SamState *sam, const char *line) {
    if (sam->num_cmd_lines >= 31) {
        memmove(&sam->cmd_lines[0], &sam->cmd_lines[1], sizeof(sam->cmd_lines[0]) * 31);
        sam->num_cmd_lines = 30;
    }
    strncpy(sam->cmd_lines[sam->num_cmd_lines], line, ASPERA_MAX_LINE_LEN - 1);
    sam->cmd_lines[sam->num_cmd_lines][ASPERA_MAX_LINE_LEN - 1] = '\0';
    sam->num_cmd_lines++;
}

static void sam_reload_file(SamState *sam, const char *filepath) {
    FILE *fp = fopen(filepath, "rb");
    if (fp) {
        size_t n = fread(sam->buf, 1, sizeof(sam->buf) - 1, fp);
        fclose(fp);
        sam->buf_len = (int)n;
        sam->buf[sam->buf_len] = '\0';
    }
}

static bool sam_poll(Window *w) {
    SamState *sam = &w->sam;
    if (sam->sam_out_fd < 0) return false;
    char buf[512];
    bool added = false;
    for (;;) {
        ssize_t n = read(sam->sam_out_fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                sam->cmd_partial[sam->cmd_partial_len] = '\0';
                // Hide internal sync file write confirmation, show everything else from real sam -d
                if (strstr(sam->cmd_partial, "sam_sync_") == NULL) {
                    sam_append_cmd_line(sam, sam->cmd_partial);
                }
                sam->cmd_partial_len = 0;
                sam->cmd_partial[0] = '\0';
                added = true;
            } else if (c >= 32 && c <= 126) {
                if (sam->cmd_partial_len < ASPERA_MAX_LINE_LEN - 2) {
                    sam->cmd_partial[sam->cmd_partial_len++] = c;
                    sam->cmd_partial[sam->cmd_partial_len] = '\0';
                }
            }
        }
    }
    if (added) {
        sam_reload_file(sam, sam->sync_path);
    }
    return added;
}

static void sam_start(Window *w) {
    SamState *sam = &w->sam;
    sam->sam_pid = -1;
    sam->sam_in_fd = -1;
    sam->sam_out_fd = -1;
    sam->num_cmd_lines = 0;
    sam->cmd_len = 0;
    sam->cmd_input[0] = '\0';
    sam->cmd_partial_len = 0;
    sam->cmd_partial[0] = '\0';
    sam->cmd_focused = true;
    sam->dirty = false;

    mkdir("build", 0755);
    mkdir("build/aspera-live", 0755);
    snprintf(sam->path, sizeof(sam->path), "build/aspera-live/sam_buffer.c");
    snprintf(sam->sync_path, sizeof(sam->sync_path), "build/aspera-live/sam_sync_%d.tmp", w->id);

    FILE *check = fopen(sam->path, "rb");
    if (!check) {
        FILE *seed = fopen(sam->path, "wb");
        if (seed) {
            const char *init_code =
                "/* Sam Editor - Plan 9 Text Component (Real sam -d) */\n"
                "#include <u.h>\n"
                "#include <libc.h>\n"
                "#include <thread.h>\n"
                "\n"
                "void threadmain(int argc, char *argv[]) {\n"
                "    print(\"Plan 9 Rio Window Online\\n\");\n"
                "}\n";
            fwrite(init_code, 1, strlen(init_code), seed);
            fclose(seed);
        }
    } else {
        fclose(check);
    }
    sam_reload_file(sam, sam->path);
    FILE *sync_fp = fopen(sam->sync_path, "wb");
    if (sync_fp) {
        fwrite(sam->buf, 1, (size_t)sam->buf_len, sync_fp);
        fclose(sync_fp);
    }

    sam_append_cmd_line(sam, "~~sam~~ [/usr/local/plan9/bin/sam -d | Tab switches pane]");

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0) return;
    if (pipe(out_pipe) < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        setenv("PLAN9", "/usr/local/plan9", 1);
        execl("/usr/local/plan9/bin/sam", "sam", "-d", sam->path, NULL);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (pid > 0) {
        int flags = fcntl(out_pipe[0], F_GETFL, 0);
        fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK);
        sam->sam_pid = (int)pid;
        sam->sam_in_fd = in_pipe[1];
        sam->sam_out_fd = out_pipe[0];
        for (int tries = 0; tries < 5; tries++) {
            usleep(1000);
            if (sam_poll(w)) break;
        }
    } else {
        close(in_pipe[1]);
        close(out_pipe[0]);
    }
}

static void sam_sync_from_buffer(Window *w) {
    SamState *sam = &w->sam;
    FILE *fp = fopen(sam->sync_path, "wb");
    if (fp) {
        fwrite(sam->buf, 1, (size_t)sam->buf_len, fp);
        fclose(fp);
    }
    if (sam->sam_in_fd >= 0) {
        char cmd[512];
        int n = snprintf(cmd, sizeof(cmd), ",d\nr %s\n", sam->sync_path);
        ssize_t unused = write(sam->sam_in_fd, cmd, (size_t)n);
        (void)unused;
        for (int tries = 0; tries < 5; tries++) {
            usleep(1000);
            sam_poll(w);
        }
    }
}

static void sam_execute_cmd(AsperaDesktop *desk, Window *w) {
    SamState *sam = &w->sam;
    char echo_line[ASPERA_MAX_LINE_LEN + 16];
    snprintf(echo_line, sizeof(echo_line), "sam%% %s", sam->cmd_input);
    sam_append_cmd_line(sam, echo_line);

    if (strcmp(sam->cmd_input, "q") == 0 || strcmp(sam->cmd_input, "Q") == 0) {
        sam->cmd_len = 0;
        sam->cmd_input[0] = '\0';
        aspera_close_window(desk, w);
        return;
    }

    if (sam->sam_in_fd >= 0 && sam->cmd_len > 0) {
        char payload[640];
        int n = snprintf(payload, sizeof(payload), "%s\nw %s\n", sam->cmd_input, sam->sync_path);
        ssize_t unused = write(sam->sam_in_fd, payload, (size_t)n);
        (void)unused;
        for (int tries = 0; tries < 8; tries++) {
            usleep(1500);
            sam_poll(w);
        }
        sam_reload_file(sam, sam->sync_path);
        if (strcmp(sam->cmd_input, "w") == 0) {
            sam->dirty = false;
        } else {
            sam->dirty = true;
        }
    }
    sam->cmd_len = 0;
    sam->cmd_input[0] = '\0';
}

static void terminal_execute(AsperaDesktop *desk, Window *w) {
    TerminalState *term = &w->term;
    char prompt_cmd[ASPERA_MAX_LINE_LEN + 16];
    snprintf(prompt_cmd, sizeof(prompt_cmd), "harvey%% %s", term->input_buf);
    terminal_append_line(term, prompt_cmd);

    char *cmd = term->input_buf;
    while (*cmd == ' ') cmd++;

    if (strcmp(cmd, "help") == 0) {
        terminal_append_line(term, "Plan 9 rc terminal (/usr/local/plan9/bin/rc live session)");
        terminal_append_line(term, "  Plan 9 rc:    fn, for, ifs, whatis, ls, lc, cat, hoc, cal, date...");
        terminal_append_line(term, "  WM Builtins:  theme <name>, desk <id>, shade, stick, new, clear, exit");
    } else if (strncmp(cmd, "theme", 5) == 0 && (cmd[5] == '\0' || cmd[5] == ' ')) {
        char *arg = cmd + 5;
        while (*arg == ' ') arg++;
        if (strcmp(arg, "e16") == 0) aspera_set_theme(desk, THEME_E16);
        else if (strcmp(arg, "win95") == 0) aspera_set_theme(desk, THEME_WIN95);
        else if (strcmp(arg, "next") == 0) aspera_set_theme(desk, THEME_NEXT);
        else if (strcmp(arg, "plan9") == 0) aspera_set_theme(desk, THEME_PLAN9);
        else aspera_cycle_theme(desk);
        char buf[64];
        snprintf(buf, sizeof(buf), "[o9aspera] Theme active: %s", desk->theme.name);
        terminal_append_line(term, buf);
    } else if (strncmp(cmd, "desk", 4) == 0 && (cmd[4] == '\0' || cmd[4] == ' ')) {
        int id = atoi(cmd + 4);
        if (id >= 0 && id < 4) {
            aspera_switch_desktop(desk, id % desk->cols, id / desk->cols);
            char buf[64];
            snprintf(buf, sizeof(buf), "[o9aspera] Switched to workspace (%d,%d)", desk->cur_x, desk->cur_y);
            terminal_append_line(term, buf);
        }
    } else if (strcmp(cmd, "shade") == 0) {
        aspera_toggle_shade(desk, w);
    } else if (strcmp(cmd, "stick") == 0) {
        aspera_toggle_sticky(desk, w);
        char buf[64];
        snprintf(buf, sizeof(buf), "[o9aspera] Sticky pinned: %s", w->is_sticky ? "TRUE" : "FALSE");
        terminal_append_line(term, buf);
    } else if (strncmp(cmd, "new", 3) == 0 && (cmd[3] == '\0' || cmd[3] == ' ')) {
        char *title = cmd + 3;
        while (*title == ' ') title++;
        if (*title == '\0') title = "Terminal [rc]";
        aspera_create_window(desk, title, w->x + 40, w->y + 40, 480, 320, CLIENT_TERMINAL);
    } else if (strcmp(cmd, "clear") == 0) {
        term->num_lines = 0;
    } else if (strcmp(cmd, "uname") == 0) {
        terminal_append_line(term, "Harvey-o9 Plan 9 amd64 (bare-metal KVM microkernel)");
    } else if (strcmp(cmd, "exit") == 0) {
        aspera_close_window(desk, w);
        return;
    } else if (strlen(cmd) > 0) {
        if (term->rc_in_fd >= 0) {
            ssize_t w1 = write(term->rc_in_fd, cmd, strlen(cmd));
            ssize_t w2 = write(term->rc_in_fd, "\n", 1);
            (void)w1;
            (void)w2;
            for (int tries = 0; tries < 8; tries++) {
                usleep(2000);
                terminal_poll_rc(desk, w);
            }
        }
    }

    win_layout_scrollbar(w, desk);
    w->vscroll.val = w->vscroll.max;

    term->input_buf[0] = '\0';
    term->input_len = 0;
}

static void note_load(Window *w, const char *path) {
    if (!w) return;
    strncpy(w->note.path, path ? path : "build/aspera-live/note.txt", sizeof(w->note.path) - 1);
    w->note.len = 0;
    w->note.buf[0] = '\0';
    w->note.dirty = false;
    w->note.save_error = false;
    FILE *fp = fopen(w->note.path, "rb");
    if (fp) {
        size_t n = fread(w->note.buf, 1, sizeof(w->note.buf) - 1, fp);
        fclose(fp);
        w->note.len = (int)n;
        w->note.buf[w->note.len] = '\0';
    }
}

static bool note_save(Window *w) {
    if (!w) return false;
    FILE *fp = fopen(w->note.path, "wb");
    if (!fp) {
        w->note.save_error = true;
        return false;
    }
    size_t wrote = fwrite(w->note.buf, 1, (size_t)w->note.len, fp);
    int closed = fclose(fp);
    if (wrote != (size_t)w->note.len || closed != 0) {
        w->note.save_error = true;
        return false;
    }
    w->note.dirty = false;
    w->note.save_error = false;
    return true;
}

// Window manager initialization
void aspera_init(AsperaDesktop *desk) {
    memset(desk, 0, sizeof(*desk));
    desk->cols = 2;
    desk->rows = 2;
    desk->cur_x = 0;
    desk->cur_y = 0;
    desk->next_win_id = 1;

    desk->pager_w = 150;
    desk->pager_h = 100;
    desk->pager_x = ASPERA_SCREEN_W - desk->pager_w - 12;
    desk->pager_y = ASPERA_SCREEN_H - desk->pager_h - 12;

    desk->shelf_menu_open = false;
    desk->shelf_clock_visible = true;
    desk->shelf_menu_hover = -1;

    aspera_set_theme(desk, THEME_E16);

    // Create default sample windows
    Window *w1 = aspera_create_window(desk, "Terminal 1 [rc]", 60, 60, 480, 320, CLIENT_TERMINAL);
    terminal_append_line(&w1->term, "Harvey Microkernel - Objective-9C Aspera Online");
    terminal_append_line(&w1->term, "Enlightenment Retained Window System Active");
    terminal_append_line(&w1->term, "Interactive DrawScrollbar, Plan 9 Cursors & rc Shell Active");
    terminal_append_line(&w1->term, "Type 'help' for shell commands, Alt+T to cycle themes.");
    win_layout_scrollbar(w1, desk);

    Window *w2 = aspera_create_window(desk, "Editor [sam]", 320, 140, 520, 380, CLIENT_EDITOR);
    (void)w2;

    Window *w3 = aspera_create_window(desk, "Clock & System Info", 560, 40, 240, 160, CLIENT_CLOCK);
    w3->desktop_id = 1; // Put clock on workspace (1,0)
    (void)w3;

    aspera_raise_window(desk, w2);
    aspera_raise_window(desk, w1);
    aspera_focus_window(desk, w1);
}

void aspera_set_theme(AsperaDesktop *desk, ThemeType type) {
    if (type >= THEME_COUNT) type = THEME_E16;
    desk->theme_idx = type;
    desk->theme = themes[type];
}

void aspera_cycle_theme(AsperaDesktop *desk) {
    aspera_set_theme(desk, (desk->theme_idx + 1) % THEME_COUNT);
}

Window *aspera_create_window(AsperaDesktop *desk, const char *title, int x, int y, int w, int h, ClientType type) {
    Window *win = malloc(sizeof(Window));
    if (!win) return NULL;
    memset(win, 0, sizeof(*win));

    win->id = desk->next_win_id++;
    strncpy(win->title, title, sizeof(win->title) - 1);
    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    win->saved_x = x;
    win->saved_y = y;
    win->saved_w = w;
    win->saved_h = h;
    win->desktop_id = desk->cur_y * desk->cols + desk->cur_x;
    win->client_type = type;
    win->term.rc_pid = -1;
    win->term.rc_in_fd = -1;
    win->term.rc_out_fd = -1;
    win->sam.sam_pid = -1;
    win->sam.sam_in_fd = -1;
    win->sam.sam_out_fd = -1;
    if (type == CLIENT_TERMINAL) {
        terminal_start_rc(win);
    } else if (type == CLIENT_EDITOR) {
        sam_start(win);
    } else if (type == CLIENT_NOTES) {
        note_load(win, "build/aspera-live/note.txt");
    }

    win_layout_scrollbar(win, desk);
    if (type == CLIENT_NOTES) {
        win->vscroll.visible = false;
    }

    // Insert at head (top of Z-order)
    if (!desk->head) {
        desk->head = win;
        desk->tail = win;
    } else {
        win->next = desk->head;
        desk->head->prev = win;
        desk->head = win;
    }

    aspera_focus_window(desk, win);
    return win;
}

void aspera_close_window(AsperaDesktop *desk, Window *w) {
    if (!w) return;

    if (w->term.rc_in_fd >= 0) close(w->term.rc_in_fd);
    if (w->term.rc_out_fd >= 0) close(w->term.rc_out_fd);
    if (w->term.rc_pid > 0) {
        kill(w->term.rc_pid, SIGTERM);
        waitpid(w->term.rc_pid, NULL, WNOHANG);
    }
    if (w->sam.sam_in_fd >= 0) close(w->sam.sam_in_fd);
    if (w->sam.sam_out_fd >= 0) close(w->sam.sam_out_fd);
    if (w->sam.sam_pid > 0) {
        kill(w->sam.sam_pid, SIGTERM);
        waitpid(w->sam.sam_pid, NULL, WNOHANG);
    }

    if (w->prev) w->prev->next = w->next;
    else desk->head = w->next;

    if (w->next) w->next->prev = w->prev;
    else desk->tail = w->prev;

    if (desk->focused == w) {
        desk->focused = desk->head;
        if (desk->focused) desk->focused->is_focused = true;
    }
    if (desk->drag_win == w) desk->drag_win = NULL;
    if (desk->hover_win == w) desk->hover_win = NULL;
    if (desk->menu_target_win == w) desk->menu_target_win = NULL;

    free(w);
}

void aspera_focus_window(AsperaDesktop *desk, Window *w) {
    for (Window *cur = desk->head; cur; cur = cur->next) {
        cur->is_focused = (cur == w);
    }
    desk->focused = w;
}

void aspera_raise_window(AsperaDesktop *desk, Window *w) {
    if (!w || w == desk->head) return;

    // Unlink w
    if (w->prev) w->prev->next = w->next;
    if (w->next) w->next->prev = w->prev;
    if (w == desk->tail) desk->tail = w->prev;

    // Insert at head
    w->prev = NULL;
    w->next = desk->head;
    if (desk->head) desk->head->prev = w;
    desk->head = w;
}

void aspera_cycle_focus(AsperaDesktop *desk) {
    if (!desk->focused || !desk->head || desk->head == desk->tail) return;
    int cur_desk = desk->cur_y * desk->cols + desk->cur_x;

    // Find next visible window
    Window *cand = desk->focused->next;
    while (cand && !cand->is_sticky && cand->desktop_id != cur_desk) {
        cand = cand->next;
    }
    if (!cand) {
        cand = desk->head;
        while (cand && !cand->is_sticky && cand->desktop_id != cur_desk) {
            cand = cand->next;
        }
    }
    if (cand) {
        aspera_raise_window(desk, cand);
        aspera_focus_window(desk, cand);
    }
}

void aspera_switch_desktop(AsperaDesktop *desk, int dx, int dy) {
    if (dx >= 0 && dx < desk->cols && dy >= 0 && dy < desk->rows) {
        desk->cur_x = dx;
        desk->cur_y = dy;
        int cur_desk = dy * desk->cols + dx;

        // Focus topmost window on new desktop if focused is not sticky
        if (!desk->focused || (!desk->focused->is_sticky && desk->focused->desktop_id != cur_desk)) {
            desk->focused = NULL;
            for (Window *w = desk->head; w; w = w->next) {
                if (w->is_sticky || w->desktop_id == cur_desk) {
                    aspera_focus_window(desk, w);
                    break;
                }
            }
        }
    }
}

void aspera_toggle_shade(AsperaDesktop *desk, Window *w) {
    if (!w) return;
    (void)desk;
    w->is_shaded = !w->is_shaded;
}

void aspera_toggle_sticky(AsperaDesktop *desk, Window *w) {
    if (!w) return;
    (void)desk;
    w->is_sticky = !w->is_sticky;
}

void aspera_toggle_maximize(AsperaDesktop *desk, Window *w) {
    if (!w) return;
    if (!w->is_maximized) {
        w->saved_x = w->x;
        w->saved_y = w->y;
        w->saved_w = w->w;
        w->saved_h = w->h;
        w->x = 0;
        w->y = 0;
        w->w = ASPERA_SCREEN_W;
        w->h = ASPERA_SCREEN_H;
        w->is_maximized = true;
        w->is_shaded = false;
    } else {
        w->x = w->saved_x;
        w->y = w->saved_y;
        w->w = w->saved_w;
        w->h = w->saved_h;
        w->is_maximized = false;
    }
    (void)desk;
}

void aspera_get_button_pos(const AsperaDesktop *desk, const Window *w, PartType btn, int *out_x, int *out_y) {
    const ThemeMetrics *t = &desk->theme;
    int bw = t->border_thickness;
    int th = t->titlebar_height;
    int bs = t->btn_size;
    int bsp = t->btn_spacing;

    int bx = w->x + w->w - bw - bs - 2;
    int by = w->y + bw + (th - bs) / 2;

    if (btn == PART_BTN_CLOSE) {
        if (out_x) *out_x = bx + bs / 2;
        if (out_y) *out_y = by + bs / 2;
        return;
    }
    if (!w->is_shaded) {
        bx -= (bs + bsp);
        if (btn == PART_BTN_MAX) {
            if (out_x) *out_x = bx + bs / 2;
            if (out_y) *out_y = by + bs / 2;
            return;
        }
    }
    bx -= (bs + bsp);
    if (btn == PART_BTN_SHADE) {
        if (out_x) *out_x = bx + bs / 2;
        if (out_y) *out_y = by + bs / 2;
        return;
    }
    bx -= (bs + bsp);
    if (btn == PART_BTN_STICK) {
        if (out_x) *out_x = bx + bs / 2;
        if (out_y) *out_y = by + bs / 2;
        return;
    }
    if (out_x) *out_x = bx;
    if (out_y) *out_y = by;
}

// Enlightenment Part-Tree Hit Testing
PartType aspera_hit_test_window(const AsperaDesktop *desk, const Window *w, int px, int py) {
    const ThemeMetrics *t = &desk->theme;
    int bw = t->border_thickness;
    int th = t->titlebar_height;
    int bs = t->btn_size;
    int bsp = t->btn_spacing;
    int vh = win_visible_h(w, t);

    if (px < w->x || px >= w->x + w->w || py < w->y || py >= w->y + vh) {
        return PART_NONE;
    }

    // Check 4 corners first (active resize areas)
    int corner_sz = bw + 8;
    if (px < w->x + corner_sz && py < w->y + corner_sz) return PART_CORNER_NW;
    if (px >= w->x + w->w - corner_sz && py < w->y + corner_sz) return PART_CORNER_NE;
    if (!w->is_shaded) {
        if (px < w->x + corner_sz && py >= w->y + vh - corner_sz) return PART_CORNER_SW;
        if (px >= w->x + w->w - corner_sz && py >= w->y + vh - corner_sz) return PART_CORNER_SE;
    }

    // Titlebar row
    if (py >= w->y + bw && py < w->y + bw + th) {
        int btn_x = w->x + w->w - bw - bs - 2;
        int btn_y = w->y + bw + (th - bs) / 2;

        // Close button
        if (px >= btn_x && px < btn_x + bs && py >= btn_y && py < btn_y + bs) {
            return PART_BTN_CLOSE;
        }
        btn_x -= (bs + bsp);

        // Maximize button
        if (!w->is_shaded) {
            if (px >= btn_x && px < btn_x + bs && py >= btn_y && py < btn_y + bs) {
                return PART_BTN_MAX;
            }
            btn_x -= (bs + bsp);
        }

        // Shade button
        if (px >= btn_x && px < btn_x + bs && py >= btn_y && py < btn_y + bs) {
            return PART_BTN_SHADE;
        }
        btn_x -= (bs + bsp);

        // Sticky button
        if (px >= btn_x && px < btn_x + bs && py >= btn_y && py < btn_y + bs) {
            return PART_BTN_STICK;
        }

        return PART_TITLEBAR;
    }

    // Borders
    if (py < w->y + bw) return PART_BORDER_N;
    if (py >= w->y + vh - bw) return PART_BORDER_S;
    if (px < w->x + bw) return PART_BORDER_W;
    if (px >= w->x + w->w - bw) return PART_BORDER_E;

    // Check scrollbar (inside client content)
    if (w->vscroll.visible &&
        px >= w->vscroll.x && px < w->vscroll.x + w->vscroll.w &&
        py >= w->vscroll.y && py < w->vscroll.y + w->vscroll.h) {
        if (scrollbar_thumb_contains(&w->vscroll, px, py)) {
            return PART_SCROLLBAR_THUMB;
        }
        if (py < scrollbar_thumb_start(&w->vscroll)) {
            return PART_SCROLLBAR_UP;
        } else {
            return PART_SCROLLBAR_DOWN;
        }
    }

    return PART_CONTENT;
}

// Enlightenment Magnetic Snapping and Edge Flipping Algorithm
static void aspera_snap_window(AsperaDesktop *desk, Window *target, int req_x, int req_y) {
    int snap = ASPERA_SNAP_DIST;
    int final_x = req_x;
    int final_y = req_y;
    int tw = target->w;
    int th = win_visible_h(target, &desk->theme);

    // 1. Enlightenment Edge Desktop Flipping
    if (desk->edge_flip_cooldown <= 0) {
        if (req_x + tw / 2 > ASPERA_SCREEN_W && desk->cur_x < desk->cols - 1) {
            target->x = 20;
            aspera_switch_desktop(desk, desk->cur_x + 1, desk->cur_y);
            target->desktop_id = desk->cur_y * desk->cols + desk->cur_x;
            desk->edge_flip_cooldown = 30; // 0.5s cooldown
            return;
        }
        if (req_x + tw / 2 < 0 && desk->cur_x > 0) {
            target->x = ASPERA_SCREEN_W - tw - 20;
            aspera_switch_desktop(desk, desk->cur_x - 1, desk->cur_y);
            target->desktop_id = desk->cur_y * desk->cols + desk->cur_x;
            desk->edge_flip_cooldown = 30;
            return;
        }
        if (req_y + th / 2 > ASPERA_SCREEN_H && desk->cur_y < desk->rows - 1) {
            target->y = 20;
            aspera_switch_desktop(desk, desk->cur_x, desk->cur_y + 1);
            target->desktop_id = desk->cur_y * desk->cols + desk->cur_x;
            desk->edge_flip_cooldown = 30;
            return;
        }
        if (req_y + th / 2 < 0 && desk->cur_y > 0) {
            target->y = ASPERA_SCREEN_H - th - 20;
            aspera_switch_desktop(desk, desk->cur_x, desk->cur_y - 1);
            target->desktop_id = desk->cur_y * desk->cols + desk->cur_x;
            desk->edge_flip_cooldown = 30;
            return;
        }
    }

    // 2. Snap to screen boundaries
    if (final_x < snap && final_x > -snap) final_x = 0;
    if (final_y < snap && final_y > -snap) final_y = 0;
    if (final_x + tw > ASPERA_SCREEN_W - snap && final_x + tw < ASPERA_SCREEN_W + snap) final_x = ASPERA_SCREEN_W - tw;
    if (final_y + th > ASPERA_SCREEN_H - snap && final_y + th < ASPERA_SCREEN_H + snap) final_y = ASPERA_SCREEN_H - th;

    // 3. Snap to neighbor windows on current desktop
    int cur_desk = desk->cur_y * desk->cols + desk->cur_x;
    for (Window *w = desk->head; w; w = w->next) {
        if (w == target) continue;
        if (!w->is_sticky && w->desktop_id != cur_desk) continue;

        int ox = w->x;
        int oy = w->y;
        int ow = w->w;
        int oh = win_visible_h(w, &desk->theme);

        // Right of target snaps to left of neighbor
        if (abs(final_x + tw - ox) <= snap) final_x = ox - tw;
        // Left of target snaps to right of neighbor
        if (abs(final_x - (ox + ow)) <= snap) final_x = ox + ow;
        // Bottom of target snaps to top of neighbor
        if (abs(final_y + th - oy) <= snap) final_y = oy - th;
        // Top of target snaps to bottom of neighbor
        if (abs(final_y - (oy + oh)) <= snap) final_y = oy + oh;

        // Alignment snaps
        if (abs(final_x - ox) <= snap) final_x = ox;
        if (abs(final_y - oy) <= snap) final_y = oy;
    }

    target->x = final_x;
    target->y = final_y;
}

// Mouse Event Handlers
void aspera_handle_mouse_move(AsperaDesktop *desk, int mx, int my) {
    desk->mouse_x = mx;
    desk->mouse_y = my;

    if (desk->edge_flip_cooldown > 0) desk->edge_flip_cooldown--;

    // Active scrollbar dragging
    if (desk->is_scrolling && desk->scroll_win) {
        Window *w = desk->scroll_win;
        w->vscroll.val = scrollbar_val_for_start(&w->vscroll, my - w->vscroll.drag_offset);
        return;
    }

    // 1. Handle active dragging
    if (desk->is_dragging && desk->drag_win) {
        int req_x = desk->drag_start_wx + (mx - desk->drag_start_mx);
        int req_y = desk->drag_start_wy + (my - desk->drag_start_my);
        aspera_snap_window(desk, desk->drag_win, req_x, req_y);
        return;
    }

    // 2. Handle active 8-way resizing
    if (desk->is_resizing && desk->drag_win) {
        Window *w = desk->drag_win;
        int dx = mx - desk->drag_start_mx;
        int dy = my - desk->drag_start_my;

        switch (desk->resize_part) {
        case PART_CORNER_SE:
        case PART_BORDER_E:
        case PART_BORDER_S:
            if (desk->drag_start_ww + dx >= 140) w->w = desk->drag_start_ww + dx;
            if (desk->drag_start_wh + dy >= 60) w->h = desk->drag_start_wh + dy;
            break;
        case PART_CORNER_SW:
        case PART_BORDER_W:
            if (desk->drag_start_ww - dx >= 140) {
                w->x = desk->drag_start_wx + dx;
                w->w = desk->drag_start_ww - dx;
            }
            if (desk->drag_start_wh + dy >= 60) w->h = desk->drag_start_wh + dy;
            break;
        case PART_CORNER_NE:
        case PART_BORDER_N:
            if (desk->drag_start_ww + dx >= 140) w->w = desk->drag_start_ww + dx;
            if (desk->drag_start_wh - dy >= 60) {
                w->y = desk->drag_start_wy + dy;
                w->h = desk->drag_start_wh - dy;
            }
            break;
        case PART_CORNER_NW:
            if (desk->drag_start_ww - dx >= 140) {
                w->x = desk->drag_start_wx + dx;
                w->w = desk->drag_start_ww - dx;
            }
            if (desk->drag_start_wh - dy >= 60) {
                w->y = desk->drag_start_wy + dy;
                w->h = desk->drag_start_wh - dy;
            }
            break;
        default:
            break;
        }
        return;
    }

    // 3. Update hover state
    desk->hover_part = PART_NONE;
    desk->hover_win = NULL;
    desk->shelf_menu_hover = -1;

    // Check Aspera Shelf dropdown menu hover
    if (desk->shelf_menu_open) {
        if (mx >= 8 && mx < 192 && my >= 34 && my < 102) {
            desk->shelf_menu_hover = (my < 68) ? 0 : 1;
            return;
        }
    }

    // Check menu hover
    if (desk->menu_open) {
        if (mx >= desk->menu_x && mx < desk->menu_x + desk->menu_w &&
            my >= desk->menu_y && my < desk->menu_y + desk->menu_h) {
            desk->menu_hover_idx = (my - desk->menu_y - 4) / 20;
            if (desk->menu_hover_idx < 0) desk->menu_hover_idx = 0;
            if (desk->menu_hover_idx >= desk->num_menu_items) desk->menu_hover_idx = desk->num_menu_items - 1;
            return;
        }
    }

    // Check pager hover
    if (mx >= desk->pager_x && mx < desk->pager_x + desk->pager_w &&
        my >= desk->pager_y && my < desk->pager_y + desk->pager_h) {
        desk->hover_part = PART_PAGER_CELL;
        return;
    }

    // Check windows in Z-order
    int cur_desk = desk->cur_y * desk->cols + desk->cur_x;
    for (Window *w = desk->head; w; w = w->next) {
        if (!w->is_sticky && w->desktop_id != cur_desk) continue;
        PartType part = aspera_hit_test_window(desk, w, mx, my);
        if (part != PART_NONE) {
            desk->hover_part = part;
            desk->hover_win = w;
            break;
        }
    }
}

static void open_root_menu(AsperaDesktop *desk, int mx, int my) {
    desk->menu_open = true;
    desk->menu_is_window = false;
    desk->menu_target_win = NULL;
    desk->menu_x = mx;
    desk->menu_y = my;
    desk->menu_w = 160;
    desk->menu_hover_idx = -1;
    desk->menu_items[0] = "New Terminal";
    desk->menu_items[1] = "Cycle Theme";
    desk->menu_items[2] = "Next Workspace";
    desk->menu_items[3] = "Prev Workspace";
    desk->menu_items[4] = "Tile Windows";
    desk->menu_items[5] = "Exit Manager";
    desk->num_menu_items = 6;
    desk->menu_h = desk->num_menu_items * 20 + 8;
}

static void open_window_menu(AsperaDesktop *desk, Window *w, int mx, int my) {
    desk->menu_open = true;
    desk->menu_is_window = true;
    desk->menu_target_win = w;
    desk->menu_x = mx;
    desk->menu_y = my;
    desk->menu_w = 170;
    desk->menu_hover_idx = -1;
    desk->menu_items[0] = w->is_shaded ? "Unshade Window" : "Shade (Roll-Up)";
    desk->menu_items[1] = w->is_sticky ? "Unpin (Unstick)" : "Pin to Desktops";
    desk->menu_items[2] = w->is_maximized ? "Restore Window" : "Maximize Window";
    desk->menu_items[3] = "Send to Next Desk";
    desk->menu_items[4] = "Close Window";
    desk->num_menu_items = 5;
    desk->menu_h = desk->num_menu_items * 20 + 8;
}

static void execute_menu_action(AsperaDesktop *desk, int idx) {
    if (desk->menu_is_window && desk->menu_target_win) {
        Window *w = desk->menu_target_win;
        switch (idx) {
        case 0: aspera_toggle_shade(desk, w); break;
        case 1: aspera_toggle_sticky(desk, w); break;
        case 2: aspera_toggle_maximize(desk, w); break;
        case 3:
            w->desktop_id = (w->desktop_id + 1) % (desk->cols * desk->rows);
            break;
        case 4: aspera_close_window(desk, w); break;
        }
    } else {
        switch (idx) {
        case 0:
            aspera_create_window(desk, "Terminal [rc]", 100, 100, 480, 320, CLIENT_TERMINAL);
            break;
        case 1: aspera_cycle_theme(desk); break;
        case 2:
            aspera_switch_desktop(desk, (desk->cur_x + 1) % desk->cols, desk->cur_y);
            break;
        case 3:
            aspera_switch_desktop(desk, (desk->cur_x - 1 + desk->cols) % desk->cols, desk->cur_y);
            break;
        case 4: {
            // Simple horizontal tile of visible windows
            int cur_desk = desk->cur_y * desk->cols + desk->cur_x;
            int count = 0;
            for (Window *w = desk->head; w; w = w->next) {
                if (w->is_sticky || w->desktop_id == cur_desk) count++;
            }
            if (count > 0) {
                int tile_w = ASPERA_SCREEN_W / count;
                int idx = 0;
                for (Window *w = desk->head; w; w = w->next) {
                    if (w->is_sticky || w->desktop_id == cur_desk) {
                        w->x = idx * tile_w;
                        w->y = 0;
                        w->w = tile_w;
                        w->h = ASPERA_SCREEN_H;
                        w->is_shaded = false;
                        w->is_maximized = false;
                        idx++;
                    }
                }
            }
            break;
        }
        case 5: exit(0); break;
        }
    }
    desk->menu_open = false;
}

void aspera_handle_mouse_down(AsperaDesktop *desk, int mx, int my, int button) {
    desk->mouse_x = mx;
    desk->mouse_y = my;
    desk->mouse_buttons |= (1 << (button - 1));

    // Left Click (Button 1)
    if (button == 1) {
        // 0a. Aspera Shelf dropdown menu click (when open)
        if (desk->shelf_menu_open && mx >= 8 && mx < 192 && my >= 34 && my < 102) {
            desk->shelf_menu_open = false;
            desk->shelf_menu_hover = -1;
            if (my < 68) {
                // Open Notes
                Window *notes_win = NULL;
                for (Window *w = desk->head; w; w = w->next) {
                    if (w->client_type == CLIENT_NOTES) {
                        notes_win = w;
                        break;
                    }
                }
                if (notes_win) {
                    if (!notes_win->is_sticky) {
                        aspera_switch_desktop(desk, notes_win->desktop_id % desk->cols,
                                            notes_win->desktop_id / desk->cols);
                    }
                    aspera_raise_window(desk, notes_win);
                    aspera_focus_window(desk, notes_win);
                } else {
                    aspera_create_window(desk, "Notes", 60, 80, 560, 440, CLIENT_NOTES);
                }
            } else {
                // Cycle theme
                aspera_cycle_theme(desk);
            }
            return;
        }

        // 0b. Aspera Top Shelf Bar click (y in 0..31)
        if (my >= 0 && my < 32) {
            bool hit_win_btn = false;
            int cur_d = desk->cur_y * desk->cols + desk->cur_x;
            for (Window *w = desk->head; w; w = w->next) {
                if (!w->is_sticky && w->desktop_id != cur_d) continue;
                PartType p = aspera_hit_test_window(desk, w, mx, my);
                if (p == PART_BTN_CLOSE || p == PART_BTN_MAX ||
                    p == PART_BTN_SHADE || p == PART_BTN_STICK) {
                    hit_win_btn = true;
                    break;
                }
            }
            if (!hit_win_btn) {
                if (mx >= 8 && mx < 80 && my >= 4 && my < 28) {
                    desk->shelf_menu_open = !desk->shelf_menu_open;
                    desk->menu_open = false;
                    return;
                }
                if (mx >= 92 && mx < 236 && my >= 5 && my < 27 && ((mx - 92) % 36) < 32) {
                    int ws = (mx - 92) / 36;
                    desk->shelf_menu_open = false;
                    aspera_switch_desktop(desk, ws % desk->cols, ws / desk->cols);
                    return;
                }
                if (mx >= 250 && mx < 810 && my >= 4 && my < 28 && ((mx - 250) % 140) < 136) {
                    int slot = (mx - 250) / 140;
                    desk->shelf_menu_open = false;
                    int idx = 0;
                    for (Window *w = desk->tail; w; w = w->prev) {
                        if (idx == slot) {
                            if (!w->is_sticky) {
                                aspera_switch_desktop(desk, w->desktop_id % desk->cols,
                                                    w->desktop_id / desk->cols);
                            }
                            aspera_raise_window(desk, w);
                            aspera_focus_window(desk, w);
                            return;
                        }
                        idx++;
                    }
                }
                if (mx >= 964 && mx < 1016 && my >= 4 && my < 28) {
                    desk->shelf_clock_visible = !desk->shelf_clock_visible;
                    desk->shelf_menu_open = false;
                    return;
                }
                desk->shelf_menu_open = false;
            }
        }

        desk->shelf_menu_open = false;

        // If menu is open, handle click
        if (desk->menu_open) {
            if (mx >= desk->menu_x && mx < desk->menu_x + desk->menu_w &&
                my >= desk->menu_y && my < desk->menu_y + desk->menu_h) {
                int item_idx = (my - desk->menu_y - 4) / 20;
                if (item_idx >= 0 && item_idx < desk->num_menu_items) {
                    execute_menu_action(desk, item_idx);
                    return;
                }
            }
            desk->menu_open = false;
            return;
        }

        // Pager Click: switch workspace
        if (mx >= desk->pager_x && mx < desk->pager_x + desk->pager_w &&
            my >= desk->pager_y && my < desk->pager_y + desk->pager_h) {
            int cell_w = desk->pager_w / desk->cols;
            int cell_h = desk->pager_h / desk->rows;
            int click_col = (mx - desk->pager_x) / cell_w;
            int click_row = (my - desk->pager_y) / cell_h;
            aspera_switch_desktop(desk, click_col, click_row);
            return;
        }

        // Window interaction in Z-order
        int cur_desk = desk->cur_y * desk->cols + desk->cur_x;
        for (Window *w = desk->head; w; w = w->next) {
            if (!w->is_sticky && w->desktop_id != cur_desk) continue;
            PartType part = aspera_hit_test_window(desk, w, mx, my);
            if (part != PART_NONE) {
                aspera_raise_window(desk, w);
                aspera_focus_window(desk, w);

                switch (part) {
                case PART_TITLEBAR:
                    desk->is_dragging = true;
                    desk->drag_win = w;
                    desk->drag_start_mx = mx;
                    desk->drag_start_my = my;
                    desk->drag_start_wx = w->x;
                    desk->drag_start_wy = w->y;
                    desk->drag_start_ww = w->w;
                    desk->drag_start_wh = w->h;
                    break;
                case PART_SCROLLBAR_THUMB:
                    desk->is_scrolling = true;
                    desk->scroll_win = w;
                    w->vscroll.dragging = true;
                    w->vscroll.drag_offset = my - scrollbar_thumb_start(&w->vscroll);
                    break;
                case PART_SCROLLBAR_UP:
                    w->vscroll.val -= w->vscroll.page;
                    if (w->vscroll.val < w->vscroll.min) w->vscroll.val = w->vscroll.min;
                    break;
                case PART_SCROLLBAR_DOWN:
                    w->vscroll.val += w->vscroll.page;
                    if (w->vscroll.val > w->vscroll.max) w->vscroll.val = w->vscroll.max;
                    break;
                case PART_BTN_CLOSE:
                    if (w->client_type == CLIENT_NOTES && w->note.dirty) {
                        note_save(w);
                    }
                    aspera_close_window(desk, w);
                    break;
                case PART_BTN_MAX:
                    aspera_toggle_maximize(desk, w);
                    break;
                case PART_BTN_SHADE:
                    aspera_toggle_shade(desk, w);
                    break;
                case PART_BTN_STICK:
                    aspera_toggle_sticky(desk, w);
                    break;
                case PART_BORDER_N:
                case PART_BORDER_S:
                case PART_BORDER_W:
                case PART_BORDER_E:
                case PART_CORNER_NW:
                case PART_CORNER_NE:
                case PART_CORNER_SW:
                case PART_CORNER_SE:
                    desk->is_resizing = true;
                    desk->resize_part = part;
                    desk->drag_win = w;
                    desk->drag_start_mx = mx;
                    desk->drag_start_my = my;
                    desk->drag_start_wx = w->x;
                    desk->drag_start_wy = w->y;
                    desk->drag_start_ww = w->w;
                    desk->drag_start_wh = w->h;
                    break;
                case PART_CONTENT:
                    if (w->client_type == CLIENT_EDITOR) {
                        int cy = w->y + desk->theme.border_thickness + desk->theme.titlebar_height;
                        w->sam.cmd_focused = (my < cy + 96);
                    }
                    break;
                default:
                    break;
                }
                return;
            }
        }
    }

    // Middle Click (Button 2) - classic Plan 9 scrollbar jump/drag
    if (button == 2) {
        int cur_desk = desk->cur_y * desk->cols + desk->cur_x;
        for (Window *w = desk->head; w; w = w->next) {
            if (!w->is_sticky && w->desktop_id != cur_desk) continue;
            PartType part = aspera_hit_test_window(desk, w, mx, my);
            if (part == PART_SCROLLBAR_THUMB || part == PART_SCROLLBAR_UP || part == PART_SCROLLBAR_DOWN) {
                aspera_raise_window(desk, w);
                aspera_focus_window(desk, w);
                desk->is_scrolling = true;
                desk->scroll_win = w;
                w->vscroll.dragging = true;
                int thumb_len = scrollbar_thumb_length(&w->vscroll);
                w->vscroll.drag_offset = thumb_len / 2;
                w->vscroll.val = scrollbar_val_for_start(&w->vscroll, my - w->vscroll.drag_offset);
                return;
            }
        }
    }

    // Right Click (Button 3)
    if (button == 3) {
        desk->shelf_menu_open = false;
        int cur_desk = desk->cur_y * desk->cols + desk->cur_x;
        for (Window *w = desk->head; w; w = w->next) {
            if (!w->is_sticky && w->desktop_id != cur_desk) continue;
            PartType part = aspera_hit_test_window(desk, w, mx, my);
            if (desk->theme_idx == THEME_PLAN9 &&
                (part == PART_SCROLLBAR_THUMB || part == PART_SCROLLBAR_UP || part == PART_SCROLLBAR_DOWN)) {
                aspera_raise_window(desk, w);
                aspera_focus_window(desk, w);
                w->vscroll.val -= w->vscroll.page;
                if (w->vscroll.val < w->vscroll.min) w->vscroll.val = w->vscroll.min;
                return;
            }
            if (part != PART_NONE) {
                aspera_raise_window(desk, w);
                aspera_focus_window(desk, w);
                open_window_menu(desk, w, mx, my);
                return;
            }
        }
        // Click on background opens root menu
        open_root_menu(desk, mx, my);
    }
}

void aspera_handle_mouse_up(AsperaDesktop *desk, int mx, int my, int button) {
    desk->mouse_x = mx;
    desk->mouse_y = my;
    desk->mouse_buttons &= ~(1 << (button - 1));

    if (button == 1 || button == 2) {
        desk->is_dragging = false;
        desk->is_resizing = false;
        desk->drag_win = NULL;
        if (desk->is_scrolling && desk->scroll_win) {
            desk->scroll_win->vscroll.dragging = false;
            desk->is_scrolling = false;
            desk->scroll_win = NULL;
        }
    }
}

void aspera_handle_mouse_wheel(AsperaDesktop *desk, int delta_y) {
    if (delta_y == 0) return;
    int cur_desk = desk->cur_y * desk->cols + desk->cur_x;
    for (Window *w = desk->head; w; w = w->next) {
        if (!w->is_sticky && w->desktop_id != cur_desk) continue;
        if (desk->mouse_x >= w->x && desk->mouse_x < w->x + w->w &&
            desk->mouse_y >= w->y && desk->mouse_y < w->y + win_visible_h(w, &desk->theme)) {
            w->vscroll.val -= delta_y * 3;
            if (w->vscroll.val < w->vscroll.min) w->vscroll.val = w->vscroll.min;
            if (w->vscroll.val > w->vscroll.max) w->vscroll.val = w->vscroll.max;
            return;
        }
    }
}

// Keyboard Event Handlers
void aspera_handle_key_down(AsperaDesktop *desk, int key, char ch, bool alt_down, bool ctrl_down) {
    // 1. Global hotkeys
    if (alt_down) {
        if (key >= '1' && key <= '4') {
            int id = key - '1';
            aspera_switch_desktop(desk, id % desk->cols, id / desk->cols);
            return;
        }
        if (key == 9) { // Tab
            aspera_cycle_focus(desk);
            return;
        }
        if (key == 's' || key == 'S') {
            if (desk->focused) aspera_toggle_shade(desk, desk->focused);
            return;
        }
        if (key == 'p' || key == 'P') {
            if (desk->focused) aspera_toggle_sticky(desk, desk->focused);
            return;
        }
        if (key == 'm' || key == 'M') {
            if (desk->focused) aspera_toggle_maximize(desk, desk->focused);
            return;
        }
        if (key == 't' || key == 'T') {
            aspera_cycle_theme(desk);
            return;
        }
        if (key == 13) { // Alt + Enter -> New Terminal
            aspera_create_window(desk, "Terminal [rc]", 120, 120, 480, 320, CLIENT_TERMINAL);
            return;
        }
    }

    // 2. Focused window Notes client input
    if (desk->focused && desk->focused->client_type == CLIENT_NOTES) {
        NoteState *note = &desk->focused->note;
        if (ctrl_down && (key == 's' || key == 'S')) {
            note_save(desk->focused);
            return;
        }
        if (key == 8 || key == 127) {
            if (note->len > 0) {
                note->len--;
                while (note->len > 0 && ((unsigned char)note->buf[note->len] & 0xc0) == 0x80)
                    note->len--;
                note->buf[note->len] = '\0';
                note->dirty = true;
                note->save_error = false;
            }
            return;
        }
        if (key == 13 || key == 10) {
            if (note->len < (int)sizeof(note->buf) - 2) {
                note->buf[note->len++] = '\n';
                note->buf[note->len] = '\0';
                note->dirty = true;
                note->save_error = false;
            }
            return;
        }
        if (!ctrl_down && ch >= 32 && ch <= 126) {
            if (note->len < (int)sizeof(note->buf) - 2) {
                note->buf[note->len++] = ch;
                note->buf[note->len] = '\0';
                note->dirty = true;
                note->save_error = false;
            }
        }
        return;
    }

    // 3. Focused window Sam editor client input
    if (desk->focused && desk->focused->client_type == CLIENT_EDITOR) {
        SamState *sam = &desk->focused->sam;
        if (key == 9 && !alt_down) {
            sam->cmd_focused = !sam->cmd_focused;
            return;
        }
        if (ctrl_down && (key == 's' || key == 'S')) {
            strncpy(sam->cmd_input, "w", sizeof(sam->cmd_input) - 1);
            sam->cmd_len = 1;
            sam_execute_cmd(desk, desk->focused);
            return;
        }
        if (sam->cmd_focused) {
            if (key == 8 || key == 127) {
                if (sam->cmd_len > 0) {
                    sam->cmd_len--;
                    sam->cmd_input[sam->cmd_len] = '\0';
                }
                return;
            }
            if (key == 13 || key == 10) {
                sam_execute_cmd(desk, desk->focused);
                return;
            }
            if (!ctrl_down && ch >= 32 && ch <= 126) {
                if (sam->cmd_len < ASPERA_MAX_LINE_LEN - 2) {
                    sam->cmd_input[sam->cmd_len++] = ch;
                    sam->cmd_input[sam->cmd_len] = '\0';
                }
            }
        } else {
            if (key == 8 || key == 127) {
                if (sam->buf_len > 0) {
                    sam->buf_len--;
                    sam->buf[sam->buf_len] = '\0';
                    sam->dirty = true;
                    sam_sync_from_buffer(desk->focused);
                }
                return;
            }
            if (key == 13 || key == 10) {
                if (sam->buf_len < (int)sizeof(sam->buf) - 2) {
                    sam->buf[sam->buf_len++] = '\n';
                    sam->buf[sam->buf_len] = '\0';
                    sam->dirty = true;
                    sam_sync_from_buffer(desk->focused);
                }
                return;
            }
            if (!ctrl_down && ch >= 32 && ch <= 126) {
                if (sam->buf_len < (int)sizeof(sam->buf) - 2) {
                    sam->buf[sam->buf_len++] = ch;
                    sam->buf[sam->buf_len] = '\0';
                    sam->dirty = true;
                    sam_sync_from_buffer(desk->focused);
                }
            }
        }
        return;
    }

    // 4. Focused window terminal client input
    if (desk->focused && desk->focused->client_type == CLIENT_TERMINAL) {
        TerminalState *term = &desk->focused->term;

        if (key == 8 || key == 127) { // Backspace
            if (term->input_len > 0) {
                term->input_len--;
                term->input_buf[term->input_len] = '\0';
            }
            return;
        }
        if (key == 13 || key == 10) { // Enter
            terminal_execute(desk, desk->focused);
            return;
        }
        if (ch >= 32 && ch <= 126) {
            if (term->input_len < ASPERA_MAX_LINE_LEN - 2) {
                term->input_buf[term->input_len++] = ch;
                term->input_buf[term->input_len] = '\0';
            }
        }
    }
}

void aspera_handle_text_input(AsperaDesktop *desk, const char *text) {
    if (!desk->focused || !text) return;
    if (desk->focused->client_type == CLIENT_NOTES) {
        NoteState *note = &desk->focused->note;
        while (*text) {
            char ch = *text++;
            if (ch >= 32 && ch <= 126 && note->len < (int)sizeof(note->buf) - 2) {
                note->buf[note->len++] = ch;
                note->buf[note->len] = '\0';
                note->dirty = true;
                note->save_error = false;
            }
        }
        return;
    }
    if (desk->focused->client_type == CLIENT_EDITOR) {
        SamState *sam = &desk->focused->sam;
        bool buf_changed = false;
        while (*text) {
            char ch = *text++;
            if (ch >= 32 && ch <= 126) {
                if (sam->cmd_focused) {
                    if (sam->cmd_len < ASPERA_MAX_LINE_LEN - 2) {
                        sam->cmd_input[sam->cmd_len++] = ch;
                        sam->cmd_input[sam->cmd_len] = '\0';
                    }
                } else {
                    if (sam->buf_len < (int)sizeof(sam->buf) - 2) {
                        sam->buf[sam->buf_len++] = ch;
                        sam->buf[sam->buf_len] = '\0';
                        sam->dirty = true;
                        buf_changed = true;
                    }
                }
            }
        }
        if (buf_changed) {
            sam_sync_from_buffer(desk->focused);
        }
        return;
    }
    if (desk->focused->client_type != CLIENT_TERMINAL) return;
    TerminalState *term = &desk->focused->term;
    while (*text) {
        char ch = *text++;
        if (ch >= 32 && ch <= 126) {
            if (term->input_len < ASPERA_MAX_LINE_LEN - 2) {
                term->input_buf[term->input_len++] = ch;
                term->input_buf[term->input_len] = '\0';
            }
        }
    }
}

// Rendering System
void aspera_render(AsperaDesktop *desk) {
    const ThemeMetrics *t = &desk->theme;
    desk->frame_count++;
    bool is_p9 = (desk->theme_idx == THEME_PLAN9);

    for (Window *pw = desk->head; pw; pw = pw->next) {
        if (pw->client_type == CLIENT_TERMINAL) {
            terminal_poll_rc(desk, pw);
        } else if (pw->client_type == CLIENT_EDITOR) {
            sam_poll(pw);
        }
    }

    // 1. Background
    draw_fill_rect(desk->pixels, 0, 0, ASPERA_SCREEN_W, ASPERA_SCREEN_H, t->bg_desktop);

    if (!is_p9) {
        // Subtle desktop grid for Enlightenment aesthetics
        for (int y = 0; y < ASPERA_SCREEN_H; y += 32) {
            for (int x = 0; x < ASPERA_SCREEN_W; x += 32) {
                desk->pixels[y * ASPERA_SCREEN_W + x] = t->frame_inactive;
            }
        }
    }

    // 2. Render Windows in Z-order (tail to head = back to front)
    int cur_desk = desk->cur_y * desk->cols + desk->cur_x;
    for (Window *w = desk->tail; w; w = w->prev) {
        if (!w->is_sticky && w->desktop_id != cur_desk) continue;

        int bw = t->border_thickness;
        int th = t->titlebar_height;
        int bs = t->btn_size;
        int bsp = t->btn_spacing;
        int vh = win_visible_h(w, t);

        uint32_t frame_col = w->is_focused ? t->frame_active : t->frame_inactive;
        uint32_t title_bg = w->is_focused ? t->title_active_bg : t->title_inactive_bg;
        uint32_t title_fg = w->is_focused ? t->title_active_fg : t->title_inactive_fg;

        // Outer borders
        draw_border_rect(desk->pixels, w->x, w->y, w->w, vh, bw, frame_col);

        // 3D Bevels (non-Plan9 themes)
        if (!is_p9) {
            draw_fill_rect(desk->pixels, w->x, w->y, w->w, 1, t->bevel_light);
            draw_fill_rect(desk->pixels, w->x, w->y, 1, vh, t->bevel_light);
            draw_fill_rect(desk->pixels, w->x, w->y + vh - 1, w->w, 1, t->bevel_dark);
            draw_fill_rect(desk->pixels, w->x + w->w - 1, w->y, 1, vh, t->bevel_dark);
        }

        // Titlebar
        draw_fill_rect(desk->pixels, w->x + bw, w->y + bw, w->w - bw * 2, th, title_bg);
        if (is_p9) {
            // Plan 9 thin black dividing line below title tag
            draw_fill_rect(desk->pixels, w->x + bw, w->y + bw + th - 1, w->w - bw * 2, 1, 0x000000);
        }

        // Titlebar action buttons (Right aligned, rendered first so btn_x bounds title text)
        int btn_x = w->x + w->w - bw - bs - 2;
        int btn_y = w->y + bw + (th - bs) / 2;

        // Close button
        bool hover_close = (desk->hover_win == w && desk->hover_part == PART_BTN_CLOSE);
        uint32_t close_bg = is_p9 ? (hover_close ? 0x448888 : title_bg) : (hover_close ? 0xff4433 : t->btn_close_col);
        uint32_t close_fg = is_p9 ? (hover_close ? 0xffffff : 0x000000) : 0xffffff;
        draw_fill_rect(desk->pixels, btn_x, btn_y, bs, bs, close_bg);
        draw_border_rect(desk->pixels, btn_x, btn_y, bs, bs, 1, is_p9 ? 0x000000 : t->bevel_dark);
        draw_char(desk->pixels, btn_x + 3, btn_y, 'x', close_fg);
        btn_x -= (bs + bsp);

        // Maximize button
        if (!w->is_shaded) {
            bool hover_max = (desk->hover_win == w && desk->hover_part == PART_BTN_MAX);
            uint32_t max_bg = is_p9 ? (hover_max ? 0x448888 : title_bg) : (hover_max ? 0x44dd66 : t->btn_max_col);
            uint32_t max_fg = is_p9 ? (hover_max ? 0xffffff : 0x000000) : 0xffffff;
            draw_fill_rect(desk->pixels, btn_x, btn_y, bs, bs, max_bg);
            draw_border_rect(desk->pixels, btn_x, btn_y, bs, bs, 1, is_p9 ? 0x000000 : t->bevel_dark);
            draw_char(desk->pixels, btn_x + 3, btn_y, '+', max_fg);
            btn_x -= (bs + bsp);
        }

        // Shade button
        bool hover_shade = (desk->hover_win == w && desk->hover_part == PART_BTN_SHADE);
        uint32_t shade_bg = is_p9 ? (hover_shade ? 0x448888 : title_bg) : (hover_shade ? 0xffaa22 : t->btn_shade_col);
        uint32_t shade_fg = is_p9 ? (hover_shade ? 0xffffff : 0x000000) : 0xffffff;
        draw_fill_rect(desk->pixels, btn_x, btn_y, bs, bs, shade_bg);
        draw_border_rect(desk->pixels, btn_x, btn_y, bs, bs, 1, is_p9 ? 0x000000 : t->bevel_dark);
        draw_char(desk->pixels, btn_x + 3, btn_y, w->is_shaded ? 'v' : '^', shade_fg);
        btn_x -= (bs + bsp);

        // Sticky button
        bool hover_stick = (desk->hover_win == w && desk->hover_part == PART_BTN_STICK);
        uint32_t stick_bg = is_p9 ? (hover_stick ? 0x448888 : title_bg) : (hover_stick ? 0x44aaff : t->btn_stick_col);
        uint32_t stick_fg = is_p9 ? (hover_stick ? 0xffffff : 0x000000) : 0xffffff;
        draw_fill_rect(desk->pixels, btn_x, btn_y, bs, bs, stick_bg);
        draw_border_rect(desk->pixels, btn_x, btn_y, bs, bs, 1, is_p9 ? 0x000000 : t->bevel_dark);
        draw_char(desk->pixels, btn_x + 3, btn_y, w->is_sticky ? '*' : 'o', stick_fg);

        // Title text (strictly clipped between left margin and leftmost action button)
        int title_min_x = w->x + bw + 4;
        int title_max_x = btn_x - 6;
        int title_min_y = w->y + bw;
        int title_max_y = w->y + bw + th;
        if (title_max_x > title_min_x) {
            char title_disp[128];
            if (w->is_sticky) {
                snprintf(title_disp, sizeof(title_disp), "* %s", w->title);
            } else {
                snprintf(title_disp, sizeof(title_disp), "%s", w->title);
            }
            draw_text_clipped(desk->pixels, w->x + bw + 8, w->y + bw + (th - 14) / 2, title_disp, title_fg,
                              title_min_x, title_min_y, title_max_x, title_max_y);
        }

        // Window Client Area (if not shaded)
        if (!w->is_shaded) {
            int cx = w->x + bw;
            int cy = w->y + bw + th;
            int cw = w->w - bw * 2;
            int ch = w->h - bw * 2 - th;

            if (cw > 0 && ch > 0) {
                draw_fill_rect(desk->pixels, cx, cy, cw, ch,
                               (w->client_type == CLIENT_NOTES) ? 0x101820 : t->content_bg);
                if (!is_p9) {
                    draw_border_rect(desk->pixels, cx, cy, cw, ch, 1, t->bevel_dark);
                }

                win_layout_scrollbar(w, desk);
                if (w->client_type == CLIENT_NOTES) {
                    w->vscroll.visible = false;
                }

                int clip_min_x = cx + 2;
                int clip_max_x = cx + cw - 2;
                int clip_min_y = cy + 2;
                int clip_max_y = cy + ch - 2;

                int text_x_offset = 8;
                if (w->client_type != CLIENT_NOTES) {
                    if (is_p9) {
                        text_x_offset = w->vscroll.w + 6;
                        clip_min_x = cx + w->vscroll.w + 4;
                    } else {
                        clip_max_x = cx + cw - w->vscroll.w - 4;
                    }
                }

                if (clip_max_x > clip_min_x && clip_max_y > clip_min_y) {
                    if (w->client_type == CLIENT_NOTES) {
                        int bar_h = (ch < 24) ? ch : 24;
                        draw_fill_rect(desk->pixels, cx, cy, cw, bar_h, 0x253747);
                        draw_text_clipped(desk->pixels, cx + 8, cy + 5,
                                          "Aspera Notes  |  Ctrl+S saves", 0xffd382,
                                          clip_min_x, clip_min_y, clip_max_x, clip_max_y);
                        if (w->note.dirty) {
                            draw_text_clipped(desk->pixels, cx + cw - 16, cy + 5, "*", 0xffd382,
                                              clip_min_x, clip_min_y, clip_max_x, clip_max_y);
                        }
                        if (w->note.save_error) {
                            draw_text_clipped(desk->pixels, cx + cw - 112, cy + 5, "Save failed", 0xff7766,
                                              clip_min_x, clip_min_y, clip_max_x, clip_max_y);
                        }
                        int columns = (cw - 16) / 8;
                        int rows = (ch - 44) / 16;
                        if (columns > 0 && rows > 0) {
                            int row = 0, col = 0;
                            const char *s = w->note.buf;
                            for (int i = 0; s[i] != '\0' && i < w->note.len; i++) {
                                if (s[i] == '\n') {
                                    row++;
                                    col = 0;
                                } else if (s[i] != '\r') {
                                    if (col >= columns) {
                                        row++;
                                        col = 0;
                                    }
                                    col++;
                                }
                            }
                            int total_lines = row + 1;
                            int start = total_lines - rows;
                            if (start < 0) start = 0;
                            row = col = 0;
                            for (int i = 0; s[i] != '\0' && i < w->note.len; i++) {
                                if (s[i] == '\n') {
                                    row++;
                                    col = 0;
                                    if (row >= start + rows) break;
                                } else if (s[i] != '\r') {
                                    if (col >= columns) {
                                        row++;
                                        col = 0;
                                        if (row >= start + rows) break;
                                    }
                                    if (row >= start && row < start + rows) {
                                        draw_char_clipped(desk->pixels, cx + 8 + col * 8,
                                                          cy + 30 + (row - start) * 16,
                                                          s[i], 0xe4eaf0,
                                                          clip_min_x, clip_min_y, clip_max_x, clip_max_y);
                                    }
                                    col++;
                                }
                            }
                        }
                    } else if (w->client_type == CLIENT_TERMINAL) {
                        // Render terminal text lines starting from scroll offset
                        int line_y = cy + 6;
                        int start_line = w->vscroll.val;

                        for (int i = start_line; i < w->term.num_lines && line_y + 14 <= clip_max_y; i++) {
                            draw_text_clipped(desk->pixels, cx + text_x_offset, line_y, w->term.lines[i], t->content_fg,
                                              clip_min_x, clip_min_y, clip_max_x, clip_max_y);
                            line_y += 16;
                        }

                        // Current input line with prompt
                        if (line_y + 14 <= clip_max_y) {
                            char prompt[ASPERA_MAX_LINE_LEN + 16];
                            if (is_p9) {
                                snprintf(prompt, sizeof(prompt), "term%% %s", w->term.input_buf);
                            } else {
                                snprintf(prompt, sizeof(prompt), "harvey%% %s", w->term.input_buf);
                            }
                            uint32_t prompt_col = is_p9 ? 0x000000 : (w->is_focused ? 0xf0c040 : t->content_fg);
                            draw_text_clipped(desk->pixels, cx + text_x_offset, line_y, prompt, prompt_col,
                                              clip_min_x, clip_min_y, clip_max_x, clip_max_y);

                            // Blinking cursor
                            if (w->is_focused && ((desk->frame_count / 20) % 2 == 0)) {
                                int cursor_x = cx + text_x_offset + (int)strlen(prompt) * 8;
                                uint32_t cursor_col = is_p9 ? 0x000000 : 0xf0c040;
                                draw_fill_rect_clipped(desk->pixels, cursor_x, line_y + 1, 8, 14, cursor_col,
                                                       clip_min_x, clip_min_y, clip_max_x, clip_max_y);
                            }
                        }
                    } else if (w->client_type == CLIENT_EDITOR) {
                        SamState *sam = &w->sam;
                        int cmd_pane_h = (ch > 140) ? 92 : (ch / 2);
                        uint32_t cmd_bg = is_p9 ? 0xeaffff : 0x142230;
                        uint32_t cmd_fg = is_p9 ? 0x000088 : 0x90d4ff;
                        draw_fill_rect_clipped(desk->pixels, clip_min_x, cy + 1, clip_max_x - clip_min_x, cmd_pane_h,
                                               cmd_bg, clip_min_x, clip_min_y, clip_max_x, clip_max_y);
                        draw_fill_rect_clipped(desk->pixels, clip_min_x, cy + cmd_pane_h, clip_max_x - clip_min_x, 2,
                                               is_p9 ? 0x000000 : 0x3b5264, clip_min_x, clip_min_y, clip_max_x, clip_max_y);

                        int max_cmd_vis = (cmd_pane_h - 20) / 15;
                        if (max_cmd_vis < 1) max_cmd_vis = 1;
                        int cmd_start = sam->num_cmd_lines - max_cmd_vis;
                        if (cmd_start < 0) cmd_start = 0;
                        int cy_line = cy + 4;
                        for (int i = cmd_start; i < sam->num_cmd_lines && cy_line + 14 <= cy + cmd_pane_h - 16; i++) {
                            draw_text_clipped(desk->pixels, cx + text_x_offset, cy_line, sam->cmd_lines[i], cmd_fg,
                                              clip_min_x, clip_min_y, clip_max_x, cy + cmd_pane_h);
                            cy_line += 15;
                        }
                        char sam_prompt[ASPERA_MAX_LINE_LEN + 16];
                        snprintf(sam_prompt, sizeof(sam_prompt), "sam%% %s", sam->cmd_input);
                        uint32_t sp_col = sam->cmd_focused ? (is_p9 ? 0x000000 : 0xffd382) : cmd_fg;
                        draw_text_clipped(desk->pixels, cx + text_x_offset, cy_line, sam_prompt, sp_col,
                                          clip_min_x, clip_min_y, clip_max_x, cy + cmd_pane_h);
                        if (w->is_focused && sam->cmd_focused && ((desk->frame_count / 20) % 2 == 0)) {
                            int cur_x = cx + text_x_offset + (int)strlen(sam_prompt) * 8;
                            draw_fill_rect_clipped(desk->pixels, cur_x, cy_line + 1, 8, 13, sp_col,
                                                   clip_min_x, clip_min_y, clip_max_x, cy + cmd_pane_h);
                        }

                        int ed_y = cy + cmd_pane_h + 6;
                        int line_no = 1;
                        const char *p = sam->buf;
                        int last_x = cx + text_x_offset;
                        int last_y = ed_y;
                        while (*p && ed_y + 14 <= clip_max_y) {
                            const char *nl = strchr(p, '\n');
                            int len = nl ? (int)(nl - p) : (int)strlen(p);
                            char line_buf[160];
                            int n = snprintf(line_buf, sizeof(line_buf), "%2d: %.*s", line_no,
                                             len > 120 ? 120 : len, p);
                            if (n < 0) n = 0;
                            draw_text_clipped(desk->pixels, cx + text_x_offset, ed_y, line_buf, t->content_fg,
                                              clip_min_x, cy + cmd_pane_h + 2, clip_max_x, clip_max_y);
                            last_x = cx + text_x_offset + (int)strlen(line_buf) * 8;
                            last_y = ed_y;
                            ed_y += 16;
                            line_no++;
                            if (!nl) break;
                            p = nl + 1;
                        }
                        if (w->is_focused && !sam->cmd_focused && ((desk->frame_count / 20) % 2 == 0)) {
                            draw_fill_rect_clipped(desk->pixels, last_x, last_y + 1, 8, 14,
                                                   is_p9 ? 0x000000 : 0xffd382,
                                                   clip_min_x, cy + cmd_pane_h + 2, clip_max_x, clip_max_y);
                        }
                    } else if (w->client_type == CLIENT_CLOCK) {
                        time_t now = time(NULL);
                        struct tm *lt = localtime(&now);
                        char time_buf[32];
                        strftime(time_buf, sizeof(time_buf), "%H:%M:%S", lt);
                        if (cy + 24 + 14 <= clip_max_y) {
                            draw_text_clipped(desk->pixels, cx + (is_p9 ? text_x_offset + 16 : 32), cy + 24, time_buf,
                                              is_p9 ? 0x000000 : 0x22aaff, clip_min_x, clip_min_y, clip_max_x, clip_max_y);
                        }
                        char date_buf[32];
                        strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %a", lt);
                        if (cy + 50 + 14 <= clip_max_y) {
                            draw_text_clipped(desk->pixels, cx + (is_p9 ? text_x_offset + 8 : 24), cy + 50, date_buf,
                                              t->content_fg, clip_min_x, clip_min_y, clip_max_x, clip_max_y);
                        }
                        if (cy + 76 + 14 <= clip_max_y) {
                            draw_text_clipped(desk->pixels, cx + (is_p9 ? text_x_offset : 16), cy + 76, "Plan 9 / Harvey",
                                              is_p9 ? 0x444444 : 0x8898a8, clip_min_x, clip_min_y, clip_max_x, clip_max_y);
                        }
                    }
                }

                // Draw interactive scrollbar
                draw_scrollbar(desk, w);
            }
        }
    }

    // 3. Enlightenment 2D Workspace Pager (Bottom-right)
    int px = desk->pager_x;
    int py = desk->pager_y;
    int pw = desk->pager_w;
    int ph = desk->pager_h;
    int cell_w = pw / desk->cols;
    int cell_h = ph / desk->rows;

    if (is_p9) {
        draw_fill_rect(desk->pixels, px, py, pw, ph, 0x777777);
        draw_border_rect(desk->pixels, px, py, pw, ph, 1, 0x000000);
    } else {
        draw_fill_rect(desk->pixels, px, py, pw, ph, 0x141820);
        draw_border_rect(desk->pixels, px, py, pw, ph, 2, t->frame_active);
    }

    for (int r = 0; r < desk->rows; r++) {
        for (int c = 0; c < desk->cols; c++) {
            int cx = px + c * cell_w;
            int cy = py + r * cell_h;
            int desk_id = r * desk->cols + c;

            // Active cell highlight
            if (c == desk->cur_x && r == desk->cur_y) {
                draw_fill_rect(desk->pixels, cx + 1, cy + 1, cell_w - 2, cell_h - 2, is_p9 ? 0x55aaaa : 0x263444);
                draw_border_rect(desk->pixels, cx, cy, cell_w, cell_h, 1, is_p9 ? 0x000000 : 0xf0c040);
            } else {
                if (is_p9) draw_fill_rect(desk->pixels, cx + 1, cy + 1, cell_w - 2, cell_h - 2, 0x9eeeee);
                draw_border_rect(desk->pixels, cx, cy, cell_w, cell_h, 1, is_p9 ? 0x000000 : t->frame_inactive);
            }

            // Draw miniature proxy rectangles of windows in this desktop
            for (Window *w = desk->tail; w; w = w->prev) {
                if (w->is_sticky || w->desktop_id == desk_id) {
                    int mw = (w->w * cell_w) / ASPERA_SCREEN_W;
                    int mh = (win_visible_h(w, t) * cell_h) / ASPERA_SCREEN_H;
                    if (mw < 3) mw = 3;
                    if (mh < 3) mh = 3;

                    int mx = cx + (w->x * cell_w) / ASPERA_SCREEN_W;
                    int my = cy + (w->y * cell_h) / ASPERA_SCREEN_H;

                    uint32_t mini_col = w->is_focused ? t->title_active_bg : t->title_inactive_bg;
                    draw_fill_rect(desk->pixels, mx, my, mw, mh, mini_col);
                    draw_border_rect(desk->pixels, mx, my, mw, mh, 1, is_p9 ? 0x000000 : t->bevel_light);
                }
            }
        }
    }

    // 4. Aspera Top Shelf Bar (y = 0..31) & Shelf Menu
    draw_fill_rect(desk->pixels, 0, 0, ASPERA_SCREEN_W, 32, 0x1b2631);
    draw_fill_rect(desk->pixels, 0, 31, ASPERA_SCREEN_W, 1, 0x3b5264);
    draw_fill_rect(desk->pixels, 8, 4, 72, 24, desk->shelf_menu_open ? 0x3d5468 : 0x314354);
    draw_border_rect(desk->pixels, 8, 4, 72, 24, 1, 0xb87d31);
    draw_text(desk->pixels, 16, 9, "Aspera", 0xffd382);

    for (int i = 0; i < 4; i++) {
        int wx = 92 + i * 36;
        uint32_t wbg = (i == cur_desk) ? 0x9a6e2e : 0x253440;
        char wlabel[2] = { (char)('1' + i), '\0' };
        draw_fill_rect(desk->pixels, wx, 5, 32, 22, wbg);
        draw_border_rect(desk->pixels, wx, 5, 32, 22, 1, 0x536675);
        draw_text(desk->pixels, wx + 12, 8, wlabel, 0xffffff);
    }

    int slot = 0;
    for (Window *w = desk->tail; w && slot < 4; w = w->prev) {
        int bx = 250 + slot * 140;
        uint32_t bbg = w->is_focused ? 0x526b7d : 0x314354;
        char label[16];
        strncpy(label, w->title, 15);
        label[15] = '\0';
        draw_fill_rect(desk->pixels, bx, 4, 136, 24, bbg);
        draw_border_rect(desk->pixels, bx, 4, 136, 24, 1, 0x536675);
        draw_text(desk->pixels, bx + 6, 9, label, 0xe4eaf0);
        slot++;
    }

    if (desk->shelf_clock_visible) {
        time_t now = time(NULL);
        struct tm *lt = localtime(&now);
        char clk[16];
        if (lt && strftime(clk, sizeof(clk), "%H:%M", lt) == 5) {
            draw_text(desk->pixels, 972, 9, clk, 0xe4eaf0);
        } else {
            draw_text(desk->pixels, 972, 9, "--:--", 0xe4eaf0);
        }
    }

    if (desk->shelf_menu_open) {
        draw_fill_rect(desk->pixels, 8, 34, 184, 68, 0x263542);
        draw_border_rect(desk->pixels, 8, 34, 184, 68, 1, 0xb87d31);
        if (desk->shelf_menu_hover == 0) {
            draw_fill_rect(desk->pixels, 9, 35, 182, 33, 0x3a5266);
        } else if (desk->shelf_menu_hover == 1) {
            draw_fill_rect(desk->pixels, 9, 69, 182, 32, 0x3a5266);
        }
        draw_fill_rect(desk->pixels, 9, 68, 182, 1, 0x526b7d);
        draw_text(desk->pixels, 20, 44, "Open Notes", 0xe4eaf0);
        draw_text(desk->pixels, 20, 78, "Cycle theme", 0xe4eaf0);
    }

    // 5. Context Menu (if open)
    if (desk->menu_open) {
        int mx = desk->menu_x;
        int my = desk->menu_y;
        int mw = desk->menu_w;
        int mh = desk->menu_h;

        if (is_p9) {
            // Plan 9 Rio Authentic Menu: Pure white box, 1px black outline, no drop shadow
            draw_fill_rect(desk->pixels, mx, my, mw, mh, 0xffffff);
            draw_border_rect(desk->pixels, mx, my, mw, mh, 1, 0x000000);

            for (int i = 0; i < desk->num_menu_items; i++) {
                int item_y = my + 4 + i * 20;
                if (i == desk->menu_hover_idx) {
                    draw_fill_rect(desk->pixels, mx + 1, item_y - 2, mw - 2, 18, 0x55aaaa);
                    draw_text(desk->pixels, mx + 8, item_y, desk->menu_items[i], 0xffffff);
                } else {
                    draw_text(desk->pixels, mx + 8, item_y, desk->menu_items[i], 0x000000);
                }
            }
        } else {
            // Drop shadow
            draw_fill_rect(desk->pixels, mx + 4, my + 4, mw, mh, 0x05080c);
            // Menu body
            draw_fill_rect(desk->pixels, mx, my, mw, mh, 0x1e242c);
            draw_border_rect(desk->pixels, mx, my, mw, mh, 1, t->frame_active);

            for (int i = 0; i < desk->num_menu_items; i++) {
                int item_y = my + 4 + i * 20;
                if (i == desk->menu_hover_idx) {
                    draw_fill_rect(desk->pixels, mx + 2, item_y - 2, mw - 4, 18, t->title_active_bg);
                    draw_text(desk->pixels, mx + 8, item_y, desk->menu_items[i], t->title_active_fg);
                } else {
                    draw_text(desk->pixels, mx + 8, item_y, desk->menu_items[i], 0xd0e0f0);
                }
            }
        }
    }

    // 6. Software Mouse Cursor
    draw_cursor(desk, desk->pixels, desk->mouse_x, desk->mouse_y);
}

int aspera_save_ppm(AsperaDesktop *desk, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    fprintf(f, "P6\n%d %d\n255\n", ASPERA_SCREEN_W, ASPERA_SCREEN_H);
    uint8_t row[ASPERA_SCREEN_W * 3];

    for (int j = 0; j < ASPERA_SCREEN_H; j++) {
        uint32_t *src = desk->pixels + j * ASPERA_SCREEN_W;
        uint8_t *dst = row;
        for (int i = 0; i < ASPERA_SCREEN_W; i++) {
            uint32_t c = src[i];
            *dst++ = (uint8_t)((c >> 16) & 0xFF);
            *dst++ = (uint8_t)((c >> 8) & 0xFF);
            *dst++ = (uint8_t)(c & 0xFF);
        }
        fwrite(row, 1, sizeof(row), f);
    }
    fclose(f);
    return 0;
}
