# volume-keeb

A volume-control knob built on a Waveshare RP2040-Zero and an HW-040 rotary encoder module. Runs QMK. Sibling project to [single-keeyb](https://github.com/joaopedropio/single-keeyb), which this repo was forked from.

## Hardware

- Waveshare RP2040-Zero
- 1x HW-040 rotary encoder module (CLK/DT/SW/+/GND)
- 1x GME12864 1.3" OLED, SH1107 controller (64x128 panel, driven rotated to 128x64 landscape), I2C (VCC/GND/SCL/SDA)

### Wiring

| HW-040 pin | RP2040-Zero pin |
|---|---|
| CLK | GP0 |
| DT | GP1 |
| SW | GP2 |
| + | 3V3 |
| GND | GND |

SW is wired directly to ground (no diode, no matrix) — QMK reads it as a direct pin with an internal pull-up, and it's the encoder module's only key.

| OLED pin | RP2040-Zero pin |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SCL | GP5 |
| SDA | GP4 |

The OLED uses the RP2040's I2C0 peripheral at the default address `0x3C`. The module has its own I2C pull-ups.

### Behavior

| Action | Result |
|---|---|
| Rotate clockwise | Volume up |
| Rotate counter-clockwise | Volume down |
| Press encoder (SW) | Mute |

The OLED shows the title of whatever Chrome is playing (e.g. a YouTube video) in double-size text, centered, or scrolling horizontally when it doesn't fit. Below it is a volume bar with the host's output volume percentage (or `MUTE`) to its right. A keyboard can't read either, so `host/volume_sync.py` pushes them over QMK Raw HID whenever it changes (and every 2 s as a heartbeat). If the script isn't running, the volume shows `--%`. The display turns off after 60 s without activity to prevent burn-in (scrolling stops then too), and wakes up on any volume or track change.

### Host volume sync (Linux)

Requires `pactl` (PulseAudio, or PipeWire with `pipewire-pulse`), `busctl` (systemd; reads Chrome's MPRIS media session) and Python 3. No other dependencies.

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
