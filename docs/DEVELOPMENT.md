# volume-keeb: development notes

Everything you need to change the firmware, the host app or the case. If you only want to build one, read the [README](../README.md) instead.

## Contents

- [Repo layout](#repo-layout)
- [Firmware](#firmware)
  - [Why QMK](#why-qmk)
  - [Building the firmware](#building-the-firmware)
  - [Keyboard definition](#keyboard-definition)
  - [The SW pin is the only key](#the-sw-pin-is-the-only-key)
  - [Display and the NV3007 driver](#display-and-the-nv3007-driver)
  - [Tuning constants](#tuning-constants)
  - [Diagnostic keymaps](#diagnostic-keymaps)
  - [Fonts](#fonts)
  - [Fonts uploaded from the host](#fonts-uploaded-from-the-host)
- [Host app](#host-app)
  - [Building the Windows app](#building-the-windows-app)
  - [Raw HID protocol](#raw-hid-protocol)
  - [Adding a theme](#adding-a-theme)
- [Case](#case)
  - [How it holds the parts](#how-it-holds-the-parts)
  - [Regenerating and checking](#regenerating-and-checking)
  - [The 3D viewer and GitHub Pages](#the-3d-viewer-and-github-pages)
  - [Adding another encoder module](#adding-another-encoder-module)
- [Releases](#releases)

## Repo layout

| Path | What it is |
|---|---|
| `qmk/waveshare_rp2040_zero_volume/` | The QMK keyboard definition: `keyboard.json`, `config.h`, `halconf.h`, `mcuconf.h`, `rules.mk`, the NV3007 driver (`qp_nv3007.c/.h`), fonts in `graphics/` and the keymaps in `keymaps/`. |
| `host/volume_sync.py`, `host/volume_keeb/` | The host app (Python 3; Linux and Windows) that talks to the knob and serves the settings page. |
| `host/70-volume-keeb.rules` | udev rule that lets the logged-in user open the knob's `hidraw` node. |
| `host/volume-sync.service` | systemd user unit that runs the daemon at login. |
| `case/volume_case.scad` | The parametric OpenSCAD case. |
| `case/volume_case-{top,base}.stl` | The two prints, exported from the `.scad`. |
| `case/view/*.stl`, `case/viewer.html` | Stand-ins for the electronics and the WebGL page that shows them inside the case. |
| `case/gen_case.sh`, `case/deploy_pages.sh` | Export + check the case; copy the viewer to GitHub Pages. |
| `flash.sh` | Build + flash without touching the buttons (Linux; see [Building the firmware](#building-the-firmware)). |
| `.github/workflows/release.yml` | Builds the `.uf2` and the `.exe` and publishes [releases](#releases). |
| `docs/wiring.svg`, `docs/gen_wiring.py` | The README's wiring picture and the script that draws it (to scale, from the `.scad`'s numbers; rerun it after changing pins). |

## Firmware

### Why QMK

The board runs QMK, not CircuitPython/KMK. CircuitPython's `usb_hid` has no USB remote-wakeup support ([adafruit/circuitpython#8774](https://github.com/adafruit/circuitpython/issues/8774)), so the knob could never wake a suspended host. Don't move it back to that stack.

### Building the firmware

Install the toolchain.

**Ubuntu / Debian:**

```sh
sudo apt install -y git python3-pip pipx gcc-arm-none-eabi libnewlib-arm-none-eabi binutils-arm-none-eabi
pipx install qmk
pipx ensurepath   # then open a new terminal
```

**Arch:**

```sh
sudo pacman -S --needed git python-pip arm-none-eabi-gcc arm-none-eabi-newlib arm-none-eabi-binutils
pip install --user qmk
```

**macOS:** `brew install qmk/qmk/qmk` installs the CLI and the toolchain.

Get QMK (the tag the [release workflow](#releases) uses, `QMK_REF`, is the one known to work) and this repo:

```sh
git clone --depth 1 --branch 0.34.4 --recurse-submodules --shallow-submodules https://github.com/qmk/qmk_firmware.git ~/qmk_firmware
qmk config user.qmk_home=$HOME/qmk_firmware
git clone https://github.com/joaopedropio/volume-keeb.git ~/volume-keeb
```

QMK only builds keyboards that live inside a `qmk_firmware` checkout. Instead of copying the files there, symlink this repo's folder in, so there is only one copy:

```sh
ln -s ~/volume-keeb/qmk/waveshare_rp2040_zero_volume ~/qmk_firmware/keyboards/handwired/onekey/waveshare_rp2040_zero_volume
```

Edit the files through this repo's path so `git status` stays meaningful. Build with:

```sh
cd ~/qmk_firmware
qmk compile -kb handwired/onekey/waveshare_rp2040_zero_volume -km volume
```

That writes `handwired_onekey_waveshare_rp2040_zero_volume_volume.uf2` to `~/qmk_firmware`. On Linux, `./flash.sh` does the build and the flash in one go: it compiles, asks the running knob to reboot into BOOTSEL (`volume_sync.py --bootloader`), waits for `RPI-RP2` and copies the file. `./flash.sh --no-build` flashes the last build; `./flash.sh some.uf2` flashes a given file. With older firmware (no `0x64` command), press BOOT+RESET by hand and it still copies once the drive appears.

### Keyboard definition

The board inherits USB VID/PID `0xFEED`/`0x6465` from `handwired/onekey`. `host/volume_keeb/protocol.py` and `host/70-volume-keeb.rules` both look for that pair; change all three together.

`config.h` sets `USB_SUSPEND_WAKEUP_DELAY 200` to give the host time to notice the remote-wakeup packet.

### The SW pin is the only key

QMK needs at least one matrix key in a `LAYOUT`. The encoder's rotation pins (`GP27`/`GP28`, under `encoder.rotary` in `keyboard.json`) are handled by `quantum/encoder.c` and never count as matrix keys. So the single matrix entry is the encoder's own push switch (`GP29`, a direct pin with internal pull-up), mapped to the custom `ENC_CLICK` keycode. Its meaning depends on the screen (select, back, double-click play/pause, hold-to-mute). That one key is what lets the board live under `handwired/onekey/` (which enforces `LAYOUT_ortho_1x1`).

### Display and the NV3007 driver

The screen is a 2.79" 142x428 NV3007 IPS LCD on SPI0, driven by Quantum Painter.

- RP2040's SPI0 mux fixes the pins: SCK/MOSI must be `GP2`/`GP3` (or `GP6`/`GP7`). MISO isn't wired (the panel is write-only), but the ChibiOS SPI driver needs a valid SPI0 MISO pin, so `config.h` declares `GP16`, which isn't broken out on the RP2040-Zero.
- QMK has no upstream NV3007 driver. `qp_nv3007.c` reuses Quantum Painter's generic `tft_panel`/SPI code (the same path as the ST7789 driver, which is why `rules.mk` lists `st7789_spi` under `QUANTUM_PAINTER_DRIVERS`, next to `surface` for the title ticker) and only supplies the controller's init sequence.
- The panel is native portrait (142x428) and is driven rotated with `QP_ROTATION_90`, so the UI is landscape, 428x142. `LCD_PANEL_WIDTH/HEIGHT` are the native size passed to the driver; `LCD_WIDTH/HEIGHT` are the rotated canvas `keymap.c` lays out against. If the UI is upside down, change `QP_ROTATION_90` to `QP_ROTATION_270` in `keymap.c`.
- The keymap never draws on the panel directly: it draws into a full-screen RGB565 framebuffer (428x142, 121 KB of RAM) and, once per loop, sends only the rectangle of pixels that changed (`qp_surface_draw`). Drawing straight to the panel showed every clear-then-redraw as a flash of the whole screen. `SURFACE_NUM_DEVICES` is 2 in `config.h`: this framebuffer and the title ticker's.
- `BLK` is a plain GPIO (`GP0`, no PWM), active-high on this module. The keymap drives it low to blank the panel while the host is suspended (`suspend_power_down_user()`).

Panel knobs in `config.h`:

| Setting | Default | When to touch it |
|---|---|---|
| `LCD_OFFSET_X` / `LCD_OFFSET_Y` | `0` / `14` | The 142 px glass sits inside the controller's 168-wide RAM. If the image is shifted or has a garbage strip on one edge, adjust these. |
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
| `SAVER_AFTER_MS_DEFAULT` | `30000` | Time without knob input (host updates don't count; a host wakeup restarts it) before the screensaver starts, until a host sends its own setting with `SCREEN_CMD`. The settings page also picks the screensaver (`saver_t`: none, drive, stars, rain, plasma, bounce) and a time after which the backlight goes off (default never). Every `SAVER_FRAME_MS` the drive, stars, rain and plasma draw straight into the framebuffer and send it to the panel (the drive only from the horizon or the tallest palm down); the bounce moves its text through the framebuffer like the UI does. All but the drive take their colors from the theme. The first turn or click after a screensaver or a dark screen only wakes it. Quantum Painter's own timeout is disabled in `config.h` (`QUANTUM_PAINTER_DISPLAY_TIMEOUT 0`): it sends DISPOFF, which turns the lit NV3007 white and fades it out slowly, so the screen goes dark by painting black and dropping the backlight. |
| `HOST_STALE_MS` | `5000` | No packet from the host for this long clears the app list. |
| `SCROLL_STEP_MS` | `350` | Marquee speed for app names that don't fit. |
| `TICKER_STEP_PX`, `TICKER_STEP_MS` | `2`, `30` | The song title's speed across the screen, right to left. It's drawn into an RGB565 surface a glyph wider than the row on each side (so glyphs at the edges are cut, not dropped), and only the visible middle is sent to the panel. |
| `MAX_APPS`, `NAME_MAX`, `TITLE_MAX` | `10`, `24`, `87` | Must match `host/volume_keeb/protocol.py`. |

### Diagnostic keymaps

For bring-up, when something doesn't work:

- `usbonly` — no display code at all, only turns the backlight pin on. Tells you whether the board enumerates over USB by itself.
- `diag` — brings USB up first, waits 3 s, then cycles the screen through solid red/green/blue/black every 1.5 s. If USB enumerates but the screen never changes, the SPI wiring or pins are wrong; if USB never enumerates, display init is hanging. Note it was written for the earlier ST7789 panel (constructor and viewport offsets), so on the NV3007 treat it as a "does SPI talk at all" test, not a correct image.

```sh
qmk compile -kb handwired/onekey/waveshare_rp2040_zero_volume -km diag
```

### Fonts

`graphics/*.qff.c` are Quantum Painter fonts generated from DejaVu Sans Mono with `qmk painter-make-font-image` + `qmk painter-convert-font-image`. Don't hand-edit them; each file's header comment has the `painter-convert-font-image` arguments (`-f mono2`). The `--size` given to `painter-make-font-image` (with `--font /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf`) is 11 for `font_ui` (13 px tall, header and labels) and 22 for `font_list` (24 px, the menu's app list, sized so 4 rows fill the screen).

### Fonts uploaded from the host

The settings page can replace each of the three fonts. The host converts the font to QFF (`host/volume_keeb/qff.py`, a from-scratch version of QMK's font tools, so the host doesn't need the QMK CLI) and uploads it into a slot of `keymaps/volume/font_store.c`:

- The slots are 64 kB each, from 1 MB into the 2 MB flash (`FONT_STORE_*` in `config.h`): far past the firmware and clear of the wear-leveled EEPROM at the end. A UF2 only rewrites the blocks it carries, so re-flashing keeps them.
- Each slot is a header page (magic, length, CRC-32) and then the QFF. The header is written last, after the data checked out against the host's CRC, so an interrupted upload reads as an empty slot.
- Erasing and programming use pico-sdk's `flash_range_*` (they run from RAM) with interrupts off, one 4 kB sector or 256-byte page at a time, so each pause is short. The USB hardware NAKs the host in the meantime, which is all the flow control the upload needs.
- The layout is fixed, so the keymap only takes an uploaded font whose line height equals the slot's (13, 27, 24 px; it reads byte 17 of the QFF); `qff.py` renders the font at the biggest size that fits, and caps glyphs at 32 px wide (`TICKER_PAD`). Any other font falls back to the built-in one.
- `font_big` used to be assumed monospaced by the title ticker; it now keeps a per-glyph width table (`big_glyph_w`), refreshed whenever the font changes.

## Host app

`host/volume_sync.py` starts `host/volume_keeb/`, which runs on Linux and Windows:

| Module | Does |
|---|---|
| `app.py` | Command line, startup, single instance (a second start opens the running one's page) |
| `engine.py` | The threads: one reads the knob (and finds it again after an unplug), one collects the app list and applies the knob's actions; pushes the look and syncs the fonts on every (re)connect |
| `knob.py` | Raw HID: `/dev/hidraw` with the standard library on Linux, hidapi elsewhere |
| `audio_linux.py` | `pactl` + MPRIS (`busctl`) |
| `audio_windows.py` | WASAPI audio sessions (pycaw) + the system media transport controls (pywinrt) |
| `web.py`, `web/index.html` | The settings page and its JSON API, on `127.0.0.1:8137` |
| `qff.py` | TTF/OTF → QFF, and the page's font previews |
| `config.py` | `config.json` and the fonts, in `~/.config/volume-keeb` or `%APPDATA%\volume-keeb` |
| `tray.py`, `autostart.py` | The tray icon (pystray) and Windows' "Start with Windows" (the HKCU `Run` key) |
| `updates.py`, `version.py` | Checks GitHub's latest release every 6 h; puts its `.uf2` on the knob (`BOOTLOADER_CMD`, wait for the `RPI-RP2` drive, copy, wait for the knob to reconnect). The app's own version is the repo's `VERSION` |

- **Linux**: apps come from `pactl list sink-inputs` (one per playing stream), plus a synthetic `Master` for the default sink. It wakes on every `pactl subscribe` event about sinks/server.
- **Windows**: apps are the processes with an audio session (all sessions of one process are one app), plus `Master` for the default output's endpoint volume. Nothing announces changes, so it polls every 0.5 s. COM is used in the multithreaded apartment, which pywinrt needs too, and only from the sync thread. **Untested on real hardware so far.**
- Media titles and play/pause: an MPRIS player (Linux) or a media session (Windows) is matched to an app by normalized name (lowercase, non-alphanumerics stripped; on Windows also `.exe` stripped, a substring match, and a few aliases such as Firefox's app id). A heuristic that covers browsers and Spotify, not a guarantee.
- The app list is resent when it changes and at least every `HEARTBEAT_S` (2 s), so the firmware's 5 s stale timer never fires while it runs.
- The knob is found by VID/PID `FEED:6465` and QMK's Raw HID usage page `0xFF60`.
- Text is reduced to printable ASCII (accents stripped) because the fonts only cover ASCII.
- The page only answers requests addressed to `localhost`, and requests that change something need an `X-Volume-Keeb` header, so other websites can't change the knob from the browser.

To try the firmware against uploaded fonts without flashing, convert them by hand and point the UI simulator at them:

```sh
cd host && python3 -m volume_keeb.qff SomeFont.ttf title /tmp/fonts/slot1.qff mono16   # slots: ui=0, title=1, list=2
UI_SIM_FONTS=/tmp/fonts python3 ../tools/ui_sim/run.py /tmp/sim
```

### Building the Windows app

`volume-keeb.exe` is a one-file PyInstaller build. The [release workflow](#releases) makes it; to build it on a Windows machine, with Python 3.10+ on `PATH`, run from `host\`:

```powershell
powershell -ExecutionPolicy Bypass -File build_windows.ps1
```

It lands in `host\dist\volume-keeb.exe`. To run from source instead: `pip install -r requirements.txt`, then `python volume_sync.py`.

### Raw HID protocol

32-byte reports over QMK Raw HID. The command bytes and layouts are defined in both `host/volume_keeb/protocol.py` and `keymap.c` (whose comment above the `*_CMD` defines has the byte layout of each); keep them in sync.

| Direction | Byte 0 | Meaning |
|---|---|---|
| host → knob | `0x60` | Begin an app-list snapshot |
| host → knob | `0x61` | One app entry (id, volume, mute, name, …) |
| host → knob | `0x62` | End of snapshot |
| host → knob | `0x54` (`'T'`) | A chunk of the media title for the app that has one |
| host → knob | `0x63` | Preset theme: an index into `keymap.c`'s `themes[]` |
| host → knob | `0x64` | Reboot into the BOOTSEL bootloader (`volume_sync.py --bootloader`, used by `flash.sh`) |
| host → knob | `0x65` | Palette: six H,S,V colors and the decor style |
| host → knob | `0x66`–`0x68` | Font upload: begin (slot, length, CRC; erases), data (offset + up to 26 bytes), end (checks, loads) |
| host → knob | `0x69` | Clear a font slot (back to the built-in font) |
| host → knob | `0x6A` | Ask which fonts the slots hold |
| host → knob | `0x6C` | Ask the firmware's version |
| host → knob | `0x6B` | Screen: screensaver kind, seconds before it starts, seconds before the screen turns off (0 = never), and a flag to start the screensaver now (the page's **Show on the knob**) |
| knob → host | `0x70` | Action for one app id: `0` set volume, `1` toggle mute, `2` play/pause |
| knob → host | `0x71` | Result of a font begin/end/clear |
| knob → host | `0x72` | Each slot's font CRC (0 = built-in) |
| knob → host | `0x73` | Firmware version: major, minor, patch. The v0.1.0 firmware doesn't answer `0x6C`, so silence means v0.1.0 |

### Adding a theme

1. Add an entry to `themes[]` in `keymap.c` and bump `THEME_COUNT`.
2. Add it to `PRESETS` and `THEME_IDS` in `host/volume_keeb/protocol.py`.
3. Rebuild and reflash; it shows up on the settings page.

The firmware doesn't store the look; the host sends it on start, on every reconnect and on every change on the page. Fonts are the exception: they live in the knob's flash, and the host only re-sends one whose CRC differs from what it has saved.

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

## Releases

`.github/workflows/release.yml` builds `volume-keeb.uf2` (in QMK's `qmk_cli` container, against the `qmk_firmware` tag in `QMK_REF`) and `volume-keeb.exe` (on a Windows runner, with `build_windows.ps1`). Pushing a `v*` tag also publishes a GitHub release with both files and the case's two STLs.

The version lives in `VERSION` at the repo root. The firmware build reads it (the keymap's `rules.mk` turns it into `FW_VERSION_*`, which the knob reports), so does the host app (`version.py`; `build_windows.ps1` bundles the file), and the workflow refuses a tag that doesn't match it. To release, bump it, commit, then tag:

```sh
echo 1.2.0 > VERSION && git commit -am "v1.2.0"
git tag v1.2.0
git push origin main v1.2.0
```

Installed apps notice the new release within 6 hours and offer it on the settings page (see the README's Updates section).

The README links to `releases/latest/download/<file>`, so keep those file names. Running the workflow by hand from the Actions tab builds the same files as workflow artifacts without publishing anything. When moving to a newer QMK, bump `QMK_REF` and the clone command in [Building the firmware](#building-the-firmware) together.
