"""Keeps the knob in step with the computer: pushes the app list, the look,
the screensaver settings and the fonts, and applies what the knob sends back.

Two threads: one reads the knob (and finds it again after an unplug), one
does everything that touches the audio system -- collecting the app list
and applying the knob's actions -- since on Windows those are COM objects
tied to the thread that made them. The settings page calls in from the
web server's threads.
"""
import queue
import sys
import threading
import time
import zlib

from .knob import Knob
from .protocol import (
    CTRL_CMD, FONT_CLEAR_CMD, FONT_END_CMD, FONT_INFO_CMD, FONT_QUERY_CMD, FONT_SLOTS, FONT_STATUS,
    FONT_STATUS_CMD, HEARTBEAT_S, PRESETS, SETTINGS_INFO_CMD, SETTINGS_QUERY_CMD, THEME_IDS, THEME_CMD, VERSION_INFO_CMD,
    VERSION_QUERY_CMD, app_packets, font_begin_packet, font_data_packets, palette_packet, parse_font_info, parse_settings_info,
    screen_packet,
)


class FontUploadError(Exception):
    pass


class Engine:
    def __init__(self, config, audio):
        self.config = config
        self.audio = audio
        self.knob = Knob()
        self.wake_event = threading.Event()
        self.actions = queue.Queue()
        self.replies = queue.Queue()
        self.font_lock = threading.Lock()  # one font operation at a time; they share self.replies
        self.need_setup = False
        self.apps = []  # the snapshot the knob has, indexed by the ids it sends back
        self.fonts_supported = None  # unknown until the knob answers (or doesn't) FONT_QUERY_CMD
        self.firmware_version = None  # "x.y.z", or None if it didn't answer (v0.1.0) or isn't connected
        self.setups = 0  # times the knob was (re)connected and brought up to date
        self.font_jobs = {slot: {"state": "idle", "progress": 0.0, "error": ""} for slot in FONT_SLOTS}

    def start(self):
        self.audio.start(self.wake)
        threading.Thread(target=self._read_loop, name="knob-reader", daemon=True).start()
        threading.Thread(target=self._sync_loop, name="sync", daemon=True).start()

    def wake(self):
        self.wake_event.set()

    def _read_loop(self):
        while True:
            if not self.knob.connected:
                if not self.knob.open():
                    time.sleep(1)
                    continue
                # The knob keeps fonts in flash but nothing else: re-push.
                self.need_setup = True
                self.wake()
            report = self.knob.read(0.5)
            if not report:
                continue
            if report[0] == CTRL_CMD and len(report) >= 4:
                self.actions.put((report[1], report[2], report[3]))
                self.wake()
            elif report[0] in (FONT_STATUS_CMD, FONT_INFO_CMD, VERSION_INFO_CMD, SETTINGS_INFO_CMD):
                self.replies.put(report)

    def _sync_loop(self):
        if hasattr(self.audio, "thread_init"):
            self.audio.thread_init()
        last_packets, last_sent = None, 0.0
        while True:
            self.wake_event.wait(self.audio.poll_s)
            self.wake_event.clear()
            if self.need_setup:
                self.need_setup = False
                self.adopt_knob_settings()
                self.push_theme()
                self.push_screen()
                self.query_version()
                self.sync_fonts()
                self.setups += 1
                last_packets = None
            while not self.actions.empty():
                app_id, action, value = self.actions.get()
                if app_id < len(self.apps):
                    try:
                        self.audio.apply(self.apps[app_id], action, value)
                    except Exception as e:
                        print(f"applying action {action} failed: {e}", file=sys.stderr)
            try:
                apps = self.audio.collect()
            except Exception as e:
                print(f"reading the audio state failed: {e}", file=sys.stderr)
                continue
            packets = list(app_packets(apps))
            if packets == last_packets and time.monotonic() - last_sent < HEARTBEAT_S:
                continue
            self.apps = apps
            for p in packets:
                if not self.knob.write(p):
                    break
            last_packets, last_sent = packets, time.monotonic()

    # -- the look ------------------------------------------------------

    def adopt_knob_settings(self):
        """Takes what was changed on the knob's own settings screen (while
        no host app was around) as this app's settings, so pushing them
        right after doesn't undo it. Firmware before v0.4.0 doesn't answer."""
        with self.font_lock:  # shares self.replies with the font commands
            self._drain_replies()
            if not self.knob.write(bytes([SETTINGS_QUERY_CMD])):
                return
            report = self._wait_reply(SETTINGS_INFO_CMD, timeout=1.0)
        info = report and parse_settings_info(report)
        if not info or not info[0]:
            return
        _, colors, decor, screen = info
        preset = next((name for name, p in PRESETS.items() if p["colors"] == colors and p["decor"] == decor), "")
        try:
            self.config.set_theme(colors, decor, preset)
            self.config.set_screen(screen)
        except ValueError as e:
            print(f"the knob's settings were unusable: {e}", file=sys.stderr)
            return
        print("took the settings changed on the knob", file=sys.stderr)

    def push_theme(self):
        d = self.config.data
        if d["preset"] in THEME_IDS:
            # Firmware from before PALETTE_CMD ignores it but knows the presets.
            self.knob.write(bytes([THEME_CMD, THEME_IDS[d["preset"]]]))
        self.knob.write(palette_packet(d["colors"], d["decor"]))

    def push_screen(self, preview=False):
        s = self.config.data["screen"]
        self.knob.write(screen_packet(s["saver"], s["saver_after"], s["off_after"], s["menu"], preview))

    def query_version(self):
        with self.font_lock:  # shares self.replies with the font commands
            self._drain_replies()
            self.firmware_version = None
            if self.knob.write(bytes([VERSION_QUERY_CMD])):
                report = self._wait_reply(VERSION_INFO_CMD, timeout=1.0)
                if report is not None:
                    self.firmware_version = ".".join(str(b) for b in report[1:4])

    # -- fonts -----------------------------------------------------------

    def _drain_replies(self):
        while not self.replies.empty():
            self.replies.get_nowait()

    def _wait_reply(self, cmd, slot=None, timeout=3.0):
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            try:
                report = self.replies.get(timeout=remaining)
            except queue.Empty:
                return None
            if report[0] == cmd and (slot is None or report[1] == slot):
                return report

    def _status(self, slot_index, timeout=3.0):
        report = self._wait_reply(FONT_STATUS_CMD, slot_index, timeout)
        if report is None:
            raise FontUploadError("the knob didn't answer")
        if report[2] != 0:
            raise FontUploadError(FONT_STATUS[report[2]] if report[2] < len(FONT_STATUS) else f"error {report[2]}")

    def _send(self, report):
        if not self.knob.write(report):
            raise FontUploadError("the knob isn't connected")

    def _upload(self, slot, qff):
        job = self.font_jobs[slot]
        index = FONT_SLOTS[slot]["index"]
        job.update(state="uploading", progress=0.0, error="")
        try:
            self._drain_replies()
            self._send(font_begin_packet(index, qff, zlib.crc32(qff)))
            self._status(index, timeout=5.0)  # erasing takes up to a second
            packets = list(font_data_packets(index, qff))
            for i, p in enumerate(packets):
                self._send(p)
                job["progress"] = (i + 1) / len(packets)
            self._send(bytes([FONT_END_CMD, index]))
            self._status(index)
            job.update(state="idle", progress=1.0)
        except FontUploadError as e:
            job.update(state="error", error=str(e))
            raise

    def _clear(self, slot):
        index = FONT_SLOTS[slot]["index"]
        self._drain_replies()
        self._send(bytes([FONT_CLEAR_CMD, index]))
        self._status(index)

    def sync_fonts(self):
        """Makes the knob's font slots match the config (it may have been
        used with another computer, or reflashed with other fonts)."""
        with self.font_lock:
            self._drain_replies()
            if not self.knob.write(bytes([FONT_QUERY_CMD])):
                return
            report = self._wait_reply(FONT_INFO_CMD, timeout=1.0)
            self.fonts_supported = report is not None
            if report is None:
                print("the knob's firmware doesn't take fonts; reflash it to use custom fonts", file=sys.stderr)
                return
            on_knob = parse_font_info(report)
            for slot, meta in FONT_SLOTS.items():
                want = self.config.data["fonts"].get(slot, {}).get("crc", 0)
                if on_knob[meta["index"]] == want:
                    continue
                try:
                    if want:
                        self._upload(slot, self.config.font_path(slot, "qff").read_bytes())
                    else:
                        self._clear(slot)
                except (FontUploadError, OSError) as e:
                    print(f"syncing the {slot} font failed: {e}", file=sys.stderr)

    def set_font(self, slot, qff):
        """Uploads `qff` (already saved in the config) if the knob is there."""
        with self.font_lock:
            if self.knob.connected:
                self._upload(slot, qff)

    def clear_font(self, slot):
        with self.font_lock:
            self.font_jobs[slot].update(state="idle", progress=0.0, error="")
            if self.knob.connected:
                self._clear(slot)
