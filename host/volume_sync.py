#!/usr/bin/env python3
"""Push a per-app volume mixer to the volume-keeb display and apply the
volume/mute/play-pause actions it sends back.

Stdlib only. Apps come from `pactl list sink-inputs` (one entry per playing
stream) plus a synthetic "Master" entry for the default sink; media titles
and play/pause control come from MPRIS players via `busctl`, matched to a
sink-input by application name. Packets are written to and read from the
knob's /dev/hidraw node over QMK Raw HID.
"""
import argparse
import glob
import json
import os
import re
import select
import subprocess
import sys
import time
import unicodedata

VID, PID = 0xFEED, 0x6465
RAW_USAGE_PAGE = bytes([0x06, 0x60, 0xFF])  # QMK Raw HID interface, usage page 0xFF60
RAW_EPSIZE = 32
APP_BEGIN_CMD = 0x60  # must match keymap.c
APP_ENTRY_CMD = 0x61
APP_END_CMD = 0x62
TITLE_CMD = 0x54  # 'T'
THEME_CMD = 0x63
CTRL_CMD = 0x70
ACTION_SET_VOLUME = 0
ACTION_TOGGLE_MUTE = 1
ACTION_PLAYPAUSE = 2
MAX_APPS = 10  # must match keymap.c
NAME_MAX = 24
TITLE_MAX = 87
HEARTBEAT_S = 2  # firmware shows "no host" after 5 s without a packet

# Index into keymap.c's `themes[]` — keep in sync with it.
THEMES = {"terminal": 0, "neobrutal": 1, "winamp": 2}


def find_hidraw():
    for node in glob.glob("/sys/class/hidraw/hidraw*"):
        try:
            with open(f"{node}/device/uevent") as f:
                uevent = f.read()
            with open(f"{node}/device/report_descriptor", "rb") as f:
                descriptor = f.read()
        except OSError:
            continue
        if f"HID_ID=0003:{VID:08X}:{PID:08X}" in uevent and descriptor.startswith(RAW_USAGE_PAGE):
            return f"/dev/{os.path.basename(node)}"
    return None


def normalize(name):
    return re.sub(r"[^a-z0-9]", "", name.lower())


def to_ascii(text):
    """Printable ASCII only, accents stripped rather than dropped."""
    text = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode()
    return " ".join("".join(c for c in text if c.isprintable()).split())


def busctl(*args):
    return subprocess.run(["busctl", "--user", *args], capture_output=True, text=True).stdout


def read_master():
    vol = subprocess.run(["pactl", "get-sink-volume", "@DEFAULT_SINK@"], capture_output=True, text=True).stdout
    mute = subprocess.run(["pactl", "get-sink-mute", "@DEFAULT_SINK@"], capture_output=True, text=True).stdout
    match = re.search(r"(\d+)%", vol)
    volume = min(int(match.group(1)), 100) if match else 0
    return {"master": True, "name": "Master", "volume": volume, "muted": "yes" in mute, "title": ""}


def read_sink_inputs():
    """One dict per playing sink-input, parsed from `pactl list sink-inputs`."""
    out = subprocess.run(["pactl", "list", "sink-inputs"], capture_output=True, text=True).stdout
    apps = []
    cur = None
    for line in out.splitlines():
        m = re.match(r"Sink Input #(\d+)", line)
        if m:
            cur = {"master": False, "index": int(m.group(1)), "name": None, "volume": 0, "muted": False, "title": ""}
            apps.append(cur)
            continue
        if cur is None:
            continue
        stripped = line.strip()
        m = re.match(r"Volume:.*?(\d+)%", stripped)
        if m:
            cur["volume"] = min(int(m.group(1)), 100)
        m = re.match(r"Mute:\s*(yes|no)", stripped)
        if m:
            cur["muted"] = m.group(1) == "yes"
        m = re.match(r'application\.name\s*=\s*"(.*)"', stripped)
        if m:
            cur["name"] = to_ascii(m.group(1))
    return [a for a in apps if a["name"]]


def read_players():
    """normalized identity -> {bus, playing, title}, one per MPRIS player."""
    players = {}
    for line in busctl("list", "--acquired").splitlines():
        name = line.split(" ", 1)[0]
        if not name.startswith("org.mpris.MediaPlayer2."):
            continue
        prop_root = ["get-property", name, "/org/mpris/MediaPlayer2"]
        try:
            identity = json.loads(busctl(*prop_root, "org.mpris.MediaPlayer2", "Identity"))["data"]
        except (ValueError, KeyError):
            identity = name.rsplit(".", 1)[-1]
        prop = [*prop_root, "org.mpris.MediaPlayer2.Player"]
        playing = '"Playing"' in busctl(*prop, "PlaybackStatus")
        title = ""
        try:
            metadata = json.loads(busctl("--json=short", *prop, "Metadata"))["data"]
            title = metadata.get("xesam:title", {}).get("data", "")
        except (ValueError, KeyError):
            pass
        players[normalize(identity)] = {"bus": name, "playing": playing, "title": to_ascii(title)}
    return players


