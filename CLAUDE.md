# volume-keeb

Volume-control knob on a Waveshare RP2040-Zero with an HW-040 rotary encoder, running QMK. See README.md for hardware/wiring/build/flash details.

Sibling project to [single-keeyb](https://github.com/joaopedropio/single-keeyb) (a separate Space-key macropad on the same MCU). The encoder was originally prototyped on the single-keeyb board, then pulled out into this standalone project — don't merge the two back together.

## Repo layout

- `qmk/waveshare_rp2040_zero_volume/` — the actual QMK custom keyboard definition (`keyboard.json`, `config.h`, `halconf.h`, `mcuconf.h`, `rules.mk`, `keymaps/volume/keymap.c`, `graphics/*.qff.c`). This is the single source of truth.
- `host/` — `volume_sync.py` (stdlib-only daemon that turns `pactl`'s per-app streams plus MPRIS players into an app list, pushes it to the knob, and applies the volume/mute/play-pause actions the knob sends back), its udev rule and systemd user unit. Raw HID packet formats: host→device `0x60`/`0x61`/`0x62` (begin/entry/end of an app-list snapshot), `0x54` (title chunk for the app with media), and `0x63` (display theme, an index into `keymap.c`'s `themes[]`, set via `volume_sync.py --theme`); device→host `0x70` (set volume / toggle mute / play-pause for one app id). Both the layout and the numeric command values are defined in `volume_sync.py` and `keymap.c` — keep them in sync.
- `qmk/waveshare_rp2040_zero_volume/graphics/*.qff.c` — Quantum Painter fonts, regenerated (not hand-edited) with `qmk painter-make-font-image` + `qmk painter-convert-font-image` from DejaVu Sans Mono; see the header comment each file carries for the exact invocation.

## Firmware architecture

QMK builds require the keyboard directory to live inside a `qmk_firmware` checkout at `keyboards/handwired/onekey/waveshare_rp2040_zero_volume/`. Rather than keeping two copies in sync, that path is a **symlink** into this repo's `qmk/waveshare_rp2040_zero_volume/`. If a fresh `qmk_firmware` checkout is ever made, recreate the symlink — don't copy the files:

```sh
ln -s /path/to/this/repo/qmk/waveshare_rp2040_zero_volume ~/qmk_firmware/keyboards/handwired/onekey/waveshare_rp2040_zero_volume
```

Edit keyboard/keymap files only inside this repo, never through the symlink target's other path — they're the same files, but editing via the repo path keeps `git status` sane.

### No separate matrix key — the encoder's SW pin is the only key

QMK requires every board to have at least one matrix key defined in a `LAYOUT` (a `layout: []` with zero entries fails linting: "No LAYOUTs defined!"). An encoder's rotation pins (`pin_a`/`pin_b`) are handled entirely separately from the matrix by `quantum/encoder.c` — they never count as a matrix key. So this board's single matrix entry is the HW-040's own `SW` pushbutton pin (GP2), mapped to a custom `ENC_CLICK` keycode whose meaning (menu select / back-to-menu / double-click pause / hold-to-mute) depends on which screen is active; `GP0`/`GP1` (CLK/DT) are declared only under `encoder.rotary` in `keyboard.json` and don't appear in the matrix at all. This is why the board can live under `handwired/onekey/` (which enforces a 1x1 `LAYOUT_ortho_1x1`) despite having no separate physical button — the encoder's built-in switch satisfies that requirement.

## Why QMK, not KMK/CircuitPython

CircuitPython's `usb_hid` module (and therefore KMK, which sits on top of it) has no implementation of the USB remote-wakeup protocol — confirmed via an open upstream issue, [adafruit/circuitpython#8774](https://github.com/adafruit/circuitpython/issues/8774). That makes wake-from-suspend impossible on that stack. Don't reintroduce CircuitPython/KMK for this project.

## Build toolchain notes

- The current dev machine is Ubuntu 24.04: QMK CLI via `pipx install qmk` (→ `~/.local/bin/qmk`), `qmk_firmware` shallow-cloned at `~/qmk_firmware`, ARM toolchain via `apt` (`gcc-arm-none-eabi`, `libnewlib-arm-none-eabi`, `binutils-arm-none-eabi`). The Arch notes below are from an earlier machine.
- QMK CLI was installed via `pip install --user qmk` under a pyenv-managed Python (system Python is externally-managed per PEP 668 on this Arch machine) — landed at `~/.local/bin/qmk`.
- ARM toolchain (`arm-none-eabi-gcc`/`newlib`/`binutils`) and `dfu-util` come from the Arch `extra` repo via `pacman`.
- Flashing is drag-and-drop UF2 (BOOTSEL bootloader), not `qmk flash`/`dfu-util` — mount the `RPI-RP2` drive and copy the `.uf2` in.
