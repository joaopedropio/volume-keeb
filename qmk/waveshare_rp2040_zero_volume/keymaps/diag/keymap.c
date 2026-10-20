// Copyright 2026 QMK
// SPDX-License-Identifier: GPL-2.0-or-later
// Diagnostic keymap: brings USB up first, and only after 3 s touches the
// display, then cycles solid red / green / blue / black every 1.5 s. If the
// board enumerates over USB but the screen never changes, the SPI wiring or
// pins are at fault; if the board never enumerates, display init hangs.
#include QMK_KEYBOARD_H
#include "gpio.h"
#include "qp.h"
#include "color.h"

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    LAYOUT_ortho_1x1(KC_A)
};

static painter_device_t lcd;
static uint8_t  stage;
static uint32_t next_at = 3000;

void keyboard_post_init_user(void) {
    gpio_set_pin_output(LCD_BLK_PIN);
    LCD_BLK_ON();
}

void housekeeping_task_user(void) {
    if (timer_read32() < next_at) return;
    next_at = timer_read32() + 1500;

    if (stage == 0) {
        lcd = qp_st7789_make_spi_device(LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT, LCD_CS_PIN, LCD_DC_PIN, LCD_RST_PIN, LCD_SPI_DIVISOR, LCD_SPI_MODE);
        qp_init(lcd, QP_ROTATION_90);
        qp_set_viewport_offsets(lcd, 18, 82);
        qp_power(lcd, true);
    }
    switch (stage % 4) {
        case 0: qp_rect(lcd, 0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, HSV_RED, true); break;
        case 1: qp_rect(lcd, 0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, HSV_GREEN, true); break;
        case 2: qp_rect(lcd, 0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, HSV_BLUE, true); break;
        case 3: qp_rect(lcd, 0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, HSV_BLACK, true); break;
    }
    qp_flush(lcd);
    stage++;
}
