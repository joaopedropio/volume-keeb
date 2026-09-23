// Copyright 2026 QMK
// SPDX-License-Identifier: GPL-2.0-or-later
#include QMK_KEYBOARD_H
#include <string.h>
#include <stdio.h>
#include "raw_hid.h"
#include "usb_descriptor.h"
#include "gpio.h"
#include "qp.h"
#include "graphics/font_ui.qff.h"
#include "graphics/font_big.qff.h"

enum custom_keycodes {
    ENC_CLICK = SAFE_RANGE,
};

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    LAYOUT_ortho_1x1(ENC_CLICK)
};

// host/volume_sync.py owns the host side of this protocol (per-app volume
// mixer over QMK Raw HID). Packet layout, byte for byte, must match its
// constants and the parsing in raw_hid_receive() below:
//
//   Host -> device, one full app-list refresh:
//     APP_BEGIN_CMD  data[1] = app count (clamped to MAX_APPS)
//     APP_ENTRY_CMD  data[1] = app id, data[2] = volume%, data[3] = flags
//                    (bit0 muted, bit1 has_media, bit2 playing),
//                    data[4] = name length, data[5..] = name bytes
//     TITLE_CMD      data[1] = app id, data[2] = offset into the title,
//                    data[3..] = chunk. Only sent for apps with has_media set.
//     APP_END_CMD    data[1] = app count (confirms APP_BEGIN); swaps the
//                    staged apps into the active list atomically.
//   Device -> host, one control action for the app currently shown/selected:
//     CTRL_CMD       data[1] = app id, data[2] = action (0 = set volume to
//                    data[3]%, 1 = toggle mute, 2 = play/pause), data[3] = value
#define APP_BEGIN_CMD 0x60
#define APP_ENTRY_CMD 0x61
#define APP_END_CMD 0x62
#define TITLE_CMD 0x54 // 'T'
#define CTRL_CMD 0x70

#define ACTION_SET_VOLUME 0
#define ACTION_TOGGLE_MUTE 1
#define ACTION_PLAYPAUSE 2

#define MAX_APPS 10
#define NAME_MAX 24
#define TITLE_MAX 87
#define HOST_STALE_MS 5000

#define DISPLAY_TIMEOUT_MS 60000
#define HOLD_MS 500
#define DOUBLE_CLICK_MS 300
#define VOLUME_STEP 5

typedef struct {
    uint8_t volume;
    bool    muted;
    bool    has_media;
    bool    playing;
    char    name[NAME_MAX + 1];
    char    title[TITLE_MAX + 1];
} app_t;

static app_t apps[MAX_APPS];
static app_t apps_staging[MAX_APPS];
static uint8_t app_count;
static uint8_t app_count_staging;
static bool    receiving_apps;

static bool     host_seen;
static uint32_t host_last_seen;
static uint32_t host_last_change;

typedef enum { SCREEN_MENU, SCREEN_APP } screen_t;
static screen_t screen      = SCREEN_MENU;
static uint8_t  menu_cursor = 0;
static uint8_t  active_app  = 0;

static bool need_full_redraw = true;
static bool dirty_menu       = false;
static bool dirty_app_meta   = false;
static bool dirty_app_vol    = false;

static uint16_t name_scroll_offset;
static uint16_t title_scroll_offset;
static uint32_t name_scroll_timer;
static uint32_t title_scroll_timer;
#define SCROLL_STEP_MS 350
#define SCROLL_GAP_CHARS 4

static bool     click_down;
static bool     click_hold_fired;
static uint32_t click_press_start;
static bool     click_pending;
static uint32_t click_pending_time;

static bool display_on = true;

static painter_device_t     lcd;
static painter_font_handle_t font_ui;
static painter_font_handle_t font_big;

#define UI_CHAR_H 13
#define BIG_CHAR_H 27
#define MARGIN 4
#define MENU_HEADER_H 22
#define MENU_TOP (MENU_HEADER_H + 4)
#define MENU_ROW_H (UI_CHAR_H + 6)
#define MENU_VISIBLE_ROWS ((LCD_HEIGHT - MENU_TOP) / MENU_ROW_H)
#define APP_NAME_Y 10
#define TITLE_Y 34
#define PERCENT_Y 110
#define BAR_Y 160
#define BAR_H 14
#define BAR_W (LCD_WIDTH - 2 * MARGIN)
#define HINT1_Y (LCD_HEIGHT - 34)
#define HINT2_Y (LCD_HEIGHT - 18)

