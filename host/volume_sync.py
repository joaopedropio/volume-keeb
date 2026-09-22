#!/usr/bin/env python3
"""Push the host's volume and Chrome's playing title to the volume-keeb OLED.

Stdlib only: volume comes from `pactl` (PulseAudio or PipeWire's pulse shim),
the title from Chrome's MPRIS D-Bus interface via `busctl`, and packets are
written straight to the knob's /dev/hidraw node over QMK Raw HID.
"""
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
VOLUME_CMD = 0x56  # must match keymap.c
TITLE_CMD = 0x54  # must match keymap.c
TITLE_MAX = 120  # must match keymap.c
MPRIS_PREFIX = "org.mpris.MediaPlayer2.chromium."  # Chrome and Chromium
HEARTBEAT_S = 2  # firmware shows "no host" after 5 s without a packet


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


def read_volume():
    vol = subprocess.run(["pactl", "get-sink-volume", "@DEFAULT_SINK@"], capture_output=True, text=True).stdout
    mute = subprocess.run(["pactl", "get-sink-mute", "@DEFAULT_SINK@"], capture_output=True, text=True).stdout
    match = re.search(r"(\d+)%", vol)
    if not match:
        return None
    return min(int(match.group(1)), 255), "yes" in mute


def busctl(*args):
    return subprocess.run(["busctl", "--user", *args], capture_output=True, text=True).stdout


def read_title():
    """Title of the media Chrome is playing, or "" if none."""
    for line in busctl("list", "--acquired").splitlines():
        name = line.split(" ", 1)[0]
        if not name.startswith(MPRIS_PREFIX):
            continue
        prop = ["get-property", name, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player"]
        if '"Playing"' not in busctl(*prop, "PlaybackStatus"):
            continue
        try:
            metadata = json.loads(busctl("--json=short", *prop, "Metadata"))["data"]
            return metadata["xesam:title"]["data"]
        except (ValueError, KeyError):
            continue
    return ""


def format_title(title):
    """Printable ASCII, truncated and NUL-padded to exactly TITLE_MAX bytes."""
    # The OLED font is ASCII only; strip accents rather than dropping letters.
    title = unicodedata.normalize("NFKD", title).encode("ascii", "ignore").decode()
    title = " ".join("".join(c for c in title if c.isprintable()).split())
    if len(title) > TITLE_MAX:
        title = title[: TITLE_MAX - 3] + "..."
    return title.encode().ljust(TITLE_MAX, b"\0")


class Knob:
    def __init__(self):
        self.fd = None

    def send(self, volume, muted, title):
        self.write(bytes([VOLUME_CMD, volume, int(muted)]))
        chunk = RAW_EPSIZE - 2
        for offset in range(0, len(title), chunk):
            self.write(bytes([TITLE_CMD, offset]) + title[offset : offset + chunk])

    def write(self, report):
        if self.fd is None:
            path = find_hidraw()
            if path is None:
                return
            try:
                self.fd = os.open(path, os.O_WRONLY)
            except OSError as e:
                print(f"can't open {path}: {e}", file=sys.stderr)
                return
            print(f"connected to {path}", file=sys.stderr)
        report = report.ljust(RAW_EPSIZE, b"\0")
        try:
            # Leading 0 = report ID; QMK's Raw HID reports are unnumbered.
            os.write(self.fd, b"\0" + report)
        except OSError:
            os.close(self.fd)
            self.fd = None  # unplugged; rediscover on the next send


def main():
    knob = Knob()
    while True:
        # `pactl subscribe` prints a line per server event; any sink/server
        # event may be a volume, mute, or default-sink change.
        sub = subprocess.Popen(["pactl", "subscribe"], stdout=subprocess.PIPE, text=True)
        try:
            while sub.poll() is None:
                ready, _, _ = select.select([sub.stdout], [], [], HEARTBEAT_S)
                if ready:
                    line = sub.stdout.readline()
                    if "sink" not in line and "server" not in line:
                        continue
                # MPRIS changes don't show up in `pactl subscribe`, so the title
                # is picked up on the next event or heartbeat.
                state = read_volume()
                if state:
                    knob.send(*state, format_title(read_title()))
        finally:
            sub.kill()
        time.sleep(1)  # audio server restarted; resubscribe


if __name__ == "__main__":
    main()
