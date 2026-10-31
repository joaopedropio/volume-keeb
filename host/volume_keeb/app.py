"""volume-keeb's host app: keeps the knob's per-app mixer in step with the
computer and serves the settings page (theme, colors, screensaver, fonts,
updates) on localhost, with a tray icon to open it.

Linux: pactl + MPRIS, /dev/hidraw. Windows: WASAPI + media sessions,
hidapi. See host/requirements.txt.
"""
import argparse
import errno
import os
import re
import subprocess
import sys
import threading
import time
import webbrowser

from . import tray
from .config import Config
from .engine import Engine
from .knob import send_bootloader
from .protocol import PRESETS
from .updates import Updates
from .web import Server


def load_audio():
    if sys.platform == "win32":
        from .audio_windows import Audio
    else:
        from .audio_linux import Audio
    return Audio()


def restart_when_session_appears():
    """Under systemd, the unit often starts at login before the desktop has
    told systemd its DISPLAY/WAYLAND_DISPLAY, so there's no tray to show the
    icon in. Keep running without it, and once systemd has them, exit with
    an error so Restart=on-failure starts us again with them."""
    if not sys.platform.startswith("linux") or "INVOCATION_ID" not in os.environ:
        return
    if os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"):
        return

    def watch():
        while True:
            time.sleep(5)
            env = subprocess.run(["systemctl", "--user", "show-environment"], capture_output=True, text=True).stdout
            if re.search(r"^(DISPLAY|WAYLAND_DISPLAY)=", env, re.M):
                print("desktop session is up; restarting to show the tray icon", file=sys.stderr)
                os._exit(75)

    threading.Thread(target=watch, daemon=True).start()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--theme", choices=sorted(PRESETS), help="switch to this preset theme (saved, like picking it on the page)")
    parser.add_argument("--port", type=int, help="settings page port (default: the config's, 8137)")
    parser.add_argument("--open", action="store_true", help="open the settings page in the browser at start")
    parser.add_argument("--no-tray", action="store_true", help="don't show a tray icon")
    parser.add_argument("--bootloader", action="store_true", help="reboot the knob into its BOOTSEL bootloader (for flashing) and exit")
    args = parser.parse_args(argv)

    if args.bootloader:
        if not send_bootloader():
            sys.exit("knob not found")
        return

    config = Config()
    if args.theme:
        preset = PRESETS[args.theme]
        config.set_theme(dict(preset["colors"]), preset["decor"], args.theme)

    engine = Engine(config, load_audio())
    updates = Updates(engine)
    try:
        server = Server(args.port or config.data["port"], engine, updates)
    except OSError as e:
        if e.errno not in (errno.EADDRINUSE, getattr(errno, "WSAEADDRINUSE", None)):
            raise
        # Most likely already running (started again from the menu or the
        # .exe): show its page instead of starting a second one.
        url = f"http://localhost:{args.port or config.data['port']}/"
        print(f"port in use, so presumably already running: {url}", file=sys.stderr)
        webbrowser.open(url)
        return
    server.start()
    engine.start()
    updates.start()
    if args.open:
        webbrowser.open(server.url)

    autostart = None
    if sys.platform == "win32":
        from . import autostart
    if not args.no_tray:
        restart_when_session_appears()
    if args.no_tray or not tray.run(server.url, autostart, updates, engine.knob):
        try:
            threading.Event().wait()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
