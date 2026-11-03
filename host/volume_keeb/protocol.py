"""Raw HID protocol shared with keymap.c -- constants and packet builders.

Every number here must match keymap.c (see the comment above its *_CMD
defines for the byte layout of each packet).
"""
import struct
import zlib

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
STORE_READ_CMD = 0x6E
CTRL_CMD = 0x70
FONT_STATUS_CMD = 0x71
FONT_INFO_CMD = 0x72
VERSION_INFO_CMD = 0x73
SETTINGS_INFO_CMD = 0x74
STORE_DATA_CMD = 0x75

ACTION_SET_VOLUME = 0
ACTION_TOGGLE_MUTE = 1
ACTION_PLAYPAUSE = 2

MAX_APPS = 10
NAME_MAX = 24
TITLE_MAX = 87
HEARTBEAT_S = 2  # firmware shows "no host" after 5 s without a packet

# Palette fields in theme_t's order, and decor_t's values.
COLOR_FIELDS = ("bg", "well", "fg", "dim", "accent", "mute")
DECORS = ("flat", "boxed", "bevel", "neon", "dial", "vu", "mixer")

# menu_layout_t's values, in order.
MENUS = ("carousel", "list", "mixer")

# saver_t's values, in order. The gif ones play what GIF_SLOTS hold.
SAVERS = ("none", "drive", "stars", "rain", "plasma", "bounce", "gif1", "gif2", "gif3", "gif4", "gif5", "gif6")

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
    "studio": {
        "decor": "dial",
        "colors": {"bg": [165, 70, 24], "well": [165, 55, 44], "fg": [28, 25, 245],
                   "dim": [160, 45, 135], "accent": [18, 235, 255], "mute": [0, 200, 240]},
    },
    "hifi": {
        "decor": "vu",
        "colors": {"bg": [12, 150, 38], "well": [30, 95, 232], "fg": [30, 55, 228],
                   "dim": [22, 110, 150], "accent": [0, 215, 210], "mute": [8, 230, 255]},
    },
    "console": {
        "decor": "mixer",
        "colors": {"bg": [160, 25, 28], "well": [160, 20, 50], "fg": [0, 0, 225],
                   "dim": [160, 20, 125], "accent": [70, 210, 235], "mute": [0, 220, 240]},
    },
}
THEME_IDS = {"terminal": 0, "neobrutal": 1, "winamp": 2, "neon": 3, "studio": 4, "hifi": 5, "console": 6}  # index into themes[]

# font_store.h's slots: the keymap's font each one replaces, and the line
# height (px) the layout was built around.
FONT_SLOTS = {
    "ui": {"index": 0, "height": 13, "label": "Header (app name, volume)"},
    "title": {"index": 1, "height": 27, "label": "Song title"},
    "list": {"index": 2, "height": 24, "label": "Menu and settings"},
}
# And the slots after them, for the gif screensavers' animations (made by
# web/gif.html; the format is described above keymap.c's anim_parse()).
# gif4-6 came with firmware v0.7.0.
GIF_SLOTS = {f"gif{n}": {"index": 2 + n} for n in range(1, 7)}
GIF_MAX_BYTES = 0x40000 - 256  # ANIM_STORE_SLOT_SIZE minus the header page
STORE_INDEX = {**{k: v["index"] for k, v in FONT_SLOTS.items()}, **{k: v["index"] for k, v in GIF_SLOTS.items()}}
FONT_MAX_GLYPH_W = 32  # keymap.c's TICKER_PAD
FONT_MAX_BYTES = 0x10000 - 256  # FONT_STORE_SLOT_SIZE minus the header page
FONT_DATA_CHUNK = RAW_EPSIZE - 6
FONT_STATUS = ["ok", "no such slot (update the knob's firmware)", "too big", "transfer out of order",
               "transfer corrupted", "the knob rejected the file"]


def palette_packet(colors, decor):
    payload = bytes([PALETTE_CMD])
    for field in COLOR_FIELDS:
        payload += bytes(colors[field])
    return payload + bytes([DECORS.index(decor)])


def screen_packet(saver, saver_after, off_after, menu, preview=False):
    """The screensaver kind, the seconds until it starts and until the
    screen turns off (0 = never), and the menu's layout."""
    return (bytes([SCREEN_CMD, SAVERS.index(saver)]) + struct.pack("<HH", saver_after, off_after)
            + bytes([int(preview), MENUS.index(menu)]))


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


def store_read_packet(slot, offset):
    return bytes([STORE_READ_CMD, slot]) + offset.to_bytes(3, "little")


def parse_store_data(report):
    """STORE_DATA_CMD -> (slot, offset, bytes)."""
    n = min(report[5], len(report) - 6)
    return report[1], int.from_bytes(report[2:5], "little"), bytes(report[6 : 6 + n])


def parse_settings_info(report):
    """SETTINGS_INFO_CMD -> (changed on the knob?, colors, decor, screen),
    or None if it holds something this app doesn't know."""
    if report[20] >= len(DECORS) or report[21] >= len(SAVERS) or report[26] >= len(MENUS):
        return None
    colors = {f: list(report[2 + 3 * i : 5 + 3 * i]) for i, f in enumerate(COLOR_FIELDS)}
    saver_after, off_after = struct.unpack_from("<HH", report, 22)
    screen = {"saver": SAVERS[report[21]], "saver_after": saver_after, "off_after": off_after, "menu": MENUS[report[26]]}
    return bool(report[1]), colors, DECORS[report[20]], screen


def parse_font_info(report):
    """FONT_INFO_CMD -> list of per-slot CRCs (0 = empty, or the built-in font)."""
    count = min(report[1], (len(report) - 2) // 4)
    return [struct.unpack_from("<I", report, 2 + 4 * i)[0] for i in range(count)]


def gif_info(data):
    """The header of an animation from the GIF page (keymap.c checks the rest)."""
    if len(data) > GIF_MAX_BYTES:
        raise ValueError(f"the animation is {len(data) // 1024} kB, over the {GIF_MAX_BYTES // 1024} kB a slot holds")
    if len(data) < 16 or data[:4] != b"KVA1":
        raise ValueError("not an animation from the GIF page")
    w, h, scale, _, frames = struct.unpack_from("<HHBBH", data, 4)
    if not (0 < w <= 428 and 0 < h <= 142 and scale and frames):
        raise ValueError("the animation doesn't fit the screen")
    return {"w": w, "h": h, "scale": scale, "frames": frames, "bytes": len(data), "crc": zlib.crc32(data)}
