#include <u.h>
#include <libc.h>
#include <thread.h>
#include <draw.h>
#include <complete.h>
#include "undo.h"
#include "font8x14.h"

#define ASPERA_SCREEN_W 1024
#define ASPERA_SCREEN_H 768
#define ASPERA_MAX_WINDOWS 32
#define ASPERA_MAX_TERMINAL_LINES 128
#define ASPERA_MAX_LINE_LEN 96
#define ASPERA_SNAP_DIST 16

typedef enum {
    PART_NONE = 0,
    PART_TITLEBAR,
    PART_BTN_CLOSE,
    PART_BTN_MAX,
    PART_BTN_SHADE,
    PART_BTN_STICK,
    PART_BORDER_N,
    PART_BORDER_S,
    PART_BORDER_W,
    PART_BORDER_E,
    PART_CORNER_NW,
    PART_CORNER_NE,
    PART_CORNER_SW,
    PART_CORNER_SE,
    PART_CONTENT,
    PART_SCROLLBAR_TRACK,
    PART_SCROLLBAR_THUMB,
    PART_SCROLLBAR_UP,
    PART_SCROLLBAR_DOWN,
    PART_PAGER_CELL,
    PART_MENU_ITEM
} PartType;

typedef enum {
    THEME_E16 = 0,
    THEME_WIN95,
    THEME_NEXT,
    THEME_PLAN9,
    THEME_COUNT
} ThemeType;

typedef enum {
    CLIENT_TERMINAL = 0,
    CLIENT_NOTES,
    CLIENT_GENERIC
} ClientType;

typedef struct {
    const char *name;
    int border_thickness;
    int titlebar_height;
    int btn_size;
    int btn_spacing;
    u32 bg_desktop;
    u32 title_active_bg;
    u32 title_active_fg;
    u32 title_inactive_bg;
    u32 title_inactive_fg;
    u32 frame_active;
    u32 frame_inactive;
    u32 bevel_light;
    u32 bevel_dark;
    u32 content_bg;
    u32 content_fg;
    u32 btn_close_col;
    u32 btn_max_col;
    u32 btn_shade_col;
    u32 btn_stick_col;
} ThemeMetrics;

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
        .name = "next-dark",
        .border_thickness = 3,
        .titlebar_height = 22,
        .btn_size = 14,
        .btn_spacing = 3,
        .bg_desktop = 0x2b2b2b,
        .title_active_bg = 0x000000,
        .title_active_fg = 0xffffff,
        .title_inactive_bg = 0x555555,
        .title_inactive_fg = 0xaaaaaa,
        .frame_active = 0x777777,
        .frame_inactive = 0x444444,
        .bevel_light = 0x999999,
        .bevel_dark = 0x111111,
        .content_bg = 0x1c1c1c,
        .content_fg = 0xe0e0e0,
        .btn_close_col = 0x333333,
        .btn_max_col = 0x333333,
        .btn_shade_col = 0x333333,
        .btn_stick_col = 0x333333,
    },
    [THEME_PLAN9] = {
        .name = "plan9-rio",
        .border_thickness = 2,
        .titlebar_height = 20,
        .btn_size = 14,
        .btn_spacing = 2,
        .bg_desktop = 0x777777,
        .title_active_bg = 0xcccc99,
        .title_active_fg = 0x000000,
        .title_inactive_bg = 0xeeeeee,
        .title_inactive_fg = 0x555555,
        .frame_active = 0x888844,
        .frame_inactive = 0x999999,
        .bevel_light = 0x999999,
        .bevel_dark = 0x000000,
        .content_bg = 0xffffff,
        .content_fg = 0x000000,
        .btn_close_col = 0xeeeeee,
        .btn_max_col = 0xeeeeee,
        .btn_shade_col = 0xeeeeee,
        .btn_stick_col = 0xeeeeee,
    }
};

typedef struct {
    int visible;
    int is_vertical;
    int x, y, w, h;
    int min, max, page, val;
    int dragging;
    int hovered;
    int drag_offset;
} AsperaScrollbar;

typedef struct {
    int rc_pid;
    int rc_in_fd;
    int rc_out_fd;
    char lines[ASPERA_MAX_TERMINAL_LINES][ASPERA_MAX_LINE_LEN];
    int num_lines;
    char input_buf[ASPERA_MAX_LINE_LEN];
    int input_len;
    char ring[16384];
    volatile int ring_head;
    volatile int ring_tail;
    char partial[ASPERA_MAX_LINE_LEN];
    int partial_len;
} TerminalState;

typedef struct {
    char buf[8192];
    int len;
    int dirty;
    int save_error;
    char path[256];
    UndoStack *undo;
} NoteState;

typedef struct Window Window;
struct Window {
    int id;
    char title[64];
    int x, y, w, h;
    int saved_x, saved_y, saved_w, saved_h;
    int is_shaded;
    int is_sticky;
    int is_maximized;
    int is_focused;
    int desktop_id;
    ClientType client_type;
    TerminalState term;
    NoteState note;
    AsperaScrollbar vscroll;
    Window *prev;
    Window *next;
};

typedef struct {
    int cols, rows;
    int cur_x, cur_y;
    Window *head;
    Window *tail;
    Window *focused;
    int next_win_id;

    ThemeType theme_idx;
    ThemeMetrics theme;

    int is_dragging;
    int is_resizing;
    Window *drag_win;
    int drag_start_mx, drag_start_my;
    int drag_start_wx, drag_start_wy;
    int drag_start_ww, drag_start_wh;
    PartType resize_part;

    int is_scrolling;
    Window *scroll_win;
    int edge_flip_cooldown;

    PartType hover_part;
    Window *hover_win;

    int pager_x, pager_y, pager_w, pager_h;

    int shelf_menu_open;
    int shelf_clock_visible;
    char shelf_clock_text[16];
    int shelf_menu_hover;

    int menu_open;
    int menu_x, menu_y, menu_w, menu_h;
    char menu_items[8][32];
    int num_menu_items;
    int menu_hover_idx;

    int mouse_x, mouse_y;
    int mouse_buttons;
    int frame_count;

    u32 *pixels;
    u8 *prior;
    int have_prior;
} AsperaDesktop;

static AsperaDesktop g_desk;

static inline int win_visible_h(const Window *w, const ThemeMetrics *t) {
    if (w->is_shaded) {
        return t->titlebar_height + t->border_thickness * 2;
    }
    return w->h;
}

static void draw_fill_rect(u32 *pixels, int x0, int y0, int w, int h, u32 color) {
    if (!pixels || w <= 0 || h <= 0) return;
    int x1 = x0 + w, y1 = y0 + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > ASPERA_SCREEN_W) x1 = ASPERA_SCREEN_W;
    if (y1 > ASPERA_SCREEN_H) y1 = ASPERA_SCREEN_H;
    for (int y = y0; y < y1; y++) {
        u32 *row = pixels + y * ASPERA_SCREEN_W;
        for (int x = x0; x < x1; x++) {
            row[x] = color;
        }
    }
}

