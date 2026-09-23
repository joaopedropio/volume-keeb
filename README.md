# volume-keeb

A per-app volume mixer built on a Waveshare RP2040-Zero, an HW-040 rotary encoder module, and a 76x284 ST7789 SPI LCD. Runs QMK. Sibling project to [single-keeyb](https://github.com/joaopedropio/single-keeyb), which this repo was forked from.

> This is the `st7789-display` branch: an experimental swap of the original 1.3" OLED for a tall SPI LCD, driven with QMK's Quantum Painter instead of the OLED driver, with a two-screen app-picker UI replacing the old single always-on-master-volume display.

## Hardware

- Waveshare RP2040-Zero
- 1x HW-040 rotary encoder module (CLK/DT/SW/+/GND)
- 1x 76x284 ST7789 SPI LCD (native-portrait panel, mounted/driven landscape — see below)

### Wiring

| HW-040 pin | RP2040-Zero pin |
|---|---|
| CLK | GP0 |
| DT | GP1 |
| SW | GP2 |
| + | 3V3 |
| GND | GND |

SW is wired directly to ground (no diode, no matrix) — QMK reads it as a direct pin with an internal pull-up, and it's the encoder module's only key.

| LCD pin | RP2040-Zero pin |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SCK | GP6 |
| MOSI (SDA) | GP7 |
| CS | GP9 |
| DC | GP10 |
| RST | GP11 |
| BLK | GP12 |

The LCD is write-only, so `SPI_MISO_PIN` (GP4) isn't physically connected — it's only declared because the RP2040's SPI0 peripheral needs a valid pin assigned to configure. `BLK` is a plain digital GPIO (not PWM) toggled by the firmware to blank the panel on the idle timeout; it isn't tied straight to 3V3.

The panel is 76x284 in its native orientation, but Quantum Painter drives it rotated 90° (`QP_ROTATION_90`), so the UI itself is landscape: 284px wide by 76px tall. `LCD_PANEL_WIDTH`/`LCD_PANEL_HEIGHT` in `config.h` are the native dimensions passed to the display driver; `LCD_WIDTH`/`LCD_HEIGHT` are the rotated, effective canvas everything else in `keymap.c` lays out against. If the mounted panel ends up showing the UI upside down, flip `QP_ROTATION_90` to `QP_ROTATION_270` in `keymap.c` — that's the only thing rotation direction affects.

### Behavior

The knob has two screens, both landscape:

- **Menu screen** — a horizontally-scrollable row of "app" chips (the default output device as `Master`, plus every currently-playing PulseAudio/PipeWire stream — a vertical list doesn't fit once the panel is only 76px tall). Rotate the encoder to move the highlighted chip; click to open that app's screen. A small marker tags whichever chip the App screen would return to, independent of the navigation cursor.
- **App screen** — a green-on-black "terminal" look: app name and `VOL nn`/`MUTE` in a thin header row, the media title (if any) large underneath, and a 20-segment volume bar along the bottom instead of one continuous fill.

| Action | Menu screen | App screen |
|---|---|---|
| Rotate | Move selection | Adjust that app's volume |
| Click | Open selected app | Back to menu |
| Double-click | — | Play/pause (if the app exposes MPRIS media controls) |
| Click and hold | — | Toggle mute |

App and title text scrolls a character at a time when it doesn't fit its row. The panel blanks after 60 s without activity (encoder/click input or a host-side change) to prevent burn-in, and wakes on either. If `host/volume_sync.py` stops sending, the app list clears and the knob falls back to the menu screen.

### Host volume sync (Linux)

Requires `pactl` (PulseAudio, or PipeWire with `pipewire-pulse`), `busctl` (systemd; reads MPRIS media sessions for titles and play/pause) and Python 3. No other dependencies. Per-app matching between a PulseAudio stream and an MPRIS player is done by normalizing both apps' names (lowercased, non-alphanumerics stripped) and comparing — good enough for common cases like a browser tab, but it's a heuristic, not a guarantee.

```sh
# Let your user write to the knob's hidraw device
sudo cp host/70-volume-keeb.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger

# Run at login (the unit expects this repo at ~/volume-keeb)
mkdir -p ~/.config/systemd/user
cp host/volume-sync.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now volume-sync
```

## Firmware

This board runs on **QMK**, not CircuitPython/KMK — CircuitPython's `usb_hid` stack has no USB remote-wakeup support, so it can't wake a suspended host on keypress ([adafruit/circuitpython#8774](https://github.com/adafruit/circuitpython/issues/8774)).

The custom keyboard definition lives in this repo at `qmk/waveshare_rp2040_zero_volume/` and is meant to be symlinked into a `qmk_firmware` checkout:

```sh
git clone https://github.com/qmk/qmk_firmware.git ~/qmk_firmware
ln -s "$(pwd)/qmk/waveshare_rp2040_zero_volume" ~/qmk_firmware/keyboards/handwired/onekey/waveshare_rp2040_zero_volume
```

### Build tools

- `arm-none-eabi-gcc`, `arm-none-eabi-newlib`, `arm-none-eabi-binutils` (ARM cross toolchain)
- `dfu-util`
- QMK CLI (`pip install qmk`)

On Arch:

```sh
sudo pacman -S --needed arm-none-eabi-gcc arm-none-eabi-newlib arm-none-eabi-binutils dfu-util
pip install --user qmk
```

On Ubuntu:

```sh
sudo apt install -y gcc-arm-none-eabi libnewlib-arm-none-eabi binutils-arm-none-eabi
pipx install qmk
git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/qmk/qmk_firmware.git ~/qmk_firmware
qmk config user.qmk_home=$HOME/qmk_firmware
```

### Build

```sh
cd ~/qmk_firmware
qmk compile -kb handwired/onekey/waveshare_rp2040_zero_volume -km volume
```

This produces `handwired_onekey_waveshare_rp2040_zero_volume_volume.uf2` in `~/qmk_firmware`.

### Flash

1. Unplug the board.
2. Plug it back in while holding the **BOOTSEL** button until it mounts as a drive named `RPI-RP2`.
3. Copy the `.uf2` file onto that drive. The board reboots automatically into the new firmware.

```sh
cp handwired_onekey_waveshare_rp2040_zero_volume_volume.uf2 /run/media/$USER/RPI-RP2/
```
