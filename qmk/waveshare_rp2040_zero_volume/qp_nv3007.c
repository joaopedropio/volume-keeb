// SPDX-License-Identifier: GPL-2.0-or-later
#include "qp_internal.h"
#include "qp_comms.h"
#include "qp_comms_spi.h"
#include "qp_tft_panel.h"
#include "qp_st77xx_opcodes.h"
#include "qp_nv3007.h"

// Vendor init sequence, taken from the Arduino_GFX/LVGL NV3007 drivers.
// Format: command, N, data[N]. Sent between the 0xFF unlock/lock writes.
static const uint8_t nv3007_vendor_init[] = {
    0x9a, 1, 0x08, 0x9b, 1, 0x08, 0x9c, 1, 0xb0, 0x9d, 1, 0x16, 0x9e, 1, 0xc4,
    0x8f, 2, 0x55, 0x04, 0x84, 1, 0x90, 0x83, 1, 0x7b, 0x85, 1, 0x33,
    0x60, 1, 0x00, 0x70, 1, 0x00, 0x61, 1, 0x02, 0x71, 1, 0x02, 0x62, 1, 0x04, 0x72, 1, 0x04,
    0x6c, 1, 0x29, 0x7c, 1, 0x29, 0x6d, 1, 0x31, 0x7d, 1, 0x31, 0x6e, 1, 0x0f, 0x7e, 1, 0x0f,
    0x66, 1, 0x21, 0x76, 1, 0x21, 0x68, 1, 0x3A, 0x78, 1, 0x3A,
    0x63, 1, 0x07, 0x73, 1, 0x07, 0x64, 1, 0x05, 0x74, 1, 0x05, 0x65, 1, 0x02, 0x75, 1, 0x02,
    0x67, 1, 0x23, 0x77, 1, 0x23, 0x69, 1, 0x08, 0x79, 1, 0x08,
    0x6a, 1, 0x13, 0x7a, 1, 0x13, 0x6b, 1, 0x13, 0x7b, 1, 0x13, 0x6f, 1, 0x00, 0x7f, 1, 0x00,
    0x50, 1, 0x00, 0x52, 1, 0xd6, 0x53, 1, 0x08, 0x54, 1, 0x08, 0x55, 1, 0x1e, 0x56, 1, 0x1c,
    0xa0, 3, 0x2b, 0x24, 0x00,
    0xa1, 1, 0x87, 0xa2, 1, 0x86, 0xa5, 1, 0x00, 0xa6, 1, 0x00, 0xa7, 1, 0x00,
    0xa8, 1, 0x36, 0xa9, 1, 0x7e, 0xaa, 1, 0x7e,
    0xB9, 1, 0x85, 0xBA, 1, 0x84, 0xBB, 1, 0x83, 0xBC, 1, 0x82, 0xBD, 1, 0x81, 0xBE, 1, 0x80,
    0xBF, 1, 0x01, 0xC0, 1, 0x02, 0xc1, 1, 0x00, 0xc2, 1, 0x00, 0xc3, 1, 0x00,
    0xc4, 1, 0x33, 0xc5, 1, 0x7e, 0xc6, 1, 0x7e,
    0xC8, 2, 0x33, 0x33, 0xC9, 1, 0x68, 0xCA, 1, 0x69, 0xCB, 1, 0x6a, 0xCC, 1, 0x6b,
    0xCD, 2, 0x33, 0x33, 0xCE, 1, 0x6c, 0xCF, 1, 0x6d, 0xD0, 1, 0x6e, 0xD1, 1, 0x6f,
    0xAB, 2, 0x03, 0x67, 0xAC, 2, 0x03, 0x6b, 0xAD, 2, 0x03, 0x68, 0xAE, 2, 0x03, 0x6c,
    0xb3, 1, 0x00, 0xb4, 1, 0x00, 0xb5, 1, 0x00, 0xB6, 1, 0x32, 0xB7, 1, 0x7e, 0xB8, 1, 0x7e,
    0xe0, 1, 0x00, 0xe1, 2, 0x03, 0x0f, 0xe2, 1, 0x04, 0xe3, 1, 0x01, 0xe4, 1, 0x0e,
    0xe5, 1, 0x01, 0xe6, 1, 0x19, 0xe7, 1, 0x10, 0xe8, 1, 0x10, 0xea, 1, 0x12,
    0xeb, 1, 0xd0, 0xec, 1, 0x04, 0xed, 1, 0x07, 0xee, 1, 0x07, 0xef, 1, 0x09,
    0xf0, 1, 0xd0, 0xf1, 1, 0x0e, 0xF9, 1, 0x17,
    0xf2, 4, 0x2c, 0x1b, 0x0b, 0x20,
    0xe9, 1, 0x29, 0xec, 1, 0x04, 0x35, 1, 0x00, 0x44, 2, 0x00, 0x10, 0x46, 1, 0x10,
};