static void draw_border_rect(u32 *pixels, int x, int y, int w, int h, int thick, u32 color) {
    if (!pixels || w <= 0 || h <= 0 || thick <= 0) return;
    draw_fill_rect(pixels, x, y, w, thick, color);
    draw_fill_rect(pixels, x, y + h - thick, w, thick, color);
    draw_fill_rect(pixels, x, y + thick, thick, h - thick * 2, color);
    draw_fill_rect(pixels, x + w - thick, y + thick, thick, h - thick * 2, color);
}

static void draw_bevel_rect(u32 *pixels, int x, int y, int w, int h, u32 bg, u32 light, u32 dark, int sunken) {
    draw_fill_rect(pixels, x, y, w, h, bg);
    u32 top_left = sunken ? dark : light;
    u32 bot_right = sunken ? light : dark;
    draw_fill_rect(pixels, x, y, w, 1, top_left);
    draw_fill_rect(pixels, x, y, 1, h, top_left);
    draw_fill_rect(pixels, x, y + h - 1, w, 1, bot_right);
    draw_fill_rect(pixels, x + w - 1, y, 1, h, bot_right);
}

static void draw_char_clipped(u32 *pixels, int x, int y, char c, u32 color,
                              int min_x, int min_y, int max_x, int max_y) {
    if (!pixels || c < 32 || c > 126) return;
    int glyph = c - 32;
    for (int row = 0; row < 14; row++) {
        int py = y + row;
        if (py < min_y || py >= max_y || py < 0 || py >= ASPERA_SCREEN_H) continue;
        u8 bits = font8x14_data[glyph][row];
        u32 *scan = pixels + py * ASPERA_SCREEN_W;
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            if (px >= min_x && px < max_x && px >= 0 && px < ASPERA_SCREEN_W) {
                if (bits & (0x80 >> col)) scan[px] = color;
            }
        }
    }
}

static void draw_text_clipped(u32 *pixels, int x, int y, const char *str, u32 color,
                              int min_x, int min_y, int max_x, int max_y) {
    if (!pixels || !str) return;
    int cx = x;
    while (*str) {
        if (cx + 8 > min_x && cx < max_x) {
            draw_char_clipped(pixels, cx, y, *str, color, min_x, min_y, max_x, max_y);
        }
        cx += 8;
        str++;
    }
}

static void draw_fill_rect_clipped(u32 *pixels, int x0, int y0, int w, int h, u32 color,
                                   int min_x, int min_y, int max_x, int max_y) {
    if (!pixels || w <= 0 || h <= 0) return;
    int x1 = x0 + w, y1 = y0 + h;
    if (x0 < min_x) x0 = min_x;
    if (y0 < min_y) y0 = min_y;
    if (x1 > max_x) x1 = max_x;
    if (y1 > max_y) y1 = max_y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > ASPERA_SCREEN_W) x1 = ASPERA_SCREEN_W;
    if (y1 > ASPERA_SCREEN_H) y1 = ASPERA_SCREEN_H;
    for (int y = y0; y < y1; y++) {
        u32 *row = pixels + y * ASPERA_SCREEN_W;
        for (int x = x0; x < x1; x++) {
            row[x] = color;
        }
    }
}

static void draw_char(u32 *pixels, int x, int y, char c, u32 color) {
    draw_char_clipped(pixels, x, y, c, color, 0, 0, ASPERA_SCREEN_W, ASPERA_SCREEN_H);
}

static void draw_text(u32 *pixels, int x, int y, const char *str, u32 color) {
    draw_text_clipped(pixels, x, y, str, color, 0, 0, ASPERA_SCREEN_W, ASPERA_SCREEN_H);
}

static void draw_cursor(const AsperaDesktop *desk, u32 *pixels, int mx, int my) {
    if (!pixels) return;
    if (desk->theme_idx == THEME_PLAN9) {
        static const u16 p9_arrow_mask[16] = {
            0xFFFF, 0xFFFF, 0xFFFE, 0xFFFC, 0xFFF0, 0xFFF0, 0xFFF8, 0xFFFC,
            0xFFFE, 0xFFFF, 0xFFFE, 0xFFFC, 0xF3F8, 0xF1F0, 0xE0E0, 0xC040
        };
        static const u16 p9_arrow_body[16] = {
            0x0000, 0x7FFE, 0x7FFC, 0x7FF0, 0x7FE0, 0x7FE0, 0x7FF0, 0x7FF8,
            0x7FFC, 0x7FFE, 0x7FFC, 0x73F8, 0x61F0, 0x60E0, 0x4040, 0x0000
        };
        static const u16 p9_box_mask[16] = {
            0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xF81F, 0xF81F, 0xF81F,
            0xF81F, 0xF81F, 0xF81F, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF
        };
        static const u16 p9_box_body[16] = {
            0x0000, 0x7FFE, 0x7FFE, 0x7FFE, 0x700E, 0x700E, 0x700E, 0x700E,
            0x700E, 0x700E, 0x700E, 0x700E, 0x7FFE, 0x7FFE, 0x7FFE, 0x0000
        };
        static const u16 p9_sight_mask[16] = {
            0x1FF8, 0x3FFC, 0x7FFE, 0xFBDF, 0xF3CF, 0xE3C7, 0xFFFF, 0xFFFF,
            0xFFFF, 0xFFFF, 0xE3C7, 0xF3CF, 0x7BDF, 0x7FFE, 0x3FFC, 0x1FF8
        };
        static const u16 p9_sight_body[16] = {
            0x0000, 0x0FF0, 0x318C, 0x2184, 0x4182, 0x4182, 0x4182, 0x7FFE,
            0x7FFE, 0x4182, 0x4182, 0x4182, 0x2184, 0x318C, 0x0FF0, 0x0000
        };

        const u16 *mask = p9_arrow_mask;
        const u16 *body = p9_arrow_body;
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
                u32 *row = pixels + py * ASPERA_SCREEN_W;
                for (int c = 0; c < 16; c++) {
                    int px = mx + c;
                    if (px >= 0 && px < ASPERA_SCREEN_W) {
                        u16 bit = 0x8000 >> c;
                        if (mask[r] & bit) {
                            if (body[r] & bit) {
                                row[px] = 0xffffff;
                            } else {
                                row[px] = 0x000000;
                            }
                        }
                    }
                }
            }
        }
        return;
    }

    // Default authentic pointer arrow with black outline and white interior
    static const u16 cursor_mask[16] = {
        0x8000, 0xC000, 0xE000, 0xF000,
        0xF800, 0xFC00, 0xFE00, 0xFF00,
        0xF800, 0xD800, 0x8C00, 0x0C00,
        0x0600, 0x0600, 0x0300, 0x0000
    };
    static const u16 cursor_fill[16] = {
        0x0000, 0x4000, 0x6000, 0x7000,
        0x7800, 0x7C00, 0x7E00, 0x7800,
        0x6800, 0x4800, 0x0400, 0x0400,
        0x0200, 0x0200, 0x0000, 0x0000
    };

    for (int r = 0; r < 16; r++) {
        int py = my + r;
        if (py >= 0 && py < ASPERA_SCREEN_H) {
            u32 *row = pixels + py * ASPERA_SCREEN_W;
            for (int c = 0; c < 16; c++) {
                int px = mx + c;
                if (px >= 0 && px < ASPERA_SCREEN_W) {
                    u16 bit = 0x8000 >> c;
                    if (cursor_mask[r] & bit) {
                        if (cursor_fill[r] & bit) {
                            row[px] = 0xffffff;
                        } else {
                            row[px] = 0x000000;
                        }
                    }
                }
            }
        }
    }
}

