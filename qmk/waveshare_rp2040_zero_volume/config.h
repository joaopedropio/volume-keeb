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
// to blank the panel on the same idle timeout the old OLED used.
#define LCD_BLK_PIN GP0
// The NV3007 module's backlight is active-high: driving the pin high turns it on.
#define LCD_BLK_ON() gpio_write_pin_high(LCD_BLK_PIN)
#define LCD_BLK_OFF() gpio_write_pin_low(LCD_BLK_PIN)

// Native panel pixel dimensions, passed straight to qp_nv3007_make_spi_device().
#define LCD_PANEL_WIDTH 142
#define LCD_PANEL_HEIGHT 428

// Glass offset inside the controller's 168-wide RAM, in the rotated frame.
#define LCD_OFFSET_X 0
#define LCD_OFFSET_Y 13
// Uncomment if colours look inverted.
// #define NV3007_INVERT

// Effective drawing canvas after QP_ROTATION_90 (see keymap.c) — everything
// else in the firmware lays out against these, landscape.
#define LCD_WIDTH 428
#define LCD_HEIGHT 142
