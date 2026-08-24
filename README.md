# volume-keeb

A volume-control knob built on a Waveshare RP2040-Zero and an HW-040 rotary encoder module. Runs QMK. Sibling project to [single-keeyb](https://github.com/joaopedropio/single-keeyb), which this repo was forked from.

## Hardware

- Waveshare RP2040-Zero
- 1x HW-040 rotary encoder module (CLK/DT/SW/+/GND)

### Wiring

| HW-040 pin | RP2040-Zero pin |
|---|---|
| CLK | GP0 |
| DT | GP1 |
| SW | GP2 |
| + | 3V3 |
| GND | GND |

SW is wired directly to ground (no diode, no matrix) — QMK reads it as a direct pin with an internal pull-up, and it's the encoder module's only key.

### Behavior

| Action | Result |
|---|---|
| Rotate clockwise | Volume up |
| Rotate counter-clockwise | Volume down |
| Press encoder (SW) | Mute |

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
