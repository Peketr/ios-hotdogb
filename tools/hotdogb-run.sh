#!/bin/bash
# One kernel run on the OnePlus 7T (hotdogb), from Linux or WSL:
#
#   tools/hotdogb-run.sh NAME -k KERNEL [-s SECS] [-a "EXTRA BOOT-ARGS"] [-q] [-L] [-T | -A] [-n]
#
#   1. payload (mkpayload.py, ADT work/boot/hotdogb.adt) + hotdogb boot image
#   2. phone to fastboot, `fastboot boot` the kernel (nothing is flashed)
#   3. record the live log (UDP 5140 over the USB network) into logs/runs/NAME/live.txt with
#      Windows Python (WSL's own network cannot receive it); `####` milestones print here as they come
#   4. wait for flamereboot=SECS (or `python tools/flame-cmd.py reboot`) to reset it back into fastboot
#   5. stop the recorder and print a summary; the phone stays in fastboot
#
# The iOS root is on userdata (sda22) since 2026-11-07, so Android must not boot
# (a failed /data mount sends it to recovery's "factory data reset" prompt).
# Root boot-args: -a "rd=ufsroot0 ufsrw=1 rootrw=1 ..."; -a "rd=ufsroot0
# ufsroot=rawdump" keeps the root read-only on rawdump (no userland).
# -q: no kernel text on screen once the GUI presents (sweetfb_noconsole=1).
# -G: the Adreno 640 in the kernel (sweetgpu=1): firmware from ../hotdogb/fw/gpu into the ADT
#     (a640_gmu.bin, a630_sqe.fw, a640_zap.mdt + .b02 from the 7T vendor partition; kernel 0149+).
# -L: no live recorder (another listener, e.g. tools/flame-live.py in a terminal, has the log).
# -T: afterwards also read the rawdump log ring (from boot, survives a crash before USB is up)
#     through TWRP (`fastboot boot` of -t IMG; not working on this phone yet, 2026-11-07).
# -A: the same through Android + Magisk (only with Android data on userdata - not now).
# -n: with -T/-A, no rescue boot (the probe shim that saves the RAM log tail into rawdump first).
# On WSL, put tools/wsl-bin first in PATH (Windows fastboot.exe/adb.exe).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
HD=$ROOT/../hotdogb
name=$1; shift
kernel="" secs=600 extra="" rescue=1 pull=none quiet=0 live=1 gpu=0
twrp=$HD/downloads/twrp/twrp-3.7.0_11-0-hotdog.img
while getopts "k:s:a:nATt:qLG" o; do
	case $o in k) kernel=$OPTARG ;; s) secs=$OPTARG ;; a) extra=$OPTARG ;; n) rescue=0 ;;
		A) pull=android ;; T) pull=twrp ;; t) twrp=$OPTARG ;; q) quiet=1 ;; L) live=0 ;; G) gpu=1 ;; *) exit 2 ;; esac
done
[ "$pull" != twrp ] || [ -f "$twrp" ] || { echo "no TWRP image $twrp" >&2; exit 2; }
[ "$pull" != none ] || rescue=0		# the rescue shim only matters when rawdump is read
[ -n "$kernel" ] || { echo "-k KERNEL required" >&2; exit 2; }
base="debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress"
base="$base unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1"
# On-screen console on (bring-up) unless -q.
base="$base wdt=-1 sweetfb_hz=60 ufslogsecs=2"
[ "$quiet" = 0 ] || base="$base sweetfb_noconsole=1"
gpufw=()
if [ "$gpu" = 1 ]; then
	for f in a640_gmu.bin a630_sqe.fw a640_zap.mdt a640_zap.b02; do
		[ -f "$HD/fw/gpu/$f" ] || { echo "-G: no $HD/fw/gpu/$f" >&2; exit 2; }
	done
	gpufw=(--gpufw "$HD/fw/gpu")
	base="$base sweetgpu=1"
fi
cmdline="$base flamereboot=$secs $extra"
out=$ROOT/logs/runs/$name
mkdir -p "$out" "$ROOT/work/boot/runs"
img=$ROOT/work/boot/runs/$name.img
probe=$ROOT/work/boot/shim-probe-hotdogb.img
cp "$kernel" "$out/kernel"
echo "$cmdline" > "$out/cmdline"

bash shim/build-hotdogb.sh "$HD/stock/dtb" "$probe" >/dev/null
python3 tools/mkpayload.py "$kernel" work/boot/hotdogb.adt "$out/payload.bin" --cmdline "$cmdline" "${gpufw[@]}" >/dev/null
python3 tools/mkboot_hotdogb.py shim/out-hotdogb/Image "$HD/stock/dtb" "$img" "$out/payload.bin"
rm -f "$out/payload.bin"

in_fastboot() { fastboot devices | grep -q fastboot; }
wait_fastboot() { for i in $(seq 1 "$1"); do in_fastboot && return 0; sleep 1; done; return 1; }