/* Shelf Static Protocol & Direct Hit Routing */
static void shelf_start(void) {
    int fd = open("/mnt/shelf/view/protocol", OREAD);
    if (fd < 0) {
        int dfd = create("/mnt/shelf/view", OREAD, DMDIR|0777);
        if (dfd >= 0) close(dfd);
        fd = create("/mnt/shelf/view/protocol", OWRITE, 0666);
        if (fd >= 0) {
            write(fd, "o9shelf 1\n", 10);
            close(fd);
        }
    } else {
        close(fd);
    }
}

/* Terminal Implementation */
static void terminal_append_line(TerminalState *term, const char *line) {
    if (term->num_lines >= ASPERA_MAX_TERMINAL_LINES) {
        memmove(&term->lines[0], &term->lines[1], sizeof(term->lines[0]) * (ASPERA_MAX_TERMINAL_LINES - 1));
        term->num_lines = ASPERA_MAX_TERMINAL_LINES - 1;
    }
    strncpy(term->lines[term->num_lines], line, ASPERA_MAX_LINE_LEN - 1);
    term->lines[term->num_lines][ASPERA_MAX_LINE_LEN - 1] = '\0';
    term->num_lines++;
}

static void terminal_reader_proc(void *arg) {
    Window *w = (Window*)arg;
    int fd = w->term.rc_out_fd;
    char buf[256];
    while (fd >= 0) {
        int n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            int next = (w->term.ring_head + 1) % sizeof(w->term.ring);
            if (next != w->term.ring_tail) {
                w->term.ring[w->term.ring_head] = buf[i];
                w->term.ring_head = next;
            }
        }
    }
}

static void terminal_start(Window *w) {
    w->term.rc_pid = -1;
    w->term.rc_in_fd = -1;
    w->term.rc_out_fd = -1;
    w->term.num_lines = 0;
    w->term.input_len = 0;
    w->term.input_buf[0] = '\0';
    w->term.partial_len = 0;
    w->term.partial[0] = '\0';
    w->term.ring_head = 0;
    w->term.ring_tail = 0;

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) return;

    int pid = rfork(RFPROC|RFFDG|RFNOTEG|RFNOWAIT);
    if (pid == 0) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        dup(in_pipe[0], 0);
        dup(out_pipe[1], 1);
        dup(out_pipe[1], 2);
        if (in_pipe[0] > 2) close(in_pipe[0]);
        if (out_pipe[1] > 2) close(out_pipe[1]);
        execl("/bin/rc", "rc", "-i", nil);
        _exits("rc exec failed");
    }
    close(in_pipe[0]);
    close(out_pipe[1]);

    w->term.rc_pid = pid;
    w->term.rc_in_fd = in_pipe[1];
    w->term.rc_out_fd = out_pipe[0];

    terminal_append_line(&w->term, "Aspera rc  |  native Plan 9 terminal");
    proccreate(terminal_reader_proc, w, 32768);
}

static int terminal_poll(AsperaDesktop *desk, Window *w) {
    int changed = 0;
    char out_buf[1024];
    int out_n = 0;

    while (w->term.ring_tail != w->term.ring_head && out_n < sizeof(out_buf) - 1) {
        out_buf[out_n++] = w->term.ring[w->term.ring_tail];
        w->term.ring_tail = (w->term.ring_tail + 1) % sizeof(w->term.ring);
    }

    if (out_n > 0) {
        out_buf[out_n] = '\0';
        print("[RC] %s\n", out_buf);
        out_buf[out_n] = '\0';
        if (strstr(out_buf, "VMTERMOK")) {
            print("ASPERA-TERM-COMMAND PASS\n");
        }
        if (strstr(out_buf, "REOPENOK")) {
            print("ASPERA-TERM-REOPEN PASS\n");
        }
        for (int i = 0; i < out_n; i++) {
            char c = out_buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                w->term.partial[w->term.partial_len] = '\0';
                terminal_append_line(&w->term, w->term.partial);
                w->term.partial_len = 0;
                w->term.partial[0] = '\0';
                changed = 1;
            } else if (c >= 32 && c <= 126) {
                if (w->term.partial_len < ASPERA_MAX_LINE_LEN - 2) {
                    w->term.partial[w->term.partial_len++] = c;
                    w->term.partial[w->term.partial_len] = '\0';
                } else {
                    w->term.partial[w->term.partial_len] = '\0';
                    terminal_append_line(&w->term, w->term.partial);
                    w->term.partial_len = 0;
                    w->term.partial[w->term.partial_len++] = c;
                    w->term.partial[w->term.partial_len] = '\0';
                    changed = 1;
                }
            } else if (c == '\t') {
                for (int s = 0; s < 4 && w->term.partial_len < ASPERA_MAX_LINE_LEN - 2; s++) {
                    w->term.partial[w->term.partial_len++] = ' ';
                }
                w->term.partial[w->term.partial_len] = '\0';
            }
        }
    }
    return changed;
}

static void terminal_send(Window *w, const char *cmd) {
    if (!w || w->term.rc_in_fd < 0 || !cmd) return;
    int len = strlen(cmd);
    write(w->term.rc_in_fd, cmd, len);
    write(w->term.rc_in_fd, "\n", 1);
}

/* Notes Implementation with Rio Undo Integration */
static void note_load(Window *w, const char *path) {
    strncpy(w->note.path, path, sizeof(w->note.path) - 1);
    w->note.path[sizeof(w->note.path) - 1] = '\0';
    w->note.len = 0;
    w->note.buf[0] = '\0';
    w->note.dirty = 0;
    w->note.save_error = 0;
    w->note.undo = undostackcreate(100);

    int fd = open(path, OREAD);
    if (fd >= 0) {
        int n = read(fd, w->note.buf, sizeof(w->note.buf) - 1);
        if (n > 0) {
            w->note.len = n;
            w->note.buf[n] = '\0';
        }
        close(fd);
    }
}

static int note_save(Window *w) {
    int fd = create(w->note.path, OWRITE, 0666);
    if (fd < 0) {
        w->note.save_error = 1;
        return 0;
    }
    int n = write(fd, w->note.buf, w->note.len);
    close(fd);
    if (n == w->note.len) {
        w->note.dirty = 0;
        w->note.save_error = 0;
        return 1;
    }
    w->note.save_error = 1;
    return 0;
}

