// font_store.h for the simulator: the slots are files instead of flash.
// With UI_SIM_FONTS=<dir>, font slot N is <dir>/slotN.qff when that exists
// (the host's converter can write one with `python3 -m volume_keeb.qff`) and
// animation slot N is <dir>/slotN.kva (the host app keeps a copy of each one
// it sends as gif-gifM.kva in its config folder); slots without a file, and
// everything when unset, are empty.
#include <stdio.h>
#include <stdlib.h>
#include "font_store.h"

static uint8_t *slot_data[STORE_SLOTS];
static uint32_t slot_len[STORE_SLOTS];
static bool     slot_read[STORE_SLOTS];

const uint8_t *font_store_font(uint8_t slot) {
    if (slot >= STORE_SLOTS) return NULL;
    if (!slot_read[slot]) {
        slot_read[slot] = true;
        const char *dir = getenv("UI_SIM_FONTS");
        if (!dir) return NULL;
        char path[512];
        snprintf(path, sizeof(path), "%s/slot%u.%s", dir, slot, slot < FONT_SLOTS ? "qff" : "kva");
        FILE *f = fopen(path, "rb");
        if (!f) return NULL;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        slot_data[slot] = malloc(n);
        if (fread(slot_data[slot], 1, n, f) != (size_t)n) {
            free(slot_data[slot]);
            slot_data[slot] = NULL;
        } else {
            slot_len[slot] = n;
        }
        fclose(f);
    }
    return slot_data[slot];
}

uint32_t font_store_length(uint8_t slot) {
    return font_store_font(slot) ? slot_len[slot] : 0;
}

uint32_t font_store_crc(uint8_t slot) {
    return font_store_font(slot) ? 1 : 0;
}

uint8_t font_store_read(uint8_t slot, uint32_t offset, uint8_t *buf, uint8_t n) {
    (void)slot, (void)offset, (void)buf, (void)n;
    return 0;
}

uint8_t font_store_begin(uint8_t slot, uint32_t length, uint32_t crc) {
    (void)slot, (void)length, (void)crc;
    return FONT_ERR_SLOT;
}

void font_store_write(uint8_t slot, uint32_t offset, const uint8_t *data, uint8_t n) {
    (void)slot, (void)offset, (void)data, (void)n;
}

uint8_t font_store_end(uint8_t slot) {
    (void)slot;
    return FONT_ERR_SEQUENCE;
}

void font_store_clear(uint8_t slot) {
    (void)slot;
}
