// Copyright 2026 QMK
// SPDX-License-Identifier: GPL-2.0-or-later
// Diagnostic keymap: no display code at all. Only drives the backlight pin
// high so we can tell whether the board enumerates over USB on its own.
#include QMK_KEYBOARD_H
#include "gpio.h"

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    LAYOUT_ortho_1x1(KC_A)
};

void keyboard_post_init_user(void) {
    gpio_set_pin_output(LCD_BLK_PIN);
    LCD_BLK_ON();
}