static int note_key(Window *w, int key, int mod) {
    NoteState *note = &w->note;
    if (mod & 2) { // Ctrl key
        if (key == 115 || key == 19) { // Ctrl+S
            return note_save(w);
        }
        if (key == 122 || key == 26) { // Ctrl+Z: Undo
            Rune *u_data = nil;
            int u_len = 0, u_pos = 0, u_type = 0;
            if (undocanundo(note->undo) && undoundo(note->undo, &u_data, &u_len, &u_pos, &u_type)) {
                if (u_type == 0 && u_len > 0) { // reverse insert = delete
                    if (note->len >= u_len) {
                        note->len -= u_len;
                        note->buf[note->len] = '\0';
                        note->dirty = 1;
                        return 1;
                    }
                }
            }
            return 0;
        }
        if (key == 21) { // Ctrl+U: clear line
            while (note->len > 0 && note->buf[note->len - 1] != '\n') {
                note->len--;
            }
            note->buf[note->len] = '\0';
            note->dirty = 1;
            return 1;
        }
        if (key == 23) { // Ctrl+W: delete word
            while (note->len > 0 && note->buf[note->len - 1] == ' ') note->len--;
            while (note->len > 0 && note->buf[note->len - 1] != ' ' && note->buf[note->len - 1] != '\n') note->len--;
            note->buf[note->len] = '\0';
            note->dirty = 1;
            return 1;
        }
        return 0;
    }

    if (key == 8) { // Backspace
        if (note->len > 0) {
            Rune r = note->buf[note->len - 1];
            undoadddelete(note->undo, &r, 1, note->len - 1);
            note->len--;
            note->buf[note->len] = '\0';
            note->dirty = 1;
            return 1;
        }
        return 0;
    }
    if (key == 13) { // Enter
        if (note->len < sizeof(note->buf) - 2) {
            Rune r = '\n';
            undoaddinsert(note->undo, &r, 1, note->len);
            note->buf[note->len++] = '\n';
            note->buf[note->len] = '\0';
            note->dirty = 1;
            return 1;
        }
        return 0;
    }
    if (key >= 32 && key <= 126) {
        if (note->len < sizeof(note->buf) - 2) {
            Rune r = (Rune)key;
            undoaddinsert(note->undo, &r, 1, note->len);
            note->buf[note->len++] = (char)key;
            note->buf[note->len] = '\0';
            note->dirty = 1;
            return 1;
        }
    }
    return 0;
}

/* Window Management */
static Window* aspera_create_window(AsperaDesktop *desk, const char *title, int x, int y, int w, int h, ClientType type) {
    Window *win = malloc(sizeof(Window));
    if (!win) return nil;
    memset(win, 0, sizeof(Window));
    win->id = desk->next_win_id++;
    strncpy(win->title, title, sizeof(win->title) - 1);
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->saved_x = x; win->saved_y = y; win->saved_w = w; win->saved_h = h;
    win->desktop_id = desk->cur_y * desk->cols + desk->cur_x;
    win->client_type = type;

    // Insert at head (top of Z-order)
    win->next = desk->head;
    win->prev = nil;
    if (desk->head) desk->head->prev = win;
    desk->head = win;
    if (!desk->tail) desk->tail = win;

    // Focus
    if (desk->focused) desk->focused->is_focused = 0;
    win->is_focused = 1;
    desk->focused = win;

    return win;
}

static void aspera_focus_window(AsperaDesktop *desk, Window *w) {
    if (!w) return;
    if (desk->focused) desk->focused->is_focused = 0;
    w->is_focused = 1;
    desk->focused = w;

    // Move to head of Z-order
    if (w != desk->head) {
        if (w->prev) w->prev->next = w->next;
        if (w->next) w->next->prev = w->prev;
        if (desk->tail == w) desk->tail = w->prev;
        w->next = desk->head;
        w->prev = nil;
        if (desk->head) desk->head->prev = w;
        desk->head = w;
    }
}

static void aspera_close_window(AsperaDesktop *desk, Window *w) {
    if (!w) return;
    if (desk->focused == w) desk->focused = nil;
    if (desk->hover_win == w) desk->hover_win = nil;
    if (desk->drag_win == w) desk->drag_win = nil;

    if (w->prev) w->prev->next = w->next;
    if (w->next) w->next->prev = w->prev;
    if (desk->head == w) desk->head = w->next;
    if (desk->tail == w) desk->tail = w->prev;

    if (w->client_type == CLIENT_TERMINAL && w->term.rc_in_fd >= 0) {
        close(w->term.rc_in_fd);
        if (w->term.rc_out_fd >= 0) close(w->term.rc_out_fd);
        postnote(PNGROUP, w->term.rc_pid, "kill");
    }
    if (w->client_type == CLIENT_NOTES && w->note.undo) {
        undostackfree(w->note.undo);
    }
    free(w);

    if (!desk->focused && desk->head) {
        aspera_focus_window(desk, desk->head);
    }
}

static void aspera_toggle_shade(AsperaDesktop *desk, Window *w) {
    if (!w) return;
    w->is_shaded = !w->is_shaded;
}

static void aspera_toggle_sticky(AsperaDesktop *desk, Window *w) {
    if (!w) return;
    w->is_sticky = !w->is_sticky;
}

static void aspera_toggle_maximize(AsperaDesktop *desk, Window *w) {
    if (!w) return;
    if (!w->is_maximized) {
        w->saved_x = w->x; w->saved_y = w->y;
        w->saved_w = w->w; w->saved_h = w->h;
        w->x = 0; w->y = 32;
        w->w = ASPERA_SCREEN_W;
        w->h = ASPERA_SCREEN_H - 32;
        w->is_maximized = 1;
    } else {
        w->x = w->saved_x; w->y = w->saved_y;
        w->w = w->saved_w; w->h = w->saved_h;
        w->is_maximized = 0;
    }
}

static void aspera_switch_desktop(AsperaDesktop *desk, int dx, int dy) {
    if (dx < 0 || dx >= desk->cols || dy < 0 || dy >= desk->rows) return;
    desk->cur_x = dx;
    desk->cur_y = dy;
    int cur_id = dy * desk->cols + dx;
    for (Window *w = desk->head; w; w = w->next) {
        if (w->is_sticky || w->desktop_id == cur_id) {
            aspera_focus_window(desk, w);
            return;
        }
    }
    desk->focused = nil;
}

static void aspera_set_theme(AsperaDesktop *desk, ThemeType type) {
    if (type >= THEME_COUNT) type = THEME_E16;
    desk->theme_idx = type;
    desk->theme = themes[type];
}

static void aspera_cycle_theme(AsperaDesktop *desk) {
    aspera_set_theme(desk, (desk->theme_idx + 1) % THEME_COUNT);
}

