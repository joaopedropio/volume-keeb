// Copyright 2026 QMK
// SPDX-License-Identifier: GPL-2.0-or-later
#include QMK_KEYBOARD_H
#include <string.h>
#include <stdio.h>
#include "raw_hid.h"
#include "usb_descriptor.h"
#include "gpio.h"
#include "qp.h"
#include "color.h"
#include "qp_comms.h"
#include "qp_surface.h"
#include "qp_nv3007.h"
#include "graphics/font_ui.qff.h"
#include "graphics/font_big.qff.h"
#include "graphics/font_list.qff.h"

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
//   Host -> device, whenever the configured theme changes (see --theme in
//   volume_sync.py) and once right after it (re)connects:
//     THEME_CMD      data[1] = theme id, indexing into `themes[]` below
//   Host -> device, from `volume_sync.py --bootloader` (flash.sh):
//     BOOTLOADER_CMD reboots into the RP2040's BOOTSEL bootloader, so the
//                    board can be re-flashed without pressing BOOT/RESET.
//   Device -> host, one control action for the app currently shown/selected:
//     CTRL_CMD       data[1] = app id, data[2] = action (0 = set volume to
//                    data[3]%, 1 = toggle mute, 2 = play/pause), data[3] = value
#define APP_BEGIN_CMD 0x60
#define APP_ENTRY_CMD 0x61
#define APP_END_CMD 0x62
#define TITLE_CMD 0x54 // 'T'
#define THEME_CMD 0x63
#define BOOTLOADER_CMD 0x64
#define CTRL_CMD 0x70

#define ACTION_SET_VOLUME 0
#define ACTION_TOGGLE_MUTE 1
#define ACTION_PLAYPAUSE 2

#define MAX_APPS 10
#define NAME_MAX 24
#define TITLE_MAX 87
#define HOST_STALE_MS 5000

#define DISPLAY_TIMEOUT_MS 30000
#define HOLD_MS 500
#define DOUBLE_CLICK_MS 300
#define VOLUME_STEP 5

// Dim green, same hue/sat as HSV_GREEN at ~35% brightness. HSV_GREEN/
// HSV_RED/HSV_BLACK come from quantum/color.h.
#define HSV_DIMGREEN 85, 255, 90
// Near-black, used as the fixed dark edge of a raised/sunken bevel
// regardless of theme -- bevel shadows read as "dark", not theme-colored.
#define HSV_BEVEL_DARK 0, 0, 10

// ---------------------------------------------------------------------
// Themes. Each is a flat palette (HSV_* triples) plus a `decor` style:
//   DECOR_FLAT   the original green-phosphor terminal look -- no boxes.
//   DECOR_BOXED  neobrutalist: thick borders + hard, unblurred drop
//                shadows (cream/black/orange reference).
//   DECOR_NEON   synthwave: thin glowing outlines (a bright line with a
//                dim halo around it) over a dark `well`, a gradient rule
//                under the header and a fg->accent gradient volume bar.
//   DECOR_BEVEL  Winamp-style: raised/sunken 1px bevel edges, with `well`
//                as the sunken "LCD readout" background (distinct from
//                `bg`, the chassis color chips/space sit on).
// `themes[]`/`THEME_COUNT` is the index space `--theme` on the host picks
// from.
// ---------------------------------------------------------------------
typedef enum { DECOR_FLAT, DECOR_BOXED, DECOR_BEVEL, DECOR_NEON } decor_t;

typedef struct {
    uint8_t bg_h, bg_s, bg_v;              // chassis / screen background
    uint8_t well_h, well_s, well_v;        // sunken LCD-readout background (DECOR_BEVEL only; equals bg otherwise)
    uint8_t fg_h, fg_s, fg_v;              // primary text / borders / bevel highlight
    uint8_t dim_h, dim_s, dim_v;           // secondary text / unlit elements
    uint8_t accent_h, accent_s, accent_v;  // lit bar segments / selection fill
    uint8_t mute_h, mute_s, mute_v;
    decor_t decor;
} theme_t;

