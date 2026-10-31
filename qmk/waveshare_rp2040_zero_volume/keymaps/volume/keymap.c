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
#include "font_store.h"

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
//   Host -> device, the look (set on the host's settings page), whenever it
//   changes and once right after the host (re)connects:
//     THEME_CMD      data[1] = theme id, copies `themes[]` entry as the palette
//     PALETTE_CMD    data[1..18] = bg, well, fg, dim, accent, mute as H,S,V
//                    triples (theme_t's field order), data[19] = decor_t
//   Host -> device, one font upload into a font_store.h slot (0 = font_ui,
//   1 = font_big, 2 = font_list); multi-byte numbers are little-endian:
//     FONT_BEGIN_CMD data[1] = slot, data[2..5] = QFF length, data[6..9] =
//                    its CRC-32. Erases the slot (up to ~1 s); replies.
//     FONT_DATA_CMD  data[1] = slot, data[2..4] = offset, data[5] = n,
//                    data[6..] = n bytes. In order, no reply.
//     FONT_END_CMD   data[1] = slot. Checks the CRC and loads it; replies.
//     FONT_CLEAR_CMD data[1] = slot. Back to the built-in font; replies.
//     FONT_QUERY_CMD replies FONT_INFO_CMD.
//   Host -> device, the screen's idle behaviour, whenever it changes and once
//   right after the host (re)connects:
//     SCREEN_CMD     data[1] = saver_t, data[2..3] = seconds without touching
//                    the knob before the screensaver starts, data[4..5] =
//                    seconds before the screen turns off (0 = never), data[6]
//                    = 1 to start the screensaver right away (a preview).
//   Host -> device, asked on every (re)connect:
//     VERSION_QUERY_CMD replies VERSION_INFO_CMD.
//   Host -> device, from `volume_sync.py --bootloader` (flash.sh):
//     BOOTLOADER_CMD reboots into the RP2040's BOOTSEL bootloader, so the
//                    board can be re-flashed without pressing BOOT/RESET.
//   Device -> host, one control action for the app currently shown/selected:
//     CTRL_CMD       data[1] = app id, data[2] = action (0 = set volume to
//                    data[3]%, 1 = toggle mute, 2 = play/pause), data[3] = value
//   Device -> host, replies to the font commands:
//     FONT_STATUS_CMD data[1] = slot, data[2] = FONT_OK / FONT_ERR_*
//     FONT_INFO_CMD  data[1] = FONT_SLOTS, then each slot's CRC-32 (0 = the
//                    built-in font) from data[2], 4 bytes apiece
//   Device -> host, the reply to VERSION_QUERY_CMD:
//     VERSION_INFO_CMD data[1..3] = major, minor, patch (the repo's VERSION
//                    at build time). The v0.1.0 release's firmware doesn't answer.
#define APP_BEGIN_CMD 0x60
#define APP_ENTRY_CMD 0x61
#define APP_END_CMD 0x62
#define TITLE_CMD 0x54 // 'T'
#define THEME_CMD 0x63
#define BOOTLOADER_CMD 0x64
#define PALETTE_CMD 0x65
#define FONT_BEGIN_CMD 0x66
#define FONT_DATA_CMD 0x67
#define FONT_END_CMD 0x68
#define FONT_CLEAR_CMD 0x69
#define FONT_QUERY_CMD 0x6A
#define SCREEN_CMD 0x6B
#define VERSION_QUERY_CMD 0x6C
#define CTRL_CMD 0x70
#define FONT_STATUS_CMD 0x71
#define FONT_INFO_CMD 0x72
#define VERSION_INFO_CMD 0x73

// Set from the repo's VERSION by rules.mk.
#ifndef FW_VERSION_MAJOR
#    define FW_VERSION_MAJOR 0
#    define FW_VERSION_MINOR 0
#    define FW_VERSION_PATCH 0
#endif

#define ACTION_SET_VOLUME 0
#define ACTION_TOGGLE_MUTE 1
#define ACTION_PLAYPAUSE 2

#define MAX_APPS 10
#define NAME_MAX 24
#define TITLE_MAX 87
#define HOST_STALE_MS 5000
#define HOST_WAIT_MS 5000 // loading screen at boot: after this, assume there's no host app

#define SAVER_AFTER_MS_DEFAULT 30000
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
// `themes[]` are the presets THEME_CMD picks from; the host can also send
// any palette and decor with PALETTE_CMD. Either way it lands in `theme`,
// which is all the drawing code looks at.
// ---------------------------------------------------------------------
typedef enum { DECOR_FLAT, DECOR_BOXED, DECOR_BEVEL, DECOR_NEON, DECOR_COUNT } decor_t;

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
#define DEFAULT_THEME 3
static theme_t theme;

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

// LOADING waits for the host app after boot. Without a host app the knob
// is a plain volume knob: HOME (the screensaver, see goto_home()) and
// SETTINGS (see render_settings_screen()) take the menu's place.
typedef enum { SCREEN_LOADING, SCREEN_MENU, SCREEN_APP, SCREEN_HOME, SCREEN_SETTINGS } screen_t;
static screen_t screen      = SCREEN_LOADING;
static uint32_t boot_at;
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

static bool display_on = true; // false only while the host is suspended

// What the screen does while the knob sits untouched (SCREEN_CMD, from the
// host's settings page): a screensaver after saver_after_ms, the backlight
// off after off_after_ms. These defaults hold until a host sends its own.
typedef enum { SAVER_NONE, SAVER_DRIVE, SAVER_STARS, SAVER_RAIN, SAVER_PLASMA, SAVER_BOUNCE, SAVER_COUNT } saver_t;
static uint8_t  saver_kind     = SAVER_DRIVE;
static uint32_t saver_after_ms = SAVER_AFTER_MS_DEFAULT;
static uint32_t off_after_ms   = 0; // never
static bool     saver_on;
static bool     idle_off;             // backlight off for inactivity
static bool     wake_swallow_release; // the click that woke the screen does nothing else
static uint32_t woke_at;              // the host waking up counts as activity too
static void     start_saver(void);
static void     stop_saver(void);
static bool     wake_ui(void);
static void     restore_ui(void);
static void     set_theme(const theme_t *next);
static void     save_settings(void);

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

// Each font is the built-in one unless the host uploaded a replacement into
// its font_store.h slot. The layout is fixed, so a replacement must have the
// same line height; the host renders it to fit. Glyphs may be any width up
// to TICKER_PAD.
static painter_font_handle_t *const font_handles[FONT_SLOTS] = {&font_ui, &font_big, &font_list};
static const uint8_t *const         font_builtin[FONT_SLOTS] = {font_font_ui, font_font_big, font_font_list};
static const uint8_t                font_height[FONT_SLOTS]  = {UI_CHAR_H, BIG_CHAR_H, LIST_CHAR_H};
#define FONT_BIG_SLOT 1
// Byte offset of a QFF file's line height: block header (5), magic (3),
// version (1), file size and its complement (4 + 4).
#define QFF_LINE_HEIGHT_OFFSET 17
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
// font_big's width of each printable ASCII glyph (titles are ASCII only).
static uint8_t big_glyph_w[0x7F - 0x20];

static uint8_t big_w(char c) {
    return (c >= 0x20 && c < 0x7F) ? big_glyph_w[c - 0x20] : 0;
}

static void send_ctrl(uint8_t app_id, uint8_t action, uint8_t value) {
    uint8_t buf[RAW_EPSIZE] = {0};
    buf[0]                  = CTRL_CMD;
    buf[1]                  = app_id;
    buf[2]                  = action;
    buf[3]                  = value;
    raw_hid_send(buf, sizeof(buf));
}