/* Hit Testing */
static PartType aspera_hit_test_window(const AsperaDesktop *desk, const Window *w, int px, int py) {
    const ThemeMetrics *t = &desk->theme;
    int bw = t->border_thickness;
    int th = t->titlebar_height;
    int bs = t->btn_size;
    int bsp = t->btn_spacing;
    int vh = win_visible_h(w, t);

    if (px < w->x || px >= w->x + w->w || py < w->y || py >= w->y + vh) {
        return PART_NONE;
    }

    int btn_x = w->x + w->w - bw - bs - 2;
    int btn_y = w->y + bw + (th - bs) / 2;

    if (px >= btn_x && px < btn_x + bs && py >= btn_y && py < btn_y + bs)
        return PART_BTN_CLOSE;
    btn_x -= (bs + bsp);

    if (!w->is_shaded) {
        if (px >= btn_x && px < btn_x + bs && py >= btn_y && py < btn_y + bs)
            return PART_BTN_MAX;
        btn_x -= (bs + bsp);
    }

    if (px >= btn_x && px < btn_x + bs && py >= btn_y && py < btn_y + bs)
        return PART_BTN_SHADE;
    btn_x -= (bs + bsp);

    if (px >= btn_x && px < btn_x + bs && py >= btn_y && py < btn_y + bs)
        return PART_BTN_STICK;

    if (py < w->y + bw + th)
        return PART_TITLEBAR;

    return PART_CONTENT;
}

/* Magnetic Snapping */
static void aspera_snap_window(AsperaDesktop *desk, Window *target, int req_x, int req_y) {
    int best_x = req_x, best_y = req_y;
    int min_dx = ASPERA_SNAP_DIST + 1, min_dy = ASPERA_SNAP_DIST + 1;
    int snap_targets_x[16], num_st_x = 0;
    int snap_targets_y[16], num_st_y = 0;

    snap_targets_x[num_st_x++] = 0;
    snap_targets_x[num_st_x++] = ASPERA_SCREEN_W - target->w;
    snap_targets_y[num_st_y++] = 32;
    snap_targets_y[num_st_y++] = ASPERA_SCREEN_H - target->h;

    for (int i = 0; i < num_st_x; i++) {
        int diff = abs(req_x - snap_targets_x[i]);
        if (diff < min_dx) { min_dx = diff; best_x = snap_targets_x[i]; }
    }
    for (int i = 0; i < num_st_y; i++) {
        int diff = abs(req_y - snap_targets_y[i]);
        if (diff < min_dy) { min_dy = diff; best_y = snap_targets_y[i]; }
    }
    target->x = best_x;
    target->y = best_y;
}

/* Mouse and Keyboard Handling */
static Window *g_note_window = nil;
static Window *g_term_window = nil;

static void aspera_handle_mouse_move(AsperaDesktop *desk, int mx, int my) {
    desk->mouse_x = mx;
    desk->mouse_y = my;

    if (desk->is_dragging && desk->drag_win) {
        int nx = desk->drag_start_wx + (mx - desk->drag_start_mx);
        int ny = desk->drag_start_wy + (my - desk->drag_start_my);
        aspera_snap_window(desk, desk->drag_win, nx, ny);
    }
}

static void aspera_handle_mouse_down(AsperaDesktop *desk, int mx, int my, int button) {
    desk->mouse_x = mx;
    desk->mouse_y = my;
    desk->mouse_buttons |= (1 << (button - 1));

    // 1. Shelf click
    if (desk->shelf_menu_open && mx >= 8 && mx < 192 && my >= 34 && my < 136) {
        desk->shelf_menu_open = 0;
        if (my < 68) { // Open / focus Notes
            if (g_note_window) aspera_focus_window(desk, g_note_window);
        } else if (my < 102) { // Cycle theme
            aspera_cycle_theme(desk);
        }
        return;
    }

    if (my < 32) {
        if (desk->shelf_menu_open) {
            desk->shelf_menu_open = 0;
        }
        if (mx >= 8 && mx < 80) { // Menu button
            desk->shelf_menu_open = !desk->shelf_menu_open;
            return;
        }
        if (mx >= 92 && mx < 236) { // Workspaces pager
            int act = ((mx - 92) / 36);
            aspera_switch_desktop(desk, act % 2, act / 2);
            return;
        }
        if (mx >= 250 && mx < 810) { // Windows tasklist
            int slot = (mx - 250) / 140;
            int idx = 0;
            for (Window *w = desk->head; w; w = w->next) {
                if (idx == slot) {
                    aspera_focus_window(desk, w);
                    break;
                }
                idx++;
            }
            return;
        }
        return;
    }

    if (desk->shelf_menu_open) {
        desk->shelf_menu_open = 0;
    }

    // 2. Window hit test
    int cur_id = desk->cur_y * desk->cols + desk->cur_x;
    for (Window *w = desk->head; w; w = w->next) {
        if (!w->is_sticky && w->desktop_id != cur_id) continue;
        PartType part = aspera_hit_test_window(desk, w, mx, my);
        if (part != PART_NONE) {
            aspera_focus_window(desk, w);
            if (part == PART_BTN_CLOSE) {
                if (w == g_note_window) note_save(w);
                aspera_close_window(desk, w);
                return;
            }
            if (part == PART_BTN_MAX) {
                aspera_toggle_maximize(desk, w);
                return;
            }
            if (part == PART_BTN_SHADE) {
                aspera_toggle_shade(desk, w);
                return;
            }
            if (part == PART_BTN_STICK) {
                aspera_toggle_sticky(desk, w);
                return;
            }
            if (part == PART_TITLEBAR) {
                desk->is_dragging = 1;
                desk->drag_win = w;
                desk->drag_start_mx = mx;
                desk->drag_start_my = my;
                desk->drag_start_wx = w->x;
                desk->drag_start_wy = w->y;
                return;
            }
            return;
        }
    }
}

static void aspera_handle_mouse_up(AsperaDesktop *desk, int mx, int my, int button) {
    desk->mouse_x = mx;
    desk->mouse_y = my;
    desk->mouse_buttons &= ~(1 << (button - 1));
    desk->is_dragging = 0;
    desk->drag_win = nil;
}

static void aspera_handle_key(AsperaDesktop *desk, int key, int mod) {
    Window *focused = desk->focused;

    // Alt + 1..4: Switch Workspace
    if ((mod & 1) && key >= '1' && key <= '4') {
        int d = key - '1';
        aspera_switch_desktop(desk, d % 2, d / 2);
        return;
    }

    // Alt + Enter: Open / Focus Terminal
    if ((mod & 1) && key == 13) {
        if (g_term_window) aspera_focus_window(desk, g_term_window);
        return;
    }

    // Ctrl+S globally saves Notes if Notes exists
    if ((mod & 2) && (key == 115 || key == 19)) {
        if (g_note_window) note_save(g_note_window);
        return;
    }

    if (!focused) return;

    if (focused->client_type == CLIENT_NOTES) {
        note_key(focused, key, mod);
        return;
    }

    if (focused->client_type == CLIENT_TERMINAL) {
        if (key == 13) { // Enter
            terminal_send(focused, focused->term.input_buf);
            focused->term.input_len = 0;
            focused->term.input_buf[0] = '\0';
            return;
        }
        if (key == 8) { // Backspace
            if (focused->term.input_len > 0) {
                focused->term.input_len--;
                focused->term.input_buf[focused->term.input_len] = '\0';
            }
            return;
        }
        if ((mod & 2) && key == 21) { // Ctrl+U
            focused->term.input_len = 0;
            focused->term.input_buf[0] = '\0';
            return;
        }
        if ((mod & 2) && key == 23) { // Ctrl+W
            while (focused->term.input_len > 0 && focused->term.input_buf[focused->term.input_len - 1] == ' ')
                focused->term.input_len--;
            while (focused->term.input_len > 0 && focused->term.input_buf[focused->term.input_len - 1] != ' ')
                focused->term.input_len--;
            focused->term.input_buf[focused->term.input_len] = '\0';
            return;
        }
        if (key >= 32 && key <= 126) {
            if (focused->term.input_len < sizeof(focused->term.input_buf) - 2) {
                focused->term.input_buf[focused->term.input_len++] = (char)key;
                focused->term.input_buf[focused->term.input_len] = '\0';
            }
            return;
        }
    }
}

