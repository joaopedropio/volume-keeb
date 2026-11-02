"""Updates from the GitHub releases: checks for a newer one now and then
(unless that's turned off on the settings page; "Check now" still works),
and puts its firmware on the knob from the settings page.

Updating the firmware asks the running knob to reboot into the RP2040's
BOOTSEL bootloader (BOOTLOADER_CMD), waits for the RPI-RP2 drive it shows
up as and copies the release's .uf2 onto it, like flash.sh. The app itself
isn't replaced: the page links to the new installer on Windows and gives the
git command on Linux.
"""
import json
import os
import struct
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

from .protocol import BOOTLOADER_CMD
from .version import VERSION, parse

REPO = "joaopedropio/volume-keeb"
LATEST_URL = f"https://api.github.com/repos/{REPO}/releases/latest"
CHECK_EVERY_S = 6 * 3600
UF2_NAME = "volume-keeb.uf2"
EXE_NAME = "volume-keeb.exe"
SETUP_NAME = "volume-keeb-setup.exe"
UF2_MAX_BYTES = 4 * 1024 * 1024
DRIVE_LABEL = "RPI-RP2"
DRIVE_WAIT_S = 30
RECONNECT_WAIT_S = 30

# UF2 block layout (https://github.com/microsoft/uf2) and the RP2040's family id.
UF2_MAGIC = (0x0A324655, 0x9E5D5157, 0x0AB16F30)
UF2_FLAG_FAMILY = 0x2000
RP2040_FAMILY = 0xE48BFF56


class UpdateError(Exception):
    pass