#define THEME_COUNT 4
static const theme_t themes[THEME_COUNT] = {
    // 0: terminal -- flat green phosphor, no decoration
    {HSV_BLACK, HSV_BLACK, HSV_GREEN, HSV_DIMGREEN, HSV_GREEN, HSV_RED, DECOR_FLAT},
    // 1: neobrutal -- cream / near-black / orange, boxed
    {28, 31, 242, 28, 31, 242, 0, 0, 26, 29, 63, 138, 20, 177, 242, 0, 173, 214, DECOR_BOXED},
    // 2: winamp -- grey-blue chassis, black LCD wells, green text, amber accent, beveled
    {170, 41, 88, 85, 85, 15, HSV_GREEN, HSV_DIMGREEN, 31, 211, 255, 1, 188, 232, DECOR_BEVEL},
    // 3: neon -- synthwave: deep violet, cyan text, hot-pink accent, glowing outlines
    {190, 235, 22, 190, 235, 22, 128, 200, 255, 190, 120, 150, 222, 210, 255, 10, 230, 255, DECOR_NEON},
};
static uint8_t current_theme = 3;

// Expands to `t->field_h, t->field_v, t->field_s` for use as the trailing
// HSV args of qp_rect()/qp_drawtext_recolor(), the same way the HSV_* macros
// spread into 3 positional args.
#define CT(field) t->field##_h, t->field##_s, t->field##_v

#define BOX_BORDER 1
#define BOX_SHADOW 2

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
static bool dirty_header     = false; // app name / VOL-MUTE row
static bool dirty_title      = false; // big song-title row
static bool dirty_ticker     = false; // just the title's text, scrolled one step
static bool dirty_bar        = false; // segmented volume bar

static uint16_t name_scroll_offset;
static uint16_t title_scroll_offset;
static uint32_t name_scroll_timer;
static uint32_t title_scroll_timer;
#define SCROLL_STEP_MS 350
#define SCROLL_GAP_CHARS 4
// The song title is a news ticker: it enters at the right edge, crosses the
// screen and leaves at the left, TICKER_STEP_PX pixels every TICKER_STEP_MS.
#define TICKER_STEP_PX 2
#define TICKER_STEP_MS 30

static bool     click_down;
static bool     click_hold_fired;
static uint32_t click_press_start;
static bool     click_pending;
static uint32_t click_pending_time;

static bool display_on = true;

// Everything is drawn into `lcd`, a full-screen RGB565 framebuffer, and only
// the rectangle of pixels that actually changed is sent to `panel` (see
// flush_screen()). Drawing straight to the panel showed every clear-then-
// redraw as a flash.
static uint16_t              lcd_fb[LCD_WIDTH * LCD_HEIGHT];
static painter_device_t      lcd;
static painter_device_t      panel;
static bool                  flush_all = true;
static painter_font_handle_t font_ui;
static painter_font_handle_t font_big;
static painter_font_handle_t font_list; // the menu's app list

#define UI_CHAR_H 13
#define BIG_CHAR_H 27
#define LIST_CHAR_H 24
#define MARGIN 6
#define TEXT_W (LCD_WIDTH - 2 * MARGIN)

// App screen: a thin header row (app name left, "VOL nn" / "MUTE" right), a
// big song-title row, and a segmented VU-meter-style volume bar along the
// bottom -- decorated per-theme (badged/framed, or sunk into an LCD well).
#define ROW1_Y 10
#define TITLE_Y 40
#define BAR_Y 106
#define BAR_H 20
#define BAR_W (LCD_WIDTH - 2 * MARGIN)
#define BAR_SEGMENTS 30
#define BAR_GAP 3
#define BAR_SEG_W ((BAR_W - (BAR_SEGMENTS - 1) * BAR_GAP) / BAR_SEGMENTS)

// Menu screen: a vertical list, one app per row ("name - song title"),
// scrolling to keep the cursor in view.
#define LIST_ROWS 4
#define LIST_ROW_H 35
#define LIST_BOX_H 32 // leaves room below each row for DECOR_BOXED's shadow
#define LIST_TOP ((LCD_HEIGHT - LIST_ROWS * LIST_ROW_H) / 2)
#define LIST_PAD 8
#define ACTIVE_DOT 5 // marks the row of the app the App screen would show, independent of the cursor

// The ticker renders into an off-screen surface one glyph wider than the
// title row on each side, so glyphs half past either edge get clipped by the
// surface instead of being dropped; only the middle TEXT_W columns are sent
// to the panel. TICKER_PAD must be at least one font_big glyph wide.
#define TICKER_PAD 32
#define TICKER_SURF_W (TEXT_W + 2 * TICKER_PAD)
static uint16_t         ticker_buf[TICKER_SURF_W * BIG_CHAR_H];
static uint16_t         ticker_out[TEXT_W * BIG_CHAR_H];
static painter_device_t ticker_surface;
static uint16_t         ticker_glyph_w;