/* Rendering */
static void aspera_render(AsperaDesktop *desk) {
    const ThemeMetrics *t = &desk->theme;
    desk->frame_count++;
    int is_p9 = (desk->theme_idx == THEME_PLAN9);

    for (Window *pw = desk->head; pw; pw = pw->next) {
        if (pw->client_type == CLIENT_TERMINAL) {
            terminal_poll(desk, pw);
        }
    }

    // 1. Background
    draw_fill_rect(desk->pixels, 0, 0, ASPERA_SCREEN_W, ASPERA_SCREEN_H, t->bg_desktop);
    if (!is_p9) {
        for (int y = 0; y < ASPERA_SCREEN_H; y += 32) {
            for (int x = 0; x < ASPERA_SCREEN_W; x += 32) {
                desk->pixels[y * ASPERA_SCREEN_W + x] = t->frame_inactive;
            }
        }
    }

    // 2. Windows in Z-order (tail to head)
    int cur_desk = desk->cur_y * desk->cols + desk->cur_x;
    for (Window *w = desk->tail; w; w = w->prev) {
        if (!w->is_sticky && w->desktop_id != cur_desk) continue;

        int bw = t->border_thickness;
        int th = t->titlebar_height;
        int bs = t->btn_size;
        int bsp = t->btn_spacing;
        int vh = win_visible_h(w, t);

        u32 frame_col = w->is_focused ? t->frame_active : t->frame_inactive;
        u32 title_bg = w->is_focused ? t->title_active_bg : t->title_inactive_bg;
        u32 title_fg = w->is_focused ? t->title_active_fg : t->title_inactive_fg;

        draw_border_rect(desk->pixels, w->x, w->y, w->w, vh, bw, frame_col);

        if (!is_p9) {
            draw_fill_rect(desk->pixels, w->x, w->y, w->w, 1, t->bevel_light);
            draw_fill_rect(desk->pixels, w->x, w->y, 1, vh, t->bevel_light);
            draw_fill_rect(desk->pixels, w->x, w->y + vh - 1, w->w, 1, t->bevel_dark);
            draw_fill_rect(desk->pixels, w->x + w->w - 1, w->y, 1, vh, t->bevel_dark);
        }

        draw_fill_rect(desk->pixels, w->x + bw, w->y + bw, w->w - bw * 2, th, title_bg);
        if (is_p9) {
            draw_fill_rect(desk->pixels, w->x + bw, w->y + bw + th - 1, w->w - bw * 2, 1, 0x000000);
        }

        int btn_x = w->x + w->w - bw - bs - 2;
        int btn_y = w->y + bw + (th - bs) / 2;

        // Close
        draw_fill_rect(desk->pixels, btn_x, btn_y, bs, bs, is_p9 ? title_bg : t->btn_close_col);
        draw_border_rect(desk->pixels, btn_x, btn_y, bs, bs, 1, is_p9 ? 0x000000 : t->bevel_dark);
        draw_char(desk->pixels, btn_x + 3, btn_y, 'x', is_p9 ? 0x000000 : 0xffffff);
        btn_x -= (bs + bsp);

        // Maximize
        if (!w->is_shaded) {
            draw_fill_rect(desk->pixels, btn_x, btn_y, bs, bs, is_p9 ? title_bg : t->btn_max_col);
            draw_border_rect(desk->pixels, btn_x, btn_y, bs, bs, 1, is_p9 ? 0x000000 : t->bevel_dark);
            draw_char(desk->pixels, btn_x + 3, btn_y, '+', is_p9 ? 0x000000 : 0xffffff);
            btn_x -= (bs + bsp);
        }

        // Shade
        draw_fill_rect(desk->pixels, btn_x, btn_y, bs, bs, is_p9 ? title_bg : t->btn_shade_col);
        draw_border_rect(desk->pixels, btn_x, btn_y, bs, bs, 1, is_p9 ? 0x000000 : t->bevel_dark);
        draw_char(desk->pixels, btn_x + 3, btn_y, w->is_shaded ? 'v' : '^', is_p9 ? 0x000000 : 0xffffff);
        btn_x -= (bs + bsp);

        // Sticky
        draw_fill_rect(desk->pixels, btn_x, btn_y, bs, bs, is_p9 ? title_bg : t->btn_stick_col);
        draw_border_rect(desk->pixels, btn_x, btn_y, bs, bs, 1, is_p9 ? 0x000000 : t->bevel_dark);
        draw_char(desk->pixels, btn_x + 3, btn_y, w->is_sticky ? '*' : 'o', is_p9 ? 0x000000 : 0xffffff);

        // Title text
        char title_disp[128];
        if (w->is_sticky) snprint(title_disp, sizeof(title_disp), "* %s", w->title);
        else snprint(title_disp, sizeof(title_disp), "%s", w->title);
        draw_text_clipped(desk->pixels, w->x + bw + 8, w->y + bw + (th - 14) / 2, title_disp, title_fg,
                          w->x + bw + 4, w->y + bw, btn_x - 6, w->y + bw + th);

        // Client Area
        if (!w->is_shaded) {
            int cx = w->x + bw;
            int cy = w->y + bw + th;
            int cw = w->w - bw * 2;
            int ch = w->h - bw * 2 - th;
            if (cw > 0 && ch > 0) {
                u32 cbg = (w->client_type == CLIENT_NOTES) ? 0x101820 : t->content_bg;
                draw_fill_rect(desk->pixels, cx, cy, cw, ch, cbg);
                if (!is_p9) draw_border_rect(desk->pixels, cx, cy, cw, ch, 1, t->bevel_dark);

                int clip_min_x = cx + 2;
                int clip_max_x = cx + cw - 2;
                int clip_min_y = cy + 2;
                int clip_max_y = cy + ch - 2;

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
                            if (s[i] == '\n') { row++; col = 0; }
                            else if (s[i] != '\r') {
                                if (col >= columns) { row++; col = 0; }
                                col++;
                            }
                        }
                        int total_lines = row + 1;
                        int start = total_lines - rows;
                        if (start < 0) start = 0;
                        row = 0; col = 0;
                        for (int i = 0; s[i] != '\0' && i < w->note.len; i++) {
                            if (s[i] == '\n') {
                                row++; col = 0;
                                if (row >= start + rows) break;
                            } else if (s[i] != '\r') {
                                if (col >= columns) {
                                    row++; col = 0;
                                    if (row >= start + rows) break;
                                }
                                if (row >= start && row < start + rows) {
                                    draw_char_clipped(desk->pixels, cx + 8 + col * 8, cy + 30 + (row - start) * 16,
                                                      s[i], 0xe4eaf0, clip_min_x, clip_min_y, clip_max_x, clip_max_y);
                                }
                                col++;
                            }
                        }
                    }
                } else if (w->client_type == CLIENT_TERMINAL) {
                    int vis_lines = (ch - 24) / 16;
                    if (vis_lines < 1) vis_lines = 1;
                    int start_line = w->term.num_lines - vis_lines;
                    if (start_line < 0) start_line = 0;
                    int line_y = cy + 4;
                    for (int i = start_line; i < w->term.num_lines && line_y + 14 <= clip_max_y; i++) {
                        draw_text_clipped(desk->pixels, cx + 8, line_y, w->term.lines[i],
                                          is_p9 ? 0x000000 : t->content_fg,
                                          clip_min_x, clip_min_y, clip_max_x, clip_max_y);
                        line_y += 16;
                    }
                    if (line_y + 14 <= clip_max_y) {
                        char prompt[ASPERA_MAX_LINE_LEN + 16];
                        snprint(prompt, sizeof(prompt), "%% %s", w->term.input_buf);
                        draw_text_clipped(desk->pixels, cx + 8, line_y, prompt,
                                          is_p9 ? 0x000000 : (w->is_focused ? 0xf0c040 : t->content_fg),
                                          clip_min_x, clip_min_y, clip_max_x, clip_max_y);
                        if (w->is_focused && ((desk->frame_count / 15) % 2 == 0)) {
                            int cursor_x = cx + 8 + (int)strlen(prompt) * 8;
                            draw_fill_rect_clipped(desk->pixels, cursor_x, line_y + 1, 8, 14,
                                                   is_p9 ? 0x000000 : 0xf0c040,
                                                   clip_min_x, clip_min_y, clip_max_x, clip_max_y);
                        }
                    }
                }
            }
        }
    }

    // 3. Top Shelf Bar
    draw_fill_rect(desk->pixels, 0, 0, ASPERA_SCREEN_W, 32, is_p9 ? 0xeeeeee : 0x18222c);
    draw_fill_rect(desk->pixels, 0, 31, ASPERA_SCREEN_W, 1, is_p9 ? 0x000000 : 0x3b5264);

    // Shelf Menu button
    draw_fill_rect(desk->pixels, 8, 4, 72, 24, is_p9 ? 0xcccc99 : 0x2e4054);
    draw_border_rect(desk->pixels, 8, 4, 72, 24, 1, is_p9 ? 0x000000 : 0x526b7d);
    draw_text(desk->pixels, 16, 9, "Aspera", is_p9 ? 0x000000 : 0xffd382);

    // Workspaces Pager (4 horizontal tabs matching Aspera shelf.o9)
    for (int i = 0; i < 4; i++) {
        int x = 92 + i * 36;
        int is_cur = (i == (desk->cur_y * desk->cols + desk->cur_x));
        u32 bg = is_cur ? (is_p9 ? 0xcccc99 : 0x9a6e2e) : (is_p9 ? 0xeeeeee : 0x253440);
        char num[2];
        num[0] = (char)('1' + i);
        num[1] = '\0';
        draw_fill_rect(desk->pixels, x, 5, 32, 22, bg);
        draw_border_rect(desk->pixels, x, 5, 32, 22, 1, is_p9 ? 0x000000 : (is_cur ? 0xb87d31 : 0x536675));
        draw_text(desk->pixels, x + 12, 9, num, is_p9 ? 0x000000 : (is_cur ? 0xffffff : 0xd0d0d0));
    }

    // Window tasklist buttons
    int slot = 0;
    for (Window *w = desk->head; w && slot < 4; w = w->next) {
        int bx = 250 + slot * 140;
        u32 bbg = w->is_focused ? (is_p9 ? 0xcccc99 : 0x526b7d) : (is_p9 ? 0xeeeeee : 0x314354);
        char label[16];
        strncpy(label, w->title, 15);
        label[15] = '\0';
        draw_fill_rect(desk->pixels, bx, 4, 136, 24, bbg);
        draw_border_rect(desk->pixels, bx, 4, 136, 24, 1, is_p9 ? 0x000000 : 0x536675);
        draw_text(desk->pixels, bx + 6, 9, label, is_p9 ? 0x000000 : 0xe4eaf0);
        slot++;
    }

    // Clock
    if (desk->shelf_clock_visible) {
        draw_text(desk->pixels, 972, 9, desk->shelf_clock_text[0] ? desk->shelf_clock_text : "--:--",
                  is_p9 ? 0x000000 : 0xe4eaf0);
    }

    // Dropdown menu if open
    if (desk->shelf_menu_open) {
        draw_fill_rect(desk->pixels, 8, 34, 184, 68, is_p9 ? 0xffffff : 0x263542);
        draw_border_rect(desk->pixels, 8, 34, 184, 68, 1, is_p9 ? 0x000000 : 0xb87d31);
        draw_fill_rect(desk->pixels, 9, 68, 182, 1, is_p9 ? 0xcccccc : 0x526b7d);
        draw_text(desk->pixels, 20, 44, "Open Notes", is_p9 ? 0x000000 : 0xe4eaf0);
        draw_text(desk->pixels, 20, 78, "Cycle theme", is_p9 ? 0x000000 : 0xe4eaf0);
    }

    // Software Mouse Cursor
    draw_cursor(desk, desk->pixels, desk->mouse_x, desk->mouse_y);
}

