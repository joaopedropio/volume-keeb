// Copyright 2026 QMK
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The slots live in reserved stretches of the RP2040's flash (FONT_STORE_*
// and ANIM_STORE_* in config.h), well past the firmware and clear of the wear-leveled EEPROM
// at the very end. A UF2 only rewrites the blocks it carries, so flashing new
// firmware leaves them alone.
//
// Each slot is a header page followed by the file. The header is written
// last, after the whole file checked out against the host's CRC, so a slot
// whose upload was cut short reads as empty, never as half a font.
#include <string.h>
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "config.h"
#include "font_store.h"

#define SLOT_MAGIC 0x3146564B // "KVF1"
#define SLOT_BASE(slot) ((slot) < ANIM_SLOT0 ? FONT_STORE_OFFSET + (uint32_t)(slot) * FONT_STORE_SLOT_SIZE : ANIM_STORE_OFFSET + (uint32_t)((slot) - ANIM_SLOT0) * ANIM_STORE_SLOT_SIZE)
#define SLOT_SIZE(slot) ((slot) < ANIM_SLOT0 ? FONT_STORE_SLOT_SIZE : ANIM_STORE_SLOT_SIZE)
#define DATA_OFFSET FLASH_PAGE_SIZE
#define DATA_MAX(slot) (SLOT_SIZE(slot) - DATA_OFFSET)

_Static_assert(FONT_STORE_OFFSET % FLASH_SECTOR_SIZE == 0, "font store must start on a sector");
_Static_assert(FONT_STORE_SLOT_SIZE % FLASH_SECTOR_SIZE == 0, "font slots must be whole sectors");
_Static_assert(ANIM_STORE_OFFSET % FLASH_SECTOR_SIZE == 0, "animation store must start on a sector");
_Static_assert(ANIM_STORE_SLOT_SIZE % FLASH_SECTOR_SIZE == 0, "animation slots must be whole sectors");
_Static_assert(FONT_STORE_OFFSET + FONT_SLOTS * FONT_STORE_SLOT_SIZE <= ANIM_STORE_OFFSET, "font and animation slots overlap");
_Static_assert(ANIM_STORE_OFFSET + ANIM_SLOTS * ANIM_STORE_SLOT_SIZE <= 0x200000 - 8192, "animation slots run into the EEPROM");

typedef struct {
    uint32_t magic;
    uint32_t length;
    uint32_t crc;
} slot_header_t;

