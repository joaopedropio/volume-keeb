// Copyright 2026 QMK
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Give the host extra time to notice the remote-wakeup packet.
#define USB_SUSPEND_WAKEUP_DELAY 200

// 76x284 ST7789 LCD, native panel orientation, mounted/driven landscape
// (rotated 90 degrees so the UI is 284x76) over SPI0.
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
#define LCD_SPI_DIVISOR 16
#define LCD_SPI_MODE 3

// Backlight is a plain digital GPIO (no PWM/dimming) toggled in the keymap
// to blank the panel on the same idle timeout the old OLED used.
#define LCD_BLK_PIN GP0
// This module's backlight is active-low: driving the pin low turns it on.
#define LCD_BLK_ON() gpio_write_pin_low(LCD_BLK_PIN)
#define LCD_BLK_OFF() gpio_write_pin_high(LCD_BLK_PIN)

// Native panel pixel dimensions, passed straight to qp_st7789_make_spi_device().
#define LCD_PANEL_WIDTH 76
#define LCD_PANEL_HEIGHT 284

// Effective drawing canvas after QP_ROTATION_90 (see keymap.c) — everything
// else in the firmware lays out against these, landscape.
#define LCD_WIDTH 284
#define LCD_HEIGHT 76
