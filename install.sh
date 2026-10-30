#!/bin/sh
# Sets volume-keeb's host app up on this Linux computer: installs what it
# needs, lets your user talk to the knob, starts it at every login and adds
# "volume-keeb settings" to the app menu. Run it again after moving the repo
# or pulling a new version; `./install.sh --uninstall` undoes it.
set -e
cd "$(dirname "$0")"
REPO=$(pwd)
UNIT_DIR=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user
APPS_DIR=${XDG_DATA_HOME:-$HOME/.local/share}/applications
RULE=/etc/udev/rules.d/70-volume-keeb.rules
URL=http://localhost:8137/

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

if [ "$1" = "--uninstall" ]; then
    say "Removing volume-keeb"
    systemctl --user disable --now volume-sync 2>/dev/null || true
    rm -f "$UNIT_DIR/volume-sync.service" "$APPS_DIR/volume-keeb.desktop"
    systemctl --user daemon-reload
    sudo rm -f "$RULE"
    echo "Done. Your settings stay in ~/.config/volume-keeb (delete it to forget them)."
    exit 0
fi

if [ "$(id -u)" = 0 ]; then
    echo "Run this as your normal user (it asks for sudo when it needs it)." >&2
    exit 1
fi

say "Installing dependencies (asks for your password)"
if command -v apt-get >/dev/null; then
    sudo apt-get update -qq
    sudo apt-get install -y python3 python3-pil python3-gi pulseaudio-utils gir1.2-ayatanaappindicator3-0.1
    sudo apt-get install -y python3-pystray 2>/dev/null || true
elif command -v pacman >/dev/null; then
    sudo pacman -S --needed --noconfirm python python-pillow python-gobject libpulse libayatana-appindicator
elif command -v dnf >/dev/null; then
    sudo dnf install -y python3 python3-pillow python3-gobject pulseaudio-utils libayatana-appindicator-gtk3
else
    echo "Unknown package manager: install Python 3, Pillow, PyGObject, pactl and libayatana-appindicator yourself."
fi
# Not packaged everywhere; it's pure Python, so a per-user pip install does.
if ! python3 -c 'import pystray' 2>/dev/null; then
    python3 -m pip install --user --break-system-packages pystray 2>/dev/null \
        || python3 -m pip install --user pystray 2>/dev/null \
        || echo "Couldn't install pystray: no tray icon, but the settings page works at $URL"
fi

say "Letting your user talk to the knob"
sudo install -m 644 host/70-volume-keeb.rules "$RULE"
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=hidraw

say "Starting it now and at every login"
mkdir -p "$UNIT_DIR"
sed "s|^ExecStart=.*|ExecStart=$REPO/host/volume_sync.py|" host/volume-sync.service > "$UNIT_DIR/volume-sync.service"
chmod +x host/volume_sync.py
systemctl --user daemon-reload
systemctl --user enable volume-sync
systemctl --user restart volume-sync

say "Adding \"volume-keeb settings\" to the app menu"
mkdir -p "$APPS_DIR"
cat > "$APPS_DIR/volume-keeb.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=volume-keeb settings
Comment=Theme, colors and fonts of the volume knob
Exec=xdg-open $URL
Icon=audio-volume-high
Categories=Settings;AudioVideo;
DESKTOP

sleep 2
if systemctl --user is-active --quiet volume-sync; then
    say "All set"
    echo "Plug in the knob (unplug and replug it if it was already in)."
    echo "Settings: the knob icon in the tray, \"volume-keeb settings\" in the app menu, or $URL"
    xdg-open "$URL" >/dev/null 2>&1 || true
else
    echo "volume-sync didn't start; see: journalctl --user -u volume-sync" >&2
    exit 1
fi