static struct {
    bool     active;
    uint8_t  slot;
    uint32_t length;
    uint32_t crc;
    uint32_t received;
    uint8_t  page[FLASH_PAGE_SIZE]; // flash is programmed a page at a time
} upload;

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (uint8_t k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

static const uint8_t *slot_ptr(uint8_t slot) {
    return (const uint8_t *)(XIP_BASE + SLOT_BASE(slot));
}

static const slot_header_t *valid_header(uint8_t slot) {
    if (slot >= STORE_SLOTS || (upload.active && upload.slot == slot)) return NULL;
    const slot_header_t *h = (const slot_header_t *)slot_ptr(slot);
    if (h->magic != SLOT_MAGIC || h->length == 0 || h->length > DATA_MAX(slot)) return NULL;
    if (crc32_update(0, slot_ptr(slot) + DATA_OFFSET, h->length) != h->crc) return NULL;
    return h;
}

// The flash can't be read (and so no code can run from it) while it's
// being erased or programmed, hence interrupts off around each call; the
// pico-sdk functions themselves run from RAM. One sector or page at a time
// keeps each pause short (~50 ms per erased sector).
static void erase(uint32_t offset, uint32_t length) {
    for (uint32_t done = 0; done < length; done += FLASH_SECTOR_SIZE) {
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(offset + done, FLASH_SECTOR_SIZE);
        restore_interrupts(ints);
    }
}

static void program_page(uint32_t offset, const uint8_t *page) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(offset, page, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
}

const uint8_t *font_store_font(uint8_t slot) {
    return valid_header(slot) ? slot_ptr(slot) + DATA_OFFSET : NULL;
}

uint32_t font_store_length(uint8_t slot) {
    const slot_header_t *h = valid_header(slot);
    return h ? h->length : 0;
}

uint32_t font_store_crc(uint8_t slot) {
    const slot_header_t *h = valid_header(slot);
    return h ? h->crc : 0;
}

// Called once per report while the host reads a slot back, so the header
// is only sanity-checked: re-checking the CRC each time would take ~20 ms a
// call for an animation, and the host checks the whole file against
// font_store_crc() anyway.
uint8_t font_store_read(uint8_t slot, uint32_t offset, uint8_t *buf, uint8_t n) {
    if (slot >= STORE_SLOTS || (upload.active && upload.slot == slot)) return 0;
    const slot_header_t *h = (const slot_header_t *)slot_ptr(slot);
    if (h->magic != SLOT_MAGIC || h->length > DATA_MAX(slot) || offset >= h->length) return 0;
    if (n > h->length - offset) n = h->length - offset;
    memcpy(buf, slot_ptr(slot) + DATA_OFFSET + offset, n);
    return n;
}

uint8_t font_store_begin(uint8_t slot, uint32_t length, uint32_t crc) {
    upload.active = false;
    if (slot >= STORE_SLOTS) return FONT_ERR_SLOT;
    if (length == 0 || length > DATA_MAX(slot)) return FONT_ERR_SIZE;
    uint32_t used = DATA_OFFSET + length;
    erase(SLOT_BASE(slot), (used + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE * FLASH_SECTOR_SIZE);
    upload.active   = true;
    upload.slot     = slot;
    upload.length   = length;
    upload.crc      = crc;
    upload.received = 0;
    memset(upload.page, 0xFF, sizeof(upload.page));
    return FONT_OK;
}

void font_store_write(uint8_t slot, uint32_t offset, const uint8_t *data, uint8_t n) {
    if (!upload.active || slot != upload.slot) return;
    if (offset != upload.received || offset + n > upload.length) {
        upload.active = false; // end() then reports the sequence error
        return;
    }
    while (n) {
        uint32_t in_page = upload.received % FLASH_PAGE_SIZE;
        uint32_t take    = FLASH_PAGE_SIZE - in_page;
        if (take > n) take = n;
        memcpy(&upload.page[in_page], data, take);
        upload.received += take;
        data += take;
        n -= take;
        if (upload.received % FLASH_PAGE_SIZE == 0) {
            program_page(SLOT_BASE(slot) + DATA_OFFSET + upload.received - FLASH_PAGE_SIZE, upload.page);
            memset(upload.page, 0xFF, sizeof(upload.page));
        }
    }
}

uint8_t font_store_end(uint8_t slot) {
    if (!upload.active || slot != upload.slot || upload.received != upload.length) {
        upload.active = false;
        return FONT_ERR_SEQUENCE;
    }
    uint32_t tail = upload.received % FLASH_PAGE_SIZE;
    if (tail) {
        program_page(SLOT_BASE(slot) + DATA_OFFSET + upload.received - tail, upload.page);
    }
    upload.active = false;
    if (crc32_update(0, slot_ptr(slot) + DATA_OFFSET, upload.length) != upload.crc) return FONT_ERR_CRC;

    slot_header_t h = {SLOT_MAGIC, upload.length, upload.crc};
    memset(upload.page, 0xFF, sizeof(upload.page));
    memcpy(upload.page, &h, sizeof(h));
    program_page(SLOT_BASE(slot), upload.page);
    return FONT_OK;
}

void font_store_clear(uint8_t slot) {
    if (slot >= STORE_SLOTS) return;
    if (upload.active && upload.slot == slot) upload.active = false;
    if (((const slot_header_t *)slot_ptr(slot))->magic == 0xFFFFFFFF) return; // already empty, spare the flash
    erase(SLOT_BASE(slot), FLASH_SECTOR_SIZE);
}
