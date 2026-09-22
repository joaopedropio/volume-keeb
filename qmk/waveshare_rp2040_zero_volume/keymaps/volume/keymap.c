// Copyright 2026 QMK
// SPDX-License-Identifier: GPL-2.0-or-later
#include QMK_KEYBOARD_H
#include <string.h>
#include "raw_hid.h"

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    LAYOUT_ortho_1x1(KC_MUTE)
};

// The keyboard can't read the host's state, so host/volume_sync.py pushes it
// over Raw HID:
//   VOLUME_CMD: data[1] = volume percent, data[2] = muted
//   TITLE_CMD:  data[1] = offset into the title, data[2..] = chunk. The host
//               always sends TITLE_MAX bytes of printable ASCII, NUL padded;
//               an empty title means nothing is playing.
// The script resends every couple of seconds; if it stops, the display shows
// "--%" instead of stale values.
#define VOLUME_CMD 0x56 // 'V'
#define TITLE_CMD 0x54  // 'T'
#define TITLE_MAX 120
#define HOST_STALE_MS 5000

static uint8_t  host_volume;
static bool     host_muted;
static bool     host_seen;
static uint32_t host_last_seen;
static uint32_t host_last_change;
static bool     redraw = true;
static char     title[TITLE_MAX + 1];
static char     title_pending[TITLE_MAX + 1];
static uint16_t scroll_pos;
static uint32_t scroll_timer;

static void host_changed(void) {
    host_last_change = timer_read32();
    redraw           = true;
#ifdef OLED_ENABLE
    oled_on(); // wake the display on changes made from the host too
#endif
}

static void receive_title(uint8_t *data, uint8_t length) {
    uint8_t offset = data[1];
    for (uint8_t i = 2; i < length && offset < TITLE_MAX; i++, offset++) {
        title_pending[offset] = data[i];
    }
    if (offset < TITLE_MAX) {
        return; // more chunks to come
    }
    if (memcmp(title, title_pending, TITLE_MAX) != 0) {
        memcpy(title, title_pending, TITLE_MAX);
        scroll_pos   = 0;
        scroll_timer = timer_read32();
        host_changed();
    }
}

void raw_hid_receive(uint8_t *data, uint8_t length) {
    if (length < 3) {
        return;
    }
    if (data[0] == TITLE_CMD) {
        receive_title(data, length);
        return;
    }
    if (data[0] != VOLUME_CMD) {
        return;
    }
    bool changed = !host_seen || data[1] != host_volume || (data[2] != 0) != host_muted;
    host_volume    = data[1];
    host_muted     = data[2] != 0;
    host_seen      = true;
    host_last_seen = timer_read32();
    if (changed) {
        host_changed();
    }
}

#ifdef OLED_ENABLE
// The driver's font is static to oled_driver.c, so take our own copy for 2x text.
#    include OLED_FONT_H

// The panel is natively 64x128 portrait; rotate to 128x64 landscape.
#    define SCREEN_WIDTH OLED_DISPLAY_HEIGHT
#    define SCALE 2
#    define CHAR_W (OLED_FONT_WIDTH * SCALE)
#    define CHAR_H (OLED_FONT_HEIGHT * SCALE)
#    define ROW_TOP 46                         // bottom row: bar + volume text
#    define TITLE_TOP ((ROW_TOP - CHAR_H) / 2) // centered in the space above
#    define VOLUME_W (4 * CHAR_W)              // fits "100%"
#    define BAR_W (SCREEN_WIDTH - VOLUME_W - 4)
#    define BAR_HEIGHT 12
#    define BAR_TOP (ROW_TOP + (CHAR_H - BAR_HEIGHT) / 2)
#    define SCROLL_GAP (3 * CHAR_W) // blank space before the title repeats
#    define SCROLL_STEP_MS 30        // 1 px per step
#    define SCROLL_PAUSE_MS 1500     // hold at the start of each loop

