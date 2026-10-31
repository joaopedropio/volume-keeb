"""Raw HID protocol shared with keymap.c -- constants and packet builders.

Every number here must match keymap.c (see the comment above its *_CMD
defines for the byte layout of each packet).
"""
import struct

VID, PID = 0xFEED, 0x6465
RAW_USAGE_PAGE = 0xFF60  # QMK's Raw HID interface
RAW_USAGE = 0x61
RAW_EPSIZE = 32

APP_BEGIN_CMD = 0x60
APP_ENTRY_CMD = 0x61
APP_END_CMD = 0x62
TITLE_CMD = 0x54  # 'T'
THEME_CMD = 0x63
BOOTLOADER_CMD = 0x64
PALETTE_CMD = 0x65
FONT_BEGIN_CMD = 0x66
FONT_DATA_CMD = 0x67
FONT_END_CMD = 0x68
FONT_CLEAR_CMD = 0x69
FONT_QUERY_CMD = 0x6A
SCREEN_CMD = 0x6B
VERSION_QUERY_CMD = 0x6C
SETTINGS_QUERY_CMD = 0x6D
CTRL_CMD = 0x70
FONT_STATUS_CMD = 0x71
FONT_INFO_CMD = 0x72
VERSION_INFO_CMD = 0x73
SETTINGS_INFO_CMD = 0x74

ACTION_SET_VOLUME = 0
ACTION_TOGGLE_MUTE = 1
ACTION_PLAYPAUSE = 2

MAX_APPS = 10
NAME_MAX = 24
TITLE_MAX = 87
HEARTBEAT_S = 2  # firmware shows "no host" after 5 s without a packet

# Palette fields in theme_t's order, and decor_t's values.
COLOR_FIELDS = ("bg", "well", "fg", "dim", "accent", "mute")
DECORS = ("flat", "boxed", "bevel", "neon")

# saver_t's values, in order.
SAVERS = ("none", "drive", "stars", "rain", "plasma", "bounce")

# keymap.c's `themes[]`, as H,S,V in QMK's 0-255 ranges -- the presets the
# settings page starts from. HSV_GREEN = 85,255,255; HSV_RED = 0,255,255.
PRESETS = {
    "terminal": {
        "decor": "flat",
        "colors": {"bg": [0, 0, 0], "well": [0, 0, 0], "fg": [85, 255, 255],
                   "dim": [85, 255, 90], "accent": [85, 255, 255], "mute": [0, 255, 255]},
    },
    "neobrutal": {
        "decor": "boxed",
        "colors": {"bg": [28, 31, 242], "well": [28, 31, 242], "fg": [0, 0, 26],
                   "dim": [29, 63, 138], "accent": [20, 177, 242], "mute": [0, 173, 214]},
    },
    "winamp": {
        "decor": "bevel",
        "colors": {"bg": [170, 41, 88], "well": [85, 85, 15], "fg": [85, 255, 255],
                   "dim": [85, 255, 90], "accent": [31, 211, 255], "mute": [1, 188, 232]},
    },
    "neon": {
        "decor": "neon",
        "colors": {"bg": [190, 235, 22], "well": [190, 235, 22], "fg": [128, 200, 255],
                   "dim": [190, 120, 150], "accent": [222, 210, 255], "mute": [10, 230, 255]},
    },
}
THEME_IDS = {"terminal": 0, "neobrutal": 1, "winamp": 2, "neon": 3}  # index into themes[]

# font_store.h's slots: the keymap's font each one replaces, and the line
# height (px) the layout was built around.
FONT_SLOTS = {
    "ui": {"index": 0, "height": 13, "label": "Header (app name, volume)"},
    "title": {"index": 1, "height": 27, "label": "Song title"},
    "list": {"index": 2, "height": 24, "label": "Menu list"},
}
FONT_MAX_GLYPH_W = 32  # keymap.c's TICKER_PAD
FONT_MAX_BYTES = 0x10000 - 256  # FONT_STORE_SLOT_SIZE minus the header page
FONT_DATA_CHUNK = RAW_EPSIZE - 6
FONT_STATUS = ["ok", "no such slot", "font too big", "transfer out of order",
               "transfer corrupted", "the knob rejected the font"]


def palette_packet(colors, decor):
    payload = bytes([PALETTE_CMD])
    for field in COLOR_FIELDS:
        payload += bytes(colors[field])
    return payload + bytes([DECORS.index(decor)])


def screen_packet(saver, saver_after, off_after, preview=False):
    """The screensaver kind, and the seconds until it starts and until the
    screen turns off (0 = never)."""
    return bytes([SCREEN_CMD, SAVERS.index(saver)]) + struct.pack("<HH", saver_after, off_after) + bytes([int(preview)])


def app_packets(apps):
    """The packets of one app-list snapshot."""
    yield bytes([APP_BEGIN_CMD, len(apps)])
    for i, app in enumerate(apps):
        has_media = bool(app.get("title"))
        flags = int(app["muted"]) | (has_media << 1) | (int(app.get("playing", False)) << 2)
        name = app["name"].encode("ascii", "ignore")[:NAME_MAX]
        yield bytes([APP_ENTRY_CMD, i, app["volume"], flags, len(name)]) + name
        if has_media:
            title = app["title"].encode("ascii", "ignore")[:TITLE_MAX]
            chunk = RAW_EPSIZE - 3
            for offset in range(0, len(title), chunk):
                yield bytes([TITLE_CMD, i, offset]) + title[offset : offset + chunk]
    yield bytes([APP_END_CMD, len(apps)])


def font_begin_packet(slot, data, crc):
    return bytes([FONT_BEGIN_CMD, slot]) + struct.pack("<II", len(data), crc)


def font_data_packets(slot, data):
    for offset in range(0, len(data), FONT_DATA_CHUNK):
        chunk = data[offset : offset + FONT_DATA_CHUNK]
        yield bytes([FONT_DATA_CMD, slot]) + offset.to_bytes(3, "little") + bytes([len(chunk)]) + chunk


def parse_settings_info(report):
    """SETTINGS_INFO_CMD -> (changed on the knob?, colors, decor, screen),
    or None if it holds something this app doesn't know."""
    if report[20] >= len(DECORS) or report[21] >= len(SAVERS):
        return None
    colors = {f: list(report[2 + 3 * i : 5 + 3 * i]) for i, f in enumerate(COLOR_FIELDS)}
    saver_after, off_after = struct.unpack_from("<HH", report, 22)
    screen = {"saver": SAVERS[report[21]], "saver_after": saver_after, "off_after": off_after}
    return bool(report[1]), colors, DECORS[report[20]], screen


def parse_font_info(report):
    """FONT_INFO_CMD -> list of per-slot CRCs (0 = built-in font)."""
    count = report[1]
    return [struct.unpack_from("<I", report, 2 + 4 * i)[0] for i in range(count)]
