// aspera_engine.h - Aspera Window System Engine
// Retained part tree, interactive live dragging, accordion shading,
// magnetic snapping, edge desktop flipping, 2D pager, terminal client,
// and full DrawScrollbar widget integration.

#ifndef ASPERA_ENGINE_H
#define ASPERA_ENGINE_H

#include <stdint.h>
#include <stdbool.h>

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

typedef struct {
    const char *name;
    int border_thickness;
    int titlebar_height;
    int btn_size;
    int btn_spacing;
    uint32_t bg_desktop;
    uint32_t title_active_bg;
    uint32_t title_active_fg;
    uint32_t title_inactive_bg;
    uint32_t title_inactive_fg;
    uint32_t frame_active;
    uint32_t frame_inactive;
    uint32_t bevel_light;
    uint32_t bevel_dark;
    uint32_t content_bg;
    uint32_t content_fg;
    uint32_t btn_close_col;
    uint32_t btn_max_col;
    uint32_t btn_shade_col;
    uint32_t btn_stick_col;
} ThemeMetrics;

typedef enum {
    CLIENT_TERMINAL = 0,
    CLIENT_EDITOR,
    CLIENT_CLOCK,
    CLIENT_NOTES
} ClientType;

typedef struct {
    char lines[ASPERA_MAX_TERMINAL_LINES][ASPERA_MAX_LINE_LEN];
    int num_lines;
    char input_buf[ASPERA_MAX_LINE_LEN];
    int input_len;
    int cursor_tick;
    int rc_pid;
    int rc_in_fd;
    int rc_out_fd;
    char partial[ASPERA_MAX_LINE_LEN];
    int partial_len;
} TerminalState;

typedef struct {
    int sam_pid;
    int sam_in_fd;
    int sam_out_fd;
    char cmd_lines[32][ASPERA_MAX_LINE_LEN];
    int num_cmd_lines;
    char cmd_input[ASPERA_MAX_LINE_LEN];
    int cmd_len;
    char cmd_partial[ASPERA_MAX_LINE_LEN];
    int cmd_partial_len;
    char buf[8192];
    int buf_len;
    bool dirty;
    bool cmd_focused;
    char path[256];
    char sync_path[256];
} SamState;

typedef struct {
    char buf[8192];
    int len;
    bool dirty;
    bool save_error;
    char path[256];
} NoteState;

// DrawScrollbar Widget (modeled from o9c stdlib/draw.o9)
typedef struct {
    bool visible;
    bool is_vertical;
    int x, y, w, h;
    int min, max, page, val;
    bool dragging;
    bool hovered;
    int drag_offset;
} AsperaScrollbar;

typedef struct Window Window;
struct Window {
    int id;
    char title[64];
    int x, y, w, h;
    int saved_x, saved_y, saved_w, saved_h;
    bool is_shaded;
    bool is_sticky;
    bool is_maximized;
    bool is_focused;
    int desktop_id; // 0..3 for 2x2 grid
    ClientType client_type;
    TerminalState term;
    SamState sam;
    NoteState note;
    AsperaScrollbar vscroll; // Integrated vertical scrollbar
    Window *prev;
    Window *next;
};

typedef struct {
    int cols, rows;
    int cur_x, cur_y; // Current virtual workspace (0..cols-1, 0..rows-1)
    Window *head;     // Top of Z-order
    Window *tail;     // Bottom of Z-order
    Window *focused;
    int next_win_id;

    ThemeType theme_idx;
    ThemeMetrics theme;

    // Interactive Dragging and Snapping
    bool is_dragging;
    bool is_resizing;
    Window *drag_win;
    int drag_start_mx, drag_start_my;
    int drag_start_wx, drag_start_wy;
    int drag_start_ww, drag_start_wh;
    PartType resize_part;

    // Interactive Scrollbar dragging
    bool is_scrolling;
    Window *scroll_win;

    // Edge flip timer to prevent infinite rapid flipping
    int edge_flip_cooldown;

    // Hover state
    PartType hover_part;
    Window *hover_win;

    // Pager widget geometry
    int pager_x, pager_y, pager_w, pager_h;

    // Aspera Top Shelf Bar state
    bool shelf_menu_open;
    bool shelf_clock_visible;
    int shelf_menu_hover;

    // Context Menu
    bool menu_open;
    int menu_x, menu_y, menu_w, menu_h;
    bool menu_is_window;
    Window *menu_target_win;
    int menu_hover_idx;
    const char *menu_items[8];
    int num_menu_items;

    // Software Framebuffer
    uint32_t pixels[ASPERA_SCREEN_W * ASPERA_SCREEN_H];

    // Pointer state
    int mouse_x, mouse_y;
    int mouse_buttons;
    int frame_count;
} AsperaDesktop;

typedef Window AsperaWindow;

// Scrollbar helper functions (modeled from Objective-9C DrawScrollbar)
int scrollbar_thumb_length(const AsperaScrollbar *sb);
int scrollbar_thumb_start(const AsperaScrollbar *sb);
int scrollbar_val_for_start(const AsperaScrollbar *sb, int start);
bool scrollbar_thumb_contains(const AsperaScrollbar *sb, int px, int py);
void win_layout_scrollbar(Window *w, const AsperaDesktop *desk);
PartType aspera_hit_test_window(const AsperaDesktop *desk, const Window *w, int px, int py);

// Engine API functions
void aspera_init(AsperaDesktop *desk);
void aspera_set_theme(AsperaDesktop *desk, ThemeType type);
void aspera_cycle_theme(AsperaDesktop *desk);
Window *aspera_create_window(AsperaDesktop *desk, const char *title, int x, int y, int w, int h, ClientType type);
void aspera_close_window(AsperaDesktop *desk, Window *w);
void aspera_focus_window(AsperaDesktop *desk, Window *w);
void aspera_raise_window(AsperaDesktop *desk, Window *w);
void aspera_cycle_focus(AsperaDesktop *desk);
void aspera_switch_desktop(AsperaDesktop *desk, int dx, int dy);
void aspera_toggle_shade(AsperaDesktop *desk, Window *w);
void aspera_toggle_sticky(AsperaDesktop *desk, Window *w);
void aspera_toggle_maximize(AsperaDesktop *desk, Window *w);
void aspera_get_button_pos(const AsperaDesktop *desk, const Window *w, PartType btn, int *out_x, int *out_y);

// Event processing
void aspera_handle_mouse_move(AsperaDesktop *desk, int mx, int my);
void aspera_handle_mouse_down(AsperaDesktop *desk, int mx, int my, int button);
void aspera_handle_mouse_up(AsperaDesktop *desk, int mx, int my, int button);
void aspera_handle_mouse_wheel(AsperaDesktop *desk, int delta_y);
void aspera_handle_key_down(AsperaDesktop *desk, int key, char ch, bool alt_down, bool ctrl_down);
void aspera_handle_text_input(AsperaDesktop *desk, const char *text);

// Rendering and export
void aspera_render(AsperaDesktop *desk);
int aspera_save_ppm(AsperaDesktop *desk, const char *path);

#endif // ASPERA_ENGINE_H
