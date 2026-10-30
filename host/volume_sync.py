#!/usr/bin/env python3
"""Entry point for volume-keeb's host app (see volume_keeb/app.py): the
per-app mixer sync, the settings page and the tray icon. The systemd unit,
flash.sh and the Windows build all start it through this file."""
from volume_keeb.app import main

if __name__ == "__main__":
    main()
