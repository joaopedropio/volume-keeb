// Copyright 2026 QMK
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Give the host extra time to notice the remote-wakeup packet.
#define USB_SUSPEND_WAKEUP_DELAY 200

// 76x284 ST7789 LCD, native panel orientation, mounted/driven landscape
// (rotated 90 degrees so the UI is 284x76) over SPI0.
// RP2040 GPIO alternate-function mux ties the SPI0 signals to specific pins;
// SCK/MOSI must be GP2/GP3/GP6/GP7 and MISO to GP0/GP4 for the SPI0
// peripheral to actually see them (GP0/GP1/GP2 are already the encoder, so
// the display uses the other half of that pin group). MISO is unused by
// this write-only display but the ChibiOS SPI driver still wants a valid,
// SPI0-capable pin configured for it.
#define SPI_DRIVER SPID0
#define SPI_SCK_PIN GP6
#define SPI_MOSI_PIN GP7
#define SPI_MISO_PIN GP4

#define LCD_CS_PIN GP9
#define LCD_DC_PIN GP10
#define LCD_RST_PIN GP11
#define LCD_SPI_DIVISOR 16
#define LCD_SPI_MODE 3

// Backlight is a plain digital GPIO (no PWM/dimming) toggled in the keymap
// to blank the panel on the same idle timeout the old OLED used.
#define LCD_BLK_PIN GP12

// Native panel pixel dimensions, passed straight to qp_st7789_make_spi_device().
#define LCD_PANEL_WIDTH 76
#define LCD_PANEL_HEIGHT 284

// Effective drawing canvas after QP_ROTATION_90 (see keymap.c) — everything
// else in the firmware lays out against these, landscape.
#define LCD_WIDTH 284
#define LCD_HEIGHT 76
