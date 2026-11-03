"""Keeps the knob in step with the computer: pushes the app list, the look,
the screensaver settings, the fonts and the gif screensavers, and applies what
the knob sends back.

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

from . import qff
from .knob import Knob
from .protocol import (
    CTRL_CMD, FONT_CLEAR_CMD, FONT_DATA_CHUNK, FONT_END_CMD, FONT_INFO_CMD, FONT_MAX_BYTES, FONT_QUERY_CMD, FONT_STATUS,
    FONT_STATUS_CMD, GIF_MAX_BYTES, GIF_SLOTS, HEARTBEAT_S, STORE_DATA_CMD, STORE_INDEX, PRESETS, SETTINGS_INFO_CMD,
    SETTINGS_QUERY_CMD, THEME_IDS, THEME_CMD, VERSION_INFO_CMD, VERSION_QUERY_CMD, app_packets, font_begin_packet,
    font_data_packets, gif_info, palette_packet, parse_font_info, parse_settings_info, parse_store_data, screen_packet,
    store_read_packet,
)

READ_WINDOW = 8  # STORE_READ_CMDs in flight while reading a file back off the knob


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
        self.font_lock = threading.Lock()  # one font/gif operation at a time; they share self.replies
        self.need_setup = False
        self.apps = []  # the snapshot the knob has, indexed by the ids it sends back
        self.fonts_supported = None  # unknown until the knob answers (or doesn't) FONT_QUERY_CMD
        self.firmware_version = None  # "x.y.z", or None if it didn't answer (v0.1.0) or isn't connected
        self.setups = 0  # times the knob was (re)connected and brought up to date
        self.gifs_supported = None  # whether the firmware has the gif slots, once it answered FONT_QUERY_CMD
        self.store_slots = None  # how many slots the knob has, once it answered FONT_QUERY_CMD
        self.store_rev = 0  # goes up when a slot's file came from (or went with) the knob, so the page reloads them
        # font and gif slot -> its upload's progress
        self.jobs = {slot: {"state": "idle", "progress": 0.0, "error": ""} for slot in STORE_INDEX}

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
                # The knob keeps its settings, fonts and gifs in flash, but
                # not the app list: bring both sides up to date.
                self.need_setup = True
                self.wake()
            report = self.knob.read(0.5)
            if not report:
                continue
            if report[0] == CTRL_CMD and len(report) >= 4:
                self.actions.put((report[1], report[2], report[3]))
                self.wake()
            elif report[0] in (FONT_STATUS_CMD, FONT_INFO_CMD, STORE_DATA_CMD, VERSION_INFO_CMD, SETTINGS_INFO_CMD):
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
                self.sync_store()
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

    def _firmware_at_least(self, *version):
        try:
            return tuple(int(n) for n in (self.firmware_version or "").split(".")) >= version
        except ValueError:
            return False

    def gif_slots(self):
        """The gif slots the knob has (all of them while that's unknown)."""
        return [s for s, v in GIF_SLOTS.items() if self.store_slots is None or v["index"] < self.store_slots]

    def query_version(self):
        with self.font_lock:  # shares self.replies with the font commands
            self._drain_replies()
            self.firmware_version = None
            if self.knob.write(bytes([VERSION_QUERY_CMD])):
                report = self._wait_reply(VERSION_INFO_CMD, timeout=1.0)
                if report is not None:
                    self.firmware_version = ".".join(str(b) for b in report[1:4])

    # -- fonts and gifs: files in the knob's flash slots ---------------------

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

    def _upload(self, slot, data):
        job = self.jobs[slot]
        index = STORE_INDEX[slot]
        job.update(state="uploading", progress=0.0, error="")
        try:
            self._drain_replies()
            self._send(font_begin_packet(index, data, zlib.crc32(data)))
            # Erasing takes up to a second for a font, a few for an animation.
            self._status(index, timeout=5.0 + len(data) / 32768)
            packets = list(font_data_packets(index, data))
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
        index = STORE_INDEX[slot]
        self._drain_replies()
        self._send(bytes([FONT_CLEAR_CMD, index]))
        self._status(index)

    def _download(self, slot, crc):
        """Reads a slot's file back off the knob (firmware v0.6.0 on)."""
        index = STORE_INDEX[slot]
        limit = GIF_MAX_BYTES if slot in GIF_SLOTS else FONT_MAX_BYTES
        job = self.jobs[slot]
        job.update(state="downloading", progress=0.0, error="")
        data, asked = bytearray(), 0
        self._drain_replies()
        while True:
            # A few requests ahead of the replies, or the round trips make it crawl.
            while asked < limit and asked < len(data) + READ_WINDOW * FONT_DATA_CHUNK:
                self._send(store_read_packet(index, asked))
                asked += FONT_DATA_CHUNK
            report = self._wait_reply(STORE_DATA_CMD, index, timeout=1.0)
            if report is None:
                raise FontUploadError("the knob didn't send the file (its firmware is older than v0.6.0)")
            _, offset, part = parse_store_data(report)
            if offset != len(data):
                raise FontUploadError("the file came back out of order")
            data += part
            job["progress"] = len(data) / limit
            if len(part) < FONT_DATA_CHUNK or len(data) >= limit:
                break
        if zlib.crc32(data) != crc:
            raise FontUploadError("the file came back corrupted")
        job.update(state="idle", progress=1.0)
        return bytes(data)

    def _take_from_knob(self, slot, crc):
        """Makes the config hold what the knob has in `slot` (crc 0 = nothing)."""
        gif = slot in GIF_SLOTS
        if not crc:
            if gif:
                self.config.clear_gif(slot)
                if self.config.data["screen"]["saver"] == slot:
                    self.config.set_screen({"saver": "drive"})
                    self.push_screen()
            else:
                self.config.clear_font(slot)
            return
        data = self._download(slot, crc)
        if gif:
            info = gif_info(data)
            info["name"] = "From the knob"
            self.config.set_gif(slot, data, info)
        else:
            self.config.adopt_font(slot, data, {"name": "From the knob", "format": qff.qff_format(data), "size": None,
                                                "crc": crc, "bytes": len(data), "from_knob": True})

    def sync_store(self):
        """Brings the font and gif slots in step. The knob's files win: it
        may have been set up on another computer, and those files live on
        it. Only what was changed here while the knob was away goes the
        other way."""
        with self.font_lock:
            self._drain_replies()
            if not self.knob.write(bytes([FONT_QUERY_CMD, 0])):
                return
            report = self._wait_reply(FONT_INFO_CMD, timeout=1.0)
            self.fonts_supported = report is not None
            if report is None:
                print("the knob's firmware doesn't take fonts; reflash it to use custom fonts", file=sys.stderr)
                return
            on_knob = parse_font_info(report)
            # A report holds 7 slots; v0.7.0 on has more and reports from the
            # slot asked for (older firmware would answer from 0 again).
            if len(on_knob) == 7 and self._firmware_at_least(0, 7, 0) and self.knob.write(bytes([FONT_QUERY_CMD, 7])):
                report = self._wait_reply(FONT_INFO_CMD, timeout=1.0)
                if report is not None:
                    on_knob += parse_font_info(report)
            self.store_slots = len(on_knob)
            self.gifs_supported = len(on_knob) > min(v["index"] for v in GIF_SLOTS.values())
            if not self.gifs_supported:
                print("the knob's firmware has no gif screensavers; update it to use them", file=sys.stderr)
            for slot, index in STORE_INDEX.items():
                if index >= len(on_knob):
                    continue
                gif = slot in GIF_SLOTS
                want = self.config.data["gifs" if gif else "fonts"].get(slot, {}).get("crc", 0)
                try:
                    if slot in self.config.data["pending"]:
                        if on_knob[index] != want:
                            if want:
                                self._upload(slot, (self.config.gif_path(slot) if gif else self.config.font_path(slot, "qff")).read_bytes())
                            else:
                                self._clear(slot)
                        self.config.set_pending(slot, False)
                    elif on_knob[index] != want:
                        self._take_from_knob(slot, on_knob[index])
                        self.store_rev += 1
                except (FontUploadError, OSError, ValueError) as e:
                    self.jobs[slot].update(state="error", error=str(e))
                    print(f"syncing the {slot} slot failed: {e}", file=sys.stderr)

    def upload(self, slot, data):
        """Uploads a font slot's QFF or a gif slot's animation (already saved
        in the config) if the knob is there; otherwise the next sync does."""
        with self.font_lock:
            self.config.set_pending(slot, True)
            if self.knob.connected:
                self._upload(slot, data)
                self.config.set_pending(slot, False)

    def clear(self, slot):
        with self.font_lock:
            self.config.set_pending(slot, True)
            self.jobs[slot].update(state="idle", progress=0.0, error="")
            if self.knob.connected:
                self._clear(slot)
                self.config.set_pending(slot, False)
