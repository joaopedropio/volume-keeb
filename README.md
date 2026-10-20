# volume-keeb

A per-app volume mixer built on a Waveshare RP2040-Zero, an HW-040 (or KY-040) rotary encoder module, and a 2.79" 142x428 NV3007 SPI LCD (custom driver in `qp_nv3007.c`). Runs QMK. Sibling project to [single-keeyb](https://github.com/joaopedropio/single-keeyb), which this repo was forked from.

The display is driven with QMK's Quantum Painter, with a two-screen app-picker UI (earlier revisions used a 1.3" OLED, then ST7789 and ILI9341 SPI panels).

## Hardware

- Waveshare RP2040-Zero
- 1x HW-040 rotary encoder module (CLK/DT/SW/+/GND); a KY-040 has the same pins and wires the same way, but the case is only drawn for the HW-040 so far (see [Case](#case))
- 1x 2.79" 142x428 NV3007 SPI IPS LCD (native-portrait panel, mounted/driven landscape — see below)

### Wiring

| HW-040 / KY-040 pin | RP2040-Zero pin |
|---|---|
| CLK | GP27 |
| DT | GP28 |
| SW | GP29 |
| + | 3V3 |
| GND | GND |

SW is wired directly to ground (no diode, no matrix) — QMK reads it as a direct pin with an internal pull-up, and it's the encoder module's only key.

| LCD pin | RP2040-Zero pin |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SCK (SCL) | GP2 |
| MOSI (SDA) | GP3 |
| CS | GP1 |
| DC | GP4 |
| RST | GP5 |
| BLK | GP0 |

The LCD is write-only, so `SPI_MISO_PIN` (GP16, not broken out on the board) isn't physically connected — it's only declared because the RP2040's SPI0 peripheral needs a valid pin assigned to configure. `BLK` is a plain digital GPIO (not PWM, active-high on the NV3007 module: high = backlight on) toggled by the firmware to blank the panel on the idle timeout; it isn't tied straight to 3V3.

The panel is 142x428 in its native orientation, but Quantum Painter drives it rotated 90° (`QP_ROTATION_90`), so the UI itself is landscape: 428px wide by 142px tall. `LCD_PANEL_WIDTH`/`LCD_PANEL_HEIGHT` in `config.h` are the native dimensions passed to the display driver; `LCD_WIDTH`/`LCD_HEIGHT` are the rotated, effective canvas everything else in `keymap.c` lays out against. If the mounted panel ends up showing the UI upside down, flip `QP_ROTATION_90` to `QP_ROTATION_270` in `keymap.c` — that's the only thing rotation direction affects.

#### NV3007 driver

QMK has no upstream NV3007 driver, so `qp_nv3007.c` provides one: it reuses Quantum Painter's generic `tft_panel`/SPI machinery (the same code path as the ST7789 driver, which is why `rules.mk` still lists `st7789_spi` under `QUANTUM_PAINTER_DRIVERS`) and only supplies the controller's own init sequence. Panel tuning knobs live in `config.h`:

- `LCD_OFFSET_X`/`LCD_OFFSET_Y` — where the 142px-wide glass sits inside the controller's 168-wide RAM, in the rotated frame (currently `0`/`13`). If the image is shifted or has a garbage strip along one edge, adjust these.
- `NV3007_INVERT` — uncomment if colors come out inverted.
- `LCD_SPI_DIVISOR`/`LCD_SPI_MODE` — SPI clock divisor (`4`) and mode (`3`).

### Behavior

The knob has two screens, both landscape:

- **Menu screen** — a horizontally-scrollable row of "app" chips (the default output device as `Master`, plus every currently-playing PulseAudio/PipeWire stream — a vertical list doesn't fit once the panel is only 142px tall). Rotate the encoder to move the highlighted chip; click to open that app's screen. A small marker tags whichever chip the App screen would return to, independent of the navigation cursor.
- **App screen** — app name and `VOL nn`/`MUTE` in a thin header row, the media title (if any) large underneath, and a 30-segment volume bar along the bottom instead of one continuous fill. Colors and decoration (flat, thick-bordered/drop-shadowed, or beveled LCD-style wells) depend on the selected theme — see [Themes](#themes) below.

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

#### Themes

`volume_sync.py --theme {terminal,neobrutal,winamp}` picks the knob's display theme (default `terminal`); the choice is pushed to the knob once at startup and again on every reconnect, since the firmware itself doesn't persist it across power cycles.

- `terminal` — the original flat green-phosphor look.
- `neobrutal` — cream/black/orange, thick borders and hard (unblurred) drop shadows, styled after [this Figma Make concept](https://www.figma.com/make/t8KsdhbALpn6rPHxM7KQUm/Volume-Controller-UI).
- `winamp` — a grey-blue chassis with black "LCD" wells (the header and title rows, the volume bar) sunk in via a raised/sunken 1px bevel, green readout text, and an amber accent, styled after the classic Winamp player skin. Menu chips read as raised buttons that go sunken when selected, like a pressed toolbar button.

To make a theme stick across reboots, add the flag to the systemd unit before enabling it:

```sh
sed -i 's/volume_sync.py/volume_sync.py --theme neobrutal/' ~/.config/systemd/user/volume-sync.service
systemctl --user daemon-reload
systemctl --user restart volume-sync
```

## Case

**See it in 3D: https://joaopedropio.github.io/volume-keeb/**

A 3D-printed two-piece case, `case/volume_case.scad` (OpenSCAD, parametric), exported to `case/volume_case-top.stl` and `case/volume_case-base.stl`. It is about 89 x 84 x 32 mm: the screen sits behind a window in a 60° face at the front, the 48 x 22 mm metal knob sits 7 mm into a plain hole in the flat top, and the RP2040-Zero's USB-C comes out the middle of the back.

Every part is held by the base. The top has no parts of its own: it comes down over everything, knob included, and takes four screws from below.

- **Screen:** stands on two ribs behind its ends. The right rib takes the module's two holes: M2 x 5 pan-head screws go in from the front of the board, into heat-set inserts. The left rib only props the other end.
- **Encoder:** sits on a frame of four walls under its PCB's edges, with two pillars inside it under the PCB's holes. M2 screws go in from above (heads up) into inserts. The frame puts the knob's underside 0.5 mm above the encoder's bushing. Its right wall stands beside the pads, so solder the encoder's wires from the top of the board.
- **RP2040-Zero:** lies upside down (buttons and USB-C toward the floor) on its own USB receptacle, with rails and a clip holding it. The clip is rigid, so put the board in tilted: far edge under the hook first, then the USB end down. **BOOT** and **RESET** are pressed with a pin through the holes marked `B` and `R` under the case.
- **Top:** four M2 x 6 flat-head screws go in from below the base, into inserts in the top's bosses.

Nothing has pin headers; solder the wires straight to the pads. You need 8 M2 heat-set inserts (3 mm long, 3.5 mm across) in all: 4 in the top, 2 in the screen rib and 2 in the encoder pillars.

To make changes, edit the variables at the top of the `.scad`: knob size and how far it sits in, borders, and so on. Then regenerate:

```sh
case/gen_case.sh   # needs openscad on the PATH
```

It also exports the electronics' stand-ins to `case/view/`, for `case/viewer.html`: a WebGL 3D view with a checkbox per part and a slider that lifts the top. The live copy at the link above is deployed with `case/deploy_pages.sh`, which copies the page and the STLs into the `joaopedropio.github.io` checkout (default `~/joaopedropio.github.io`, under `volume-keeb/`). Commit and push that repo afterwards. To try the page locally, serve the folder, because browsers won't fetch the STLs from a `file://` page:

```sh
python3 -m http.server -d case 8000   # then open http://localhost:8000/viewer.html
```

The script also runs the model's two checks and fails if either finds an overlap (touching faces are fine). `part="check"` intersects the parts with stand-ins for the electronics. `part="insert"` intersects the top with everything it meets on its way down.

The encoder is chosen with `encoder` in the `.scad`. Only `"hw040"` has measurements so far: they come from `hw-040-encoder-module.stp` and `hw-040-module-dimensions.webp`. A KY-040 needs its own entry in `enc_dims()` (PCB size, hole positions, shaft position, body, bushing and shaft heights) before `encoder = "ky040"` renders.

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
2. Plug it back in while holding the **BOOTSEL** button until it mounts as a drive named `RPI-RP2`. Inside the case, press BOOT with a pin through the hole marked `B` under the case.
3. Copy the `.uf2` file onto that drive. The board reboots automatically into the new firmware.

```sh
cp handwired_onekey_waveshare_rp2040_zero_volume_volume.uf2 /run/media/$USER/RPI-RP2/
```
