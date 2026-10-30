#!/bin/sh
# Build the firmware and flash it without touching BOOT/RESET: ask the running
# firmware to reboot into the BOOTSEL bootloader (volume_sync.py --bootloader),
# wait for the RPI-RP2 drive and copy the UF2 onto it. If the board is already
# in BOOTSEL (or runs firmware older than this command), the copy still
# happens once the drive appears.
set -e
cd "$(dirname "$0")"
KB=handwired/onekey/waveshare_rp2040_zero_volume
QMK_HOME=${QMK_HOME:-$HOME/qmk_firmware}
UF2=$QMK_HOME/handwired_onekey_waveshare_rp2040_zero_volume_volume.uf2

[ "$1" = "--no-build" ] || (cd "$QMK_HOME" && qmk compile -kb $KB -km volume)

find_drive() { lsblk -rno NAME,LABEL | awk '$2=="RPI-RP2"{print "/dev/"$1; exit}'; }

if [ -z "$(find_drive)" ]; then
    python3 host/volume_sync.py --bootloader || echo "couldn't reach the knob: press BOOT+RESET by hand"
fi

echo "waiting for RPI-RP2..."
for _ in $(seq ${FLASH_WAIT_S:-60}); do
    dev=$(find_drive)
    if [ -n "$dev" ]; then
        mnt=$(lsblk -rno MOUNTPOINT "$dev")
        if [ -z "$mnt" ]; then
            udisksctl mount -b "$dev" >/dev/null
            mnt=$(lsblk -rno MOUNTPOINT "$dev")
        fi
        cp "$UF2" "$mnt/" && sync
        echo "flashed"
        exit 0
    fi
    sleep 1
done
echo "RPI-RP2 never showed up" >&2
exit 1
