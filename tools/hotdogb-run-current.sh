#!/bin/bash
# The current recommended hotdogb run: the latest kernel and boot-args we test.
# Updated with every new test. Run from WSL Ubuntu, anywhere:
#
#   /mnt/c/Users/Peketr/ios-flame/src/tools/hotdogb-run-current.sh [NAME]
#
# NAME defaults to the run tag below plus the date and time (logs/runs/NAME/),
# so earlier runs are never overwritten. The run records the live log itself into
# logs/runs/NAME/live.txt (192.168.7.1 on "Ethernet 2"; close other flame-live.py listeners first)
# and prints the #### milestones as they come. End early: python tools\flame-cmd.py reboot
#
# Also (2026-11-09, charging): kernel 0169 = 0168 + charging switched on (PM8150B 0x1042) while the gauge reads 3-43 C,
# off above 45 C ("#### CHARGING ENABLED/DISABLED", "#### CHARGING CONTROL UNAVAILABLE"; sweetcharge=0 = never write).
# Plugged in, the current in "SWEET BATTERY: gauge ..." should turn positive. 0168 = 0167 + a read-only PM8150B charger register dump ("#### CHARGER REGISTER DUMP"
# at the first battery reading and on every plug/unplug). Boot with USB power in; after ~1 min unplug, wait 30 s, plug back.
# Current (2026-11-09, sensors step 1): kernel 0167 (now 0168); new userland boot job org.iosonpixel.sensord (phone/sensors,
# tools/hotdogb-sensors-install.py, log /var/local/sensord.log, samples /var/local/imu.txt). ~45 s after boot it boots the
# SLPI and serves it. Milestones in order: "#### STARTING SENSORS (SLPI)", "#### SLPI BOOTED", "#### FASTRPC CHANNEL OPEN",
# "#### FASTRPC ATTACHED TO THE SENSORS PD", "#### FASTRPC LISTENER RUNNING", "#### FIRST FILE OPENED BY THE SLPI",
# "#### SLPI SENSOR PD UP", "#### SSC SERVICE UP", "#### FOUND ACCEL/GYRO/MAG SENSOR", "#### FIRST ACCEL/GYRO/MAG SAMPLE".
# Failure lines: "#### SLPI ERROR FATAL", "#### FASTRPC ... FAILED/STOPPED", "#### FASTRPC REVERSE CALL NOT IMPLEMENTED".
# sensord.log is in the iOS root, not the live log: pull it with flame-files.py get /var/local/sensord.log.
# Before: kernel 0167 = 0166 + GPIO 88/89 muxed to qup8 (0166 timed out: pins were GPIOs). 0166 = battery from the 7T's bq27411 fuel gauge (default on; sweetbattery=0 = old
# static 100%). Look for "#### BATTERY GAUGE UP" and "#### FIRST BATTERY READING: N% ..." ~15 s after start; compare N
# with the status bar / Settings > Battery; plug/unplug USB power -> "#### CHARGER CONNECTED/DISCONNECTED" within 20 s.
# Until kernel-0166 is fetched the script falls back to kernel-0165 (says so). The GPS step below is unchanged.
# GPS step 2 (2026-11-09) - same kernel; gpsfeed boot job (/var/local/gpsfeed.log) hands wland's fixes to
# locationd. Outdoors, wait for "#### FIRST LOCATION SENT TO LOCATIOND", then open Maps. CommCenter stays off.
# Also on the phone: GPS wland (phone/wifi/gnss.c, "#### FIRST GPS FIX", /var/local/gps.fix) and Location Services on.
# Before (2026-11-08): kernel 0158 (sweet90=3: panel at 90 Hz + INTF_1 tear check rescaled + 11.1 ms vsync; 1 = panel only, 2 = vsync only) on 0157 (90 Hz switch) on 0156.
# 0156 (brightness slider -> panel; backlight service on by default, sweetbl=1 = panel
# off/on only) on 0155 (backlight IODisplayParameters + /backlight panel description) on 0154 (panel dark while the display is off; sweetbl=2 adds the backlight service, which
# stalls backboardd) on 0153 (logs and accepts display/backlight property keys) on 0152 (panel brightness: DCS 0x51 + AppleARMBacklight, sweetbl=0 = off) on 0151 (GPU present fills the panel, sweetfb_fill=0 = bars; a client's first 3
# GPU resets forgiven, sweetgpuforgive=N) with the Adreno 640 on (-G: sweetgpu=1 + the 7T's GPU firmware from
# ../hotdogb/fw/gpu; 0149 = the 7T's zap shader region, 0150 = margins cleared after CPU-copy frames). iOS root on userdata read-write, with wland
# (Wi-Fi at boot, log /var/local/wland.log) and oslog-wifid (/var/local/oslog-wifid.log) launchd jobs.
# No kernel text on screen once the GUI presents (-q), 900 s, live log only. Wi-Fi off: sweetwifi=0.
# Metal: sweetmtl=2 with SweetMetal2.bundle (metal2vulkan + Turnip) in /System/Library/Extensions, installed by
# tools/hotdogb-metal2-install.py; its log is /var/local/mtl2.log. Without the bundle: drop sweetmtl=2.
set -euo pipefail
cd "$(dirname "$0")/.."

KERNEL=work/boot/kernel-0169
TAG=k0169-charge
if [ ! -f "$KERNEL" ] && [ -f work/boot/kernel-0168 ]; then
	echo "no $KERNEL yet: using kernel-0168 (no charge enable)" >&2
	KERNEL=work/boot/kernel-0168 TAG=k0168-sensors-chg
fi
SECS=900
OPTS=(-q -G)
ARGS="rd=ufsroot0 ufsrw=1 rootrw=1 sweetusb=2 sweetmtl=2 sweet90=3"

name=${1:-$TAG-$(date +%m%d-%H%M)}
[ -f "$KERNEL" ] || { echo "no kernel $KERNEL (fetch it from the NAS first)" >&2; exit 1; }
sed -i 's/\r$//' tools/hotdogb-run.sh ../hotdogb/logpull.sh
export PATH="$PWD/tools/wsl-bin:$PATH"
echo "run $name: $KERNEL ${OPTS[*]} -s $SECS -a \"$ARGS\""
exec tools/hotdogb-run.sh "$name" "${OPTS[@]}" -k "$KERNEL" -s "$SECS" -a "$ARGS"
