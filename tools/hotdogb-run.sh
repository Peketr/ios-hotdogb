#!/bin/bash
# One kernel run on the OnePlus 7T (hotdogb), from Linux or WSL:
#
#   tools/hotdogb-run.sh NAME -k KERNEL [-s SECS] [-a "EXTRA BOOT-ARGS"]
#
#   1. payload (mkpayload.py, ADT work/boot/hotdogb.adt) + hotdogb boot image
#   2. phone to fastboot, `fastboot boot` the kernel (nothing is flashed)
#   3. wait for flamereboot=SECS to reset it back into fastboot
#   4. probe shim (saves the previous kernel's RAM log tail), back to fastboot
#   5. boot Android, pull the end of rawdump with root (hotdogb/logpull.sh)
#   6. decode into logs/runs/NAME/ and print a summary
#
# No root boot-args (rd=ufsroot0 ufsrw rootrw): the iOS root is not on sda22.
# On WSL, put tools/wsl-bin first in PATH (Windows fastboot.exe/adb.exe).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
HD=$ROOT/../hotdogb
name=$1; shift
kernel="" secs=600 extra="" rescue=1
while getopts "k:s:a:n" o; do
	case $o in k) kernel=$OPTARG ;; s) secs=$OPTARG ;; a) extra=$OPTARG ;; n) rescue=0 ;; *) exit 2 ;; esac
done
# -n: no rescue boot, so shim.txt is the kernel boot's own shim log (the probe
# shim would overwrite it); loses the RAM-log rescue.
[ -n "$kernel" ] || { echo "-k KERNEL required" >&2; exit 2; }
base="debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress"
base="$base unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1"
# On-screen console on (bring-up): no sweetfb_noconsole=1.
base="$base wdt=-1 sweetfb_hz=60 ufslogsecs=2"
cmdline="$base flamereboot=$secs $extra"
out=$ROOT/logs/runs/$name
mkdir -p "$out" "$ROOT/work/boot/runs"
img=$ROOT/work/boot/runs/$name.img
probe=$ROOT/work/boot/shim-probe-hotdogb.img
cp "$kernel" "$out/kernel"
echo "$cmdline" > "$out/cmdline"

bash shim/build-hotdogb.sh "$HD/stock/dtb" "$probe" >/dev/null
python3 tools/mkpayload.py "$kernel" work/boot/hotdogb.adt "$out/payload.bin" --cmdline "$cmdline" >/dev/null
python3 tools/mkboot_hotdogb.py shim/out-hotdogb/Image "$HD/stock/dtb" "$img" "$out/payload.bin"
rm -f "$out/payload.bin"

in_fastboot() { fastboot devices | grep -q fastboot; }
wait_fastboot() { for i in $(seq 1 "$1"); do in_fastboot && return 0; sleep 1; done; return 1; }

if ! in_fastboot; then adb reboot bootloader 2>/dev/null || true; fi
wait_fastboot 90 || { echo "phone not in fastboot" >&2; exit 1; }
t0=$(date +%s)
echo "$(date +%T) boot $name ($secs s): $cmdline"
fastboot boot "$img" 2>&1 | tail -1
sleep 15
if ! wait_fastboot $((secs + 180)); then
	echo "run did not return to fastboot in $((secs + 180)) s; leaving it" >&2
	exit 1
fi
echo "$(date +%T) back in fastboot after $(( $(date +%s) - t0 )) s"
if [ "$rescue" = 1 ]; then
	echo "$(date +%T) rescue boot"
	fastboot boot "$probe" 2>&1 | tail -1
	sleep 15
	wait_fastboot 90 || { echo "rescue boot did not return" >&2; exit 1; }
fi
echo "$(date +%T) booting Android to read rawdump"
fastboot reboot 2>&1 | tail -1
adb wait-for-device
for i in $(seq 1 180); do
	[ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && break; sleep 2
done
adb push "$HD/logpull.sh" /data/local/tmp/logpull.sh >/dev/null
adb shell "su -c 'sh /data/local/tmp/logpull.sh'"
for f in shim rescue xnu; do adb exec-out "cat /data/local/tmp/shimlog/$f.raw" > "$out/$f.raw"; done

tr -d '\000' < "$out/shim.raw" > "$out/shim.txt"
python3 - "$out/rescue.raw" "$out/rescue.txt" <<'PY'
import struct, sys
raw = open(sys.argv[1], 'rb').read()
if raw[:8] != b'SWEETRSC':
    open(sys.argv[2], 'w').write('(no rescue)\n'); sys.exit(0)
total, n = struct.unpack_from('<QI', raw, 8)
open(sys.argv[2], 'wb').write(raw[4096:4096 + n].replace(b'\0', b''))
PY
python3 tools/flame-log-decode.py "$out/xnu.raw" "$out/xnu.txt" >/dev/null || echo "(xnu ring did not decode)" > "$out/xnu.txt"
rm -f "$out/shim.raw" "$out/rescue.raw"

echo "=== $name: $(wc -l < "$out/xnu.txt") xnu lines, rescue $(wc -c < "$out/rescue.txt") bytes, logs in logs/runs/$name"
grep -a -i -m 20 "panic\|CRASH \|stuck\|timeout\|failed" "$out/xnu.txt" "$out/rescue.txt" | cut -c1-200 || true
echo "--- last lines (xnu ring)"; tail -5 "$out/xnu.txt" | cut -c1-200
echo "--- last lines (RAM rescue)"; tail -5 "$out/rescue.txt" | cut -c1-200
echo "--- shim"; tail -5 "$out/shim.txt" | cut -c1-200