def _fetch(url, limit):
    req = urllib.request.Request(url, headers={"User-Agent": f"volume-keeb/{VERSION}", "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=20) as res:
        data = res.read(limit + 1)
    if len(data) > limit:
        raise UpdateError("download too big")
    return data


def check_uf2(data):
    """Raises unless `data` is a UF2 image for the RP2040."""
    if not data or len(data) % 512:
        raise UpdateError("not a UF2 file")
    for off in range(0, len(data), 512):
        m0, m1, flags = struct.unpack_from("<III", data, off)
        family = struct.unpack_from("<I", data, off + 28)[0]
        end = struct.unpack_from("<I", data, off + 508)[0]
        if (m0, m1, end) != UF2_MAGIC:
            raise UpdateError("not a UF2 file")
        if not flags & UF2_FLAG_FAMILY or family != RP2040_FAMILY:
            raise UpdateError("the UF2 isn't for the RP2040")


def _has_info(root):
    return (Path(root) / "INFO_UF2.TXT").exists()


def find_drive():
    """The mounted RPI-RP2 drive's root, or None. Mounts it on Linux if the
    desktop didn't."""
    if sys.platform == "win32":
        import ctypes

        k32 = ctypes.windll.kernel32
        mask = k32.GetLogicalDrives()
        for i in range(26):
            if not mask & (1 << i):
                continue
            root = f"{chr(65 + i)}:\\"
            label = ctypes.create_unicode_buffer(64)
            if k32.GetVolumeInformationW(root, label, 64, None, None, None, None, 0) and label.value == DRIVE_LABEL and _has_info(root):
                return root
        return None
    if sys.platform == "darwin":
        root = f"/Volumes/{DRIVE_LABEL}"
        return root if _has_info(root) else None
    try:
        out = subprocess.run(["lsblk", "-rno", "NAME,LABEL,MOUNTPOINT"], capture_output=True, text=True).stdout
    except OSError:
        return None
    for line in out.splitlines():
        fields = line.split(" ")
        if len(fields) < 2 or fields[1] != DRIVE_LABEL:
            continue
        mount = fields[2].encode().decode("unicode_escape") if len(fields) > 2 else ""
        if not mount:
            subprocess.run(["udisksctl", "mount", "-b", f"/dev/{fields[0]}"], capture_output=True)
            continue  # picked up on the next poll
        if _has_info(mount):
            return mount
    return None


class Updates:
    def __init__(self, engine):
        self.engine = engine
        self.latest = None  # {"version", "url", "uf2", "exe", "setup"} of the latest release
        self.checked_at = 0.0
        self.error = ""
        self.job = {"state": "idle", "error": ""}  # the firmware update
        self.lock = threading.Lock()
        self.on_change = None  # called when there comes to be an update or not, e.g. to refresh the tray menu

    def start(self):
        def loop():
            shown = None
            while True:
                if self.engine.config.data["check_updates"] and time.time() - self.checked_at >= CHECK_EVERY_S:
                    self.check()
                # The firmware's version is only known once the knob connects.
                now = (self.app_update(), self.firmware_update())
                if now != shown and self.on_change:
                    shown = now
                    self.on_change()
                time.sleep(10)

        threading.Thread(target=loop, name="updates", daemon=True).start()

    def check(self):
        try:
            rel = json.loads(_fetch(LATEST_URL, 1024 * 1024))
            assets = {a["name"]: a["browser_download_url"] for a in rel.get("assets", [])}
            self.latest = {"version": rel["tag_name"].lstrip("v"), "url": rel["html_url"],
                           "uf2": assets.get(UF2_NAME), "exe": assets.get(EXE_NAME), "setup": assets.get(SETUP_NAME)}
            self.error = ""
        except (OSError, ValueError, KeyError, UpdateError) as e:
            self.error = f"couldn't check for updates: {e}"
        self.checked_at = time.time()

    def _newer_than(self, version):
        latest = parse(self.latest["version"]) if self.latest else None
        return bool(latest and (parse(version) or (0, 0, 0)) < latest)

    def app_update(self):
        return self._newer_than(VERSION)

    def firmware_update(self):
        if not (self.latest and self.latest["uf2"] and self.engine.knob.connected):
            return False
        # Firmware that doesn't answer VERSION_QUERY_CMD is v0.1.0 or older.
        return self._newer_than(self.engine.firmware_version or "0.1.0")

    def state(self):
        frozen = getattr(sys, "frozen", False)
        return {
            "app_version": VERSION,
            "firmware_version": self.engine.firmware_version,
            "latest": self.latest,
            "app_update": self.app_update(),
            "firmware_update": self.firmware_update(),
            "checked_at": self.checked_at,
            "auto": self.engine.config.data["check_updates"],
            "error": self.error,
            "job": self.job,
            "platform": "windows" if sys.platform == "win32" else "mac" if sys.platform == "darwin" else "linux",
            "frozen": frozen,
            "repo_dir": None if frozen else str(Path(__file__).resolve().parent.parent.parent),
        }

    # -- the firmware --------------------------------------------------------

    def update_firmware(self):
        """Starts putting the latest release's firmware on the knob; the page
        follows it through `job`."""
        with self.lock:
            if self.job["state"] not in ("idle", "done", "error"):
                raise UpdateError("already updating")
            if not (self.latest and self.latest["uf2"]):
                raise UpdateError("no release with firmware found; check for updates first")
            self.job = {"state": "downloading", "error": ""}
        threading.Thread(target=self._flash, name="firmware-update", daemon=True).start()

    def _flash(self):
        job = self.job
        try:
            data = _fetch(self.latest["uf2"], UF2_MAX_BYTES)
            check_uf2(data)

            drive = find_drive()  # already in BOOTSEL?
            if drive is None:
                job["state"] = "rebooting"
                if not self.engine.knob.write(bytes([BOOTLOADER_CMD])):
                    raise UpdateError("the knob isn't connected")
                job["state"] = "waiting"
                deadline = time.monotonic() + DRIVE_WAIT_S
                while drive is None and time.monotonic() < deadline:
                    time.sleep(0.5)
                    drive = find_drive()
                if drive is None:
                    raise UpdateError(f"the {DRIVE_LABEL} drive never showed up; the knob may run firmware too old "
                                      "to reboot on its own: hold BOOT, tap RESET, and try again")

            job["state"] = "copying"
            setups = self.engine.setups
            try:
                with open(Path(drive) / UF2_NAME, "wb") as f:
                    f.write(data)
                    f.flush()
                    os.fsync(f.fileno())
            except OSError as e:
                # The drive vanishes the moment the RP2040 has taken the
                # whole image, which can beat the close.
                print(f"copy to {drive} ended with {e}", file=sys.stderr)

            job["state"] = "restarting"
            deadline = time.monotonic() + RECONNECT_WAIT_S
            while self.engine.setups == setups and time.monotonic() < deadline:
                time.sleep(0.5)
            if self.engine.setups == setups:
                raise UpdateError("copied, but the knob didn't come back; unplug it and plug it in again")
            job["state"] = "done"
        except (OSError, UpdateError) as e:
            job.update(state="error", error=str(e))