# ---- live recorder (Windows Python through WSL interop) ----
WINPY=${WINPY:-/mnt/c/Python312/python.exe}
PS=/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe
live_txt=$out/live.txt
live_on=0
live_start() {
	[ "$live" = 1 ] || { echo "(-L: no live recorder)"; return 0; }
	[ -x "$WINPY" ] && [ -x "$PS" ] || { echo "(no Windows python/powershell: no live recorder)"; return 0; }
	if ! "$PS" -NoProfile -Command 'if (Get-NetUDPEndpoint -LocalPort 5140 -ErrorAction SilentlyContinue) { exit 1 }'; then
		echo "(UDP 5140 is held by another listener - e.g. flame-live.py in a terminal: it gets the log, not $name/live.txt)"
		return 0
	fi
	: > "$live_txt"
	"$WINPY" "$(wslpath -w "$ROOT/tools/flame-live.py")" "$(wslpath -w "$live_txt")" --secs $((secs + 300)) \
		--grep '####|panic|SWEET CMD' 2>/dev/null | sed -u 's/^/  live: /' &
	live_on=1
	echo "live log -> logs/runs/$name/live.txt (watch: Get-Content -Wait logs\\runs\\$name\\live.txt)"
}
live_stop() {
	[ "$live_on" = 1 ] || return 0
	live_on=0
	"$PS" -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | Where-Object { \$_.CommandLine -like '*flame-live.py*$name*live.txt*' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }" || true
	wait 2>/dev/null || true
}
trap live_stop EXIT

if ! in_fastboot; then adb reboot bootloader 2>/dev/null || true; fi
if ! wait_fastboot 20; then
	# The 7T's fastboot sometimes stops answering after a reset (on its screen, but `fastboot devices`
	# empty or "no link"); `fastboot reboot bootloader` waits for it and restarts the bootloader.
	echo "$(date +%T) not visible in fastboot: fastboot reboot bootloader"
	timeout 60 fastboot reboot bootloader 2>&1 | tail -1 || true
fi
wait_fastboot 90 || { echo "phone not in fastboot" >&2; exit 1; }
live_start
t0=$(date +%s)
echo "$(date +%T) boot $name ($secs s): $cmdline"
fastboot boot "$img" 2>&1 | tail -1
sleep 15
if ! wait_fastboot $((secs + 180)); then
	echo "run did not return to fastboot in $((secs + 180)) s; leaving it" >&2
	exit 1
fi
echo "$(date +%T) back in fastboot after $(( $(date +%s) - t0 )) s"
sleep 2
live_stop

if [ "$pull" != none ]; then
	if [ "$rescue" = 1 ]; then
		echo "$(date +%T) rescue boot"
		fastboot boot "$probe" 2>&1 | tail -1
		sleep 15
		wait_fastboot 90 || { echo "rescue boot did not return" >&2; exit 1; }
	fi
	if [ "$pull" = android ]; then
		echo "$(date +%T) booting Android to read rawdump"
		fastboot reboot 2>&1 | tail -1
		adb wait-for-device
		for i in $(seq 1 180); do
			[ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && break; sleep 2
		done
		adb push "$HD/logpull.sh" /data/local/tmp/logpull.sh >/dev/null
		adb shell "su -c 'sh /data/local/tmp/logpull.sh'"
		for f in shim rescue xnu; do adb exec-out "cat /data/local/tmp/shimlog/$f.raw" > "$out/$f.raw"; done
	else
		echo "$(date +%T) booting TWRP to read rawdump"
		fastboot boot "$twrp" 2>&1 | tail -1
		ok=0
		for i in $(seq 1 90); do
			[ "$(adb shell 'echo ok' 2>/dev/null | tr -d '\r')" = ok ] && { ok=1; break; }; sleep 2
		done
		[ "$ok" = 1 ] || { echo "TWRP adb did not come up" >&2; exit 1; }
		adb push "$HD/logpull.sh" /tmp/logpull.sh >/dev/null
		adb shell "sh /tmp/logpull.sh /tmp/shimlog"
		for f in shim rescue xnu; do adb exec-out "cat /tmp/shimlog/$f.raw" > "$out/$f.raw"; done
		echo "$(date +%T) TWRP: back to fastboot"
		adb reboot bootloader 2>/dev/null || true
		wait_fastboot 60 || echo "phone did not return to fastboot from TWRP" >&2
	fi
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
fi

log=$out/xnu.txt
[ -s "$log" ] || log=$live_txt
if [ ! -s "$log" ]; then
	echo "=== $name: no log (live recorder off or nothing received; logs/runs/$name)"
	exit 0
fi
echo "=== $name: $(wc -l < "$log") lines in $(basename "$log"), logs in logs/runs/$name"
grep -a "####" "$log" | cut -c1-160 | tail -20 || true
grep -a -i -m 20 "panic\|stuck\|timeout" "$log" | cut -c1-200 || true
echo "$(grep -a -c "CRASH " "$log" || true) CRASH lines"
echo "--- last lines"; tail -5 "$log" | cut -c1-200
if [ "$pull" != none ]; then
	echo "--- last lines (RAM rescue)"; tail -5 "$out/rescue.txt" | cut -c1-200
	echo "--- shim"; tail -5 "$out/shim.txt" | cut -c1-200
fi
