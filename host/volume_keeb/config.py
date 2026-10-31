"""Settings kept between runs: the look (palette + decor), the screen's idle
behaviour, the uploaded fonts and the gif screensavers, in a JSON file plus the
font and animation files next to it.

Linux: $XDG_CONFIG_HOME/volume-keeb (~/.config/volume-keeb)
Windows: %APPDATA%\\volume-keeb
"""
import copy
import json
import os
import sys
import threading
from pathlib import Path

from .protocol import COLOR_FIELDS, DECORS, FONT_SLOTS, GIF_SLOTS, MENUS, PRESETS, SAVERS

DEFAULT_PRESET = "neon"
DEFAULT_PORT = 8137
DEFAULT_SCREEN = {"saver": "drive", "saver_after": 30, "off_after": 0, "menu": "carousel"}  # seconds; 0 = never
MAX_SECONDS = 65535  # SCREEN_CMD's 16-bit fields


def config_dir():
    if sys.platform == "win32":
        base = Path(os.environ.get("APPDATA") or Path.home() / "AppData" / "Roaming")
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
    return base / "volume-keeb"


def _defaults():
    preset = PRESETS[DEFAULT_PRESET]
    return {
        "preset": DEFAULT_PRESET,  # what the colors started from; "" once edited
        "decor": preset["decor"],
        "colors": copy.deepcopy(preset["colors"]),
        "screen": dict(DEFAULT_SCREEN),
        # slot -> {"name": original file name, "format": mono2/4/16, "size": pt, "crc": of the QFF}
        "fonts": {},
        # slot -> {"name", "w", "h", "scale", "frames", "bytes", "crc": of the animation}
        "gifs": {},
        "port": DEFAULT_PORT,
    }


def _valid_screen(v):
    return (isinstance(v, dict) and v.get("saver") in SAVERS and v.get("menu") in MENUS
            and all(isinstance(v.get(k), int) and 0 <= v[k] <= MAX_SECONDS for k in ("saver_after", "off_after"))
            and v["saver_after"] >= 1)


def _valid_hsv(v):
    return isinstance(v, list) and len(v) == 3 and all(isinstance(x, int) and 0 <= x <= 255 for x in v)


class Config:
    def __init__(self, directory=None):
        self.dir = Path(directory) if directory else config_dir()
        self.path = self.dir / "config.json"
        self.lock = threading.Lock()
        self.data = _defaults()
        try:
            with open(self.path) as f:
                saved = json.load(f)
        except (OSError, ValueError):
            saved = {}
        for key in self.data:
            if key in saved:
                self.data[key] = saved[key]
        self._sanitize()

    def _sanitize(self):
        d = self.data
        if d["decor"] not in DECORS:
            d["decor"] = PRESETS[DEFAULT_PRESET]["decor"]
        colors = d["colors"] if isinstance(d["colors"], dict) else {}
        d["colors"] = {f: colors[f] if _valid_hsv(colors.get(f)) else PRESETS[DEFAULT_PRESET]["colors"][f] for f in COLOR_FIELDS}
        if isinstance(d["screen"], dict):
            d["screen"] = {**DEFAULT_SCREEN, **d["screen"]}  # settings saved before a key existed
        if not _valid_screen(d["screen"]):
            d["screen"] = dict(DEFAULT_SCREEN)
        d["screen"] = {k: d["screen"][k] for k in DEFAULT_SCREEN}
        fonts = d["fonts"] if isinstance(d["fonts"], dict) else {}
        d["fonts"] = {s: v for s, v in fonts.items() if s in FONT_SLOTS and self.font_path(s, "qff").exists()}
        gifs = d["gifs"] if isinstance(d["gifs"], dict) else {}
        d["gifs"] = {s: v for s, v in gifs.items() if s in GIF_SLOTS and self.gif_path(s).exists()}

    def save(self):
        self.dir.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        with open(tmp, "w") as f:
            json.dump(self.data, f, indent=2)
        os.replace(tmp, self.path)

    def font_path(self, slot, ext):
        """The slot's original font ("src") or its converted QFF ("qff")."""
        return self.dir / f"font-{slot}.{ext}"

    def gif_path(self, slot):
        return self.dir / f"gif-{slot}.kva"

    def set_theme(self, colors, decor, preset):
        if decor not in DECORS or set(colors) != set(COLOR_FIELDS) or not all(map(_valid_hsv, colors.values())):
            raise ValueError("bad theme")
        with self.lock:
            self.data.update(colors=colors, decor=decor, preset=preset if preset in PRESETS else "")
            self.save()

    def set_screen(self, screen):
        if isinstance(screen, dict):
            screen = {**self.data["screen"], **screen}
        if not _valid_screen(screen):
            raise ValueError("bad screen settings")
        with self.lock:
            self.data["screen"] = {k: screen[k] for k in DEFAULT_SCREEN}
            self.save()

    def set_font(self, slot, font_bytes, qff, info):
        with self.lock:
            self.dir.mkdir(parents=True, exist_ok=True)
            self.font_path(slot, "src").write_bytes(font_bytes)
            self.font_path(slot, "qff").write_bytes(qff)
            self.data["fonts"][slot] = info
            self.save()

    def clear_font(self, slot):
        with self.lock:
            self.data["fonts"].pop(slot, None)
            for ext in ("src", "qff"):
                self.font_path(slot, ext).unlink(missing_ok=True)
            self.save()

    def set_gif(self, slot, data, info):
        with self.lock:
            self.dir.mkdir(parents=True, exist_ok=True)
            self.gif_path(slot).write_bytes(data)
            self.data["gifs"][slot] = info
            self.save()

    def clear_gif(self, slot):
        with self.lock:
            self.data["gifs"].pop(slot, None)
            self.gif_path(slot).unlink(missing_ok=True)
            self.save()
