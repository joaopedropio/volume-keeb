"""The knob's Raw HID interface: Linux's /dev/hidraw with the standard
library alone, anywhere else through hidapi (`pip install hidapi`).

Writes may come from any thread; reads from one reader thread. A failed
write or read closes the device, and the next open() finds it again.
"""
import glob
import os
import select
import sys
import threading

from .protocol import BOOTLOADER_CMD, PID, RAW_EPSIZE, RAW_USAGE, RAW_USAGE_PAGE, VID

try:
    import hid  # hidapi
except ImportError:
    hid = None

# A hidraw report descriptor opens with Usage Page (0xFF60), little-endian.
RAW_DESCRIPTOR_PREFIX = bytes([0x06, RAW_USAGE_PAGE & 0xFF, RAW_USAGE_PAGE >> 8])


def find_hidraw():
    for node in glob.glob("/sys/class/hidraw/hidraw*"):
        try:
            with open(f"{node}/device/uevent") as f:
                uevent = f.read()
            with open(f"{node}/device/report_descriptor", "rb") as f:
                descriptor = f.read()
        except OSError:
            continue
        if f"HID_ID=0003:{VID:08X}:{PID:08X}" in uevent and descriptor.startswith(RAW_DESCRIPTOR_PREFIX):
            return f"/dev/{os.path.basename(node)}"
    return None


def find_hidapi():
    if hid is None:
        return None
    for info in hid.enumerate(VID, PID):
        if info.get("usage_page") == RAW_USAGE_PAGE and info.get("usage") == RAW_USAGE:
            return info["path"]
    return None


class _Hidraw:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR)

    def write(self, data):
        os.write(self.fd, data)

    def read(self, timeout):
        ready, _, _ = select.select([self.fd], [], [], timeout)
        return os.read(self.fd, RAW_EPSIZE) if ready else None

    def close(self):
        os.close(self.fd)


class _Hidapi:
    def __init__(self, path):
        self.dev = hid.device()
        self.dev.open_path(path)

    def write(self, data):
        if self.dev.write(data) < 0:
            raise OSError("hid write failed")

    def read(self, timeout):
        data = self.dev.read(RAW_EPSIZE, int(timeout * 1000))
        return bytes(data) if data else None

    def close(self):
        self.dev.close()


def _locate():
    if sys.platform.startswith("linux"):
        path = find_hidraw()
        return (_Hidraw, path) if path else (None, None)
    path = find_hidapi()
    return (_Hidapi, path) if path else (None, None)


class Knob:
    def __init__(self):
        self.dev = None
        self.path = None
        self.lock = threading.Lock()
        self.on_change = None  # called with True/False when it's plugged in or unplugged, e.g. to change the tray icon

    def _changed(self):
        if self.on_change:
            try:
                self.on_change(self.dev is not None)
            except Exception as e:
                print(f"knob on_change failed: {e}", file=sys.stderr)

    @property
    def connected(self):
        return self.dev is not None

    def open(self):
        """Opens the knob if it's plugged in; returns whether it's open now."""
        with self.lock:
            if self.dev is not None:
                return True
            kind, path = _locate()
            if kind is None:
                return False
            try:
                self.dev = kind(path)
            except (OSError, IOError) as e:
                print(f"can't open {path!r}: {e}", file=sys.stderr)
                return False
            self.path = path
            print(f"connected to {path!r}", file=sys.stderr)
            self._changed()
            return True

    def _drop(self):
        if self.dev is not None:
            try:
                self.dev.close()
            except OSError:
                pass
            self.dev = None
            print("knob disconnected", file=sys.stderr)
            self._changed()

    def write(self, report):
        """Sends one report; False if the knob isn't there."""
        # Leading 0 = report ID; QMK's Raw HID reports are unnumbered.
        data = b"\0" + report.ljust(RAW_EPSIZE, b"\0")
        with self.lock:
            if self.dev is None:
                return False
            try:
                self.dev.write(data)
                return True
            except (OSError, IOError, ValueError):
                self._drop()
                return False

    def read(self, timeout):
        """One report, or None after `timeout` seconds (or if unplugged)."""
        dev = self.dev
        if dev is None:
            return None
        try:
            return dev.read(timeout)
        except (OSError, IOError, ValueError):
            with self.lock:
                if self.dev is dev:
                    self._drop()
            return None


def send_bootloader():
    """Reboots the knob into BOOTSEL (flash.sh); False if it isn't found."""
    knob = Knob()
    if not knob.open():
        return False
    knob.write(bytes([BOOTLOADER_CMD]))
    knob._drop()
    return True