static void lcd_clear_region(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    const theme_t *t = &theme;
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
    const theme_t *t = &theme;
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
    const theme_t *t = &theme;
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
// Whole glyphs already off the left are skipped, and the ones past the right.
static void draw_ticker(void) {
    const theme_t *t    = &theme;
    const char    *text = active_title();
    qp_rect(ticker_surface, 0, 0, TICKER_SURF_W - 1, BIG_CHAR_H - 1, CT(well), true);

    size_t  len   = strlen(text);
    int32_t x     = (int32_t)TEXT_W - title_scroll_offset;
    size_t  first = 0;
    while (first < len && x + big_w(text[first]) <= 0) {
        x += big_w(text[first]);
        first++;
    }
    size_t  n     = 0;
    int32_t end_x = x;
    while (first + n < len && end_x < TEXT_W) end_x += big_w(text[first + n++]);
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
    const theme_t *t = &theme;
    if (t->decor == DECOR_BEVEL) {
        draw_box(MARGIN - 2, TITLE_Y, LCD_WIDTH - MARGIN + 1, TITLE_Y + BIG_CHAR_H + 1, t, false, true);
    }
    draw_ticker();
}

// A VU-meter-style row of segments instead of one continuous fill, in an
// optional bordered/shadowed frame.
static void draw_bar(void) {
    const theme_t *t         = &theme;
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

// List rows (the menu's apps, the settings): a box per row as the theme
// draws them. A selected row is a solid accent bar, except in DECOR_NEON
// where it's an accent outline and the text lights up instead; unselected
// rows in the flat and neon themes are bare text. Returns whether the row is
// filled, which row_text() needs to pick its colors.
#define ROW_X0 MARGIN
#define ROW_X1 (LCD_WIDTH - MARGIN - 1 - BOX_SHADOW)
#define ROW_TEXT_X (ROW_X0 + LIST_PAD)
static bool draw_row(uint16_t y0, bool sel) {
    const theme_t *t = &theme;
    if (sel || (t->decor != DECOR_FLAT && t->decor != DECOR_NEON)) {
        draw_box(ROW_X0, y0, ROW_X1, y0 + LIST_BOX_H - 1, t, sel, false);
    }
    return sel && t->decor != DECOR_NEON;
}

// Text on a row: the main text in fg (accent when selected), `secondary`
// text in the dim color (fg when selected).
static void row_text(uint16_t x, uint16_t y, const char *text, bool sel, bool filled, bool secondary) {
    const theme_t *t = &theme;
    if (filled) {
        qp_drawtext_recolor(lcd, x, y, font_list, text, CT(bg), CT(accent));
    } else if (sel) {
        if (secondary) {
            qp_drawtext_recolor(lcd, x, y, font_list, text, CT(fg), CT(well));
        } else {
            qp_drawtext_recolor(lcd, x, y, font_list, text, CT(accent), CT(well));
        }
    } else if (secondary) {
        qp_drawtext_recolor(lcd, x, y, font_list, text, CT(dim), CT(bg));
    } else {
        qp_drawtext_recolor(lcd, x, y, font_list, text, CT(fg), CT(bg));
    }
}

static void render_menu_screen(void) {
    const theme_t *t = &theme;
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

    uint16_t text_w = ROW_X1 - LIST_PAD - ACTIVE_DOT - LIST_PAD - ROW_TEXT_X;

    for (uint8_t row = 0; row < visible; row++) {
        uint8_t  idx    = start + row;
        app_t   *a      = &apps[idx];
        bool     sel    = idx == menu_cursor;
        uint16_t y0     = LIST_TOP + row * LIST_ROW_H;
        bool     filled = draw_row(y0, sel);

        // Marks whichever row the App screen would return to -- independent
        // of the navigation cursor, same idea as the reference's "ACTIVE" tag.
        if (idx == active_app) {
            uint16_t dx = ROW_X1 - LIST_PAD - ACTIVE_DOT + 1;
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
        row_text(ROW_TEXT_X, ty, name, sel, filled, false);

        // The song title, if any, after the name in the secondary color.
        if (a->has_media && a->title[0] && name_w < text_w) {
            char song[TITLE_MAX + 4];
            char fit[TITLE_MAX + 6];
            snprintf(song, sizeof(song), " - %s", a->title);
            fit_text(song, fit, sizeof(fit), font_list, text_w - name_w);
            row_text(ROW_TEXT_X + name_w, ty, fit, sel, filled, true);
        }
    }
}

// ---------------------------------------------------------------------
// Loading: right after boot, until the host app sends its first app list
// or HOST_WAIT_MS passes. A bar fills over that time.
// ---------------------------------------------------------------------
#define LOADING_FRAME_MS 100
#define LOADING_BAR_W 200
#define LOADING_BAR_H 6
static uint32_t loading_frame_time;

static void render_loading_screen(void) {
    const theme_t *t = &theme;
    lcd_clear_region(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    const char *name = "volume-keeb";
    uint16_t    w    = qp_textwidth(font_big, name);
    qp_drawtext_recolor(lcd, (LCD_WIDTH - w) / 2, 32, font_big, name, CT(fg), CT(bg));

    uint32_t elapsed = timer_elapsed32(boot_at);
    char     msg[32];
    uint8_t  dots = (elapsed / 400) % 4;
    snprintf(msg, sizeof(msg), "waiting for the computer%.*s", dots, "...");
    // Measured without the dots, so the text doesn't shift as they come and go.
    uint16_t mw = qp_textwidth(font_ui, "waiting for the computer");
    qp_drawtext_recolor(lcd, (LCD_WIDTH - mw) / 2, 74, font_ui, msg, CT(dim), CT(bg));

    uint16_t x0 = (LCD_WIDTH - LOADING_BAR_W) / 2, y0 = 100;
    uint16_t fill = elapsed >= HOST_WAIT_MS ? LOADING_BAR_W : elapsed * LOADING_BAR_W / HOST_WAIT_MS;
    qp_rect(lcd, x0, y0, x0 + LOADING_BAR_W - 1, y0 + LOADING_BAR_H - 1, CT(dim), false);
    if (fill > 2) qp_rect(lcd, x0 + 1, y0 + 1, x0 + fill - 2, y0 + LOADING_BAR_H - 2, CT(accent), true);
}

// ---------------------------------------------------------------------
// Without a host app the knob works on its own:
//   HOME      the screensaver runs as the home screen (or, with none picked,
//             a screen of hints). Turning sends the volume media keys,
//             holding sends mute, a click opens SETTINGS.
//   SETTINGS  a header saying there's no app, and a list of what the knob
//             can change by itself: the theme (the presets), the
//             screensaver, its idle time and the screen-off time, and Exit.
//             Click a row to edit it (its value gets arrows and turning
//             changes it, live), click again to leave it. Exit, holding the
//             knob or leaving it alone for saver_after_ms goes back HOME.
// Changes go to EEPROM at once; a host app that connects later sends its
// own settings and brings back the menu.
// ---------------------------------------------------------------------
enum { SET_THEME, SET_SAVER, SET_SAVER_AFTER, SET_OFF_AFTER, SET_EXIT, SET_COUNT };
#define SETTINGS_ROWS 3
#define SETTINGS_TOP (LCD_HEIGHT - SETTINGS_ROWS * LIST_ROW_H)
static uint8_t settings_cursor;
static bool    settings_editing;

static const char *const theme_names[THEME_COUNT] = {"terminal", "neobrutal", "winamp", "neon"};
static const char *const saver_names[SAVER_COUNT] = {"none", "drive", "stars", "rain", "plasma", "bounce"};
static const char *const settings_labels[SET_COUNT] = {"Theme", "Screensaver", "Start after", "Screen off", "Exit"};
// The choices offered on the knob, in seconds (the settings page has the same).
static const uint16_t saver_after_steps[] = {15, 30, 60, 120, 300, 600, 900, 1800, 3600};
static const uint16_t off_after_steps[]   = {0, 60, 300, 600, 900, 1800, 3600, 7200, 14400};

// themes[] index of the current palette, or -1 if the host set a custom one.
static int8_t theme_index(void) {
    for (uint8_t i = 0; i < THEME_COUNT; i++) {
        if (memcmp(&theme, &themes[i], sizeof(theme)) == 0) return i;
    }
    return -1;
}

static void fmt_duration(char *out, size_t n, uint32_t sec) {
    if (!sec) {
        snprintf(out, n, "never");
    } else if (sec % 3600 == 0) {
        snprintf(out, n, "%lu h", (unsigned long)(sec / 3600));
    } else if (sec % 60 == 0) {
        snprintf(out, n, "%lu min", (unsigned long)(sec / 60));
    } else {
        snprintf(out, n, "%lu s", (unsigned long)sec);
    }
}

static void settings_value(uint8_t item, char *out, size_t n) {
    int8_t ti = theme_index();
    switch (item) {
        case SET_THEME: snprintf(out, n, "%s", ti < 0 ? "custom" : theme_names[ti]); break;
        case SET_SAVER: snprintf(out, n, "%s", saver_names[saver_kind]); break;
        case SET_SAVER_AFTER: fmt_duration(out, n, saver_after_ms / 1000); break;
        case SET_OFF_AFTER: fmt_duration(out, n, off_after_ms / 1000); break;
        default: out[0] = '\0';
    }
}

static void render_settings_screen(void) {
    const theme_t *t = &theme;
    lcd_clear_region(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    uint16_t hy = (SETTINGS_TOP - UI_CHAR_H) / 2;
    qp_drawtext_recolor(lcd, ROW_TEXT_X, hy, font_ui, "NO APP ON THIS COMPUTER", CT(mute), CT(bg));
    const char *hint = settings_editing ? "turn to change, click when done" : "click: change   hold: exit";
    qp_drawtext_recolor(lcd, ROW_X1 - LIST_PAD - qp_textwidth(font_ui, hint), hy, font_ui, hint, CT(dim), CT(bg));

    uint8_t start = 0;
    if (settings_cursor > SETTINGS_ROWS / 2) start = settings_cursor - SETTINGS_ROWS / 2;
    if (start > SET_COUNT - SETTINGS_ROWS) start = SET_COUNT - SETTINGS_ROWS;
    for (uint8_t row = 0; row < SETTINGS_ROWS; row++) {
        uint8_t  item   = start + row;
        bool     sel    = item == settings_cursor;
        uint16_t y0     = SETTINGS_TOP + row * LIST_ROW_H;
        uint16_t ty     = y0 + (LIST_BOX_H - LIST_CHAR_H) / 2;
        bool     filled = draw_row(y0, sel);
        row_text(ROW_TEXT_X, ty, settings_labels[item], sel, filled, false);

        char value[24], shown[28];
        settings_value(item, value, sizeof(value));
        if (!value[0]) continue;
        snprintf(shown, sizeof(shown), sel && settings_editing ? "< %s >" : "%s", value);
        row_text(ROW_X1 - LIST_PAD - qp_textwidth(font_list, shown), ty, shown, sel, filled, !(sel && settings_editing));
    }
}

// The next step above (or below) `cur` in a list of choices.
static uint16_t step_choice(const uint16_t *steps, uint8_t n, uint32_t cur, bool up) {
    if (up) {
        for (uint8_t i = 0; i < n; i++) {
            if (steps[i] > cur) return steps[i];
        }
        return steps[n - 1];
    }
    for (int8_t i = n - 1; i >= 0; i--) {
        if (steps[i] < cur) return steps[i];
    }
    return steps[0];
}

static void settings_change(bool up) {
    switch (settings_cursor) {
        case SET_THEME: {
            int8_t i = theme_index();
            i        = i < 0 ? (up ? 0 : THEME_COUNT - 1) : (i + (up ? 1 : THEME_COUNT - 1)) % THEME_COUNT;
            set_theme(&themes[i]);
            break;
        }
        case SET_SAVER:
            saver_kind = (saver_kind + (up ? 1 : SAVER_COUNT - 1)) % SAVER_COUNT;
            break;
        case SET_SAVER_AFTER:
            saver_after_ms = step_choice(saver_after_steps, sizeof(saver_after_steps) / sizeof(saver_after_steps[0]), saver_after_ms / 1000, up) * 1000UL;
            break;
        case SET_OFF_AFTER:
            off_after_ms = step_choice(off_after_steps, sizeof(off_after_steps) / sizeof(off_after_steps[0]), off_after_ms / 1000, up) * 1000UL;
            break;
    }
    save_settings();
}

static void settings_turn(bool clockwise) {
    if (settings_editing) {
        settings_change(clockwise);
    } else if (clockwise ? settings_cursor + 1 < SET_COUNT : settings_cursor > 0) {
        settings_cursor += clockwise ? 1 : -1;
    }
    dirty_menu = true;
}

static void goto_home(void);

static void settings_click(void) {
    if (settings_cursor == SET_EXIT) {
        goto_home();
        return;
    }
    settings_editing = !settings_editing;
    dirty_menu       = true;
}

// HOME with no screensaver picked: the name and what the knob does.
static void render_home_screen(void) {
    const theme_t *t = &theme;
    lcd_clear_region(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    const char *name = "volume-keeb";
    qp_drawtext_recolor(lcd, (LCD_WIDTH - qp_textwidth(font_big, name)) / 2, 26, font_big, name, CT(fg), CT(bg));
    const char *head = "NO APP ON THIS COMPUTER";
    qp_drawtext_recolor(lcd, (LCD_WIDTH - qp_textwidth(font_ui, head)) / 2, 72, font_ui, head, CT(mute), CT(bg));
    const char *hint = "turn: volume   hold: mute   click: settings";
    qp_drawtext_recolor(lcd, (LCD_WIDTH - qp_textwidth(font_ui, hint)) / 2, 100, font_ui, hint, CT(dim), CT(bg));
}

// Every screen but the App screen redraws whole, on dirty_menu.
static void render_list_screen(void) {
    switch (screen) {
        case SCREEN_LOADING: render_loading_screen(); break;
        case SCREEN_SETTINGS: render_settings_screen(); break;
        case SCREEN_HOME: render_home_screen(); break;
        default: render_menu_screen(); break;
    }
}

static void goto_settings(void) {
    screen           = SCREEN_SETTINGS;
    settings_cursor  = 0;
    settings_editing = false;
    need_full_redraw = true;
}

static void goto_home(void) {
    screen           = SCREEN_HOME;
    settings_editing = false;
    if (saver_kind != SAVER_NONE) {
        start_saver();
    } else {
        stop_saver();
        need_full_redraw = true;
    }
}

// A dark screen comes back on HOME, without swallowing the touch.
static void home_light_up(void) {
    if (!idle_off) return;
    idle_off = false;
    LCD_BLK_ON();
    restore_ui();
    goto_home();
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
    if (screen == SCREEN_HOME) {
        home_light_up();
        tap_code(clockwise ? KC_AUDIO_VOL_UP : KC_AUDIO_VOL_DOWN);
        return false;
    }
    if (wake_ui()) return false;
    if (screen == SCREEN_LOADING) return false;
    if (screen == SCREEN_SETTINGS) {
        settings_turn(clockwise);
        return false;
    }
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

    // HOME and SETTINGS tell a click from a hold (HOLD_MS, fired from
    // housekeeping_task_user()) on their own.
    if (screen == SCREEN_HOME) {
        if (record->event.pressed) {
            click_down        = true;
            click_hold_fired  = false;
            click_press_start = timer_read32();
        } else {
            click_down = false;
            if (!click_hold_fired) {
                wake_ui();
                goto_settings();
            }
        }
        return false;
    }

    if (record->event.pressed && wake_ui()) {
        wake_swallow_release = true;
        return false;
    }
    if (wake_swallow_release && !record->event.pressed) {
        wake_swallow_release = false;
        return false;
    }

    if (screen == SCREEN_LOADING) return false;
    if (screen == SCREEN_SETTINGS) {
        if (record->event.pressed) {
            click_down        = true;
            click_hold_fired  = false;
            click_press_start = timer_read32();
        } else {
            click_down = false;
            if (!click_hold_fired) settings_click();
        }
        return false;
    }
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
    if (screen == SCREEN_LOADING || screen == SCREEN_HOME || screen == SCREEN_SETTINGS) goto_menu();
    if (changed) {
        dirty_menu         = true;
        dirty_header         = true;
        dirty_title            = true;
        dirty_bar                = true;
    }
}

static void set_theme(const theme_t *next) {
    if (memcmp(&theme, next, sizeof(theme)) == 0) return;
    theme            = *next;
    need_full_redraw = true;
    save_settings();
}

// What the knob keeps in EEPROM: the last settings it got, from the host or
// its own settings screen, so it behaves the same without the host app.
// EECONFIG_USER_DATA_VERSION (config.h) must change with this layout.
typedef struct {
    theme_t  theme;
    uint8_t  saver_kind;
    uint16_t saver_after_s, off_after_s;
} settings_t;
_Static_assert(sizeof(settings_t) <= EECONFIG_USER_DATA_SIZE, "settings_t outgrew EECONFIG_USER_DATA_SIZE");

static void save_settings(void) {
    settings_t s;
    memset(&s, 0, sizeof(s)); // the padding goes to EEPROM too
    s.theme         = theme;
    s.saver_kind    = saver_kind;
    s.saver_after_s = saver_after_ms / 1000;
    s.off_after_s   = off_after_ms / 1000;
    eeconfig_update_user_datablock(&s, 0, sizeof(s)); // only writes what changed
}

static void load_settings(void) {
    settings_t s;
    if (!eeconfig_is_user_datablock_valid()) return;
    eeconfig_read_user_datablock(&s, 0, sizeof(s));
    if (s.theme.decor >= DECOR_COUNT || s.saver_kind >= SAVER_COUNT || !s.saver_after_s) return;
    theme          = s.theme;
    saver_kind     = s.saver_kind;
    saver_after_ms = s.saver_after_s * 1000UL;
    off_after_ms   = s.off_after_s * 1000UL;
}

// (Re)loads `slot`'s font: the uploaded one if the slot holds a usable one,
// otherwise the built-in. Returns whether the uploaded one was taken.
static bool load_font(uint8_t slot) {
    painter_font_handle_t *h = font_handles[slot];
    if (*h) qp_close_font(*h);
    *h                 = NULL;
    const uint8_t *qff = font_store_font(slot);
    if (qff && qff[QFF_LINE_HEIGHT_OFFSET] == font_height[slot]) *h = qp_load_font_mem(qff);
    bool uploaded = *h != NULL;
    if (!uploaded) *h = qp_load_font_mem(font_builtin[slot]);

    if (slot == FONT_BIG_SLOT) {
        char s[2] = {0};
        for (uint8_t i = 0; i < sizeof(big_glyph_w); i++) {
            s[0]           = 0x20 + i;
            big_glyph_w[i] = qp_textwidth(font_big, s);
        }
        title_scroll_offset = 0;
    }
    need_full_redraw = true;
    return uploaded;
}

static void send_font_status(uint8_t slot, uint8_t status) {
    uint8_t buf[RAW_EPSIZE] = {FONT_STATUS_CMD, slot, status};
    raw_hid_send(buf, sizeof(buf));
}

static uint32_t read_le(const uint8_t *p, uint8_t n) {
    uint32_t v = 0;
    while (n--) v = v << 8 | p[n];
    return v;
}

static void handle_font_cmd(uint8_t *data, uint8_t length) {
    uint8_t slot = data[1];
    if (data[0] != FONT_QUERY_CMD && slot >= FONT_SLOTS) {
        if (data[0] != FONT_DATA_CMD) send_font_status(slot, FONT_ERR_SLOT);
        return;
    }
    switch (data[0]) {
        case FONT_BEGIN_CMD: {
            if (length < 10) return;
            // The font being replaced lives in the slot about to be erased.
            qp_close_font(*font_handles[slot]);
            *font_handles[slot] = qp_load_font_mem(font_builtin[slot]);
            send_font_status(slot, font_store_begin(slot, read_le(&data[2], 4), read_le(&data[6], 4)));
            break;
        }
        case FONT_DATA_CMD: {
            if (length < 6) return;
            uint8_t n = data[5];
            if (n > length - 6) n = length - 6;
            font_store_write(slot, read_le(&data[2], 3), &data[6], n);
            break;
        }
        case FONT_END_CMD: {
            uint8_t status = font_store_end(slot);
            if (status == FONT_OK && !load_font(slot)) {
                font_store_clear(slot);
                status = FONT_ERR_FONT;
            }
            if (status != FONT_OK) load_font(slot);
            send_font_status(slot, status);
            break;
        }
        case FONT_CLEAR_CMD: {
            font_store_clear(slot);
            load_font(slot);
            send_font_status(slot, FONT_OK);
            break;
        }
        case FONT_QUERY_CMD: {
            uint8_t buf[RAW_EPSIZE] = {FONT_INFO_CMD, FONT_SLOTS};
            for (uint8_t i = 0; i < FONT_SLOTS; i++) {
                uint32_t crc = font_store_crc(i);
                for (uint8_t b = 0; b < 4; b++) buf[2 + i * 4 + b] = crc >> (8 * b);
            }
            raw_hid_send(buf, sizeof(buf));
            break;
        }
    }
}

void raw_hid_receive(uint8_t *data, uint8_t length) {
    if (length < 1) return;
    switch (data[0]) {
        case FONT_BEGIN_CMD:
        case FONT_DATA_CMD:
        case FONT_END_CMD:
        case FONT_CLEAR_CMD:
        case FONT_QUERY_CMD: {
            if (length < 2) return;
            handle_font_cmd(data, length);
            break;
        }
        case PALETTE_CMD: {
            if (length < 20 || data[19] >= DECOR_COUNT) return;
            theme_t next;
            memset(&next, 0, sizeof(next)); // set_theme() compares the padding too
            memcpy(&next, &data[1], 18); // theme_t opens with the six H,S,V triples
            next.decor = data[19];
            set_theme(&next);
            break;
        }
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
        case VERSION_QUERY_CMD: {
            uint8_t reply[RAW_EPSIZE] = {VERSION_INFO_CMD, FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH};
            raw_hid_send(reply, sizeof(reply));
            break;
        }
        case SCREEN_CMD: {
            if (length < 7 || data[1] >= SAVER_COUNT) return;
            // A running screensaver of another kind stops; if the knob is
            // still idle, the new one takes over on the next housekeeping pass.
            if (saver_on && data[1] != saver_kind) stop_saver();
            saver_kind     = data[1];
            saver_after_ms = read_le(&data[2], 2) * 1000;
            off_after_ms   = read_le(&data[4], 2) * 1000;
            if (saver_after_ms < 1000) saver_after_ms = 1000;
            save_settings();
            if (data[6] && saver_kind != SAVER_NONE && display_on) {
                wake_ui();
                woke_at = timer_read32(); // so the preview isn't turned off right away
                start_saver();
            }
            break;
        }
        case THEME_CMD: {
            if (length < 2) return;
            uint8_t id = data[1];
            if (id >= THEME_COUNT) return;
            set_theme(&themes[id]);
            break;
        }
    }
}

void keyboard_post_init_user(void) {
    gpio_set_pin_output(LCD_BLK_PIN);
    LCD_BLK_ON();
    theme = themes[DEFAULT_THEME];
    load_settings();
    boot_at = timer_read32();

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

    for (uint8_t slot = 0; slot < FONT_SLOTS; slot++) load_font(slot);

    ticker_surface = qp_make_rgb565_surface(TICKER_SURF_W, BIG_CHAR_H, ticker_buf);
    qp_init(ticker_surface, QP_ROTATION_0);

    need_full_redraw = true;
}

// The UI comes back by repainting everything: the screensaver and the blank
// panel both went around the framebuffer's dirty tracking.
static void restore_ui(void) {
    need_full_redraw = true;
    flush_all        = true;
}

// Blank by painting black and dropping the backlight, not with qp_power():
// DISPOFF on the NV3007 flashes the lit glass white and lets it fade to
// black over several seconds.
static void blank_panel(void) {
    qp_rect(panel, 0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, 0, 0, 0, true);
    qp_flush(panel);
    LCD_BLK_OFF();
}

void suspend_power_down_user(void) {
    if (!display_on) return;
    display_on = false;
    blank_panel();
}

// Waking the computer brings the UI back, and the idle timers start over.
void suspend_wakeup_init_user(void) {
    if (display_on) return;
    display_on = true;
    saver_on   = false;
    idle_off   = false;
    woke_at    = timer_read32();
    LCD_BLK_ON();
    restore_ui();
}

static uint32_t idle_ms(void) {
    uint32_t input = last_input_activity_elapsed(), wake = timer_elapsed32(woke_at);
    return input < wake ? input : wake;
}

// Sends the framebuffer's changed pixels to the panel (all of it after the
// panel was blanked, since the framebuffer didn't see that).
static void flush_screen(void) {
    qp_surface_draw(lcd, panel, 0, 0, flush_all);
    flush_all = false;
}

// ---------------------------------------------------------------------
// Screensavers: after saver_after_ms without touching the knob (or right
// away, as a preview from the settings page), one of these replaces the UI:
//   SAVER_DRIVE   an endless synthwave drive -- a striped sun behind a city
//                 skyline, a road running into the horizon through a striped
//                 sea, palm trees going by and a car seen from behind,
//                 swaying with the road's curves. Fixed colors.
//   SAVER_STARS   flying through a star field, near stars streaking.
//   SAVER_RAIN    columns of glyphs raining down, Matrix-style.
//   SAVER_PLASMA  a slowly churning plasma cycling through the theme's colors.
//   SAVER_BOUNCE  what's playing (or "volume-keeb") bouncing off the edges,
//                 changing color at each bounce.
// All but the drive take their colors from the theme. Only the knob stops a
// screensaver (see wake_ui()); host updates don't.
//
// Except for the bounce, which moves one small block of text through the
// framebuffer like the UI does, they draw straight into lcd_fb and send it to
// the panel without the surface's dirty tracking, since most of the screen
// changes every frame anyway.
// ---------------------------------------------------------------------
#define SAVER_FRAME_MS 40
#define SCENE_PX 2
#define SCENE_W (LCD_WIDTH / SCENE_PX)  // 214
#define SCENE_H (LCD_HEIGHT / SCENE_PX) // 71
#define HORIZON 27                      // first ground row
#define GROUND_ROWS (SCENE_H - HORIZON)
#define ROAD_CX (SCENE_W / 2)
#define DEPTH_K 32768        // depth of ground row r (1 = horizon) is DEPTH_K / r
#define DRIVE_SPEED 5        // depth units per ms
#define CURVE_PX 30          // how far the vanishing point swings at most
#define CURVE_PERIOD_MS 9000 // one full left-right-left sway
#define PALMS 16             // alternating sides
#define PALM_SPACING 2048    // depth between consecutive palms
#define PALM_NEAR 400

// Panel byte order (byte-swapped RGB565), like the surface stores it.
#define RGB565(r, g, b) ((uint16_t)(((r) & 0xF8) << 8 | ((g) & 0xFC) << 3 | (b) >> 3))
#define SCENE_RGB(r, g, b) ((uint16_t)(RGB565(r, g, b) >> 8 | RGB565(r, g, b) << 8))

static uint32_t saver_start;
static uint32_t saver_frame_time;
static uint32_t saver_last_t;   // the previous frame's time, for savers that step
static uint8_t  saver_prev_top; // drive: highest scene row the last frame touched
static uint32_t saver_seed;

static uint32_t saver_rand(void) {
    saver_seed = saver_seed * 1664525u + 1013904223u;
    return saver_seed >> 8;
}

// Sends the framebuffer to the panel from row y0 down.
static void saver_send(uint16_t y0) {
    qp_viewport(panel, 0, y0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    qp_pixdata(panel, &lcd_fb[y0 * LCD_WIDTH], (uint32_t)(LCD_HEIGHT - y0) * LCD_WIDTH);
}

static const char *const palm_sprite[] = {
    "......p..yp.y.........",
    "...pppp.ppypppy.p.....",
    "..pppppppppppppypp.y..",
    ".yppypppppppppppppppy.",
    "pp.y.pppppppppypyppppy",
    "p....ppppppppppp.yp.py",
    "....yppppppppppppp..py",
    "...pppp.pppppypp.y..p.",
    "..ppy.y.pp.pyp.py.p.y.",
    "..py..y.p..p.y..y.p...",
    "..p...yp...py...y.y...",
    ".......p...py....y....",
    "...........py.........",
    "...........py.........",
    "...........py.........",
    "...........py.........",
    "...........py.........",
    "..........py..........",
    "..........py..........",
    "..........py..........",
    "..........py..........",
    "..........py..........",
    "..........py..........",
    "..........py..........",
    ".........py...........",
    ".........py...........",
    ".........py...........",
    ".........py...........",
    ".........py...........",
    ".........py...........",
    ".........ppy..........",
    ".........ppy..........",
};
#define PALM_W 22
#define PALM_H 32
#define PALM_ROOT 10 // trunk column at the bottom row

static const char *const car_sprite[] = {
    "..........kkkkkkkkkkkkkkkkkkkk..........",
    ".........kPPPPPPPPPPPPPPPPPPPPk.........",
    "........kPkkkkkkkkkkkkkkkkkkkkPk........",
    ".......kPkCCCCCCcccccccccccccckPk.......",
    "......kPkCCCCCccccccccccccccccckPk......",
    ".....kPkCCCCcccccccccccccccccccckPk.....",
    "....kPPkkkkkkkkkkkkkkkkkkkkkkkkkkPPk....",
    "..kkPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPkk..",
    ".kPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPk.",
    "kqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqk",
    "kqrrRRRRrrqqqqqqqqqqqqqqqqqqqqrrRRRRrrqk",
    "kqrrrrrrrrqqqqqqqwwwwwwqqqqqqqrrrrrrrrqk",
    "kqddddddddqqqqqqqwkkkkwqqqqqqqddddddddqk",
    "kqqqqqqqqqqqqqqqqwwwwwwqqqqqqqqqqqqqqqqk",
    "kgwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwgk",
    "kddddgddddddddddddddddddddddddddddgddddk",
    ".kkkkkkksssssssssssssssssssssssskkkkkkk.",
    ".kkkkkkksssssssssssssssssssssssskkkkkkk.",
    "ssssssssssssssssssssssssssssssssssssssss",
    "..ssssssssssssssssssssssssssssssssssss..",
};
#define CAR_W 40
#define CAR_H 20

static uint16_t sprite_color(char c) {
    switch (c) {
        case 'p': return SCENE_RGB(255, 96, 180); // palm leaves
        case 'y': return SCENE_RGB(255, 236, 0);
        case 'k': return SCENE_RGB(16, 8, 24);
        case 'P': return SCENE_RGB(150, 110, 220); // car body, lit
        case 'q': return SCENE_RGB(108, 72, 176);
        case 'd': return SCENE_RGB(64, 40, 112);
        case 'c': return SCENE_RGB(0, 200, 210); // glass
        case 'C': return SCENE_RGB(170, 255, 255);
        case 'r': return SCENE_RGB(200, 16, 48); // tail lights
        case 'R': return SCENE_RGB(255, 90, 110);
        case 'w': return SCENE_RGB(240, 240, 240);
        case 'g': return SCENE_RGB(150, 150, 165);
        case 's': return SCENE_RGB(150, 60, 16); // shadow on the road
        default: return 0;
    }
}

static inline void scene_px(int16_t x, int16_t y, uint16_t c) {
    if (x < 0 || x >= SCENE_W || y < 0 || y >= SCENE_H) return;
    uint16_t *p = &lcd_fb[(y * SCENE_PX) * LCD_WIDTH + x * SCENE_PX];
    p[0] = p[1] = p[LCD_WIDTH] = p[LCD_WIDTH + 1] = c;
}

static uint32_t scene_hash(uint32_t a, uint32_t b) {
    uint32_t h = a * 73856093u ^ b * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    return h ^ (h >> 15);
}

static uint16_t lerp_rgb(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1, uint16_t i, uint16_t n) {
    return SCENE_RGB(r0 + (r1 - r0) * i / n, g0 + (g1 - g0) * i / n, b0 + (b1 - b0) * i / n);
}

// The sky never moves: violet bands warming to orange at the horizon, a
// striped sun, mountains on both sides and the city in front of the sun.
static void draw_sky(void) {
    for (int16_t y = 0; y < HORIZON; y++) {
        uint16_t band = y / 3, bands = (HORIZON - 1) / 3;
        uint16_t c    = band * 2 < bands ? lerp_rgb(60, 16, 110, 150, 50, 170, band, bands / 2) : lerp_rgb(150, 50, 170, 255, 120, 110, band - bands / 2, bands - bands / 2);
        for (int16_t x = 0; x < SCENE_W; x++) scene_px(x, y, c);
    }

    // Sun: yellow to pink downwards, cut by gaps that widen towards the horizon.
    const int16_t sr = 20, scy = HORIZON - 1;
    for (int16_t y = scy - sr; y < HORIZON; y++) {
        int16_t d = scy - y;
        if (d < 12 && (d % 4 == 1 || (d < 6 && d % 4 == 2))) continue;
        uint16_t c = lerp_rgb(255, 240, 40, 255, 70, 140, y - (scy - sr), sr);
        for (int16_t x = ROAD_CX - sr; x <= ROAD_CX + sr; x++) {
            int16_t dx = x - ROAD_CX;
            if (dx * dx + d * d <= sr * sr) scene_px(x, y, c);
        }
    }

    // Mountains: overlapping jagged peaks, low in the middle so the sun shows.
    static const int8_t peaks[][3] = {{8, 12, 1}, {30, 17, 1}, {52, 10, 1}, {72, 6, 1}, {142, 6, 1}, {163, 11, 1}, {186, 16, 1}, {208, 12, 1}};
    for (int16_t x = 0; x < SCENE_W; x++) {
        int16_t h = 0;
        for (uint8_t i = 0; i < sizeof(peaks) / sizeof(peaks[0]); i++) {
            int16_t ph = peaks[i][1] - (x > peaks[i][0] ? x - peaks[i][0] : peaks[i][0] - x) * 3 / 4 - (scene_hash(x, i) & 1);
            if (ph > h) h = ph;
        }
        for (int16_t k = 0; k < h; k++) {
            scene_px(x, HORIZON - 1 - k, k == h - 1 ? SCENE_RGB(230, 110, 200) : SCENE_RGB(72, 30, 100));
        }
    }

    // City: black towers with scattered lit windows and a couple of spires.
    static const uint8_t towers[][3] = {
        {78, 6, 8}, {84, 5, 13}, {89, 7, 9}, {96, 4, 19}, {100, 6, 11}, {106, 5, 7}, {111, 4, 16}, {115, 7, 10}, {122, 5, 14}, {127, 6, 8}, {133, 4, 18}, {137, 5, 6},
    };
    static const uint16_t windows[] = {SCENE_RGB(0, 240, 240), SCENE_RGB(255, 80, 170), SCENE_RGB(255, 236, 0)};
    for (uint8_t i = 0; i < sizeof(towers) / sizeof(towers[0]); i++) {
        int16_t x0 = towers[i][0], w = towers[i][1], h = towers[i][2];
        for (int16_t y = HORIZON - h; y < HORIZON; y++) {
            for (int16_t x = x0; x < x0 + w; x++) {
                uint32_t r = scene_hash(x * 31 + y, i);
                bool     lit = x > x0 && x < x0 + w - 1 && y > HORIZON - h && r % 7 == 0;
                scene_px(x, y, lit ? windows[(r >> 8) % 3] : SCENE_RGB(10, 4, 20));
            }
        }
        if (h >= 16) {
            for (int16_t k = 1; k <= 4; k++) scene_px(x0 + w / 2, HORIZON - h - k, SCENE_RGB(10, 4, 20));
            scene_px(x0 + w / 2, HORIZON - h - 5, SCENE_RGB(255, 80, 170));
        }
    }
}

// Ground row `r` (1 at the horizon): centre and half-width of the road.
static int16_t road_cx(int16_t r, int16_t curve) {
    if (r >= GROUND_ROWS) return ROAD_CX;
    int16_t far = GROUND_ROWS - r;
    return ROAD_CX + curve * far * far / (GROUND_ROWS * GROUND_ROWS);
}

static int16_t road_hw(int16_t r) {
    return 1 + r * 3 / 2;
}

static void draw_ground(uint32_t travel, int16_t curve) {
    const uint16_t road[2]  = {SCENE_RGB(255, 132, 24), SCENE_RGB(240, 110, 20)};
    const uint16_t white    = SCENE_RGB(250, 250, 250);
    const uint16_t sea[4]   = {SCENE_RGB(0, 236, 240), SCENE_RGB(240, 16, 72), SCENE_RGB(240, 16, 72), SCENE_RGB(150, 10, 70)};
    for (int16_t r = 1; r <= GROUND_ROWS; r++) {
        int16_t  y    = HORIZON + r - 1;
        uint32_t z    = DEPTH_K / r + travel;
        int16_t  cx   = road_cx(r, curve);
        int16_t  hw   = road_hw(r);
        int16_t  edge = 1 + r / 16;
        bool     dash = (z >> 9) & 1;
        uint16_t tar  = road[(z >> 10) & 1];
        uint32_t band = z >> 8;
        for (int16_t x = 0; x < SCENE_W; x++) {
            int16_t  dx  = x - cx;
            int16_t  adx = dx < 0 ? -dx : dx;
            uint16_t c;
            if (adx <= hw) {
                c = (adx > hw - edge || (dash && adx * 16 <= r)) ? white : tar;
            } else {
                // Sea: brick-like dashes, stretched wider the closer they are.
                int32_t wx = (int32_t)dx * 48 / r + (int32_t)(scene_hash(band, 0) & 31);
                c          = sea[scene_hash(band, (uint32_t)(wx >> 5)) & 3];
            }
            scene_px(x, y, c);
        }
    }
}

// Palms stand just off the road's edges; returns the highest row drawn.
static int16_t draw_palms(uint32_t travel, int16_t curve) {
    uint32_t zs[PALMS];
    uint8_t  order[PALMS];
    for (uint8_t i = 0; i < PALMS; i++) {
        zs[i]    = (i * PALM_SPACING + PALMS * PALM_SPACING - travel % (PALMS * PALM_SPACING)) % (PALMS * PALM_SPACING) + PALM_NEAR;
        order[i] = i;
    }
    // Far to near, so nearer palms cover farther ones.
    for (uint8_t i = 1; i < PALMS; i++) {
        for (uint8_t j = i; j > 0 && zs[order[j]] > zs[order[j - 1]]; j--) {
            uint8_t t = order[j];
            order[j]  = order[j - 1];
            order[j - 1] = t;
        }
    }
    int16_t top = HORIZON;
    for (uint8_t n = 0; n < PALMS; n++) {
        uint8_t  i    = order[n];
        uint32_t r16  = (uint32_t)DEPTH_K * 16 / zs[i]; // ground row, 1/16 px
        if (r16 < 24) continue;
        int16_t  r    = r16 / 16;
        uint32_t s    = r16 * 256 / (GROUND_ROWS * 16); // scale, 1/256
        int8_t   side = (i & 1) ? 1 : -1;
        int16_t  base = HORIZON - 1 + r;
        int16_t  h    = PALM_H * s / 256;
        int16_t  w    = PALM_W * s / 256;
        if (h < 2) continue;
        int16_t root = road_cx(r, curve) + side * (road_hw(r) + (int16_t)(6 * s / 256) + 1);
        int16_t x0   = root - PALM_ROOT * (int32_t)s / 256;
        if (side > 0) x0 = root - (PALM_W - 1 - PALM_ROOT) * (int32_t)s / 256; // mirrored: lean towards the road
        if (base - h + 1 < top) top = base - h + 1;
        for (int16_t dy = 0; dy < h; dy++) {
            int16_t sy = dy * 256 / s;
            int16_t y  = base - h + 1 + dy;
            if (y >= SCENE_H) break;
            for (int16_t dx = 0; dx < w; dx++) {
                int16_t sx = dx * 256 / s;
                if (sx >= PALM_W || sy >= PALM_H) continue;
                char c = palm_sprite[sy][side > 0 ? PALM_W - 1 - sx : sx];
                if (c != '.') scene_px(x0 + dx, y, sprite_color(c));
            }
        }
    }
    return top < 0 ? 0 : top;
}

static void draw_car(uint32_t t, int16_t curve) {
    int16_t x0 = ROAD_CX - CAR_W / 2 - curve / 5;
    int16_t y0 = SCENE_H - CAR_H - ((t / 110) % 5 == 0 ? 1 : 0);
    for (int16_t y = 0; y < CAR_H; y++) {
        for (int16_t x = 0; x < CAR_W; x++) {
            char c = car_sprite[y][x];
            if (c != '.') scene_px(x0 + x, y0 + y, sprite_color(c));
        }
    }
}

static int16_t saver_curve(uint32_t t) {
    // Triangle wave eased into a smooth sway, no floats needed.
    int32_t p = (int32_t)(t % CURVE_PERIOD_MS) * 4096 / CURVE_PERIOD_MS; // 0..4095
    int32_t q = p < 2048 ? p : 4096 - p;                                 // 0..2048..0
    int32_t u = q - 1024;                                                // -1024..1024
    int32_t e = u * (3 * 1048576 - u * u) / (2 * 1048576);               // smoothstep-like
    return (int16_t)(CURVE_PX * e / 1024);
}

static void render_drive(uint32_t t) {
    uint32_t travel = t * DRIVE_SPEED;
    int16_t  curve  = saver_curve(t);
    draw_sky();
    draw_ground(travel, curve);
    int16_t top = draw_palms(travel, curve);
    draw_car(t, curve);

    // Send from the highest row that changed, now or in the last frame.
    uint8_t from   = top < saver_prev_top ? top : saver_prev_top;
    saver_prev_top = top;
    saver_send(from * SCENE_PX);
}

// ---- colors from the theme ----

typedef struct {
    uint8_t r, g, b;
} saver_rgb_t;

static saver_rgb_t theme_rgb(uint8_t h, uint8_t s, uint8_t v) {
    rgb_t c = hsv_to_rgb_nocie((hsv_t){h, s, v}); // what Quantum Painter uses
    return (saver_rgb_t){c.r, c.g, c.b};
}

// a + (b - a) * i / n, in panel byte order.
static uint16_t mix_rgb(saver_rgb_t a, saver_rgb_t b, uint16_t i, uint16_t n) {
    return lerp_rgb(a.r, a.g, a.b, b.r, b.g, b.b, i, n);
}

// The brighter of the theme's text and accent colors: what the dark-
// background savers draw with (neobrutal's text is near-black, for one).
static saver_rgb_t saver_ink(void) {
    const theme_t *t = &theme;
    return t->fg_v >= t->accent_v ? theme_rgb(CT(fg)) : theme_rgb(CT(accent));
}

static void fill_fb(uint16_t c) {
    for (uint32_t i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) lcd_fb[i] = c;
}

static inline void fb_px(int16_t x, int16_t y, uint16_t c) {
    if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT) return;
    lcd_fb[y * LCD_WIDTH + x] = c;
}

// ---- stars: flying through a field of stars ----
// Stars live in a box in front of the viewer and come closer every frame;
// one that passes the viewer or leaves the screen starts again far away.
#define STARS 140
#define STAR_FAR 4096
#define STAR_NEAR 48
#define STAR_FOCAL 440   // screen px per world unit at depth 1
#define STAR_SPEED 3     // depth units per 2 ms
#define STAR_TRAIL 260   // a near star's streak: where it was this much farther away

typedef struct {
    int16_t  x, y;
    uint16_t z;
} star_t;
static star_t stars[STARS];

static void star_reset(star_t *s, bool anywhere) {
    s->x = (int16_t)(saver_rand() % 4001) - 2000;
    s->y = (int16_t)(saver_rand() % 1401) - 700;
    s->z = anywhere ? STAR_NEAR + saver_rand() % (STAR_FAR - STAR_NEAR) : STAR_FAR;
}

static void start_stars(void) {
    for (uint8_t i = 0; i < STARS; i++) star_reset(&stars[i], true);
}

static void render_stars(uint32_t t) {
    uint32_t    step = (t - saver_last_t) * STAR_SPEED / 2;
    saver_rgb_t ink  = saver_ink(), white = {255, 255, 255}, black = {0, 0, 0};
    const theme_t *th = &theme;
    saver_rgb_t    accent = theme_rgb(th->accent_h, th->accent_s, th->accent_v);
    fill_fb(0);
    for (uint8_t i = 0; i < STARS; i++) {
        star_t *s = &stars[i];
        s->z      = s->z > STAR_NEAR + step ? s->z - step : 0;
        int32_t sx = LCD_WIDTH / 2 + (int32_t)s->x * STAR_FOCAL / (s->z ? s->z : 1);
        int32_t sy = LCD_HEIGHT / 2 + (int32_t)s->y * STAR_FOCAL / (s->z ? s->z : 1);
        if (!s->z || sx < 0 || sx >= LCD_WIDTH || sy < 0 || sy >= LCD_HEIGHT) {
            star_reset(s, false);
            continue;
        }
        // Brighter as it comes closer; a third white, the rest in the theme's colors.
        saver_rgb_t base   = i % 3 == 0 ? white : i % 3 == 1 ? ink : accent;
        uint16_t    near   = STAR_FAR - s->z; // 0..STAR_FAR
        uint16_t    c      = mix_rgb(black, base, near < STAR_FAR / 2 ? near / 8 + 40 : 296, 296);
        int32_t     z0     = s->z + STAR_TRAIL;
        int32_t     tx     = LCD_WIDTH / 2 + (int32_t)s->x * STAR_FOCAL / z0;
        int32_t     ty     = LCD_HEIGHT / 2 + (int32_t)s->y * STAR_FOCAL / z0;
        int32_t     dx = sx - tx, dy = sy - ty;
        int32_t     n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
        for (int32_t k = 1; k <= n; k++) { // the streak, fading towards its tail
            fb_px(tx + dx * k / n, ty + dy * k / n, mix_rgb(black, base, k * (near / 16 + 8) / n, STAR_FAR / 16 + 8));
        }
        fb_px(sx, sy, c);
        if (s->z < STAR_FAR / 4) { // near ones are 2x2
            fb_px(sx + 1, sy, c);
            fb_px(sx, sy + 1, c);
            fb_px(sx + 1, sy + 1, c);
        }
    }
    saver_send(0);
}

// ---- rain: columns of glyphs raining down ----
// Each column has one drop: a bright head and a tail fading behind it. The
// glyphs stay put on their cells and a few change every frame.
#define RAIN_CW 7  // a 5x7 glyph plus the gaps
#define RAIN_CH 10
#define RAIN_COLS (LCD_WIDTH / RAIN_CW)
#define RAIN_ROWS ((LCD_HEIGHT + RAIN_CH - 1) / RAIN_CH)
#define RAIN_X0 ((LCD_WIDTH - RAIN_COLS * RAIN_CW) / 2 + 1)
#define RAIN_FLIPS 14 // glyphs changed per frame
#define RAIN_MIRROR 0x80

// 5x7 glyphs, one byte per row, bit 4 = the left column: digits and some
// katakana-like shapes. Drawn mirrored half the time, like the film's.
static const uint8_t rain_font[][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    {0x1F, 0x01, 0x05, 0x06, 0x04, 0x04, 0x08}, {0x01, 0x02, 0x04, 0x0C, 0x14, 0x04, 0x04}, {0x04, 0x1F, 0x11, 0x01, 0x02, 0x04, 0x08},
    {0x00, 0x1F, 0x04, 0x04, 0x04, 0x04, 0x1F}, {0x08, 0x1F, 0x09, 0x09, 0x09, 0x11, 0x06}, {0x04, 0x1F, 0x04, 0x1F, 0x04, 0x04, 0x04},
    {0x08, 0x0F, 0x11, 0x01, 0x02, 0x04, 0x08}, {0x18, 0x01, 0x18, 0x01, 0x02, 0x04, 0x18}, {0x15, 0x15, 0x01, 0x02, 0x02, 0x04, 0x08},
    {0x00, 0x0A, 0x0A, 0x11, 0x11, 0x11, 0x00}, {0x0E, 0x00, 0x0E, 0x00, 0x1E, 0x01, 0x00}, {0x1F, 0x01, 0x01, 0x0F, 0x01, 0x01, 0x1F},
    {0x0E, 0x00, 0x1F, 0x01, 0x02, 0x04, 0x08}, {0x11, 0x11, 0x11, 0x11, 0x01, 0x02, 0x0C}, {0x04, 0x04, 0x08, 0x08, 0x12, 0x1F, 0x01},
};
#define RAIN_GLYPHS (sizeof(rain_font) / sizeof(rain_font[0]))

typedef struct {
    int16_t head; // in 1/16 rows
    uint8_t speed; // 1/16 rows per frame
    uint8_t len;   // rows
} drop_t;
static drop_t  drops[RAIN_COLS];
static uint8_t rain_cells[RAIN_COLS][RAIN_ROWS];

static uint8_t rain_glyph(void) {
    return saver_rand() % RAIN_GLYPHS | (saver_rand() & 1 ? RAIN_MIRROR : 0);
}

static void drop_reset(drop_t *d, bool anywhere) {
    d->speed = 3 + saver_rand() % 7;
    d->len   = 5 + saver_rand() % 12;
    d->head  = anywhere ? (int16_t)(saver_rand() % ((RAIN_ROWS + 16) * 16)) - 8 * 16 : -(int16_t)(saver_rand() % (RAIN_ROWS * 16));
}

static void start_rain(void) {
    for (uint8_t c = 0; c < RAIN_COLS; c++) {
        drop_reset(&drops[c], true);
        for (uint8_t r = 0; r < RAIN_ROWS; r++) rain_cells[c][r] = rain_glyph();
    }
}

static void draw_rain_glyph(int16_t x, int16_t y, uint8_t g, uint16_t c) {
    const uint8_t *rows = rain_font[g & ~RAIN_MIRROR];
    for (uint8_t r = 0; r < 7; r++) {
        for (uint8_t b = 0; b < 5; b++) {
            if (rows[r] & (g & RAIN_MIRROR ? 1 << b : 0x10 >> b)) fb_px(x + b, y + r, c);
        }
    }
}

static void render_rain(void) {
    saver_rgb_t ink = saver_ink(), white = {255, 255, 255}, black = {0, 0, 0};
    for (uint8_t i = 0; i < RAIN_FLIPS; i++) rain_cells[saver_rand() % RAIN_COLS][saver_rand() % RAIN_ROWS] = rain_glyph();
    fill_fb(0);
    for (uint8_t c = 0; c < RAIN_COLS; c++) {
        drop_t *d = &drops[c];
        d->head += d->speed;
        int16_t head = d->head / 16;
        if (head - d->len >= RAIN_ROWS) {
            drop_reset(d, false);
            continue;
        }
        for (int16_t r = head - d->len + 1; r <= head; r++) {
            if (r < 0 || r >= RAIN_ROWS) continue;
            int16_t  behind = head - r;
            uint16_t col    = behind == 0 ? mix_rgb(ink, white, 3, 4) : mix_rgb(black, ink, d->len - behind, d->len);
            draw_rain_glyph(RAIN_X0 + c * RAIN_CW, r * RAIN_CH + 1, rain_cells[c][r], col);
        }
    }
    saver_send(0);
}

// ---- plasma: overlapping waves through a looping palette ----
#define PLASMA_PAL 64
static int8_t   sin8[256]; // one period, +-127
static uint16_t plasma_pal[PLASMA_PAL];

static void start_plasma(void) {
    // A parabola per half period is close enough to a sine at this size.
    for (uint16_t i = 0; i < 256; i++) {
        int16_t x = i & 127;
        int16_t v = (int32_t)x * (128 - x) * 127 / 4096;
        sin8[i]   = i < 128 ? v : -v;
    }
    // bg -> secondary -> accent -> text -> bg, so cycling through it loops.
    const theme_t *t       = &theme;
    saver_rgb_t    stops[] = {theme_rgb(CT(bg)), theme_rgb(CT(dim)), theme_rgb(CT(accent)), theme_rgb(CT(fg)), theme_rgb(CT(bg))};
    for (uint8_t i = 0; i < PLASMA_PAL; i++) {
        uint8_t seg = i / (PLASMA_PAL / 4), k = i % (PLASMA_PAL / 4);
        plasma_pal[i] = mix_rgb(stops[seg], stops[seg + 1], k, PLASMA_PAL / 4);
    }
}

static void render_plasma(uint32_t t) {
    uint8_t a = t / 23, b = t / 37, c = t / 29, d = t / 17, shift = t / 90;
    int16_t cx = SCENE_W / 2 + sin8[(uint8_t)(t / 61)] * 70 / 127;
    int16_t cy = SCENE_H / 2 + sin8[(uint8_t)(t / 47)] * 30 / 127;
    for (int16_t y = 0; y < SCENE_H; y++) {
        int16_t dy = y - cy, vy = sin8[(uint8_t)(y * 5 - b)];
        for (int16_t x = 0; x < SCENE_W; x++) {
            int16_t  dx = x - cx;
            int16_t  v  = sin8[(uint8_t)(x * 3 + a)] + vy + sin8[(uint8_t)((x + y * 2) * 2 + c)] + sin8[(uint8_t)(((dx * dx + dy * dy * 4) >> 6) - d)];
            uint16_t i  = (((v + 512) >> 4) + shift) % PLASMA_PAL; // v is within +-508
            scene_px(x, y, plasma_pal[i]);
        }
    }
    saver_send(0);
}

// ---- bounce: what's playing bouncing off the edges ----
#define BOUNCE_MAX_W 300
#define BOUNCE_GAP 4 // between the title and the app name under it

static int16_t bounce_x, bounce_y, bounce_dx, bounce_dy;
static uint16_t bounce_w, bounce_h; // the block drawn last frame
static uint8_t  bounce_color;

static void start_bounce(void) {
    const theme_t *t = &theme;
    qp_rect(lcd, 0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, CT(bg), true);
    flush_all = true;
    bounce_x  = 20 + saver_rand() % 100;
    bounce_y  = 10 + saver_rand() % 40;
    bounce_dx = 2;
    bounce_dy = 1;
    bounce_w = bounce_h = 0;
}

static void render_bounce(void) {
    const theme_t *t = &theme;
    char           title[TITLE_MAX + 1] = "volume-keeb", sub[NAME_MAX + 1] = "";
    for (uint8_t i = 0; i < app_count; i++) {
        if (apps[i].playing && apps[i].title[0]) {
            fit_text(apps[i].title, title, sizeof(title), font_big, BOUNCE_MAX_W);
            fit_text(apps[i].name, sub, sizeof(sub), font_ui, BOUNCE_MAX_W);
            break;
        }
    }
    uint16_t tw = qp_textwidth(font_big, title), sw = sub[0] ? qp_textwidth(font_ui, sub) : 0;
    uint16_t w = tw > sw ? tw : sw, h = BIG_CHAR_H + (sub[0] ? BOUNCE_GAP + UI_CHAR_H : 0);

    if (bounce_w) qp_rect(lcd, bounce_x, bounce_y, bounce_x + bounce_w - 1, bounce_y + bounce_h - 1, CT(bg), true);
    bounce_x += bounce_dx;
    bounce_y += bounce_dy;
    bool hit = false;
    if (bounce_x <= 0 || bounce_x + w >= LCD_WIDTH) {
        bounce_dx = -bounce_dx;
        hit       = true;
    }
    if (bounce_y <= 0 || bounce_y + h >= LCD_HEIGHT) {
        bounce_dy = -bounce_dy;
        hit       = true;
    }
    if (hit) bounce_color++;
    if (bounce_x < 0) bounce_x = 0;
    if (bounce_x + w > LCD_WIDTH) bounce_x = LCD_WIDTH - w;
    if (bounce_y < 0) bounce_y = 0;
    if (bounce_y + h > LCD_HEIGHT) bounce_y = LCD_HEIGHT - h;

    // Text, accent and muted colors in turn.
    uint8_t c = bounce_color % 3;
    uint8_t ch = c == 0 ? t->fg_h : c == 1 ? t->accent_h : t->mute_h;
    uint8_t cs = c == 0 ? t->fg_s : c == 1 ? t->accent_s : t->mute_s;
    uint8_t cv = c == 0 ? t->fg_v : c == 1 ? t->accent_v : t->mute_v;
    qp_drawtext_recolor(lcd, bounce_x + (w - tw) / 2, bounce_y, font_big, title, ch, cs, cv, CT(bg));
    if (sub[0]) qp_drawtext_recolor(lcd, bounce_x + (w - sw) / 2, bounce_y + BIG_CHAR_H + BOUNCE_GAP, font_ui, sub, CT(dim), CT(bg));
    bounce_w = w;
    bounce_h = h;
    flush_screen();
}

// ----

static void render_saver_frame(void) {
    uint32_t t = timer_elapsed32(saver_start);
    switch (saver_kind) {
        case SAVER_STARS: render_stars(t); break;
        case SAVER_RAIN: render_rain(); break;
        case SAVER_PLASMA: render_plasma(t); break;
        case SAVER_BOUNCE: render_bounce(); break;
        default: render_drive(t); break;
    }
    saver_last_t = t;
}

static void start_saver(void) {
    saver_on       = true;
    saver_start    = timer_read32();
    saver_last_t   = 0;
    saver_seed     = saver_start * 2654435761u + 1;
    saver_prev_top = 0; // the drive's first frame goes out whole
    switch (saver_kind) {
        case SAVER_STARS: start_stars(); break;
        case SAVER_RAIN: start_rain(); break;
        case SAVER_PLASMA: start_plasma(); break;
        case SAVER_BOUNCE: start_bounce(); break;
    }
    render_saver_frame();
    saver_frame_time = timer_read32();
}

static void stop_saver(void) {
    if (!saver_on) return;
    saver_on = false;
    restore_ui();
}

// The first touch after the screensaver started or the screen went off only
// brings the UI back.
static bool wake_ui(void) {
    if (idle_off) {
        idle_off = false;
        LCD_BLK_ON();
        restore_ui();
        return true;
    }
    if (saver_on) {
        stop_saver();
        return true;
    }
    return false;
}

void housekeeping_task_user(void) {
    if (host_seen && timer_elapsed32(host_last_seen) > HOST_STALE_MS) {
        host_seen  = false;
        app_count  = 0;
        active_app = 0;
        menu_cursor = 0;
        goto_home();
    }
    if (screen == SCREEN_LOADING && timer_elapsed32(boot_at) > HOST_WAIT_MS) goto_home();

    if ((screen == SCREEN_HOME || screen == SCREEN_SETTINGS) && click_down && !click_hold_fired && timer_elapsed32(click_press_start) >= HOLD_MS) {
        click_hold_fired = true;
        if (screen == SCREEN_HOME) {
            tap_code(KC_AUDIO_MUTE);
        } else {
            goto_home();
        }
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

    if (!display_on || idle_off) return;
    uint32_t idle = idle_ms();
    if (off_after_ms && idle > off_after_ms) {
        saver_on = false;
        idle_off = true;
        if (screen == SCREEN_SETTINGS) screen = SCREEN_HOME; // so waking it acts as HOME
        blank_panel();
        return;
    }
    if (screen == SCREEN_SETTINGS && idle > saver_after_ms) {
        goto_home();
    } else if (!saver_on && saver_kind != SAVER_NONE && idle > saver_after_ms) {
        start_saver();
    }
    if (saver_on) {
        if (timer_elapsed32(saver_frame_time) >= SAVER_FRAME_MS) {
            saver_frame_time = timer_read32();
            render_saver_frame();
        }
        return;
    }

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

    if (screen == SCREEN_LOADING && timer_elapsed32(loading_frame_time) >= LOADING_FRAME_MS) {
        loading_frame_time = timer_read32();
        dirty_menu         = true;
    }

    if (need_full_redraw) {
        if (screen == SCREEN_APP) {
            render_app_screen(true);
        } else {
            render_list_screen();
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

    if (screen != SCREEN_APP) {
        if (dirty_menu) {
            render_list_screen();
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