oled_rotation_t oled_init_user(oled_rotation_t rotation) {
    return OLED_ROTATION_90;
}

// Draws `len` chars of `str` at 2x into the `width` px wide area at (`left`,
// `top`), shifted left by `scroll` px (negative shifts right, for alignment).
// With `period` > 0 the text repeats every `period` px (for marquee scrolling).
static void draw_text_2x(uint8_t left, uint8_t top, uint8_t width, const char *str, uint16_t len, int16_t scroll, uint16_t period) {
    for (uint8_t x = 0; x < width; x++) {
        int16_t col = x + scroll;
        if (period) {
            col %= period;
        }
        uint16_t index  = col / CHAR_W;
        char     c      = col >= 0 && index < len ? str[index] : ' ';
        uint8_t  column = 0;
        if (c >= OLED_FONT_START && c <= OLED_FONT_END) {
            column = pgm_read_byte(&font[(c - OLED_FONT_START) * OLED_FONT_WIDTH + (col % CHAR_W) / SCALE]);
        }
        for (uint8_t y = 0; y < CHAR_H; y++) {
            oled_write_pixel(left + x, top + y, column & (1 << (y / SCALE)));
        }
    }
}

static void render_bar(uint8_t percent) {
    uint8_t fill = (percent > 100 ? 100 : percent) * (BAR_W - 2) / 100;
    for (uint8_t x = 0; x < BAR_W; x++) {
        for (uint8_t y = BAR_TOP; y < BAR_TOP + BAR_HEIGHT; y++) {
            bool border = x == 0 || x == BAR_W - 1 || y == BAR_TOP || y == BAR_TOP + BAR_HEIGHT - 1;
            oled_write_pixel(x, y, border || (x <= fill && host_seen && !host_muted));
        }
    }
}

bool oled_task_user(void) {
    if (host_seen && timer_elapsed32(host_last_seen) > HOST_STALE_MS) {
        host_seen = false;
        title[0]  = '\0';
        redraw    = true;
    }
    // Rendering calls oled_on(), which resets the display timeout, so leave the
    // buffer alone while the display is asleep.
    if (!is_oled_on()) {
        return false;
    }

    const char *text     = title[0] ? title : "VOLUME";
    uint16_t    len      = strlen(text);
    uint16_t    period   = len * CHAR_W + SCROLL_GAP;
    bool        overflow = len * CHAR_W > SCREEN_WIDTH;
    bool        idle     = last_input_activity_elapsed() > OLED_TIMEOUT && timer_elapsed32(host_last_change) > OLED_TIMEOUT;
    if (overflow && !idle) {
        if (timer_elapsed32(scroll_timer) >= (scroll_pos == 0 ? SCROLL_PAUSE_MS : SCROLL_STEP_MS)) {
            scroll_timer = timer_read32();
            scroll_pos   = (scroll_pos + 1) % period;
            redraw       = true;
        }
    } else if (scroll_pos != 0) {
        // Park at the start once idle so the display can time out.
        scroll_pos = 0;
        redraw     = true;
    }
    if (!redraw) {
        return false;
    }
    redraw = false;

    int16_t title_scroll = overflow ? scroll_pos : -(SCREEN_WIDTH - len * CHAR_W) / 2;
    draw_text_2x(0, TITLE_TOP, SCREEN_WIDTH, text, len, title_scroll, overflow ? period : 0);

    char volume[8];
    if (!host_seen) {
        strcpy(volume, "--%");
    } else if (host_muted) {
        strcpy(volume, "MUTE");
    } else {
        strcpy(volume, get_u8_str(host_volume, ' '));
        strcat(volume, "%");
    }
    // Right-aligned beside the bar.
    uint8_t volume_len = strlen(volume);
    draw_text_2x(SCREEN_WIDTH - VOLUME_W, ROW_TOP, VOLUME_W, volume, volume_len, -(VOLUME_W - volume_len * CHAR_W), 0);
    render_bar(host_volume);
    return false;
}
#endif
