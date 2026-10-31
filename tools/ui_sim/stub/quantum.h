// Host-side stand-in for QMK's quantum.h: just enough for keymap.c and
// Quantum Painter to compile on the PC.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "util.h"
#include "color.h"
#include "config.h"

#define PROGMEM
#define SAFE_RANGE 0x7E00
#define MATRIX_ROWS 1
#define MATRIX_COLS 1
#define LAYOUT_ortho_1x1(k) {{k}}
#define RAW_EPSIZE 32
#define pin_t uint32_t
#define GP0 0
#define GP1 1
#define GP2 2
#define GP3 3
#define GP4 4
#define GP5 5
#define GP16 16

typedef struct { struct { bool pressed; } event; } keyrecord_t;

extern uint32_t sim_now;
static inline uint32_t timer_read32(void) { return sim_now; }
static inline uint32_t timer_elapsed32(uint32_t t) { return sim_now - t; }
static inline uint32_t timer_read(void) { return sim_now; }
static inline uint32_t timer_elapsed(uint32_t t) { return sim_now - t; }
uint32_t last_input_activity_elapsed(void);
static inline void gpio_set_pin_output(pin_t p) { (void)p; }
static inline void gpio_write_pin_high(pin_t p) { (void)p; }
static inline void gpio_write_pin_low(pin_t p) { (void)p; }
static inline void raw_hid_send(uint8_t *d, uint8_t n) { (void)d; (void)n; }
static inline void wait_ms(int ms) { (void)ms; }
static inline void reset_keyboard(void) {}
// The EEPROM's user datablock lives in memory (sim.c), blank at start like a
// freshly flashed board.
bool     eeconfig_is_user_datablock_valid(void);
uint32_t eeconfig_read_user_datablock(void *data, uint32_t offset, uint32_t length);
uint32_t eeconfig_update_user_datablock(const void *data, uint32_t offset, uint32_t length);
// Media keys, as the knob sends them without a host app (sim.c logs them).
#define KC_AUDIO_MUTE 0x00A8
#define KC_AUDIO_VOL_UP 0x00A9
#define KC_AUDIO_VOL_DOWN 0x00AA
void tap_code(uint8_t code);