tft_panel_dc_reset_painter_device_t nv3007_drivers[1] = {0};

static bool qp_nv3007_init(painter_device_t device, painter_rotation_t rotation) {
    qp_comms_command(device, ST77XX_CMD_RESET);
    wait_ms(120);

    qp_comms_command_databyte(device, 0xFF, 0xA5); // unlock vendor registers
    for (size_t i = 0; i < sizeof(nv3007_vendor_init);) {
        uint8_t cmd = nv3007_vendor_init[i];
        uint8_t n   = nv3007_vendor_init[i + 1];
        qp_comms_command_databuf(device, cmd, &nv3007_vendor_init[i + 2], n);
        i += 2 + n;
    }
    qp_comms_command_databyte(device, 0xFF, 0x00); // lock again

    qp_comms_command_databyte(device, ST77XX_SET_PIX_FMT, 0x05); // RGB565
    qp_comms_command(device, ST77XX_CMD_SLEEP_OFF);
    wait_ms(22);
    qp_comms_command(device, ST77XX_CMD_DISPLAY_ON);
#ifdef NV3007_INVERT
    qp_comms_command(device, ST77XX_CMD_INVERT_ON);
#endif

    const uint8_t madctl[] = {
        [QP_ROTATION_0]   = ST77XX_MADCTL_RGB,
        [QP_ROTATION_90]  = ST77XX_MADCTL_RGB | ST77XX_MADCTL_MX | ST77XX_MADCTL_MV,
        [QP_ROTATION_180] = ST77XX_MADCTL_RGB | ST77XX_MADCTL_MX | ST77XX_MADCTL_MY,
        [QP_ROTATION_270] = ST77XX_MADCTL_RGB | ST77XX_MADCTL_MV | ST77XX_MADCTL_MY,
    };
    qp_comms_command_databyte(device, ST77XX_SET_MADCTL, madctl[rotation]);
    return true;
}

static const tft_panel_dc_reset_painter_driver_vtable_t nv3007_driver_vtable = {
    .base =
        {
            .init            = qp_nv3007_init,
            .power           = qp_tft_panel_power,
            .clear           = qp_tft_panel_clear,
            .flush           = qp_tft_panel_flush,
            .pixdata         = qp_tft_panel_pixdata,
            .viewport        = qp_tft_panel_viewport,
            .palette_convert = qp_tft_panel_palette_convert_rgb565_swapped,
            .append_pixels   = qp_tft_panel_append_pixels_rgb565,
            .append_pixdata  = qp_tft_panel_append_pixdata,
        },
    .num_window_bytes   = 2,
    .swap_window_coords = false,
    .opcodes =
        {
            .display_on         = ST77XX_CMD_DISPLAY_ON,
            .display_off        = ST77XX_CMD_DISPLAY_OFF,
            .set_column_address = ST77XX_SET_COL_ADDR,
            .set_row_address    = ST77XX_SET_ROW_ADDR,
            .enable_writes      = ST77XX_SET_MEM,
        },
};

painter_device_t qp_nv3007_make_spi_device(uint16_t panel_width, uint16_t panel_height, pin_t chip_select_pin, pin_t dc_pin, pin_t reset_pin, uint16_t spi_divisor, int spi_mode) {
    tft_panel_dc_reset_painter_device_t *driver = &nv3007_drivers[0];
    if (driver->base.driver_vtable) return NULL;

    driver->base.driver_vtable         = (const painter_driver_vtable_t *)&nv3007_driver_vtable;
    driver->base.comms_vtable          = (const painter_comms_vtable_t *)&spi_comms_with_dc_vtable;
    driver->base.panel_width           = panel_width;
    driver->base.panel_height          = panel_height;
    driver->base.rotation              = QP_ROTATION_0;
    driver->base.offset_x              = 0;
    driver->base.offset_y              = 0;
    driver->base.native_bits_per_pixel = 16;

    driver->base.comms_config                                   = &driver->spi_dc_reset_config;
    driver->spi_dc_reset_config.spi_config.chip_select_pin      = chip_select_pin;
    driver->spi_dc_reset_config.spi_config.divisor              = spi_divisor;
    driver->spi_dc_reset_config.spi_config.lsb_first            = false;
    driver->spi_dc_reset_config.spi_config.mode                 = spi_mode;
    driver->spi_dc_reset_config.dc_pin                          = dc_pin;
    driver->spi_dc_reset_config.reset_pin                       = reset_pin;
    driver->spi_dc_reset_config.command_params_uses_command_pin = false;

    if (!qp_internal_register_device((painter_device_t)driver)) {
        memset(driver, 0, sizeof(*driver));
        return NULL;
    }
    return (painter_device_t)driver;
}
