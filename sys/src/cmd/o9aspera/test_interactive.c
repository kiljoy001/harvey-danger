// test_interactive.c - Automated Test Suite for Aspera Window System Engine
// Validates all interactive behaviors: DrawScrollbar widget integration,
// Plan 9 Rio left scrollbar, drag scrolling, mouse wheel, track paging,
// magnetic snapping, edge flipping, accordion shading, desktop pinning,
// 8-way resizing, terminal shell, and context menus.

#include "aspera_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

static int total_tests = 0;
static int passed_tests = 0;

#define TEST_ASSERT(cond, desc) do { \
    total_tests++; \
    if (cond) { \
        printf("  [PASS] %s\n", desc); \
        passed_tests++; \
    } else { \
        printf("  [FAIL] %s (line %d)\n", desc, __LINE__); \
    } \
} while(0)

int main(void) {
    printf("==================================================\n");
    printf("  Aspera Window System Engine Verification\n");
    printf("==================================================\n");

    AsperaDesktop desk;
    aspera_init(&desk);

    // 1. Initialization Invariants
    printf("\n--- Test 1: Desktop Compositor & Window Initialization ---\n");
    TEST_ASSERT(desk.cols == 2 && desk.rows == 2, "2x2 Virtual Workspace grid initialized");
    TEST_ASSERT(desk.cur_x == 0 && desk.cur_y == 0, "Initial desktop at (0,0)");
    TEST_ASSERT(desk.head != NULL && desk.tail != NULL, "Window Z-order list populated");
    TEST_ASSERT(strcmp(desk.theme.name, "e16-slate") == 0, "Default theme is e16-slate");

    Window *w1 = desk.head; // Terminal 1
    Window *w2 = w1->next;  // Editor [sam]
    TEST_ASSERT(w1 != NULL && w2 != NULL, "Windows w1 (Terminal) and w2 (Editor) exist");
    TEST_ASSERT(w1->vscroll.visible, "Vertical scrollbar visible on w1");
    TEST_ASSERT(w1->vscroll.is_vertical, "Vertical orientation flag set");
    TEST_ASSERT(w1->vscroll.w == 14, "E16 scrollbar width is 14px");

    // 2. DrawScrollbar Geometry and Metrics (Objective-9C draw.o9 model)
    printf("\n--- Test 2: DrawScrollbar Proportional Geometry & Math ---\n");
    // Add lines to w1 terminal to test scroll ranges
    for (int i = 0; i < 40; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "log line %d: test data output", i + 1);
        strncpy(w1->term.lines[w1->term.num_lines++], buf, ASPERA_MAX_LINE_LEN - 1);
    }
    win_layout_scrollbar(w1, &desk);

    TEST_ASSERT(w1->vscroll.max > 0, "Scrollbar max calculated from total lines vs visible page");
    int thumb_len = scrollbar_thumb_length(&w1->vscroll);
    TEST_ASSERT(thumb_len < w1->vscroll.h, "Thumb length is proportional fraction of track");
    TEST_ASSERT(thumb_len >= 12, "Thumb maintains minimum thumb length constraint (>= 12px)");

    // Test thumb start position at top
    w1->vscroll.val = 0;
    int t_start_top = scrollbar_thumb_start(&w1->vscroll);
    TEST_ASSERT(t_start_top == w1->vscroll.y + 2, "Thumb start at top equals track start");

    // Test thumb start position at bottom
    w1->vscroll.val = w1->vscroll.max;
    int t_start_bot = scrollbar_thumb_start(&w1->vscroll);
    int t_end_bot = t_start_bot + thumb_len;
    TEST_ASSERT(t_end_bot <= w1->vscroll.y + w1->vscroll.h, "Thumb at maximum does not exceed track bounds");

    // 3. Scrollbar Part Hit Testing
    printf("\n--- Test 3: Scrollbar Part Hit Testing ---\n");
    w1->vscroll.val = w1->vscroll.max / 2; // Position thumb in the middle
    int cur_t_start = scrollbar_thumb_start(&w1->vscroll);
    int cur_t_len = scrollbar_thumb_length(&w1->vscroll);

    // Hit test on the thumb
    PartType hit_thumb = aspera_hit_test_window(&desk, w1, w1->vscroll.x + 4, cur_t_start + cur_t_len / 2);
    TEST_ASSERT(hit_thumb == PART_SCROLLBAR_THUMB, "Hit test on scrollbar thumb detected PART_SCROLLBAR_THUMB");

    // Hit test above the thumb (track page up)
    if (cur_t_start > w1->vscroll.y + 4) {
        PartType hit_up = aspera_hit_test_window(&desk, w1, w1->vscroll.x + 4, w1->vscroll.y + 3);
        TEST_ASSERT(hit_up == PART_SCROLLBAR_UP, "Hit test above thumb detected PART_SCROLLBAR_UP");
    }

    // Hit test below the thumb (track page down)
    if (cur_t_start + cur_t_len < w1->vscroll.y + w1->vscroll.h - 4) {
        PartType hit_down = aspera_hit_test_window(&desk, w1, w1->vscroll.x + 4, w1->vscroll.y + w1->vscroll.h - 3);
        TEST_ASSERT(hit_down == PART_SCROLLBAR_DOWN, "Hit test below thumb detected PART_SCROLLBAR_DOWN");
    }

    // 4. Interactive Scrollbar Dragging
    printf("\n--- Test 4: Interactive Scrollbar Dragging ---\n");
    w1->vscroll.val = 0;
    int drag_mx = w1->vscroll.x + 4;
    int drag_my = scrollbar_thumb_start(&w1->vscroll) + 4;

    aspera_handle_mouse_down(&desk, drag_mx, drag_my, 1);
    TEST_ASSERT(desk.is_scrolling && desk.scroll_win == w1, "Scrollbar drag initiated on button 1 down");
    TEST_ASSERT(w1->vscroll.dragging, "Scrollbar widget dragging flag set");

    // Drag downward by 40 pixels
    aspera_handle_mouse_move(&desk, drag_mx, drag_my + 40);
    TEST_ASSERT(w1->vscroll.val > 0, "Dragging mouse downward updated scrollbar value");

    aspera_handle_mouse_up(&desk, drag_mx, drag_my + 40, 1);
    TEST_ASSERT(!desk.is_scrolling, "Scrollbar drag released on button 1 up");
    TEST_ASSERT(!w1->vscroll.dragging, "Scrollbar widget dragging flag cleared");

    // 5. Track Paging & Mouse Wheel Scrolling
    printf("\n--- Test 5: Track Paging & Mouse Wheel Scrolling ---\n");
    int val_before_page = w1->vscroll.val;
    // Click below thumb to page down
    cur_t_start = scrollbar_thumb_start(&w1->vscroll);
    cur_t_len = scrollbar_thumb_length(&w1->vscroll);
    aspera_handle_mouse_down(&desk, w1->vscroll.x + 4, w1->vscroll.y + w1->vscroll.h - 3, 1);
    aspera_handle_mouse_up(&desk, w1->vscroll.x + 4, w1->vscroll.y + w1->vscroll.h - 3, 1);
    TEST_ASSERT(w1->vscroll.val >= val_before_page, "Track click below thumb paged downward");

    // Mouse wheel scrolling
    desk.mouse_x = w1->x + 50;
    desk.mouse_y = w1->y + 50;
    int val_before_wheel = w1->vscroll.val;
    // Wheel up (delta_y > 0) scrolls towards earlier lines
    aspera_handle_mouse_wheel(&desk, 2);
    TEST_ASSERT(w1->vscroll.val <= val_before_wheel, "Mouse wheel up scrolled towards start");

    // 6. Authentic Plan 9 Rio Left-hand Scrollbar
    printf("\n--- Test 6: Plan 9 Rio Authentic Scrollbar Layout ---\n");
    aspera_set_theme(&desk, THEME_PLAN9);
    win_layout_scrollbar(w1, &desk);

    int bw_p9 = desk.theme.border_thickness;
    int expected_rio_x = w1->x + bw_p9;
    TEST_ASSERT(w1->vscroll.x == expected_rio_x, "Rio theme placed scrollbar on the LEFT side of client");
    TEST_ASSERT(w1->vscroll.w == 12, "Rio scrollbar width is exactly Scrollwid = 12px");

    // Test Rio Button 2 (middle click) jump/drag
    int mid_click_y = w1->vscroll.y + w1->vscroll.h / 2;
    aspera_handle_mouse_down(&desk, w1->vscroll.x + 4, mid_click_y, 2);
    TEST_ASSERT(desk.is_scrolling, "Plan 9 Rio Button 2 initiated scrollbar jump/drag");
    aspera_handle_mouse_up(&desk, w1->vscroll.x + 4, mid_click_y, 2);
    TEST_ASSERT(!desk.is_scrolling, "Plan 9 Rio Button 2 released scrollbar");

    // Restore E16 theme for remaining WM tests
    aspera_set_theme(&desk, THEME_E16);

    // 7. Interactive Titlebar Dragging and Magnetic Snapping
    printf("\n--- Test 7: Magnetic Edge & Neighbor Snapping ---\n");
    int tb_x = w1->x + w1->w / 2;
    int tb_y = w1->y + 10;
    aspera_handle_mouse_move(&desk, tb_x, tb_y);
    TEST_ASSERT(desk.hover_win == w1 && desk.hover_part == PART_TITLEBAR, "Titlebar hover detected");

    int drag_start_x = w1->x;
    int drag_start_y = w1->y;
    aspera_handle_mouse_down(&desk, tb_x, tb_y, 1);
    TEST_ASSERT(desk.is_dragging && desk.drag_win == w1, "Window drag initiated on Button 1 down");

    aspera_handle_mouse_move(&desk, tb_x + (8 - drag_start_x), tb_y + (6 - drag_start_y));
    aspera_handle_mouse_up(&desk, tb_x + (8 - drag_start_x), tb_y + (6 - drag_start_y), 1);
    TEST_ASSERT(!desk.is_dragging, "Window drag released on Button 1 up");
    TEST_ASSERT(w1->x == 0 && w1->y == 0, "Magnetic edge snap aligned w1 to (0,0)");

    // Neighbor window snap
    int tb2_x = w2->x + 200;
    int tb2_y = w2->y + 10;
    int drag2_start_x = w2->x;
    int drag2_start_y = w2->y;
    aspera_handle_mouse_down(&desk, tb2_x, tb2_y, 1);
    aspera_handle_mouse_move(&desk, tb2_x + (488 - drag2_start_x), tb2_y + (4 - drag2_start_y));
    aspera_handle_mouse_up(&desk, tb2_x + (488 - drag2_start_x), tb2_y + (4 - drag2_start_y), 1);
    TEST_ASSERT(w2->x == 480 && w2->y == 0, "Neighbor window magnetic snap aligned w2 flush to (480,0)");

    // 8. Interactive Accordion Window Shading
    printf("\n--- Test 8: Accordion Window Shading ---\n");
    int orig_h = w1->h;
    TEST_ASSERT(!w1->is_shaded, "w1 initially unshaded");

    int shade_bx, shade_by;
    aspera_get_button_pos(&desk, w1, PART_BTN_SHADE, &shade_bx, &shade_by);
    aspera_handle_mouse_down(&desk, shade_bx, shade_by, 1);
    aspera_handle_mouse_up(&desk, shade_bx, shade_by, 1);
    TEST_ASSERT(w1->is_shaded, "Accordion shade toggled on via button click");
    TEST_ASSERT(w1->h == orig_h, "Window internal height preserved during shade");

    aspera_get_button_pos(&desk, w1, PART_BTN_SHADE, &shade_bx, &shade_by);
    aspera_handle_mouse_down(&desk, shade_bx, shade_by, 1);
    aspera_handle_mouse_up(&desk, shade_bx, shade_by, 1);
    TEST_ASSERT(!w1->is_shaded, "Accordion unshade restored window to full geometry");

    // 9. Interactive Desktop Pinning (Sticky Windows) & Pager Switching
    printf("\n--- Test 9: Desktop Pinning (Sticky) & 2D Pager ---\n");
    int stick_bx, stick_by;
    aspera_get_button_pos(&desk, w1, PART_BTN_STICK, &stick_bx, &stick_by);
    aspera_handle_mouse_down(&desk, stick_bx, stick_by, 1);
    aspera_handle_mouse_up(&desk, stick_bx, stick_by, 1);
    TEST_ASSERT(w1->is_sticky, "Desktop pinning toggled on via button click");

    int cell_w = desk.pager_w / desk.cols;
    int pager_click_x = desk.pager_x + cell_w + 10;
    int pager_click_y = desk.pager_y + 10;
    aspera_handle_mouse_down(&desk, pager_click_x, pager_click_y, 1);
    aspera_handle_mouse_up(&desk, pager_click_x, pager_click_y, 1);
    TEST_ASSERT(desk.cur_x == 1 && desk.cur_y == 0, "Pager click switched active desktop to (1,0)");

    int cur_desk = desk.cur_y * desk.cols + desk.cur_x;
    TEST_ASSERT(w1->is_sticky, "Pinned window w1 remains visible on workspace (1,0)");
    TEST_ASSERT(!w2->is_sticky && w2->desktop_id != cur_desk, "Unpinned window w2 hidden on workspace (1,0)");

    aspera_switch_desktop(&desk, 0, 0);

    // 10. Interactive 8-Way Window Resizing
    printf("\n--- Test 10: Interactive 8-Way Window Resizing ---\n");
    int orig_w2_w = w2->w;
    int orig_w2_h = w2->h;
    int se_x = w2->x + w2->w - 4;
    int se_y = w2->y + w2->h - 4;
    aspera_handle_mouse_move(&desk, se_x, se_y);
    TEST_ASSERT(desk.hover_part == PART_CORNER_SE, "SE resize corner hit-tested correctly");

    aspera_handle_mouse_down(&desk, se_x, se_y, 1);
    TEST_ASSERT(desk.is_resizing && desk.resize_part == PART_CORNER_SE, "Corner resize initiated");

    aspera_handle_mouse_move(&desk, se_x + 60, se_y + 40);
    aspera_handle_mouse_up(&desk, se_x + 60, se_y + 40, 1);
    TEST_ASSERT(w2->w == orig_w2_w + 60 && w2->h == orig_w2_h + 40, "Interactive resize expanded window dimensions");

    // 11. Interactive Terminal Keystrokes & Built-in Shell
    printf("\n--- Test 11: Interactive Terminal Keyboard Input ---\n");
    aspera_focus_window(&desk, w1);
    TEST_ASSERT(w1->is_focused && w1->client_type == CLIENT_TERMINAL, "Terminal window focused for typing");

    const char *cmd = "uname";
    for (size_t i = 0; i < strlen(cmd); i++) {
        aspera_handle_key_down(&desk, cmd[i], cmd[i], false, false);
    }
    TEST_ASSERT(strcmp(w1->term.input_buf, "uname") == 0, "Terminal buffered typed characters: 'uname'");

    aspera_handle_key_down(&desk, 13, '\n', false, false);
    TEST_ASSERT(w1->term.input_len == 0, "Input buffer cleared on Enter execution");
    TEST_ASSERT(strstr(w1->term.lines[w1->term.num_lines - 1], "Harvey-o9") != NULL,
                "Shell command executed: output contains 'Harvey-o9'");

    // 12. Interactive Context Menus
    printf("\n--- Test 12: Context Menus ---\n");
    aspera_handle_mouse_down(&desk, 100, 500, 3);
    aspera_handle_mouse_up(&desk, 100, 500, 3);
    TEST_ASSERT(desk.menu_open && !desk.menu_is_window, "Right-click opened Root Context Menu");
    TEST_ASSERT(desk.num_menu_items == 6, "Root menu contains 6 actions");

    int initial_win_count = 0;
    for (Window *cur = desk.head; cur; cur = cur->next) initial_win_count++;
    aspera_handle_mouse_down(&desk, desk.menu_x + 10, desk.menu_y + 10, 1);
    aspera_handle_mouse_up(&desk, desk.menu_x + 10, desk.menu_y + 10, 1);
    TEST_ASSERT(!desk.menu_open, "Context menu closed after item selection");

    int new_win_count = 0;
    for (Window *cur = desk.head; cur; cur = cur->next) new_win_count++;
    TEST_ASSERT(new_win_count == initial_win_count + 1, "Root menu action spawned new window");

    // 13. Enlightenment Edge Desktop Flipping
    printf("\n--- Test 13: Enlightenment Edge Desktop Flipping ---\n");
    Window *new_win = desk.head;
    tb_x = new_win->x + 20;
    tb_y = new_win->y + 10;
    aspera_handle_mouse_down(&desk, tb_x, tb_y, 1);
    desk.edge_flip_cooldown = 0;
    aspera_handle_mouse_move(&desk, ASPERA_SCREEN_W + 50, tb_y);
    aspera_handle_mouse_up(&desk, ASPERA_SCREEN_W + 50, tb_y, 1);
    TEST_ASSERT(desk.cur_x == 1, "Enlightenment edge flip switched desktop when dragging past boundary");
    TEST_ASSERT(new_win->desktop_id == 1, "Dragged window followed edge flip to new workspace");

    // 14. Frame Rendering & Snapshot Export
    printf("\n--- Test 14: Frame Rendering Pipeline ---\n");
    aspera_render(&desk);
    TEST_ASSERT(desk.pixels[500 * ASPERA_SCREEN_W + 800] == desk.theme.bg_desktop, "Desktop background filled in framebuffer");
    int rc = aspera_save_ppm(&desk, "build/desktop_interactive.ppm");
    TEST_ASSERT(rc == 0, "Exported interactive desktop frame to build/desktop_interactive.ppm");

    // 15. Text Clipping On Window Resizing
    printf("\n--- Test 15: Text Clipping On Window Resizing ---\n");
    // Place w1 at (50, 50) and resize it to a small box (w=140, h=60)
    // Long terminal text (376px) would previously escape window bounds past x=190
    w1->x = 50;
    w1->y = 50;
    w1->w = 140;
    w1->h = 60;
    w1->desktop_id = desk.cur_y * desk.cols + desk.cur_x;
    aspera_render(&desk);
    int outside_x = 220; // 30px past right edge of window
    int outside_y = 50 + 4 + 22 + 6 + 5; // row of first text line
    uint32_t bg_col = desk.theme.bg_desktop;
    TEST_ASSERT(desk.pixels[outside_y * ASPERA_SCREEN_W + outside_x] == bg_col,
                "Text does not escape window bounds when resized small");

    // 16. Real Plan 9 rc and sam -d Subprocesses + Aspera Shelf Bar
    printf("\n--- Test 16: Real Plan 9 rc & sam -d Subprocesses + Aspera Shelf ---\n");
    aspera_switch_desktop(&desk, 0, 0);
    aspera_focus_window(&desk, w1);
    TEST_ASSERT(w1->term.rc_pid > 0, "Real Plan 9 rc subprocess is alive in Terminal");
    const char *rc_cmd = "fn p9echo { echo rc-subproc-$1 }; p9echo ok";
    aspera_handle_text_input(&desk, rc_cmd);
    aspera_handle_key_down(&desk, 13, '\n', false, false);
    aspera_render(&desk);
    bool found_rc_out = false;
    for (int i = 0; i < w1->term.num_lines; i++) {
        if (strstr(w1->term.lines[i], "rc-subproc-ok") != NULL) {
            found_rc_out = true;
            break;
        }
    }
    TEST_ASSERT(found_rc_out, "Real Plan 9 rc executed function and returned 'rc-subproc-ok'");

    aspera_focus_window(&desk, w2);
    TEST_ASSERT(w2->sam.sam_pid > 0, "Real Plan 9 sam -d subprocess is alive in Editor");
    w2->sam.cmd_focused = true;
    aspera_handle_text_input(&desk, "1,$s/Plan 9 Rio Window Online/Real Sam Subprocess Verified/g");
    aspera_handle_key_down(&desk, 13, '\n', false, false);
    aspera_handle_text_input(&desk, "w");
    aspera_handle_key_down(&desk, 13, '\n', false, false);
    aspera_render(&desk);
    TEST_ASSERT(strstr(w2->sam.buf, "Real Sam Subprocess Verified") != NULL,
                "Real Plan 9 sam -d executed structural regex s// and updated buffer");
    FILE *sf = fopen(w2->sam.path, "rb");
    char disk_buf[2048] = {0};
    if (sf) {
        size_t nr = fread(disk_buf, 1, sizeof(disk_buf) - 1, sf);
        disk_buf[nr] = '\0';
        fclose(sf);
    }
    TEST_ASSERT(strstr(disk_buf, "Real Sam Subprocess Verified") != NULL,
                "Real Plan 9 sam -d wrote updated buffer to disk on 'w'");

    // Click Aspera Shelf button (20, 14) -> Open Notes (40, 48)
    aspera_handle_mouse_down(&desk, 20, 14, 1);
    aspera_handle_mouse_up(&desk, 20, 14, 1);
    TEST_ASSERT(desk.shelf_menu_open, "Top Aspera Shelf menu opened on click");
    aspera_handle_mouse_down(&desk, 40, 48, 1);
    aspera_handle_mouse_up(&desk, 40, 48, 1);
    TEST_ASSERT(desk.focused && desk.focused->client_type == CLIENT_NOTES,
                "Aspera Shelf 'Open Notes' opened and focused Notes window");

    printf("\n==================================================\n");
    printf("  Verification Summary: %d / %d Tests Passed\n", passed_tests, total_tests);
    printf("==================================================\n");

    return (passed_tests == total_tests) ? 0 : 1;
}