/* Halatha 9P Display Transport */
static int present_frame(AsperaDesktop *desk, int preferred_slot) {
    static const char *base = "build/aspera-live";
    char path[256], command[32];
    int total = ASPERA_SCREEN_W * ASPERA_SCREEN_H * sizeof(u32);
    int dirty = 0;

    if (desk->have_prior) {
        for (int y = 0; y < ASPERA_SCREEN_H; y += 16) {
            for (int x = 0; x < ASPERA_SCREEN_W; x += 32) {
                int changed = 0;
                for (int row = 0; row < 16 && !changed; row++) {
                    changed = memcmp(desk->pixels + (y + row) * ASPERA_SCREEN_W + x,
                                     desk->prior + ((y + row) * ASPERA_SCREEN_W + x) * sizeof(u32),
                                     32 * sizeof(u32)) != 0;
                }
                dirty += changed;
            }
        }
    }

    int slot = -1, delta = 0;
    snprint(path, sizeof(path), "%s/dev/ctl", base);

    for (int attempt = 0; attempt < 50 && slot < 0; attempt++) {
        int candidate = (preferred_slot + attempt) % 2;
        int ctl = open(path, OWRITE);
        if (ctl >= 0) {
            if (desk->have_prior && dirty <= 1536) {
                int n = snprint(command, sizeof(command), "begin-delta %d\n", candidate);
                if (write(ctl, command, n) == n) {
                    slot = candidate;
                    delta = 1;
                }
            }
            if (slot < 0) {
                int n = snprint(command, sizeof(command), "begin %d\n", candidate);
                if (write(ctl, command, n) == n) {
                    slot = candidate;
                }
            }
            close(ctl);
        }
        if (slot < 0) sleep(2);
    }

    if (slot < 0) return -1;

    snprint(path, sizeof(path), "%s/dev/screen%d", base, slot);
    int fd = open(path, OWRITE);
    if (fd < 0) return -1;

    int good = 1;
    if (delta) {
        u8 tile[8 + 32 * 16 * sizeof(u32)];
        u8 *batch = malloc(dirty * sizeof(tile) + 1);
        int batchlen = 0;

        for (int y = 0; good && y < ASPERA_SCREEN_H; y += 16) {
            for (int x = 0; good && x < ASPERA_SCREEN_W; x += 32) {
                int changed = 0;
                for (int row = 0; row < 16 && !changed; row++) {
                    changed = memcmp(desk->pixels + (y + row) * ASPERA_SCREEN_W + x,
                                     desk->prior + ((y + row) * ASPERA_SCREEN_W + x) * sizeof(u32),
                                     32 * sizeof(u32)) != 0;
                }
                if (!changed) continue;

                tile[0] = x;
                tile[1] = x >> 8;
                tile[2] = y;
                tile[3] = y >> 8;
                tile[4] = 32;
                tile[5] = 32 >> 8;
                tile[6] = 16;
                tile[7] = 16 >> 8;
                for (int row = 0; row < 16; row++) {
                    memmove(tile + 8 + row * 32 * sizeof(u32),
                            desk->pixels + (y + row) * ASPERA_SCREEN_W + x,
                            32 * sizeof(u32));
                }

                if (batchlen + sizeof(tile) > 30840) {
                    if (write(fd, batch, batchlen) != batchlen) good = 0;
                    batchlen = 0;
                }
                memmove(batch + batchlen, tile, sizeof(tile));
                batchlen += sizeof(tile);
            }
        }
        if (good && batchlen > 0) {
            if (write(fd, batch, batchlen) != batchlen) good = 0;
        }
        free(batch);
    } else {
        int sent = 0;
        while (good && sent < total) {
            int n = write(fd, (u8*)desk->pixels + sent, total - sent);
            if (n <= 0) good = 0;
            else sent += n;
        }
    }
    close(fd);

    snprint(path, sizeof(path), "%s/dev/ctl", base);
    int ctl = open(path, OWRITE);
    if (ctl >= 0) {
        int n = snprint(command, sizeof(command), good ? "commit %d\n" : "abort %d\n", slot);
        write(ctl, command, n);
        close(ctl);
    }

    if (good) {
        memmove(desk->prior, desk->pixels, total);
        desk->have_prior = 1;
        return slot;
    }
    return -1;
}

