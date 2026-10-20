# volume-keeb 🎚️

A desk knob that works as a **per-app volume mixer**. Turn it to change the volume of Spotify, your browser, a game or the whole system. Click to pick an app, hold to mute, double-click to play/pause. A 2.79" bar-shaped LCD shows what's playing.

It's built from cheap parts (a Waveshare RP2040-Zero, an HW-040 rotary encoder and a 2.79" NV3007 LCD) in a 3D-printed case, and runs [QMK](https://qmk.fm). A small Linux daemon tells the knob which apps are playing and applies what you do on it.

**See the case in 3D: https://joaopedropio.github.io/volume-keeb/**

This README is a step-by-step guide to building your own. If you want to change the firmware, the daemon or the case, see [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Contents

- [Features](#features-)
- [Requirements](#requirements-)
- [Bill of materials](#bill-of-materials-)
- [Tools](#tools-)
- [Step 1: Print the case](#step-1-print-the-case-)
- [Step 2: Build the firmware](#step-2-build-the-firmware-)
- [Step 3: Flash the board](#step-3-flash-the-board-)
- [Step 4: Wire it up](#step-4-wire-it-up-)
- [Step 5: Test before closing](#step-5-test-before-closing-)
- [Step 6: Assemble the case](#step-6-assemble-the-case-)
- [Step 7: Set up the computer](#step-7-set-up-the-computer-)
- [Using it](#using-it-)
- [Themes](#themes-)
- [Troubleshooting](#troubleshooting-)
- [Credits](#credits-)

## Features [🔼](#contents)

- **Per-app volume**: every app that is playing sound gets its own entry, plus `Master` for the system's default output.
- **Media info**: the title of what's playing (from MPRIS: Spotify, browsers, most media players) shown large on the screen.
- **Play/pause and mute** from the knob.
- **Three themes**: `terminal` (green phosphor), `neobrutal` (cream, black and orange, hard shadows) and `winamp` (the classic Winamp skin).
- **Screen saver**: the screen turns off after 60 s without activity and wakes on the knob or on any change on the computer.
- **Wakes the computer** from suspend.
- **No drivers**: it's a plain USB HID device. The daemon uses only Python's standard library.

## Requirements [🔼](#contents)

> [!IMPORTANT]
> The app mixer needs **Linux** with PulseAudio, or PipeWire with `pipewire-pulse` (the default on Ubuntu, Fedora, Arch and most current distros), plus systemd. Windows and macOS aren't supported by the daemon yet.

You also need:

- Access to a 3D printer (or a printing service).
- A soldering iron. There are no connectors or pin headers: every wire is soldered straight to the pads.
- A Linux machine to build the firmware (macOS works too for building; see [Step 2](#step-2-build-the-firmware-)).

## Bill of materials [🔼](#contents)

| Part | Qty | Notes |
|---|---|---|
| Waveshare **RP2040-Zero** | 1 | The version **without** pin headers. Clones work if they have the same size and pinout (the case is drawn around Waveshare's board). |
| **HW-040** rotary encoder module | 1 | The 5-pin module (`CLK`, `DT`, `SW`, `+`, `GND`) with a 6 mm shaft. A KY-040 is electrically identical, but the case is only drawn for the HW-040 so far. |
| **2.79" 142x428 NV3007 SPI IPS LCD** module | 1 | 78 x 25.9 mm bar-shaped module, 8 pads (`GND VCC SCL SDA RST DC CS BLK`). Search for "2.79 inch LCD NV3007 142x428". |
| Metal knob, **48 mm diameter, 22 mm tall**, for a 6 mm shaft | 1 | The case's hole is sized for it (`knob_d`/`knob_h` in the `.scad` if yours is different). About 8.5 mm of shaft goes into it, so it needs a bore at least ~9 mm deep. |
| M2 heat-set brass insert, **3 mm long, 3.5 mm outer diameter** | 8 | 4 for the top, 2 for the screen, 2 for the encoder. |
| M2 x 5 pan-head screw | 4 | 2 for the screen, 2 for the encoder. |
| M2 x 6 flat-head (countersunk) screw | 4 | Close the case from below. |
| Thin silicone wire (28–30 AWG) | ~1 m | 13 wires in total. Several colors help a lot. |
| USB-C cable | 1 | Data cable, not charge-only. |
| Printed case: `volume_case-top.stl` + `volume_case-base.stl` | 1 set | See [Step 1](#step-1-print-the-case-). |

## Tools [🔼](#contents)

- Soldering iron, solder, flux.
- A heat-set insert tip for the soldering iron (or a regular tip you don't care about).
- Wire stripper and flush cutters.
- Small Phillips screwdriver for M2.
- A pin or straightened paper clip, to press BOOT/RESET through the bottom.
- Optional: a multimeter to check for shorts, Kapton tape, tweezers.

## Step 1: Print the case [🔼](#contents)

Download the two STLs from the [`case/`](case/) folder:

- [`volume_case-top.stl`](case/volume_case-top.stl): print with the **open side down**.
- [`volume_case-base.stl`](case/volume_case-base.stl): print **flat**.

The assembled case is about 89 x 84 x 32 mm. Use your slicer's preview to decide on supports for your printer; PLA or PETG are fine.

Before printing, open the [3D view](https://joaopedropio.github.io/volume-keeb/) to see where each part goes. Tick the parts on and off and use the slider to lift the top. It's the best map you'll have during assembly.

> [!TIP]
> If your knob isn't 48 x 22 mm, or you want to change anything else, edit the variables at the top of [`case/volume_case.scad`](case/volume_case.scad) in [OpenSCAD](https://openscad.org) and run `case/gen_case.sh`. See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#case).

### Press in the heat-set inserts

Do this **now**, before any electronics go in. It's much easier on an empty part.

1. **Top (4):** the four bosses inside the top, near the corners.
2. **Screen rib (2):** the right-hand rib behind the sloped face. These go in from the **front** (the side the screen will rest on).
3. **Encoder pillars (2):** the two round pillars inside the rectangular frame on the base.

Heat the insert with the iron and push it straight in until it's flush. Let it cool before touching it.

## Step 2: Build the firmware [🔼](#contents)

> [!NOTE]
> Flash the board and test each part as you wire it. If something is wrong, it's much easier to fix before the case is closed.

### Install the toolchain

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

### Get QMK and this repo

```sh
git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/qmk/qmk_firmware.git ~/qmk_firmware
qmk config user.qmk_home=$HOME/qmk_firmware

git clone https://github.com/joaopedropio/volume-keeb.git ~/volume-keeb
```

Clone this repo to `~/volume-keeb`: the systemd unit in [Step 7](#step-7-set-up-the-computer-) expects it there.

QMK only builds keyboards that live inside its own folder, so link this repo's keyboard definition into it:

```sh
ln -s ~/volume-keeb/qmk/waveshare_rp2040_zero_volume ~/qmk_firmware/keyboards/handwired/onekey/waveshare_rp2040_zero_volume
```

### Compile

```sh
cd ~/qmk_firmware
qmk compile -kb handwired/onekey/waveshare_rp2040_zero_volume -km volume
```

This produces `handwired_onekey_waveshare_rp2040_zero_volume_volume.uf2` in `~/qmk_firmware`.

## Step 3: Flash the board [🔼](#contents)

The RP2040 has a built-in bootloader that shows up as a USB drive, so flashing is just copying a file.

1. Unplug the board.
2. Hold the **BOOT** button and plug it in. Release once a drive named `RPI-RP2` appears.
3. Copy the `.uf2` onto that drive:

   ```sh
   cp ~/qmk_firmware/handwired_onekey_waveshare_rp2040_zero_volume_volume.uf2 /media/$USER/RPI-RP2/      # Ubuntu
   cp ~/qmk_firmware/handwired_onekey_waveshare_rp2040_zero_volume_volume.uf2 /run/media/$USER/RPI-RP2/  # Arch
   cp ~/qmk_firmware/handwired_onekey_waveshare_rp2040_zero_volume_volume.uf2 /Volumes/RPI-RP2/          # macOS
   ```

   Or just drag it in with your file manager.
4. The board reboots by itself into the new firmware and the drive disappears.

Once it's in the case, BOOT is pressed with a pin through the hole marked **`B`** under the case (**`R`** is RESET). To re-flash later: hold B, tap R, release B.

## Step 4: Wire it up [🔼](#contents)

### Pinout

**Encoder (HW-040):**

| Encoder pin | RP2040-Zero pin |
|---|---|
| `CLK` | `GP27` |
| `DT` | `GP28` |
| `SW` | `GP29` |
| `+` | `3V3` |
| `GND` | `GND` |

**Screen (NV3007):**

| LCD pin | RP2040-Zero pin |
|---|---|
| `VCC` | `3V3` |
| `GND` | `GND` |
| `SCL` (SCK) | `GP2` |
| `SDA` (MOSI) | `GP3` |
| `CS` | `GP1` |
| `DC` | `GP4` |
| `RST` | `GP5` |
| `BLK` | `GP0` |

> [!CAUTION]
> Power the encoder and the screen from **3V3**, not 5V. The RP2040's pins are not 5V tolerant.

> [!NOTE]
> The RP2040-Zero has one `3V3` pad and one `GND` pad, but both the encoder and the screen need them. Join each pair of wires into one (twist and tin them together, or splice them with heat-shrink) and solder that to the pad.

```
                 ┌──────────────── RP2040-Zero ────────────────┐
  HW-040                                                    NV3007 LCD
  CLK ───────────── GP27                 GP0 ──────────────── BLK
  DT  ───────────── GP28                 GP1 ──────────────── CS
  SW  ───────────── GP29                 GP2 ──────────────── SCL
  +   ──────┬────── 3V3                  GP3 ──────────────── SDA
  GND ───┬──┼────── GND                  GP4 ──────────────── DC
         │  │                            GP5 ──────────────── RST
         │  └──────────────────────────────────────────────── VCC
         └─────────────────────────────────────────────────── GND
```

### Wire lengths and routing

Use the [3D view](https://joaopedropio.github.io/volume-keeb/) to see where each module sits: the screen at the front on the sloped face, the encoder under the knob in the middle, the RP2040-Zero at the back (upside down, USB-C out of the back wall).

- Leave enough slack to lift each module out of its seat while the others are mounted, but not so much that the bundle won't fit under the top. Lay the parts out in the empty base, measure, and add a couple of centimeters.
- **Encoder:** solder its wires from the **top** of the PCB (the side with the shaft). The frame's wall sits right beside the pads underneath, so wires coming out of the bottom won't fit.
- **Screen:** the 8 pads are on the left end of the module (seen from the front), the opposite end from the two mounting holes. Solder the wires on the back and keep the joints low.
- **RP2040-Zero:** it lies **upside down** in the case, so the wires leave from what will be its top once mounted. Solder to the pads on the board's edge without bending the wires sharply.

## Step 5: Test before closing [🔼](#contents)

Before anything is screwed down, plug the board in:

1. The screen lights up and shows the UI. With no daemon running yet there are no apps to pick.
2. Do [Step 7](#step-7-set-up-the-computer-) on your computer now (or just run `~/volume-keeb/host/volume_sync.py` in a terminal after installing the udev rule). `Master` and any app playing sound appear on the screen.
3. Turning the knob moves the selection; clicking opens the app; turning changes its volume on the computer.

If something doesn't work, check [Troubleshooting](#troubleshooting-) now, while every joint is still reachable.

## Step 6: Assemble the case [🔼](#contents)

Everything mounts on the **base**. The top is only a cover that goes on last.

1. **Screen.** Lay the module face-down on the sloped face, glass toward the window. Its right end (the one with the two holes) goes on the rib with the inserts: fix it with two **M2 x 5** pan-head screws from the front of the board. The left end just rests on the other rib.
2. **Encoder.** Sit the PCB on the rectangular frame, shaft up, and screw it to the two pillars with two **M2 x 5** pan-head screws from above.
3. **RP2040-Zero.** Turn it **upside down** (buttons and USB-C facing the floor). The clip is rigid, so put it in **tilted**: far edge under the clip's hook first, then lower the USB end down until the USB-C sits in the hole in the back wall.
4. **Tuck the wires** so none sits where the top's walls or the four corner bosses will come down.
5. **Top.** Lower the top straight down over everything, the encoder's shaft through the round hole.
6. **Screws.** Turn it over and drive four **M2 x 6** flat-head screws through the base into the top's inserts.
7. **Knob.** Push the knob onto the shaft. It sits 7 mm into the hole and floats just above the encoder; it shouldn't rub on anything.

## Step 7: Set up the computer [🔼](#contents)

The knob only shows apps while `volume_sync.py` runs on the computer. It needs `pactl`, `busctl` (both installed by default on most systemd + PipeWire/PulseAudio desktops) and Python 3. On a minimal system, `pactl` is in `pulseaudio-utils` (Debian/Ubuntu) or `libpulse` (Arch).

**1. Allow your user to talk to the knob:**

```sh
sudo cp ~/volume-keeb/host/70-volume-keeb.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```

**2. Run the daemon at login:**

```sh
mkdir -p ~/.config/systemd/user
cp ~/volume-keeb/host/volume-sync.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now volume-sync
```

The unit runs `~/volume-keeb/host/volume_sync.py`. If you cloned the repo somewhere else, edit `ExecStart` in `~/.config/systemd/user/volume-sync.service`.

Check it's running with `systemctl --user status volume-sync` and its log with `journalctl --user -u volume-sync -f`.

## Using it [🔼](#contents)

The knob has two screens:

- **Menu**: a horizontal row of apps: `Master` (your default output) plus every app that's playing sound right now. A small dot marks the app the App screen would go back to.
- **App**: the app's name and volume (or `MUTE`) at the top, the media title (if any) in large text, and a 30-segment volume bar along the bottom.

| Action | Menu | App |
|---|---|---|
| Turn | Move the selection | Change that app's volume (5% per step) |
| Click | Open the selected app | Back to the menu |
| Double-click | — | Play/pause (apps with media controls) |
| Click and hold | — | Mute / unmute |

Long names and titles scroll. If the daemon stops (or the computer sleeps), the app list clears after 5 s and the knob goes back to the menu.

## Themes [🔼](#contents)

| Theme | Look |
|---|---|
| `terminal` (default) | Flat green-phosphor terminal. |
| `neobrutal` | Cream, black and orange, thick borders and hard drop shadows, after [this Figma concept](https://www.figma.com/make/t8KsdhbALpn6rPHxM7KQUm/Volume-Controller-UI). |
| `winamp` | Grey-blue chassis with sunken black "LCD" wells, green text and an amber accent, after the classic Winamp skin. Menu items look like toolbar buttons that press in when selected. |

The theme is set by the daemon, so it's chosen on the computer:

```sh
sed -i 's|volume_sync.py$|volume_sync.py --theme winamp|' ~/.config/systemd/user/volume-sync.service
systemctl --user daemon-reload
systemctl --user restart volume-sync
```

## Troubleshooting [🔼](#contents)

| Symptom | What to check |
|---|---|
| No `RPI-RP2` drive appears | Use a data USB-C cable (not charge-only). Hold BOOT *before* plugging in. |
| Screen stays black | `VCC`/`GND` on 3V3/GND; `BLK` on `GP0`. Wait a minute: the screen also turns off after 60 s idle, so turn the knob. |
| Screen lights up but shows garbage or nothing | `SCL`→`GP2` and `SDA`→`GP3` (SPI only works on those pins), `CS`/`DC`/`RST` on `GP1`/`GP4`/`GP5`. Shorter wires help. |
| Image shifted, or a noisy strip on one edge | Adjust `LCD_OFFSET_X`/`LCD_OFFSET_Y` in `qmk/.../config.h` and rebuild. |
| Colors inverted | Uncomment `NV3007_INVERT` in `config.h` and rebuild. |
| UI upside down | Change `QP_ROTATION_90` to `QP_ROTATION_270` in `keymaps/volume/keymap.c` and rebuild. |
| Knob turns the wrong way | Swap the `CLK` and `DT` wires (or `pin_a`/`pin_b` in `keyboard.json`). |
| Turning skips or jumps | Check the `CLK`/`DT` joints; the HW-040's pull-ups need `+` on 3V3. |
| Click does nothing | `SW` on `GP29`. |
| Screen works but no apps appear | Is the daemon running (`systemctl --user status volume-sync`)? Did you install the udev rule and replug? `journalctl --user -u volume-sync` shows errors. |
| An app plays but isn't listed | Only apps with an active stream appear. Check `pactl list sink-inputs`. |
| No title or play/pause for an app | The app needs MPRIS support, and its name must roughly match its audio stream's name. Check `busctl --user list \| grep mpris`. |

Still stuck? There are two bring-up keymaps (`usbonly` and `diag`) described in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#diagnostic-keymaps). Open an issue with what you see.

## Credits [🔼](#contents)

- [MaxMix](https://github.com/rubenhenares/MaxMix): the shape of the case.
- [dongle-slayer](https://github.com/joaopedropio/dongle-slayer): the NV3007 module's dimensions and the case and 3D-viewer style.
- [single-keeyb](https://github.com/joaopedropio/single-keeyb): the sibling macropad this project started from.
- [snake-dongle](https://github.com/joaopedropio/snake-dongle): this README's format.
- [QMK](https://qmk.fm) and its Quantum Painter.
