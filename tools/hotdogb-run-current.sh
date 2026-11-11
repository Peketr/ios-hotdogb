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
# Current (2026-11-11, where the one-row GPU copies come from): kernel 0177 (WSL build) = 0175 + 0176 (unaligned
# IOSurface rows rounded to 64 bytes; no measurable effect) + the gpuhist command also groups the 2D blits into one-row
# runs. Once the home screen is up, swipe (python tools\hotdogb-swipebench.py 30 --no-unlock) and meanwhile run
# python tools\flame-cmd.py "gpuhist 100000 2". Expect "#### GPUHIST BLIT RUNS: ~11000 blits, M of them in R one-row
# runs" and "GPUHIST run k: ..." lines in logs\runs\NAME\live.txt: step = the buffer pitch (a multiple of 64 or not),
# x = the start's misalignment in pixels, dst pitch 0 = a copy back to a buffer. Back to 0175: KERNEL=work/boot/kernel-0175.
# Before (2026-11-11, panel copy thread): kernel 0175 (WSL build) = 0173 + 0174 (gpuhist command) + the panel copy
# on its own thread, so vsync never waits for the GPU (sweetpthread=0 = copy in the vsync timer as before). Expect
# "SWEET FB: 5s: ... vsync late max ~1000 us, 0 ticks > 1/8 period late" while swiping (was up to 264 ms late).
# Measured: real fps unchanged, ~33-35 swaps/s (k0175-pthread-1111 / k0175-nothread-1111). Earlier swipebench scores
# (0173's 64/68) were inflated by long stats windows; the real 0173 figure is 33.6 (HANDOFF). Test: USE the phone and
# judge whether scrolling LOOKS more even than with -a "... sweetpthread=0". Back to 0173: KERNEL=work/boot/kernel-0173.
# Before (2026-11-10, big cores first): kernel 0173 (WSL build) = 0172 + the scheduler gives threads at priority 37
# and up an idle A76 (cpu4-7) before an A55, lower ones an idle A55 first (sweetbig=N sets the priority, sweetbig=0 =
# old behaviour; sweetbigmask=0xf0). Expect "#### BIG CORES FIRST FOR PRIORITY 37 AND UP (CPU MASK 0xf0)" (early: it
# may be before the live log starts). "TOP thread" lines now end in "cpu N" = the core the thread ran on last.
# Test: unlock, then (PowerShell, in src) python tools\hotdogb-swipebench.py 30 --no-unlock, twice; compare with
# 27/23 fps (0172, services off) and 34/36 (0172 with the junk load). In CPU TOP during the swipes the
# backboardd "com.apple.coreanimation" thread should say cpu 4-7 and the per cpu % should sit on the last four.
# Measured (k0173-swipe-big-1110): 63.8 and 67.7 fps, render thread on cpu 7 / 5. Now: use the phone and
# watch heat (SWEET BATTERY temp) against k0172-nolog-1110. A/B: -a "... sweetbig=0".
# Before (2026-11-10, in use without the log stream): same kernel 0172 and args. The idle A/B is done
# (k0172-nolog-1110: 0.45 cores busy instead of 5.1; oslog-reportcrash removed and vmd disabled on the phone).
# Swipebench done too (HANDOFF): 27/23 fps without the junk load, 34/36 fps with it, 29/25 with the governors off:
# the render thread sits on the A55s when the phone is idle. Next kernel: fast cores first for UI threads.
# Until then: USE the phone (scroll, open apps, Settings) and compare CPU TOP, heat and smoothness; watch whether
# coreduetd comes back in the TOP list. If `fastboot boot` says "Load Error": fastboot set_active a.
# Before (2026-11-10, A/B without the log stream): same kernel 0172 and args. k0172-top-1109-2354 showed diagnosticd
# 140% + oslog 99% (our `oslog -l` boot job) + vmd 97% + coreduetd 67% of ~5 busy cores. Before this run, with the
# phone up (PowerShell, in src): python tools\hotdogb-launchd-job.py list, then `remove NAME` for every oslog-* job,
# `disable com.apple.voicemail.vmd`, python tools\flame-cmd.py reboot. Then this script: leave it untouched 3 minutes.
# Compare "#### CPU TOP ... cores busy", the TOP proc list, and mA / temp in "SWEET BATTERY: gauge" with that run.
# Then: python tools\flame-cmd.py sample coreduetd 200 20 (and vmd if it is still there) + tools\sweet-profile.py.
# Before (2026-11-09, what keeps the cores busy): kernel 0172 (WSL build) = 0171 + CPU top. Every 60 s (sweettop=N
# seconds, 0 = off) the log gets "#### CPU TOP: 5000 ms, N threads, X cores busy; per cpu %: ..." followed by
# "TOP proc NAME[pid] N% (user N sys N) N threads" and "TOP thread NAME[pid] tid N "name" N% ..." lines, busiest first
# (100% = one core), up to "TOP --- done". On demand, while it runs (PowerShell, in src):
#   python tools\flame-cmd.py top 10 25          10 s, 25 lines each; the report lands in logs\runs\NAME\live.txt
#   python tools\flame-cmd.py sample NAME 200 20 then python tools\sweet-profile.py: where a busy user process is
# Let it sit on the home screen for 2-3 minutes untouched first (the idle picture is what heats the phone), then use it.
# The 0171 test (GPU buffer handles) below still applies.
# Before (2026-11-09, display freeze): kernel 0171 (WSL build) = 0170 + GPU buffer handles go round the table (no
# immediate reuse). Aimed at the freeze where the picture blinks between two old frames while the OS runs on
# (k0170-gov-1109-2238 at 534 s: "SWEET MTL: v2 backboardd submit refused: IB ... (outside the window)").
# Expect "#### GPU BUFFER HANDLES ROTATE (NO IMMEDIATE REUSE)" at the GPU bring-up. Use the phone hard (app switcher,
# many fingers, open/close apps): no "submit refused" line = good. If it still comes, the new line after it
# "SWEET MTL: v2 backboardd refused IB: handle N is live/not a buffer, kernel iova ..." says which case it is.
# Back to 0170: KERNEL=work/boot/kernel-0170.
# Before (2026-11-09, heat): kernel 0170 (WSL build) = 0169 + CPU and GPU frequency governors (both were pinned high:
# CPU clusters at their top frequency since 0086, GPU at 499 MHz since 0107). Expect "#### CPU GOVERNOR RUNNING" early
# (before the live log starts: it may only show in a rawdump log) and "#### GPU GOVERNOR RUNNING" after the GPU bring-up,
# then every 30 s "SWEET CPUFREQ: 30s: pwrcl avg N now N MHz load N% ..." and "SWEET GPU: gov 30s: avg N MHz ...".
# Compare: how warm the phone gets, the battery temperature in "SWEET BATTERY: gauge ..." against a 0169 run, and
# whether scrolling got slower. "#### GPU GOVERNOR STOPPED" = the GMU refused a vote (frequency stays where it was).
# Old behaviour: -a "... sweetcpugov=0 sweetgpugov=0" (one or both). Back to 0169: KERNEL=work/boot/kernel-0169-wsl.
# Before (2026-11-09, build check): the same 0169, built in WSL (tools/wsl-build.sh, kernel-0169-wsl) instead of on the Mac.
# Expect the same milestones as the Mac 0169 run k0169-charge-1109-2146 (touch, battery gauge, charging, GPU).
# Back to the Mac build: KERNEL=work/boot/kernel-0169.
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

KERNEL=${KERNEL:-work/boot/kernel-0177}
TAG=k0177-blitruns
if [ ! -f "$KERNEL" ] && [ -f work/boot/kernel-0175 ]; then
	echo "no $KERNEL yet: using kernel-0175 (no blit runs in gpuhist)" >&2
	KERNEL=work/boot/kernel-0175 TAG=k0175-pthread
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
