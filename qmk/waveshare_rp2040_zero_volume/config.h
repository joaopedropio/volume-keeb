// Copyright 2026 QMK
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Give the host extra time to notice the remote-wakeup packet.
#define USB_SUSPEND_WAKEUP_DELAY 200

// 2.79" 142x428 NV3007 LCD, native panel orientation, mounted/driven landscape
// (rotated 90 degrees so the UI is 428x142) over SPI0.
// RP2040 GPIO alternate-function mux ties the SPI0 signals to specific pins;
// SCK/MOSI must be GP2/GP3 (or GP6/GP7) and MISO GP0/GP4/GP16/GP20 for the
// SPI0 peripheral to actually see them. MISO is unused by this write-only
// display but the ChibiOS SPI driver still wants a valid, SPI0-capable pin
// configured for it; GP16 isn't broken out on the RP2040-Zero, so it
// doesn't collide with the DC/BL GPIOs.
#define SPI_DRIVER SPID0
#define SPI_SCK_PIN GP2
#define SPI_MOSI_PIN GP3
#define SPI_MISO_PIN GP16

#define LCD_CS_PIN GP1
#define LCD_DC_PIN GP4
#define LCD_RST_PIN GP5
#define LCD_SPI_DIVISOR 4
#define LCD_SPI_MODE 3

// Backlight is a plain digital GPIO (no PWM/dimming) toggled in the keymap
// to blank the panel while the host is suspended.
#define LCD_BLK_PIN GP0
// The NV3007 module's backlight is active-high: driving the pin high turns it on.
#define LCD_BLK_ON() gpio_write_pin_high(LCD_BLK_PIN)
#define LCD_BLK_OFF() gpio_write_pin_low(LCD_BLK_PIN)

// Quantum Painter's own idle timeout (30 s by default) sends DISPOFF behind
// the keymap's back, which flashes the lit NV3007 white and fades it out.
// The keymap runs a screensaver instead (DISPLAY_TIMEOUT_MS), so turn it off.
#define QUANTUM_PAINTER_DISPLAY_TIMEOUT 0

// Two RGB565 surfaces: the full-screen framebuffer and the title ticker.
#ifndef SURFACE_NUM_DEVICES
#    define SURFACE_NUM_DEVICES 2
#endif

// Native panel pixel dimensions, passed straight to qp_nv3007_make_spi_device().
#define LCD_PANEL_WIDTH 142
#define LCD_PANEL_HEIGHT 428

// Glass offset inside the controller's 168-wide RAM, in the rotated frame.
#define LCD_OFFSET_X 0
#define LCD_OFFSET_Y 14
// Uncomment if colours look inverted.
// #define NV3007_INVERT

// Effective drawing canvas after QP_ROTATION_90 (see keymap.c) — everything
// else in the firmware lays out against these, landscape.
#define LCD_WIDTH 428
#define LCD_HEIGHT 142

// The keymap's settings (theme, screensaver, screen-off time) in EEPROM, so
// the knob keeps them without the host app. Bump the version when the
// layout of keymap.c's settings_t changes.
#define EECONFIG_USER_DATA_SIZE 32
#define EECONFIG_USER_DATA_VERSION 1

// Files uploaded from the host (keymaps/volume/font_store.c). The firmware
// gets the flash's first FIRMWARE_MAX bytes (it's under 100 kB; the build
// fails past this, see keymaps/volume/rules.mk) and the wear-leveled EEPROM
// the last 8 kB; the slots take the rest:
//   0x030000  gif4-6  ANIM_STORE_SLOT_SIZE each, from ANIM_STORE_LOW_OFFSET
//   0x100000  fonts   FONT_SLOTS x FONT_STORE_SLOT_SIZE
//   0x130000  gif1-3  from ANIM_STORE_OFFSET, up to 0x1F0000
// The fonts and gif1-3 are where they've been since the gifs came in, so
// updating keeps them; gif4-6 came with v0.7.0.
#define FIRMWARE_MAX 0x30000
#define FONT_STORE_OFFSET 0x100000
#define FONT_STORE_SLOT_SIZE 0x10000
#define ANIM_STORE_OFFSET 0x130000
#define ANIM_STORE_LOW_OFFSET FIRMWARE_MAX
#define ANIM_STORE_SLOT_SIZE 0x40000