static void send_ctrl(uint8_t app_id, uint8_t action, uint8_t value) {
    uint8_t buf[RAW_EPSIZE] = {0};
    buf[0]                  = CTRL_CMD;
    buf[1]                  = app_id;
    buf[2]                  = action;
    buf[3]                  = value;
    raw_hid_send(buf, sizeof(buf));
}

static void lcd_clear_region(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    const theme_t *t = &themes[current_theme];
    qp_rect(lcd, x0, y0, x1, y1, CT(bg), true);
}

// A themed rectangle. `selected` picks an accent fill (a lit segment frame,
// a chosen menu chip); `sunken` only matters for DECOR_BEVEL, where it picks
// a recessed "LCD well" look (dark-then-light bevel, `well` fill) over a
// raised "button" look (light-then-dark bevel, `bg` fill) -- a selected
// bevel element always reads as pressed/sunken regardless of `sunken`.
//   DECOR_BOXED: a hard (unblurred) offset shadow behind a thick border.
//   DECOR_FLAT:  just the fill, or (unselected) an outline over the screen
//                bg -- the original terminal look.
static void draw_box(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, const theme_t *t, bool selected, bool sunken) {
    if (t->decor == DECOR_BEVEL) {
        if (selected) {
            qp_rect(lcd, x0, y0, x1, y1, CT(accent), true);
        } else if (sunken) {
            qp_rect(lcd, x0, y0, x1, y1, CT(well), true);
        } else {
            qp_rect(lcd, x0, y0, x1, y1, CT(bg), true);
        }
        bool inset = sunken || selected;
        if (inset) {
            qp_line(lcd, x0, y0, x1, y0, HSV_BEVEL_DARK);
            qp_line(lcd, x0, y0, x0, y1, HSV_BEVEL_DARK);
            qp_line(lcd, x0, y1, x1, y1, CT(fg));
            qp_line(lcd, x1, y0, x1, y1, CT(fg));
        } else {
            qp_line(lcd, x0, y0, x1, y0, CT(fg));
            qp_line(lcd, x0, y0, x0, y1, CT(fg));
            qp_line(lcd, x0, y1, x1, y1, HSV_BEVEL_DARK);
            qp_line(lcd, x1, y0, x1, y1, HSV_BEVEL_DARK);
        }
    } else if (t->decor == DECOR_NEON) {
        // Halo one pixel outside the line, in a darker shade of its hue.
        qp_rect(lcd, x0, y0, x1, y1, CT(well), true);
        if (selected) {
            qp_rect(lcd, x0, y0, x1, y1, t->accent_h, t->accent_s, t->accent_v / 3, false);
            qp_rect(lcd, x0 + 1, y0 + 1, x1 - 1, y1 - 1, CT(accent), false);
        } else {
            qp_rect(lcd, x0 + 1, y0 + 1, x1 - 1, y1 - 1, t->dim_h, t->dim_s, t->dim_v / 2, false);
        }
    } else if (t->decor == DECOR_BOXED) {
        qp_rect(lcd, x0 + BOX_SHADOW, y0 + BOX_SHADOW, x1 + BOX_SHADOW, y1 + BOX_SHADOW, CT(fg), true);
        qp_rect(lcd, x0, y0, x1, y1, CT(fg), true);
        if (selected) {
            qp_rect(lcd, x0 + BOX_BORDER, y0 + BOX_BORDER, x1 - BOX_BORDER, y1 - BOX_BORDER, CT(accent), true);
        } else {
            qp_rect(lcd, x0 + BOX_BORDER, y0 + BOX_BORDER, x1 - BOX_BORDER, y1 - BOX_BORDER, CT(bg), true);
        }
    } else if (selected) {
        qp_rect(lcd, x0, y0, x1, y1, CT(accent), true);
    } else {
        qp_rect(lcd, x0, y0, x1, y1, CT(dim), false);
    }
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

// Draws `text` left-aligned in a `w`x`h` box at (x,y) when it fits, otherwise
// scrolls it a character at a time, wrapping with a gap of blank columns.
// `offset` is the per-field scroll position owned by the caller; color is
// passed as an HSV_* triple (e.g. HSV_GREEN) or CT(field).
static void draw_marquee(uint16_t x, uint16_t y, uint16_t w, uint16_t h, painter_font_handle_t font, const char *text, uint16_t offset, uint8_t fg_h, uint8_t fg_s, uint8_t fg_v) {
    const theme_t *t = &themes[current_theme];
    qp_rect(lcd, x, y, x + w - 1, y + h - 1, CT(well), true);
    if (!text[0]) return;
    uint16_t full_w = qp_textwidth(font, text);
    if (full_w <= w) {
        qp_drawtext_recolor(lcd, x, y, font, text, fg_h, fg_s, fg_v, CT(well));
        return;
    }
    size_t len   = strlen(text);
    size_t total = len + SCROLL_GAP_CHARS;
    char   window[48];
    size_t wi = 0;
    window[0] = '\0';
    for (size_t i = 0; i < total && wi < sizeof(window) - 2; i++) {
        size_t idx     = (offset + i) % total;
        window[wi]     = idx < len ? text[idx] : ' ';
        window[wi + 1] = '\0';
        if (qp_textwidth(font, window) > w) {
            window[wi] = '\0';
            break;
        }
        wi++;
    }
    qp_drawtext_recolor(lcd, x, y, font, window, fg_h, fg_s, fg_v, CT(well));
}

// Hue `i`/`n` of the way from fg to accent (DECOR_NEON's gradients).
static uint8_t neon_hue(const theme_t *t, uint16_t i, uint16_t n) {
    return t->fg_h + (int16_t)(t->accent_h - t->fg_h) * i / (n ? n : 1);
}

// App name (left, dim) and "VOL nn" / "MUTE" (right). Boxed themes badge
// just the VOL/MUTE text; the beveled theme sinks the whole row into one
// LCD-style well, closer to how Winamp's own time/info readouts read.
static void draw_header(void) {
    const theme_t *t = &themes[current_theme];
    app_t         *a = &apps[active_app];
    lcd_clear_region(0, 0, LCD_WIDTH - 1, TITLE_Y - 1);
    if (t->decor == DECOR_BEVEL) {
        draw_box(0, 0, LCD_WIDTH - 1, TITLE_Y - 1, t, false, true);
    }

    char vol_buf[10];
    if (!host_seen) {
        strcpy(vol_buf, "VOL --");
    } else if (a->muted) {
        strcpy(vol_buf, "MUTE");
    } else {
        snprintf(vol_buf, sizeof(vol_buf), "VOL %u", a->volume);
    }
    uint16_t vol_w = qp_textwidth(font_ui, vol_buf);
    uint16_t pad   = 3;
    uint16_t bx1   = LCD_WIDTH - MARGIN - 1;
    uint16_t vol_x = bx1 - BOX_BORDER - pad - vol_w;
    uint16_t bx0   = vol_x - pad - BOX_BORDER;
    uint16_t by0   = ROW1_Y - pad;
    uint16_t by1   = ROW1_Y + UI_CHAR_H - 1 + pad + BOX_BORDER;
    if (t->decor == DECOR_BOXED) {
        draw_box(bx0, by0, bx1, by1, t, false, false);
    }
    if (a->muted) {
        qp_drawtext_recolor(lcd, vol_x, ROW1_Y, font_ui, vol_buf, CT(mute), CT(well));
    } else {
        qp_drawtext_recolor(lcd, vol_x, ROW1_Y, font_ui, vol_buf, CT(fg), CT(well));
    }

    uint16_t name_w = bx0 - MARGIN - 4;
    draw_marquee(MARGIN, ROW1_Y, name_w, UI_CHAR_H, font_ui, a->name, name_scroll_offset, CT(dim));

    if (t->decor == DECOR_NEON) {
        uint16_t y = TITLE_Y - 8;
        for (uint16_t x = MARGIN; x < LCD_WIDTH - MARGIN; x += 4) {
            uint8_t h = neon_hue(t, x - MARGIN, LCD_WIDTH - 2 * MARGIN);
            qp_rect(lcd, x, y, x + 3, y, h, 255, 255, true);
            qp_rect(lcd, x, y + 1, x + 3, y + 1, h, 255, 90, true);
        }
    }
}

static const char *active_title(void) {
    app_t *a = &apps[active_app];
    return a->has_media ? a->title : "";
}

// The big song title, at `title_scroll_offset` pixels into its trip from the
// right edge to the left; dims along with everything else while muted.
// font_big is monospaced, so whole glyphs off the left are skipped by index.
static void draw_ticker(void) {
    const theme_t *t    = &themes[current_theme];
    const char    *text = active_title();
    qp_rect(ticker_surface, 0, 0, TICKER_SURF_W - 1, BIG_CHAR_H - 1, CT(well), true);

    size_t  len   = strlen(text);
    int32_t x     = (int32_t)TEXT_W - title_scroll_offset;
    size_t  first = 0;
    if (x < 0) {
        first = (size_t)(-x) / ticker_glyph_w;
        x += (int32_t)(first * ticker_glyph_w);
    }
    size_t n = 0;
    while (first + n < len && x + (int32_t)(n * ticker_glyph_w) < TEXT_W) n++;
    if (n) {
        char visible[TITLE_MAX + 1];
        memcpy(visible, text + first, n);
        visible[n] = '\0';
        if (apps[active_app].muted) {
            qp_drawtext_recolor(ticker_surface, x + TICKER_PAD, 0, font_big, visible, CT(dim), CT(well));
        } else {
            qp_drawtext_recolor(ticker_surface, x + TICKER_PAD, 0, font_big, visible, CT(fg), CT(well));
        }
    }

    for (uint16_t row = 0; row < BIG_CHAR_H; row++) {
        memcpy(&ticker_out[row * TEXT_W], &ticker_buf[row * TICKER_SURF_W + TICKER_PAD], TEXT_W * sizeof(uint16_t));
    }
    qp_viewport(lcd, MARGIN, TITLE_Y, MARGIN + TEXT_W - 1, TITLE_Y + BIG_CHAR_H - 1);
    qp_pixdata(lcd, ticker_out, TEXT_W * BIG_CHAR_H);
}

static void draw_title(void) {
    const theme_t *t = &themes[current_theme];
    if (t->decor == DECOR_BEVEL) {
        draw_box(MARGIN - 2, TITLE_Y, LCD_WIDTH - MARGIN + 1, TITLE_Y + BIG_CHAR_H + 1, t, false, true);
    }
    draw_ticker();
}

// A VU-meter-style row of segments instead of one continuous fill, in an
// optional bordered/shadowed frame.
static void draw_bar(void) {
    const theme_t *t         = &themes[current_theme];
    app_t         *a         = &apps[active_app];
    uint8_t        percent   = a->volume > 100 ? 100 : a->volume;
    uint8_t        lit       = (host_seen && !a->muted) ? (uint8_t)(percent * BAR_SEGMENTS / 100) : 0;
    uint16_t       actual_w  = BAR_SEGMENTS * BAR_SEG_W + (BAR_SEGMENTS - 1) * BAR_GAP;
    uint16_t       start_x   = MARGIN + (BAR_W - actual_w) / 2;

    lcd_clear_region(0, BAR_Y - 4, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    if (t->decor != DECOR_FLAT) {
        draw_box(start_x - 3, BAR_Y - 3, start_x + actual_w - 1 + 3, BAR_Y + BAR_H - 1 + 3, t, false, true);
    }
    for (uint8_t i = 0; i < BAR_SEGMENTS; i++) {
        uint16_t x = start_x + i * (BAR_SEG_W + BAR_GAP);
        if (i < lit && t->decor == DECOR_NEON) {
            qp_rect(lcd, x, BAR_Y, x + BAR_SEG_W - 1, BAR_Y + BAR_H - 1, neon_hue(t, i, BAR_SEGMENTS - 1), 230, 255, true);
        } else if (i < lit) {
            qp_rect(lcd, x, BAR_Y, x + BAR_SEG_W - 1, BAR_Y + BAR_H - 1, CT(accent), true);
        } else if (t->decor == DECOR_NEON) {
            qp_rect(lcd, x, BAR_Y, x + BAR_SEG_W - 1, BAR_Y + BAR_H - 1, t->dim_h, t->dim_s, t->dim_v / 3, true);
        } else {
            qp_rect(lcd, x, BAR_Y, x + BAR_SEG_W - 1, BAR_Y + BAR_H - 1, CT(dim), true);
        }
    }
}

static void render_app_screen(bool full) {
    if (full) {
        lcd_clear_region(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    }
    draw_header();
    draw_title();
    draw_bar();
}

static void render_menu_screen(void) {
    const theme_t *t = &themes[current_theme];
    lcd_clear_region(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    if (app_count == 0) {
        const char *msg = "no apps";
        uint16_t    w   = qp_textwidth(font_list, msg);
        qp_drawtext_recolor(lcd, (LCD_WIDTH - w) / 2, (LCD_HEIGHT - LIST_CHAR_H) / 2, font_list, msg, CT(dim), CT(bg));
        return;
    }

    uint8_t start = 0;
    if (app_count > LIST_ROWS) {
        uint8_t half = LIST_ROWS / 2;
        if (menu_cursor > half) start = menu_cursor - half;
        if (start > app_count - LIST_ROWS) start = app_count - LIST_ROWS;
    }
    uint8_t visible = app_count - start;
    if (visible > LIST_ROWS) visible = LIST_ROWS;

    uint16_t x0     = MARGIN;
    uint16_t x1     = LCD_WIDTH - MARGIN - 1 - BOX_SHADOW;
    uint16_t text_x = x0 + LIST_PAD;
    uint16_t text_w = x1 - LIST_PAD - ACTIVE_DOT - LIST_PAD - text_x;

    for (uint8_t row = 0; row < visible; row++) {
        uint8_t  idx = start + row;
        app_t   *a   = &apps[idx];
        bool     sel = idx == menu_cursor;
        // A selected row is a solid accent bar, except in DECOR_NEON where
        // it's an accent outline and the text lights up instead.
        bool     filled = sel && t->decor != DECOR_NEON;
        uint16_t y0  = LIST_TOP + row * LIST_ROW_H;
        // Unselected rows in the flat theme are bare text, not outlined.
        if (sel || (t->decor != DECOR_FLAT && t->decor != DECOR_NEON)) {
            draw_box(x0, y0, x1, y0 + LIST_BOX_H - 1, t, sel, false);
        }

        // Marks whichever row the App screen would return to -- independent
        // of the navigation cursor, same idea as the reference's "ACTIVE" tag.
        if (idx == active_app) {
            uint16_t dx = x1 - LIST_PAD - ACTIVE_DOT + 1;
            uint16_t dy = y0 + (LIST_BOX_H - ACTIVE_DOT) / 2;
            if (filled) {
                qp_rect(lcd, dx, dy, dx + ACTIVE_DOT - 1, dy + ACTIVE_DOT - 1, CT(bg), true);
            } else {
                qp_rect(lcd, dx, dy, dx + ACTIVE_DOT - 1, dy + ACTIVE_DOT - 1, CT(fg), true);
            }
        }

        uint16_t ty = y0 + (LIST_BOX_H - LIST_CHAR_H) / 2;
        char     name[NAME_MAX + 3];
        fit_text(a->name, name, sizeof(name), font_list, text_w);
        uint16_t name_w = qp_textwidth(font_list, name);
        if (filled) {
            qp_drawtext_recolor(lcd, text_x, ty, font_list, name, CT(bg), CT(accent));
        } else if (sel) {
            qp_drawtext_recolor(lcd, text_x, ty, font_list, name, CT(accent), CT(well));
        } else {
            qp_drawtext_recolor(lcd, text_x, ty, font_list, name, CT(fg), CT(bg));
        }

        // The song title, if any, after the name in the secondary color.
        if (a->has_media && a->title[0] && name_w < text_w) {
            char song[TITLE_MAX + 4];
            char fit[TITLE_MAX + 6];
            snprintf(song, sizeof(song), " - %s", a->title);
            fit_text(song, fit, sizeof(fit), font_list, text_w - name_w);
            if (filled) {
                qp_drawtext_recolor(lcd, text_x + name_w, ty, font_list, fit, CT(bg), CT(accent));
            } else if (sel) {
                qp_drawtext_recolor(lcd, text_x + name_w, ty, font_list, fit, CT(fg), CT(well));
            } else {
                qp_drawtext_recolor(lcd, text_x + name_w, ty, font_list, fit, CT(dim), CT(bg));
            }
        }
    }
}

static void goto_menu(void) {
    screen            = SCREEN_MENU;
    menu_cursor       = active_app;
    need_full_redraw  = true;
}

static void goto_app(uint8_t id) {
    active_app          = id;
    screen               = SCREEN_APP;
    name_scroll_offset   = 0;
    title_scroll_offset  = 0;
    need_full_redraw     = true;
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
    dirty_header              = true;
    dirty_title                = true;
    dirty_bar                   = true;
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
        a->volume     = (uint8_t)v;
        dirty_header  = true;
        dirty_bar     = true;
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
    if (active_app < n && strcmp(apps[active_app].title, apps_staging[active_app].title) != 0) {
        title_scroll_offset = 0;
    }
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
        dirty_menu         = true;
        dirty_header         = true;
        dirty_title            = true;
        dirty_bar                = true;
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
        case BOOTLOADER_CMD: {
            reset_keyboard();
            break;
        }
        case THEME_CMD: {
            if (length < 2) return;
            uint8_t id = data[1];
            if (id >= THEME_COUNT || id == current_theme) return;
            current_theme     = id;
            need_full_redraw = true;
            break;
        }
    }
}

void keyboard_post_init_user(void) {
    gpio_set_pin_output(LCD_BLK_PIN);
    LCD_BLK_ON();

    panel = qp_nv3007_make_spi_device(LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT, LCD_CS_PIN, LCD_DC_PIN, LCD_RST_PIN, LCD_SPI_DIVISOR, LCD_SPI_MODE);
    qp_init(panel, QP_ROTATION_90);
    // The 142x428 glass sits inside the NV3007's 168x428 RAM; in the rotated
    // (landscape) frame the spare columns become a Y offset. Tune
    // LCD_OFFSET_Y in config.h if the image is shifted or clipped.
    qp_set_viewport_offsets(panel, LCD_OFFSET_X, LCD_OFFSET_Y);
    qp_power(panel, true);

    lcd = qp_make_rgb565_surface(LCD_WIDTH, LCD_HEIGHT, lcd_fb);
    qp_init(lcd, QP_ROTATION_0);
    lcd_clear_region(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);

    font_ui   = qp_load_font_mem(font_font_ui);
    font_big  = qp_load_font_mem(font_font_big);
    font_list = qp_load_font_mem(font_font_list);

    ticker_surface = qp_make_rgb565_surface(TICKER_SURF_W, BIG_CHAR_H, ticker_buf);
    qp_init(ticker_surface, QP_ROTATION_0);
    ticker_glyph_w = qp_textwidth(font_big, "M");

    need_full_redraw = true;
}

static void wake_display(void) {
    if (display_on) return;
    display_on = true;
    LCD_BLK_ON();
    qp_power(panel, true);
    need_full_redraw = true;
    flush_all        = true;
}

// Blank by painting black and dropping the backlight, not with qp_power():
// DISPOFF on the NV3007 flashes the lit glass white and lets it fade to
// black over several seconds.
static void sleep_display(void) {
    if (!display_on) return;
    display_on = false;
    qp_rect(panel, 0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, 0, 0, 0, true);
    qp_flush(panel);
    LCD_BLK_OFF();
}

// Sends the framebuffer's changed pixels to the panel (all of it after the
// panel was blanked, since the framebuffer didn't see that).
static void flush_screen(void) {
    qp_surface_draw(lcd, panel, 0, 0, flush_all);
    flush_all = false;
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
        if (qp_textwidth(font_ui, a->name) > TEXT_W && timer_elapsed32(name_scroll_timer) >= SCROLL_STEP_MS) {
            name_scroll_timer = timer_read32();
            name_scroll_offset++;
            dirty_header = true;
        }
        const char *title = active_title();
        if (title[0] && timer_elapsed32(title_scroll_timer) >= TICKER_STEP_MS) {
            title_scroll_timer = timer_read32();
            uint16_t trip       = TEXT_W + qp_textwidth(font_big, title);
            title_scroll_offset = (title_scroll_offset + TICKER_STEP_PX) % trip;
            dirty_ticker        = true;
        }
    }

    if (need_full_redraw) {
        if (screen == SCREEN_MENU) {
            render_menu_screen();
        } else {
            render_app_screen(true);
        }
        need_full_redraw = false;
        dirty_menu         = false;
        dirty_header          = false;
        dirty_title             = false;
        dirty_ticker            = false;
        dirty_bar                 = false;
        flush_screen();
        return;
    }

    if (screen == SCREEN_MENU) {
        if (dirty_menu) {
            render_menu_screen();
            dirty_menu = false;
        }
    } else {
        if (dirty_header) {
            draw_header();
            dirty_header = false;
        }
        if (dirty_title) {
            draw_title();
            dirty_title  = false;
            dirty_ticker = false;
        } else if (dirty_ticker) {
            draw_ticker();
            dirty_ticker = false;
        }
        if (dirty_bar) {
            draw_bar();
            dirty_bar = false;
        }
    }
    flush_screen();
}
