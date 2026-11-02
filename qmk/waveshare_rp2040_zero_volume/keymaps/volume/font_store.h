// Copyright 2026 QMK
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Files uploaded from the host (FONT_*_CMD in keymap.c), kept in flash so
// they survive reboots and re-flashing: a QFF font in each of the first
// FONT_SLOTS slots, then a screensaver animation (keymap.c's ANIM format)
// in each of the ANIM_SLOTS after them.
#define FONT_SLOTS 3
#define ANIM_SLOTS 3
#define ANIM_SLOT0 FONT_SLOTS
#define STORE_SLOTS (FONT_SLOTS + ANIM_SLOTS)

enum {
    FONT_OK,
    FONT_ERR_SLOT,     // no such slot
    FONT_ERR_SIZE,     // empty, or bigger than the slot holds
    FONT_ERR_SEQUENCE, // data out of order, or no upload in progress
    FONT_ERR_CRC,      // what landed in flash doesn't match the host's CRC
    FONT_ERR_FONT,     // not a QFF / animation the keymap can use (see keymap.c)
};

// The file in `slot`, or NULL when it holds none (or a damaged one).
const uint8_t *font_store_font(uint8_t slot);
// Its length in bytes, 0 when it holds none.
uint32_t font_store_length(uint8_t slot);
// CRC-32 (zlib's) of the file in `slot`, 0 when it holds none.
uint32_t font_store_crc(uint8_t slot);
// Copies up to `n` bytes of the file in `slot` from `offset` into `buf`;
// returns how many (0 past its end, or when it holds none).
uint8_t font_store_read(uint8_t slot, uint32_t offset, uint8_t *buf, uint8_t n);

// An upload: begin() erases the slot, write() takes the file in order,
// end() checks the CRC and only then marks the slot valid.
uint8_t font_store_begin(uint8_t slot, uint32_t length, uint32_t crc);
void    font_store_write(uint8_t slot, uint32_t offset, const uint8_t *data, uint8_t n);
uint8_t font_store_end(uint8_t slot);
// Empties `slot` (a font slot goes back to the built-in font).
void font_store_clear(uint8_t slot);