def collect_apps():
    """Master plus up to MAX_APPS-1 sink-inputs, each matched to a player."""
    apps = [read_master()]
    players = read_players()
    for app in read_sink_inputs():
        player = players.get(normalize(app["name"]))
        if player:
            app["title"] = player["title"]
            app["playing"] = player["playing"]
            app["player_bus"] = player["bus"]
        apps.append(app)
    return apps[:MAX_APPS]


class Knob:
    def __init__(self, theme_id=0):
        self.fd = None
        self.theme_id = theme_id

    def ensure_open(self):
        if self.fd is not None:
            return self.fd
        path = find_hidraw()
        if path is None:
            return None
        try:
            self.fd = os.open(path, os.O_RDWR)
        except OSError as e:
            print(f"can't open {path}: {e}", file=sys.stderr)
            return None
        print(f"connected to {path}", file=sys.stderr)
        # The knob remembers nothing across a reconnect, so re-push the
        # configured theme every time it (re)appears.
        self._write_raw(bytes([THEME_CMD, self.theme_id]))
        return self.fd

    def _write_raw(self, report):
        report = report.ljust(RAW_EPSIZE, b"\0")
        try:
            # Leading 0 = report ID; QMK's Raw HID reports are unnumbered.
            os.write(self.fd, b"\0" + report)
        except OSError:
            self.close()

    def write(self, report):
        fd = self.ensure_open()
        if fd is None:
            return
        self._write_raw(report)

    def read(self):
        if self.fd is None:
            return None
        try:
            return os.read(self.fd, RAW_EPSIZE)
        except OSError:
            self.close()
            return None

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None  # unplugged; rediscover on the next send


def send_apps(knob, apps):
    knob.write(bytes([APP_BEGIN_CMD, len(apps)]))
    for i, app in enumerate(apps):
        has_media = bool(app.get("title"))
        flags = int(app["muted"]) | (has_media << 1) | (int(app.get("playing", False)) << 2)
        name = app["name"].encode("ascii", "ignore")[:NAME_MAX]
        knob.write(bytes([APP_ENTRY_CMD, i, app["volume"], flags, len(name)]) + name)
        if has_media:
            title = app["title"].encode("ascii", "ignore")[:TITLE_MAX]
            chunk = RAW_EPSIZE - 3
            for offset in range(0, len(title), chunk):
                knob.write(bytes([TITLE_CMD, i, offset]) + title[offset : offset + chunk])
    knob.write(bytes([APP_END_CMD, len(apps)]))


def apply_action(app, action, value):
    target = "@DEFAULT_SINK@" if app["master"] else str(app["index"])
    if action == ACTION_SET_VOLUME:
        cmd = "set-sink-volume" if app["master"] else "set-sink-input-volume"
        subprocess.run(["pactl", cmd, target, f"{value}%"])
    elif action == ACTION_TOGGLE_MUTE:
        cmd = "set-sink-mute" if app["master"] else "set-sink-input-mute"
        subprocess.run(["pactl", cmd, target, "toggle"])
    elif action == ACTION_PLAYPAUSE and app.get("player_bus"):
        busctl("call", app["player_bus"], "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", "PlayPause")


def handle_report(knob, apps_by_id):
    report = knob.read()
    if not report or len(report) < 4 or report[0] != CTRL_CMD:
        return
    app = apps_by_id.get(report[1])
    if app is not None:
        apply_action(app, report[2], report[3])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--theme",
        choices=sorted(THEMES),
        default="terminal",
        help="display theme to push to the knob (default: %(default)s)",
    )
    args = parser.parse_args()

    knob = Knob(theme_id=THEMES[args.theme])
    apps_by_id = {}
    while True:
        # `pactl subscribe` prints a line per server event; any sink/server
        # event may be a volume, mute, sink-input, or default-sink change.
        sub = subprocess.Popen(["pactl", "subscribe"], stdout=subprocess.PIPE, text=True)
        try:
            while sub.poll() is None:
                fd = knob.ensure_open()
                waitables = [sub.stdout] + ([fd] if fd is not None else [])
                ready, _, _ = select.select(waitables, [], [], HEARTBEAT_S)
                if fd is not None and fd in ready:
                    handle_report(knob, apps_by_id)
                if sub.stdout in ready:
                    line = sub.stdout.readline()
                    if "sink" not in line and "server" not in line:
                        continue
                apps = collect_apps()
                apps_by_id = {i: app for i, app in enumerate(apps)}
                send_apps(knob, apps)
        finally:
            sub.kill()
        time.sleep(1)  # audio server restarted; resubscribe


if __name__ == "__main__":
    main()
