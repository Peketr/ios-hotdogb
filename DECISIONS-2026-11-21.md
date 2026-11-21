# Autonomous porting session, 2026-11-21 01:30-10:00

You asked me to work through the night without asking anything. Every decision I made is below, in order,
with the reason. The work itself is tracked in GUACAMOLE-PORT.md; run logs are in logs/runs/.

## Decisions

1. (01:30) Order of work: first find out why CoreAnimation never registers for vsync (0187 showed
   "vsyncs sent 0"), because frame pacing looked like the bigger win than GPU composition. Then the
   composition / present patches, then the cheap IOSurface / WebKit fixes.
2. (01:30) Kernel 0187 and the run-script changes from earlier stay uncommitted, like everything tonight:
   committing / pushing is left to you (one command at the end of this file).
3. (01:45) Ported guacamole 0154 as **0188** (IOMFBDisplayRefresh + TimingElements at our real vsync rate, 90 Hz
   with sweet90=3) instead of chasing selector 72: their notes show iOS 16's window server never asks for vsync and
   clocks itself from that property. `sweetfb_refresh=0` turns it off for A/B tests.
4. (01:55) First 0188 run (k0188-refresh-1121) was inconclusive (swipes 36.8 then 29.3 fps; 0187: 33.8), because
   5-6.5 of 8 cores were busy with background work: aggregated 100%, diagnosticd 90% + oslog 70% (an oslog log job is
   back, see memory: "don't run oslog -l as a boot job"), coreduetd 74%, callservicesd 49%. Decision: remove the
   oslog-* boot jobs and disable com.apple.aggregated (analytics only) with tools/hotdogb-launchd-job.py (reversible:
   `enable LABEL` / re-add the job). coreduetd and callservicesd left alone for now (could break things).
   Then A/B 0188 with and without sweetfb_refresh=0 on the quieter phone.
5. (01:50) Removed org.iosonpixel.oslog-loc (the Compass debug log job) and disabled com.apple.aggregated on the phone
   (tools/hotdogb-launchd-job.py; undo: `add oslog-loc ...` / `enable com.apple.aggregated`).
6. (01:55) Ported three cheap compatibility fixes, built (kernel 0191), **not booted yet**:
   0189 IOSurface maxima + plane row bytes (guacamole 0214/0215), 0190 WebKit memory limit to 2 GB not fatal
   (0142, `sweetwkmem=0` = off), 0191 processes keep CS_VALID on a bad page (0263; keychain for re-signed apps).
7. (02:00) My mistake: a second run started while the cleanup run's script still waited on fastboot, so two scripts
   drove the phone at once ("fastboot: error: Command failed"). After that, `fastboot getvar slot-unbootable:a`
   hung / answered "Variable Not found". Fix in tools/hotdogb-run.sh: both slot getvars now have a 10 s timeout.
8. (02:10) **The phone dropped off USB entirely** (no fastboot, adb or USB network, even after
   `fastboot reboot bootloader` x3). It probably needs the buttons (hold power + vol-up/down into fastboot) - that is
   the one thing I cannot do. Decision: keep porting with build-only checks and retry the phone every so often. All
   kernels built tonight but not booted are marked "not booted" in GUACAMOLE-PORT.md.
9. (02:25) **0192 present at swap** (guacamole 0150/0157, rewritten for our 0175 present thread: the swap wakes the
   thread instead of waiting for the next vsync tick). Built, **default OFF** (`sweetfbatswap=1` = on) because I
   could not measure it: an untested latency change should not become the default while you are away.
10. (02:30) Skipped as not needed here: guacamole 0238 GPU hang recovery (we already reset a stuck GPU: a6_recover,
   lost work counted as done), 0148 64 GB GPU VA (our Metal ABI already reports 39-bit VA), 0131 zero-copy surfaces
   (only the OpenGL/RedwoodGPU path; we run Metal).
11. (02:30) Skipped for now: the GPU layer-composition series (0169-0185, 0225). Their reason was partly transparent
   overlays falling back to a CPU blend; our 0188 run shows **0 CPU fallbacks** and ~1 ms GPU copies per frame, so
   there is nothing for it to fix yet. Revisit if "CPU fallbacks" ever shows up in the 5 s lines.
12. (02:35) **0193 Files app / Safari downloads** = guacamole 0265 + 0266 unchanged (they applied to our tree with an
   offset), built, not booted.
13. Open question for a phone run: in k0188-refresh-1121 the phone kept swapping ~11 frames/s while idle after the
   swipes (0187 idled at ~0). Could be 0188's refresh property or just something left animating; A/B with
   `EXTRA=" sweetfb_refresh=0"`.
14. (02:40) **0194** = guacamole 0143 (WebKit 'b3a8' tiles get one BGRA plane), **0195** = guacamole 0262 (CPU stand-in
   for AppleM2ScalerCSCDriver; our logs show iOS looking it up), both built, not booted. Skipped 0253 (CommCenter only).
15. (02:40) Not porting the alert slider tonight: the 7T's slider is two I2C hall sensors (DTBO fragments 122/123), not
   GPIO pins as on the 7 Pro - a new driver that needs the phone to calibrate. Haptics: the 7T DTBO also lists an
   AW8697 at 0x5A, so guacamole's haptics might apply after all - left for when the phone is back.
16. (02:20) Phone still off USB (no fastboot/adb/ping since 02:10). A watcher waits for it; when it shows up I boot
   kernel 0195 (all built patches) first, then A/B sweetfb_refresh=0 and sweetfbatswap=1. If it never comes back,
   0189-0195 stay untested: hold the buttons (Vol- + Power) to get to fastboot.
17. (08:40) Phone came back in fastboot at 08:40. Booted **kernel 0195** (k0195-all-1121): boots to SpringBoard, touch,
   Wi-Fi, battery all up, no panic. Seen working: 0189 (IOSURFACE LIMITS line), 0191 (bad code pages in chronod,
   contextstored, callservicesd allowed), 0195 (backboardd + SpringBoard find and open AppleM2ScalerCSCDriver, 0x0).
   Idle redraw is gone (0.2 fps idle), so the ~11 fps from entry 13 was not 0188.
   Not visible: the 0188/0192 lines, because the live log starts at ~54 s and they print earlier (the 0188 run missed them too).
   0190/0193/0194 only print when WebKit / Files run; not exercised. The swipebench gave no fps peak (max 7.6 fps in 5 s
   windows), so no fps number this run. The A/B tests did not fit before 10:00 and are still to do.
   run-current now points at kernel 0195.
18. (afternoon) A/B on kernel 0195, swipebench x2 each: 0188 on 38.7 fps, off 35.9; 0192 present-at-swap on 40.8.
   Both kept on: 0188 is the kernel default, sweetfbatswap=1 added to hotdogb-run-current.sh's boot-args (the kernel
   default stays off). One run per setting, so the numbers are rough.
19. hotdogb-run.sh: fastboot boot gets a 120 s limit and one retry after a bootloader restart (it refused or hung on
   about every other boot today). audiod set to ProcessType Interactive on the phone (pri 4 -> 31), crackle not yet re-heard.
20. **0196** = guacamole 0152 (touch poll rate) as a boot-arg, sweettouchms=N, default 8 ms (unchanged). At 4 ms one
   swipe run stalled the kernel's frame and GPU stats threads (vsync 764 ms late, no fps); at 8 ms the same kernel ran
   41 fps clean. Kernel 0196 rebuilt with the 8 ms default; run-current points at it.
