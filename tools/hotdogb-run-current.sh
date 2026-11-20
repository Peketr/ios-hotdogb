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
# Motion sensors (2026-11-11): the run is the stable 0183 build whatever state sensord is in. The sensord boot job
# only starts the SLPI when the boot args carry sweetsensors=1, which this script adds only with SENSORS=1:
#   SENSORS=1 /mnt/c/.../hotdogb-run-current.sh    -> kernel 0184 (motion HID services + kern.sweet_motion), TAG k0184-sensors
# Without SENSORS=1: kernel 0183, sensord logs "no sweetsensors=1 in the boot args: not starting" and touches nothing.
# SENSORS=1 state (2026-11-20, d-sensors-1120-1439): WORKS. sensord fills Oppo's SMEM sensor-devinfo item 130
# (accel LSM6DSM, mag MMC5603) before booting the SLPI; ~42 s after SLPI start "#### FOUND ACCEL/GYRO/MAG SENSOR",
# then "#### FIRST ACCEL/GYRO/COMPASS EVENT SENT TO IOS HID" (~116 s into the boot). Ran 420 s, no GPI timeouts.
# Axis signs, autorotate and compass calibration are untested (need the phone moved).
# Compass app (2026-11-20, c-compass9, SENSORS=1 SECS=1200): the phone runs a patched locationd from /usr/local/bin/locationd
# (job com.apple.locationd program; /usr/libexec/locationd untouched): CLCompass7/AOP check at 0x6bd158 -> 0, legacy
# CLMagnetometer::isAvailable at 0x73b2a8 -> 1, re-signed with its own entitlements; mobile pref CompassIgnoreGyro=true.
# locationd now builds the legacy compass on our HID magnetometer and computes headings (oslog-loc job, /var/local/oslog-loc.log:
# "Type,Compass,magneticHeading,229.61,...accuracy,-1.0,...level,0"), but uncalibrated: |B| ~131 uT (hard-iron offset), so
# the Compass app shows no heading yet. Next: rotate the phone in a figure-8 with Compass open, then check level/accuracy.
# Autorotate (2026-11-20, qc-test-1120-2148): WORKS with two shared-cache patches on the phone (Apple files, not in git;
# originals kept off-repo): dyld_shared_cache_arm64.05 @0x3de0fec d39f81d2->d39781d2 (CoreMotion hwType 103 capability
# mask: clear bit 6 = no AOP orientation, so CLOrientationNotifier runs its software accel detector), and
# dyld_shared_cache_arm64.03 @0x15bb030 54010035->0a000014 (QuartzCore: never ask for a rotated framebuffer; our Metal
# can't make rotation=3 IOSurface textures: "Failed to allocate [2436 x 1125] BGRA framebuffer").
# Compass test (2026-11-20, cmp-test): the Compass app's CMMotionManager asks CoreMotion for 9AxisWithNorthReference;
# CLDeviceMotion::enableSensorFusionWithMode (0x1919c4520) builds the AOP/SPU fusion service when capability bit 4 is
# set (it waits forever on HID {0xff0c,4}) and the software CLSensorFusionService9Axis otherwise. Same .05 instruction,
# first d39581d2 (mask 0xcae): Compass crashed, assert "CLCompass7 requires deviceMotionService" - the compass factory
# (0x191c62080) makes CLCompass7 when bit 5 OR bit 4 is set, else the legacy CLCompass (hwType 103 magnetometer check
# 0x1919bd768 returns 0x1f, bit 0 set). Now d39181d2 (mask 0xc8e: bits 4, 5, 6 clear), cmp2-test.
# cmp2-test-1120-2256: no crash; Compass runs "Legacy compass" + CLSensorFusionService9Axis on our HID mag/accel/gyro,
# calibrates within ~30 s of figure-8s (bias x ~79 uT, |B| ~35 uT after, calibrationLevel 2, locationd heading 107..202
# acc 15), but the dial stays at 0: CMDeviceMotion yaw stays ~0, MagneticFieldAccuracy 0, Heading 0 (the 9-axis MEKF never
# pulls yaw to north). Next run: mobile pref com.apple.CoreMotion LogSensorFusion=1 to see its [CompassIntoKF] /
# "Aborted yawUpdate" / "UpdatingYaw!" lines. Log job oslog-loc (-t 900 -l locationd Compass). Open Compass, turn it.
# cmp3-test (k0186, LogSensorFusion=1): touch OK. Mag axes match accel/gyro (gravity-dip and gyro-prediction tests,
# identity mapping best). The 9-axis MEKF feedCompassData (0x191c20b60) only uses the compass when calibrationLevel > 2
# ("cmp w21,#2; b.gt" at 0x191c20c54); our legacy compass stays at level 1 (sometimes 2), so yaw never gets a north
# reference. kf-test: same .05 file offset 0x403cc54 bf0a0071 -> bf020071 (cmp w21,#0: level >= 1 is used).
# kf-test-1121: COMPASS DIAL MOVES. 173 "UpdatingYaw!", calibration level 2, accuracy 30 deg; user: "not all that precise".
# Mag 16 Hz, noise ~0.3 uT, sphere-fit residual 1.8 uT on |B| ~48 uT. Next: better hard/soft-iron calibration or a
# higher mag rate in sensord so the legacy compass reaches level 3 (then the MEKF gate patch is not needed).
# Touch failure (cmlog-prep-1120-2322): "SWEET S3706: L17A enable failed 16; stopping" at 321 s - RPMH lock held by a
# WLAN rail vote (ldoc11/rfclka2) and 0185 returned EBUSY at once. 0186 waits up to 500 ms ("#### RPMH WAITED N MS").
# SENSORS=1 now boots kernel 0186 (0185 + RPMH wait) with sweetinitwait=1: if launchd dies (e.g. a broken shared cache) the kernel stays up
# ("#### initproc ...: KERNEL STAYS UP FOR THE FILE SERVER") so flame-files.py can repair the root over USB.
# Motion log (sensord ssc.c, always on): "#### ORIENTATION AT START", "#### MOTION START", "motion:" traces every 250 ms,
# "#### MOTION END ... rotation x/y/z deg ... gravity before -> after, tilt N deg", "#### ORIENTATION: FACE UP|..." -
# move / rotate the phone during a SENSORS=1 run and check axis signs there (iOS frame: +x right, +y top, +z out).
# Vibration test (sensord vib.c, PM8150B haptics over SPMI from /dev/pmem): put a file /var/local/sensors/vibtest
# (content "400" = accel Hz) and run SENSORS=1; 10 s after accel streams the motor goes off/on in 4 s windows and
# sensord.log gets "#### VIB ON/OFF Wn ..." lines and "#### VIBRATION VISIBLE|NOT VISIBLE IN THE ACCELEROMETER DATA";
# raw samples in /var/local/vib.csv. Delete vibtest afterwards (it raises the accel rate and buzzes every boot).
# Run length: SECS=N (default 900).
# Current (2026-11-20, BLE with Wi-Fi auto-join): same kernel 0183; btd11 (phone/bt, tools/hotdogb-bt-install.py) resyncs the
# chip stream. 0183 made wifid join ~5 s after en0, the moment btd brings BT up; serving then started mid-packet and the
# old btd swallowed bluetoothd's first answer (restart every 30 s). k0183-blefix: "<ctlr resync" lines, then HCI reset and
# "LE meta sub 0x2" reports in /var/local/btd.log. btd11 also frames+drops the firmw. debug log (ACL 0xedc; k0183-btd11-1120-1033:
# stats "fw debug 3, resyncs 0", LE scan on, 55 reports, bluetoothd stays on pid 92). Expect the two H4 BYTES milestones.
# Before - Current (2026-11-11, Wi-Fi auto-join WORKS): kernel 0183 (WSL build) = 0182 + RSSI_BOUNDS (106) answered, a guessed
# layout {version 1, three bounds -100 dBm}. k0183-bounds-1111-2002: wifid joined by itself 3 s after its first scan. Expect "#### WIFI RSSI BOUNDS ANSWERED: -100 DBM (0183)" when wifid starts.
# Do NOT touch Settings > Wi-Fi for 3 minutes. Fixed = "#### WIFID ASKS TO JOIN A NETWORK" by itself. wifid's own log:
# /var/local/oslog-wifid-0182.log on the phone. EXTRA=" sweetwifibounds=0" = refused as in 0182.
# Before (2026-11-11, Wi-Fi auto-join): kernel 0182 (WSL build) = 0181 + scan results for wifid carry noise -95 dBm
# and snr, and the NOISE request is answered (wifid refused the known network as "Below minimum RSSI threshold" at
# -41 dBm). Expect "#### WIFI SCAN RESULTS CARRY NOISE -95 DBM AND SNR (0182, MODE 1)" at wifid's first scan (~70-90 s).
# Do NOT touch Settings > Wi-Fi: wait 3 minutes. Fixed = "#### WIFID ASKS TO JOIN A NETWORK" appears by itself and the
# Wi-Fi icon comes up. Not fixed: rerun with EXTRA=" sweetwifinoise=2" (also sets the field before the noise);
# EXTRA=" sweetwifinoise=0" = old behaviour. Back to 0181: KERNEL=work/boot/kernel-0181.
# Before (2026-11-11, GPU images and perf counters): kernel 0181 (WSL build) = 0180 + gpuhist lists the sampled images
# ("#### GPUHIST IMAGES: ...") and a `gpuperf [SECS]` command. Once the home screen is up: python tools\flame-cmd.py
# "gpuhist 8000 3", python tools\flame-cmd.py "gpuperf 20", python tools\hotdogb-swipebench.py 30, then read the
# "GPUHIST image" and "gpuperf" lines in logs\runs\NAME\live.txt. k0181-perf-1111: 23/25 images tiled (UBWC), CP never
# starved, GPU idle ~59% while swiping (HANDOFF). Back to 0180: KERNEL=work/boot/kernel-0180.
# Before (2026-11-11, GPU clock measured): kernel 0180 (WSL build) = 0179 + the governor measures the real clock:
# "#### GPU CLOCK MEASURED: 674 MHZ AT THE 675 MHZ LEVEL" and per 30 s "gov clock: level ... measured ..." lines.
# Measured (k0180-gpuclk-1111): 674 MHz, but the heavy backboardd IB stays ~24 ms and swipebench ~38-40 fps (HANDOFF).
# Back to 0179: KERNEL=work/boot/kernel-0179.
# Before (2026-11-11, GPU clock): kernel 0179 (WSL build) = 0178 + GPU level 6 = 675 MHz at NOM_L1 (the top of the
# phone's own stock GPU table), the governor's top by default. Expect "#### GPU PERF TABLE UP TO 675 MHZ (6 LEVELS)" and
# "#### GPU GOVERNOR RUNNING (every 100 ms, 257-675 MHz ...)". Unlock, swipe (python tools\hotdogb-swipebench.py 30
# --no-unlock) and compare fps and the heavy backboardd IB time ("IB us:" in SWEET GPU 5s jobs, ~25 ms at 499) and the
# battery temperature with k0177-repack-1111. "#### GPU GOVERNOR STOPPED" or GPU faults = 675 not taken: -a "...
# sweetgpufreq=5" (585) or sweetgpufreq=4 (old 499 cap). Back to 0178: KERNEL=work/boot/kernel-0178.
# Before (2026-11-11, broken Control Center glyphs): kernel 0178 (WSL build) = 0177 + 0176's row rounding off by
# default (it sheared 5 SpringBoard LA08 glyph surfaces; sweetsurfalign=1 = rounding on). Expect "#### SURFACE ROWS AS
# GIVEN BY DEFAULT (0178)". Unlock, open Control Center (python tools\flame-cmd.py "swipe 960 15 960 1100 400") and
# capture: python tools\flame-shot cc.png --pa 0x9c000000 --w 1080 --h 2400 --stride 4352. Airplane, cellular,
# flashlight, camera and the NFC module should be unsheared. Back to 0177: KERNEL=work/boot/kernel-0177.
# Before (2026-11-11, where the one-row GPU copies come from): kernel 0177 (WSL build) = 0175 + 0176 (unaligned
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

KERNEL=${KERNEL:-work/boot/kernel-0183}
TAG=k0183-btd11
SECS=${SECS:-900}
OPTS=(-q -G)
SENS=""
[ "${SENSORS:-0}" = 1 ] && { SENS=" sweetsensors=1 sweetinitwait=1"; TAG=k0186-sensors; KERNEL=${KERNEL_SENSORS:-work/boot/kernel-0186}; }
ARGS="rd=ufsroot0 ufsrw=1 rootrw=1 sweetusb=2 sweetmtl=2 sweet90=3${SENS}${EXTRA:-}"

name=${1:-$TAG-$(date +%m%d-%H%M)}
[ -f "$KERNEL" ] || { echo "no kernel $KERNEL (fetch it from the NAS first)" >&2; exit 1; }
sed -i 's/\r$//' tools/hotdogb-run.sh ../hotdogb/logpull.sh
export PATH="$PWD/tools/wsl-bin:$PATH"
echo "run $name: $KERNEL ${OPTS[*]} -s $SECS -a \"$ARGS\""
exec tools/hotdogb-run.sh "$name" "${OPTS[@]}" -k "$KERNEL" -s "$SECS" -a "$ARGS"