static void send_ctrl(uint8_t app_id, uint8_t action, uint8_t value) {
    uint8_t buf[RAW_EPSIZE] = {0};
    buf[0]                  = CTRL_CMD;
    buf[1]                  = app_id;
    buf[2]                  = action;
    buf[3]                  = value;
    raw_hid_send(buf, sizeof(buf));
}

static void lcd_clear_region(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    qp_rect(lcd, x0, y0, x1, y1, HSV_BLACK, true);
}

// Shrinks `src` into `out` until it fits within `max_w` px in `font`,
// replacing the tail with ".." when it had to cut something.
static void fit_text(const char *src, char *out, size_t out_size, painter_font_handle_t font, uint16_t max_w) {
    size_t n = strlen(src);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, src, n);
    out[n] = '\0';
    if (qp_textwidth(font, out) <= max_w) return;
    while (n > 0 && qp_textwidth(font, out) > max_w) {
        n--;
        out[n] = '\0';
    }
    uint16_t ellipsis_w = qp_textwidth(font, "..");
    while (n > 2 && qp_textwidth(font, out) + ellipsis_w > max_w) {
        n--;
        out[n] = '\0';
    }
    if (n < strlen(src) && n + 2 < out_size) {
        out[n]     = '.';
        out[n + 1] = '.';
        out[n + 2] = '\0';
    }
}

// Draws `text` centered in a `w`x`h` box at (x,y) when it fits, otherwise
// scrolls it a character at a time, wrapping with a gap of blank columns.
// `offset` is the per-field scroll position owned by the caller.
static void draw_marquee(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *text, uint16_t offset) {
    lcd_clear_region(x, y, x + w - 1, y + h - 1);
    if (!text[0]) return;
    uint16_t full_w = qp_textwidth(font_ui, text);
    if (full_w <= w) {
        qp_drawtext_recolor(lcd, x + (w - full_w) / 2, y, font_ui, text, HSV_WHITE, HSV_BLACK);
        return;
    }
    size_t len   = strlen(text);
    size_t total = len + SCROLL_GAP_CHARS;
    char   window[40];
    size_t wi = 0;
    window[0] = '\0';
    for (size_t i = 0; i < total && wi < sizeof(window) - 2; i++) {
        size_t idx     = (offset + i) % total;
        window[wi]     = idx < len ? text[idx] : ' ';
        window[wi + 1] = '\0';
        if (qp_textwidth(font_ui, window) > w) {
            window[wi] = '\0';
            break;
        }
        wi++;
    }
    qp_drawtext_recolor(lcd, x, y, font_ui, window, HSV_WHITE, HSV_BLACK);
}

static void draw_percent_and_bar(void) {
    app_t *a = &apps[active_app];

    lcd_clear_region(0, PERCENT_Y, LCD_WIDTH - 1, PERCENT_Y + BIG_CHAR_H - 1);
    char buf[6];
    if (!host_seen) {
        strcpy(buf, "--");
    } else if (a->muted) {
        strcpy(buf, "MUTE");
    } else {
        snprintf(buf, sizeof(buf), "%u%%", a->volume);
    }
    uint16_t w = qp_textwidth(font_big, buf);
    uint16_t px = (LCD_WIDTH - w) / 2;
    if (a->muted) {
        qp_drawtext_recolor(lcd, px, PERCENT_Y, font_big, buf, HSV_RED, HSV_BLACK);
    } else {
        qp_drawtext_recolor(lcd, px, PERCENT_Y, font_big, buf, HSV_SPRINGGREEN, HSV_BLACK);
    }

    lcd_clear_region(0, BAR_Y, LCD_WIDTH - 1, BAR_Y + BAR_H - 1);
    uint16_t x0 = MARGIN, y0 = BAR_Y, x1 = MARGIN + BAR_W - 1, y1 = BAR_Y + BAR_H - 1;
    qp_rect(lcd, x0, y0, x1, y1, HSV_WHITE, false);
    if (host_seen && !a->muted) {
        uint8_t  percent = a->volume > 100 ? 100 : a->volume;
        uint16_t fill    = percent * (BAR_W - 2) / 100;
        if (fill > 0) {
            qp_rect(lcd, x0 + 1, y0 + 1, x0 + fill, y1 - 1, HSV_SPRINGGREEN, true);
        }
    }
}

