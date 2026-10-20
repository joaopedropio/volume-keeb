# volume-keeb: development notes

Everything you need to change the firmware, the host daemon or the case. If you only want to build one, read the [README](../README.md) instead.

## Contents

- [Repo layout](#repo-layout)
- [Firmware](#firmware)
  - [Why QMK](#why-qmk)
  - [Keyboard definition](#keyboard-definition)
  - [The SW pin is the only key](#the-sw-pin-is-the-only-key)
  - [Display and the NV3007 driver](#display-and-the-nv3007-driver)
  - [Tuning constants](#tuning-constants)
  - [Diagnostic keymaps](#diagnostic-keymaps)
  - [Fonts](#fonts)
- [Host daemon](#host-daemon)
  - [Raw HID protocol](#raw-hid-protocol)
  - [Adding a theme](#adding-a-theme)
- [Case](#case)
  - [How it holds the parts](#how-it-holds-the-parts)
  - [Regenerating and checking](#regenerating-and-checking)
  - [The 3D viewer and GitHub Pages](#the-3d-viewer-and-github-pages)
  - [Adding another encoder module](#adding-another-encoder-module)

## Repo layout

| Path | What it is |
|---|---|
| `qmk/waveshare_rp2040_zero_volume/` | The QMK keyboard definition: `keyboard.json`, `config.h`, `halconf.h`, `mcuconf.h`, `rules.mk`, the NV3007 driver (`qp_nv3007.c/.h`), fonts in `graphics/` and the keymaps in `keymaps/`. |
| `host/volume_sync.py` | The Linux daemon (Python 3, standard library only) that talks to the knob. |
| `host/70-volume-keeb.rules` | udev rule that lets the logged-in user open the knob's `hidraw` node. |
| `host/volume-sync.service` | systemd user unit that runs the daemon at login. |
| `case/volume_case.scad` | The parametric OpenSCAD case. |
| `case/volume_case-{top,base}.stl` | The two prints, exported from the `.scad`. |
| `case/view/*.stl`, `case/viewer.html` | Stand-ins for the electronics and the WebGL page that shows them inside the case. |
| `case/gen_case.sh`, `case/deploy_pages.sh` | Export + check the case; copy the viewer to GitHub Pages. |

## Firmware

### Why QMK

The board runs QMK, not CircuitPython/KMK. CircuitPython's `usb_hid` has no USB remote-wakeup support ([adafruit/circuitpython#8774](https://github.com/adafruit/circuitpython/issues/8774)), so the knob could never wake a suspended host. Don't move it back to that stack.

### Keyboard definition

QMK only builds keyboards that live inside a `qmk_firmware` checkout. Instead of copying the files there, symlink this repo's folder in, so there is only one copy:

```sh
ln -s "$(pwd)/qmk/waveshare_rp2040_zero_volume" ~/qmk_firmware/keyboards/handwired/onekey/waveshare_rp2040_zero_volume
```

Edit the files through this repo's path so `git status` stays meaningful. Build with:

```sh
qmk compile -kb handwired/onekey/waveshare_rp2040_zero_volume -km volume
```

The board inherits USB VID/PID `0xFEED`/`0x6465` from `handwired/onekey`. `host/volume_sync.py` and `host/70-volume-keeb.rules` both look for that pair; change all three together.

`config.h` sets `USB_SUSPEND_WAKEUP_DELAY 200` to give the host time to notice the remote-wakeup packet.

### The SW pin is the only key

QMK needs at least one matrix key in a `LAYOUT`. The encoder's rotation pins (`GP27`/`GP28`, under `encoder.rotary` in `keyboard.json`) are handled by `quantum/encoder.c` and never count as matrix keys. So the single matrix entry is the encoder's own push switch (`GP29`, a direct pin with internal pull-up), mapped to the custom `ENC_CLICK` keycode. Its meaning depends on the screen (select, back, double-click play/pause, hold-to-mute). That one key is what lets the board live under `handwired/onekey/` (which enforces `LAYOUT_ortho_1x1`).

### Display and the NV3007 driver

The screen is a 2.79" 142x428 NV3007 IPS LCD on SPI0, driven by Quantum Painter.

- RP2040's SPI0 mux fixes the pins: SCK/MOSI must be `GP2`/`GP3` (or `GP6`/`GP7`). MISO isn't wired (the panel is write-only), but the ChibiOS SPI driver needs a valid SPI0 MISO pin, so `config.h` declares `GP16`, which isn't broken out on the RP2040-Zero.
- QMK has no upstream NV3007 driver. `qp_nv3007.c` reuses Quantum Painter's generic `tft_panel`/SPI code (the same path as the ST7789 driver, which is why `rules.mk` lists `st7789_spi` under `QUANTUM_PAINTER_DRIVERS`) and only supplies the controller's init sequence.
- The panel is native portrait (142x428) and is driven rotated with `QP_ROTATION_90`, so the UI is landscape, 428x142. `LCD_PANEL_WIDTH/HEIGHT` are the native size passed to the driver; `LCD_WIDTH/HEIGHT` are the rotated canvas `keymap.c` lays out against. If the UI is upside down, change `QP_ROTATION_90` to `QP_ROTATION_270` in `keymap.c`.
- `BLK` is a plain GPIO (`GP0`, no PWM), active-high on this module. The keymap drives it low to blank the panel after the idle timeout.

Panel knobs in `config.h`:

| Setting | Default | When to touch it |
|---|---|---|
| `LCD_OFFSET_X` / `LCD_OFFSET_Y` | `0` / `13` | The 142 px glass sits inside the controller's 168-wide RAM. If the image is shifted or has a garbage strip on one edge, adjust these. |
| `NV3007_INVERT` | off | Uncomment if colors come out inverted. |
| `LCD_SPI_DIVISOR` | `4` | Raise it if long or noisy wires corrupt the image. |
| `LCD_SPI_MODE` | `3` | SPI mode. |

Earlier revisions used a 1.3" OLED, then ST7789 and ILI9341 panels. Those are gone from `keymaps/volume`; the `diag` keymap still uses the ST7789 constructor.

### Tuning constants

All in `keymaps/volume/keymap.c`:

| Constant | Default | Meaning |
|---|---|---|
| `VOLUME_STEP` | `5` | Percent per encoder detent. |
| `HOLD_MS` | `500` | Click-and-hold time for mute. |
| `DOUBLE_CLICK_MS` | `300` | Window for a double-click (play/pause). |
| `DISPLAY_TIMEOUT_MS` | `60000` | Idle time before the backlight goes off. |
| `HOST_STALE_MS` | `5000` | No packet from the host for this long clears the app list. |
| `SCROLL_STEP_MS` | `350` | Marquee speed for names and titles that don't fit. |
| `MAX_APPS`, `NAME_MAX`, `TITLE_MAX` | `10`, `24`, `87` | Must match `volume_sync.py`. |

### Diagnostic keymaps

For bring-up, when something doesn't work:

- `usbonly` — no display code at all, only turns the backlight pin on. Tells you whether the board enumerates over USB by itself.
- `diag` — brings USB up first, waits 3 s, then cycles the screen through solid red/green/blue/black every 1.5 s. If USB enumerates but the screen never changes, the SPI wiring or pins are wrong; if USB never enumerates, display init is hanging. Note it was written for the earlier ST7789 panel (constructor and viewport offsets), so on the NV3007 treat it as a "does SPI talk at all" test, not a correct image.

```sh
qmk compile -kb handwired/onekey/waveshare_rp2040_zero_volume -km diag
```

### Fonts

`graphics/*.qff.c` are Quantum Painter fonts generated from DejaVu Sans Mono with `qmk painter-make-font-image` + `qmk painter-convert-font-image`. Don't hand-edit them; each file's header comment has the exact command to regenerate it.

## Host daemon

`host/volume_sync.py` is Linux-only. It needs `pactl` (PulseAudio, or PipeWire with `pipewire-pulse`), `busctl` (systemd, for MPRIS) and Python 3.

- Apps come from `pactl list sink-inputs` (one per playing stream), plus a synthetic `Master` entry for the default sink.
- Media titles and play/pause come from MPRIS players over D-Bus (`busctl --user`). A stream and a player are matched by normalized app name (lowercase, non-alphanumerics stripped). It's a heuristic that works for the common cases (browsers, Spotify), not a guarantee.
- It wakes on every `pactl subscribe` event about sinks/server, and at least every `HEARTBEAT_S` (2 s), so the firmware's 5 s stale timer never fires while it runs.
- It finds the knob by scanning `/sys/class/hidraw/*` for VID/PID `FEED:6465` and a report descriptor starting with QMK's Raw HID usage page `0xFF60`.
- Text is reduced to printable ASCII (accents stripped) because the fonts only cover ASCII.

### Raw HID protocol

32-byte reports over QMK Raw HID. The command bytes and layouts are defined in both `volume_sync.py` and `keymap.c`; keep them in sync.

| Direction | Byte 0 | Meaning |
|---|---|---|
| host → knob | `0x60` | Begin an app-list snapshot |
| host → knob | `0x61` | One app entry (id, volume, mute, name, …) |
| host → knob | `0x62` | End of snapshot |
| host → knob | `0x54` (`'T'`) | A chunk of the media title for the app that has one |
| host → knob | `0x63` | Display theme: an index into `keymap.c`'s `themes[]` |
| knob → host | `0x70` | Action for one app id: `0` set volume, `1` toggle mute, `2` play/pause |

### Adding a theme

1. Add an entry to `themes[]` in `keymap.c` and bump `THEME_COUNT`.
2. Add its name and index to `THEMES` in `volume_sync.py`.
3. Rebuild and reflash; run the daemon with `--theme <name>`.

The firmware doesn't store the theme; the daemon sends it on start and every reconnect.

## Case

`case/volume_case.scad` is parametric OpenSCAD, written in the style of [dongle-slayer](https://github.com/joaopedropio/dongle-slayer)'s `case/dongle_case.scad` (branch `case-3d`), which is also where the NV3007 module's numbers come from. The shape follows the MaxMix base (the untracked `maxmix_base_*.stl` in the repo root are only a visual reference).

### How it holds the parts

Every part is mounted on the base; the top only covers it.

- **Screen:** two ribs behind its ends on a 60° face. The right rib takes the module's two holes (M2 x 5 pan-head screws from the front of the board into heat-set inserts); the left rib only props the other end.
- **Encoder:** a frame of four walls under the PCB's edges plus two pillars under its holes, M2 screws from above into inserts. The frame's walls avoid the pins, pads and body lugs that come through the PCB. It puts the knob's underside `knob_gap` (0.5 mm) above the bushing.
- **RP2040-Zero:** upside down (buttons and USB-C toward the floor) on its own USB receptacle and a block under its far end, held by rails and a rigid clip. BOOT and RESET are pressed with a pin through holes in the bottom, marked `B` and `R`.
- **Knob:** goes through a plain hole with no floor, `knob_sink` (7 mm) deep, so the top lifts straight off over it.
- **Top:** four M2 x 6 flat-head (countersunk) screws from below the base into inserts in the top's bosses.
- **Inserts:** M2, 3 mm long, 3.5 mm across, in 3.2 mm holes with room for the screw tip past them.

The parameters at the top of the file (`knob_d`, `knob_h`, `knob_sink`, `wall`, `angle`, `depth`, `clr`, `fit`, `insert_d`, …) are the ones meant to be changed.

### Regenerating and checking

```sh
case/gen_case.sh   # needs openscad (2021+) and python3
```

It exports `volume_case-{top,base}.stl` and the stand-ins in `case/view/`, then runs two checks and fails unless both come out empty (touching faces are allowed):

- `part="check"` — the case parts intersected with the electronics' stand-ins.
- `part="insert"` — the top intersected with everything it meets on its way down over the base.

To see an overlap: `openscad -D 'part="check"' case/volume_case.scad`. `part="view"` (the default) shows everything assembled.

### The 3D viewer and GitHub Pages

`case/viewer.html` is a plain WebGL page (adapted from dongle-slayer's `web/js/case3d.js`) with a checkbox per part and a slider that lifts the top. Browsers won't load the STLs from `file://`, so serve it:

```sh
python3 -m http.server -d case 8000   # http://localhost:8000/viewer.html
```

The live copy is at https://joaopedropio.github.io/volume-keeb/. This repo holds the canonical page; `case/deploy_pages.sh [site checkout]` copies it and the STLs into `~/joaopedropio.github.io/volume-keeb/`. After changing the model or the page, run `gen_case.sh`, then `deploy_pages.sh`, and commit and push both repos.

### Adding another encoder module

The encoder is chosen with `encoder` in the `.scad`. Only `"hw040"` is measured (from `hw-040-encoder-module.stp` and `hw-040-module-dimensions.webp`). `"ky040"` deliberately fails an `assert` until real measurements are added to `enc_dims()`: PCB size and thickness, hole positions and diameter, shaft position, body size and height, bushing and shaft diameter and height, frame wall thicknesses and notches. Measure a real module; don't guess.
