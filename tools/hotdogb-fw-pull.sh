#!/bin/bash
# Read the modem/WLAN firmware and the modem's remote-filesystem partitions from the OnePlus 7T,
# through TWRP (`fastboot boot` only; everything is read, nothing on the phone is written).
#
#   PATH=$PWD/tools/wsl-bin:$PATH tools/hotdogb-fw-pull.sh      (WSL, phone in fastboot)
#
# Out (../hotdogb/fw, outside git - Qualcomm/OnePlus firmware and the phone's IMEI data):
#   modem-image/  the whole image/ folder of the modem partition (vfat, NON-HLOS)
#   modem/        what wland serves: modem.mdt + modem.bNN, wlanmdsp.mbn, *.jsn (bdwlan.bin: pick by hand)
#   rmtfs/        modemst1.img modemst2.img fsg.img fsc.img (also a backup of them)
set -euo pipefail
cd "$(dirname "$0")/.."
HD=$PWD/../hotdogb
OUT=$HD/fw
twrp=$HD/downloads/twrp/twrp-3.7.0_11-0-hotdog.img
mkdir -p "$OUT/modem-image" "$OUT/modem" "$OUT/rmtfs"

fastboot devices | grep -q fastboot || { echo "phone not in fastboot" >&2; exit 1; }
echo "$(date +%T) booting TWRP"
fastboot boot "$twrp" 2>&1 | tail -1
ok=0
for i in $(seq 1 90); do
	[ "$(adb shell 'echo ok' 2>/dev/null | tr -d '\r')" = ok ] && { ok=1; break; }; sleep 2
done
[ "$ok" = 1 ] || { echo "TWRP adb did not come up" >&2; exit 1; }

slot=$(adb shell getprop ro.boot.slot_suffix | tr -d '\r')
echo "slot '$slot'; partitions:"
adb shell "ls /dev/block/by-name" | tr -d '\r' | grep -E '^(modem|modemst|fsg|fsc|persist|bluetooth|dsp)' | tr '\n' ' '; echo
modem=modem$slot
adb shell "mkdir -p /tmp/modem && (mountpoint -q /tmp/modem || mount -t vfat -o ro /dev/block/by-name/$modem /tmp/modem) && ls /tmp/modem"
adb exec-out "tar -C /tmp/modem/image -cf - ." | tar -xf - -C "$OUT/modem-image"
adb shell "umount /tmp/modem" || true
echo "modem image: $(ls "$OUT/modem-image" | wc -l) files, $(du -sh "$OUT/modem-image" | cut -f1)"

for p in modemst1 modemst2 fsg fsc; do
	adb exec-out "dd if=/dev/block/by-name/$p bs=1048576 2>/dev/null" > "$OUT/rmtfs/$p.img"
	echo "rmtfs $p: $(stat -c %s "$OUT/rmtfs/$p.img") bytes"
done
chmod 600 "$OUT"/rmtfs/*.img

# What wland serves (names are case-folded by the vfat mount: keep them lower-case, as the modem asks)
for f in "$OUT"/modem-image/*; do
	b=$(basename "$f" | tr 'A-Z' 'a-z')
	case $b in modem.mdt|modem.b[0-9]*|wlanmdsp.mbn|*.jsn) cp -f "$f" "$OUT/modem/$b" ;; esac
done
echo "modem/: $(ls "$OUT/modem" | wc -l) files"
echo "board data candidates:"; ls -la "$OUT/modem-image" | grep -i -E 'bdwlan|bdf|regdb' || echo "(none in the modem image)"

echo "$(date +%T) TWRP: back to fastboot"
adb reboot bootloader 2>/dev/null || true
for i in $(seq 1 60); do fastboot devices | grep -q fastboot && break; sleep 1; done
fastboot devices
