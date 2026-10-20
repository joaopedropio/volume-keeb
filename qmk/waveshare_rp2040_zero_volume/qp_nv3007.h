// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "qp.h"

// NV3007 (142x428 IPS) SPI panel. QMK has no upstream driver for it, so this
// reuses the generic tft_panel/SPI machinery shared with the ST7789 and only
// supplies the controller-specific init sequence.
painter_device_t qp_nv3007_make_spi_device(uint16_t panel_width, uint16_t panel_height, pin_t chip_select_pin, pin_t dc_pin, pin_t reset_pin, uint16_t spi_divisor, int spi_mode);