static void render_app_screen(bool full) {
    app_t *a = &apps[active_app];
    if (full) {
        lcd_clear_region(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
        const char *hint1 = "HOLD: mute";
        const char *hint2 = "x2: pause";
        uint16_t    w1    = qp_textwidth(font_ui, hint1);
        uint16_t    w2    = qp_textwidth(font_ui, hint2);
        if (w1 <= LCD_WIDTH) qp_drawtext_recolor(lcd, (LCD_WIDTH - w1) / 2, HINT1_Y, font_ui, hint1, HSV_TEAL, HSV_BLACK);
        if (w2 <= LCD_WIDTH) qp_drawtext_recolor(lcd, (LCD_WIDTH - w2) / 2, HINT2_Y, font_ui, hint2, HSV_TEAL, HSV_BLACK);
    }
    draw_marquee(MARGIN, APP_NAME_Y, LCD_WIDTH - 2 * MARGIN, UI_CHAR_H, a->name, name_scroll_offset);
    draw_marquee(MARGIN, TITLE_Y, LCD_WIDTH - 2 * MARGIN, UI_CHAR_H, a->has_media ? a->title : "", title_scroll_offset);
    draw_percent_and_bar();
}

static void render_menu_screen(bool full) {
    if (full) {
        lcd_clear_region(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
        qp_drawtext_recolor(lcd, MARGIN, 6, font_ui, "APPS", HSV_WHITE, HSV_BLACK);
        qp_rect(lcd, 0, MENU_HEADER_H - 2, LCD_WIDTH - 1, MENU_HEADER_H - 2, HSV_WHITE, true);
    }
    lcd_clear_region(0, MENU_TOP, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    if (app_count == 0) {
        qp_drawtext_recolor(lcd, MARGIN, MENU_TOP + 4, font_ui, "no apps", HSV_WHITE, HSV_BLACK);
        return;
    }

    uint8_t start = 0;
    if (app_count > MENU_VISIBLE_ROWS) {
        uint8_t half = MENU_VISIBLE_ROWS / 2;
        if (menu_cursor > half) start = menu_cursor - half;
        if (start > app_count - MENU_VISIBLE_ROWS) start = app_count - MENU_VISIBLE_ROWS;
    }

    for (uint8_t row = 0; row < MENU_VISIBLE_ROWS && (uint8_t)(start + row) < app_count; row++) {
        uint8_t  idx = start + row;
        uint16_t y   = MENU_TOP + row * MENU_ROW_H;
        bool     sel = idx == menu_cursor;
        if (sel) {
            qp_rect(lcd, 0, y, LCD_WIDTH - 1, y + MENU_ROW_H - 1, HSV_SPRINGGREEN, true);
        }
        char fit[NAME_MAX + 3];
        fit_text(apps[idx].name, fit, sizeof(fit), font_ui, LCD_WIDTH - 2 * MARGIN);
        if (sel) {
            qp_drawtext_recolor(lcd, MARGIN, y + 3, font_ui, fit, HSV_BLACK, HSV_SPRINGGREEN);
        } else {
            qp_drawtext_recolor(lcd, MARGIN, y + 3, font_ui, fit, HSV_WHITE, HSV_BLACK);
        }
    }
}

static void goto_menu(void) {
    screen      = SCREEN_MENU;
    menu_cursor = active_app;
    need_full_redraw = true;
}

static void goto_app(uint8_t id) {
    active_app           = id;
    screen                = SCREEN_APP;
    name_scroll_offset    = 0;
    title_scroll_offset   = 0;
    need_full_redraw      = true;
}

static void handle_menu_select(void) {
    if (app_count == 0) return;
    goto_app(menu_cursor);
}

static void handle_app_single_click(void) {
    goto_menu();
}

static void handle_app_double_click(void) {
    send_ctrl(active_app, ACTION_PLAYPAUSE, 0);
}

static void handle_app_hold(void) {
    apps[active_app].muted = !apps[active_app].muted;
    dirty_app_vol            = true;
    send_ctrl(active_app, ACTION_TOGGLE_MUTE, 0);
}

bool encoder_update_user(uint8_t index, bool clockwise) {
    if (screen == SCREEN_MENU) {
        if (app_count == 0) return false;
        if (clockwise) {
            if (menu_cursor + 1 < app_count) menu_cursor++;
        } else if (menu_cursor > 0) {
            menu_cursor--;
        }
        dirty_menu = true;
    } else {
        if (app_count == 0) return false;
        app_t  *a = &apps[active_app];
        int16_t v = (int16_t)a->volume + (clockwise ? VOLUME_STEP : -VOLUME_STEP);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        a->volume  = (uint8_t)v;
        dirty_app_vol = true;
        send_ctrl(active_app, ACTION_SET_VOLUME, a->volume);
    }
    return false;
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (keycode != ENC_CLICK) return true;

    if (screen == SCREEN_MENU) {
        if (!record->event.pressed) {
            handle_menu_select();
        }
        return false;
    }

    if (record->event.pressed) {
        click_down        = true;
        click_hold_fired  = false;
        click_press_start = timer_read32();
    } else {
        click_down = false;
        if (!click_hold_fired) {
            if (click_pending) {
                click_pending = false;
                handle_app_double_click();
            } else {
                click_pending      = true;
                click_pending_time = timer_read32();
            }
        }
    }
    return false;
}

static void apply_app_snapshot(void) {
    bool    changed = app_count_staging != app_count;
    uint8_t n        = app_count_staging;
    if (!changed) {
        for (uint8_t i = 0; i < n; i++) {
            app_t *a = &apps[i];
            app_t *b = &apps_staging[i];
            if (a->volume != b->volume || a->muted != b->muted || a->has_media != b->has_media || a->playing != b->playing || strcmp(a->name, b->name) != 0 || strcmp(a->title, b->title) != 0) {
                changed = true;
                break;
            }
        }
    }
    memcpy(apps, apps_staging, sizeof(app_t) * n);
    app_count = n;
    if (menu_cursor >= app_count) menu_cursor = app_count ? app_count - 1 : 0;
    if (active_app >= app_count) {
        active_app = 0;
        if (screen == SCREEN_APP) {
            screen            = SCREEN_MENU;
            need_full_redraw = true;
        }
    }
    host_seen      = true;
    host_last_seen = timer_read32();
    if (changed) {
        host_last_change = timer_read32();
        dirty_menu        = true;
        dirty_app_meta     = true;
        dirty_app_vol       = true;
    }
}

void raw_hid_receive(uint8_t *data, uint8_t length) {
    if (length < 1) return;
    switch (data[0]) {
        case APP_BEGIN_CMD: {
            if (length < 2) return;
            app_count_staging = data[1] > MAX_APPS ? MAX_APPS : data[1];
            receiving_apps      = true;
            break;
        }
        case APP_ENTRY_CMD: {
            if (!receiving_apps || length < 5) return;
            uint8_t id = data[1];
            if (id >= MAX_APPS) return;
            app_t *a   = &apps_staging[id];
            a->volume    = data[2] > 100 ? 100 : data[2];
            a->muted     = data[3] & 0x01;
            a->has_media = data[3] & 0x02;
            a->playing   = data[3] & 0x04;
            uint8_t name_len = data[4];
            uint8_t avail     = length - 5;
            if (name_len > avail) name_len = avail;
            if (name_len > NAME_MAX) name_len = NAME_MAX;
            memcpy(a->name, &data[5], name_len);
            a->name[name_len] = '\0';
            a->title[0]         = '\0';
            break;
        }
        case TITLE_CMD: {
            if (!receiving_apps || length < 3) return;
            uint8_t id     = data[1];
            uint8_t offset = data[2];
            if (id >= MAX_APPS || offset >= TITLE_MAX) return;
            uint8_t avail = length - 3;
            uint8_t n      = avail;
            if (offset + n > TITLE_MAX) n = TITLE_MAX - offset;
            memcpy(&apps_staging[id].title[offset], &data[3], n);
            apps_staging[id].title[offset + n] = '\0';
            break;
        }
        case APP_END_CMD: {
            if (!receiving_apps) return;
            receiving_apps = false;
            apply_app_snapshot();
            break;
        }
    }
}

void keyboard_post_init_user(void) {
    gpio_set_pin_output(LCD_BLK_PIN);
    gpio_write_pin_high(LCD_BLK_PIN);

    lcd = qp_st7789_make_spi_device(LCD_WIDTH, LCD_HEIGHT, LCD_CS_PIN, LCD_DC_PIN, LCD_RST_PIN, LCD_SPI_DIVISOR, LCD_SPI_MODE);
    qp_init(lcd, QP_ROTATION_0);
    qp_power(lcd, true);
    qp_rect(lcd, 0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, HSV_BLACK, true);

    font_ui  = qp_load_font_mem(font_font_ui);
    font_big = qp_load_font_mem(font_font_big);

    need_full_redraw = true;
}

static void wake_display(void) {
    if (display_on) return;
    display_on = true;
    gpio_write_pin_high(LCD_BLK_PIN);
    qp_power(lcd, true);
    need_full_redraw = true;
}

static void sleep_display(void) {
    if (!display_on) return;
    display_on = false;
    qp_power(lcd, false);
    gpio_write_pin_low(LCD_BLK_PIN);
}

void housekeeping_task_user(void) {
    if (host_seen && timer_elapsed32(host_last_seen) > HOST_STALE_MS) {
        host_seen  = false;
        app_count  = 0;
        active_app = 0;
        menu_cursor = 0;
        if (screen == SCREEN_APP) screen = SCREEN_MENU;
        need_full_redraw = true;
    }

    if (screen == SCREEN_APP) {
        if (click_down && !click_hold_fired && timer_elapsed32(click_press_start) >= HOLD_MS) {
            click_hold_fired = true;
            click_pending      = false;
            handle_app_hold();
        }
        if (click_pending && timer_elapsed32(click_pending_time) > DOUBLE_CLICK_MS) {
            click_pending = false;
            handle_app_single_click();
        }
    }

    bool idle = last_input_activity_elapsed() > DISPLAY_TIMEOUT_MS && timer_elapsed32(host_last_change) > DISPLAY_TIMEOUT_MS;
    if (idle) {
        sleep_display();
    } else {
        wake_display();
    }
    if (!display_on) return;

    if (screen == SCREEN_APP && app_count) {
        app_t *a = &apps[active_app];
        if (qp_textwidth(font_ui, a->name) > LCD_WIDTH - 2 * MARGIN && timer_elapsed32(name_scroll_timer) >= SCROLL_STEP_MS) {
            name_scroll_timer = timer_read32();
            name_scroll_offset++;
            dirty_app_meta = true;
        }
        const char *title = a->has_media ? a->title : "";
        if (title[0] && qp_textwidth(font_ui, title) > LCD_WIDTH - 2 * MARGIN && timer_elapsed32(title_scroll_timer) >= SCROLL_STEP_MS) {
            title_scroll_timer = timer_read32();
            title_scroll_offset++;
            dirty_app_meta = true;
        }
    }

    if (need_full_redraw) {
        if (screen == SCREEN_MENU) {
            render_menu_screen(true);
        } else {
            render_app_screen(true);
        }
        need_full_redraw = false;
        dirty_menu         = false;
        dirty_app_meta      = false;
        dirty_app_vol        = false;
        return;
    }

    if (screen == SCREEN_MENU) {
        if (dirty_menu) {
            render_menu_screen(false);
            dirty_menu = false;
        }
    } else {
        if (dirty_app_meta) {
            app_t *a = &apps[active_app];
            draw_marquee(MARGIN, APP_NAME_Y, LCD_WIDTH - 2 * MARGIN, UI_CHAR_H, a->name, name_scroll_offset);
            draw_marquee(MARGIN, TITLE_Y, LCD_WIDTH - 2 * MARGIN, UI_CHAR_H, a->has_media ? a->title : "", title_scroll_offset);
            dirty_app_meta = false;
        }
        if (dirty_app_vol) {
            draw_percent_and_bar();
            dirty_app_vol = false;
        }
    }
}
