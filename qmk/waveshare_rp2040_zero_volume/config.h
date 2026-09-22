// Copyright 2026 QMK
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Give the host extra time to notice the remote-wakeup packet.
#define USB_SUSPEND_WAKEUP_DELAY 200

// 1.3" OLED (SH1107, 64x128 native, mounted landscape via OLED_ROTATION_90) on I2C0: SDA=GP4, SCL=GP5.
// QMK's I2C1_* pin names are driver-agnostic; I2C_DRIVER picks the peripheral.
#define I2C_DRIVER I2CD0
#define I2C1_SDA_PIN GP4
#define I2C1_SCL_PIN GP5

// 64X128 implies OLED_IC_SH1107 and the 32-row COM offset this panel needs;
// without that offset output starts mid-screen.
#define OLED_DISPLAY_64X128