static void update_clock(AsperaDesktop *desk) {
    int fd = open("build/aspera-live/clock", OREAD);
    if (fd >= 0) {
        char buf[32];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            char *nl = strchr(buf, '\n');
            if (nl) *nl = '\0';
            strncpy(desk->shelf_clock_text, buf, sizeof(desk->shelf_clock_text) - 1);
            desk->shelf_clock_text[sizeof(desk->shelf_clock_text) - 1] = '\0';
        }
    }
}

void threadmain(int argc, char *argv[]) {
    memset(&g_desk, 0, sizeof(g_desk));
    g_desk.cols = 2;
    g_desk.rows = 2;
    g_desk.pixels = malloc(ASPERA_SCREEN_W * ASPERA_SCREEN_H * sizeof(u32));
    g_desk.prior = malloc(ASPERA_SCREEN_W * ASPERA_SCREEN_H * sizeof(u32));
    g_desk.shelf_clock_visible = 1;
    strcpy(g_desk.shelf_clock_text, "--:--");
    aspera_set_theme(&g_desk, THEME_E16);

    // Start Shelf Service and mount /mnt/shelf
    shelf_start();

    // Notes window
    g_note_window = aspera_create_window(&g_desk, "Notes", 60, 80, 560, 440, CLIENT_NOTES);
    note_load(g_note_window, "/lib/aspera/note.txt");

    // Terminal window
    g_term_window = aspera_create_window(&g_desk, "Terminal [rc]", 110, 115, 590, 410, CLIENT_TERMINAL);
    terminal_start(g_term_window);

    // Initial render and publication
    aspera_render(&g_desk);
    int published = present_frame(&g_desk, 0);
    if (published < 0) {
        print("ASPERA-LIVE FAIL initial frame\n");
        threadexitsall("initial frame failed");
    }

    print("ASPERA-LIVE READY\n");

    // Open persistent input file descriptor
    int input_fd = open("build/aspera-live/dev/input", OREAD);
    if (input_fd < 0) {
        print("ASPERA-LIVE FAIL input dev\n");
        threadexitsall("input dev failed");
    }

    int idle_ticks = 0;
    int cur_slot = 1;

    for (;;) {
        int events_processed = 0;
        char line[128];
        int n;

        // Drain pending input events to coalesce mouse moves and eliminate freeze
        while ((n = read(input_fd, line, sizeof(line) - 1)) > 0) {
            events_processed++;
            line[n] = '\0';
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';

            if (strncmp(line, "down ", 5) == 0) {
                int x = 0, y = 0;
                char *sp = line + 5;
                x = atoi(sp);
                sp = strchr(sp, ' ');
                if (sp) y = atoi(sp + 1);
                aspera_handle_mouse_down(&g_desk, x, y, 1);
            } else if (strncmp(line, "move ", 5) == 0) {
                int x = 0, y = 0;
                char *sp = line + 5;
                x = atoi(sp);
                sp = strchr(sp, ' ');
                if (sp) y = atoi(sp + 1);
                aspera_handle_mouse_move(&g_desk, x, y);
            } else if (strncmp(line, "up", 2) == 0) {
                aspera_handle_mouse_up(&g_desk, g_desk.mouse_x, g_desk.mouse_y, 1);
            } else if (strncmp(line, "key ", 4) == 0) {
                int key = 0, mod = 0;
                char *sp = line + 4;
                key = atoi(sp);
                sp = strchr(sp, ' ');
                if (sp) mod = atoi(sp + 1);
                aspera_handle_key(&g_desk, key, mod);
            }
            if (events_processed >= 16) break;
        }

        int term_changed = 0;
        if (g_term_window) {
            term_changed = terminal_poll(&g_desk, g_term_window);
        }

        if (events_processed > 0 || term_changed) {
            aspera_render(&g_desk);
            present_frame(&g_desk, cur_slot % 2);
            cur_slot++;
            idle_ticks = 0;
        } else {
            sleep(5);
            idle_ticks++;

            if (idle_ticks >= 20) {
                update_clock(&g_desk);
                idle_ticks = 0;
            }
        }
    }
}
