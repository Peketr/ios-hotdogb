# Porting from ios-guacamole (OnePlus 7 Pro)

Source: https://github.com/coreycusick/ios-guacamole/tree/ios16 (commit 29e3cf3, 2026-11-20, MIT,
retired upstream). Same SM8150 / Adreno 640 and same ios-sweet lineage as ours, so kernel work
mostly carries over. Patches live in `xnu/patches/`, notes in `docs/guacamole.md` and `NOTES.md`.
Get it again with: `git clone --depth 1 -b ios16 https://github.com/coreycusick/ios-guacamole`

Our hotdogb series ended at 0186 when this list was written; ported patches get new numbers
from 0187 onward. Mark each item done (with our patch number) as it lands. [x] done+tested, [~] built (see note), [-] skipped (reason).

## Priority 1: performance (display / GPU)

- [x] Stats first, so gains are measurable: 0151 fb-frame-stats, 0153 fb-protocol-stats,
      0155 fb-gpu-busy-stat -> our 0187 (fps / copies / vsync-late were already in 0087/0175; added the
      swap-gap histogram + longest gap, vsyncs sent, IOMFB selector counts, GPU busy % of vsyncs as a second
      "SWEET FB: 5s gaps:" line; buckets in 90 Hz periods)
      First run k0187-fstats-1121b (2026-11-21, swipebench 33.8 fps): swap gaps spread over <23/<34/<50 ms,
      GPU busy ~40% of vsyncs, and **vsyncs sent 0**: backboardd never calls selector 72 (kSweetFBNotifications),
      so CoreAnimation gets no vsync callback and paces frames itself. Next: find why CA skips 72 (a missing
      IOMFB property or capability?) before the composition work; vsync-paced frames may be the bigger win.
- [x] Present at swap: 0150/0157 -> our **0192** (swap wakes our 0175 present thread). A/B 2026-11-21 on kernel 0195:
      40.8 fps with `sweetfbatswap=1` vs 38.7 without (ab-0192-on-3 vs ab-0188-on-1); on in hotdogb-run-current.sh. 0149 (GPU 2D present) not needed: we present on the GPU.
- [x] **0188 IOMFBDisplayRefresh** (guacamole 0154): A/B 2026-11-21 on kernel 0195: 38.7 fps on vs 35.9 with
      `sweetfb_refresh=0` (ab-0188-on-1 vs ab-0188-off-2). On by default.
- [-] Skipped for now (0 CPU fallbacks in our runs, nothing to fix) - GPU layer composition: 0169 frame-handover-sync, 0170 compose-layers, 0171 layer-order-top-only,
      0172 two-layers-on-gpu, 0183 black-frame-layout, 0184 layers-persist, 0185 higher-layer-on-top,
      0225 top-layer-edge-bands (removes the CPU blend fallback that costs frame rate)
- [-] 0131 zero-copy surfaces: skipped, OpenGL/RedwoodGPU path only (we run Metal)
- [-] 0147/0148: skipped, we have the governor + 675 MHz (0170/0179) and 39-bit GPU VA already
- [-] 0238 gpu-hang-recovery: skipped, ours already resets a stuck GPU (a6_recover)
- [x] 0152 touch 4 ms / 400 kHz -> our **0196** (`sweettouchms=N`). Our S3706 already ran 400 kHz and polled every
      8 ms. 4 ms stalled the frame/GPU stats threads mid-swipe (ab-0196-4ms-1); 8 ms gave 41 fps (ab-0196-8ms-1),
      so the default stays 8 ms.
- [-] 0156 big-cores-and-cpu-clocks: we already set CPU clocks (0086 flame-cpufreq, 0170 governor, 0173 big cores first)
- [-] 0240/0241 fb-scale2 (@2x, 828x1792): skipped. Guacamole dropped it themselves: UIKit showed the red
      rdar://45025538 status bar at both @2x sizes, and the MobileGestalt cache has to be moved aside for it.
- [ ] guacamole userland "0242" Metal frontend per-draw overhead: of it, only the cull/winding/fill part applies to
      our phone/metal2 (associated NSNumbers + nmb_render_set_raster on every set; we have no per-draw tracing).
      Small gain; needs a Mac build (xcrun, work/m2v-bridge, work/turnip). 0145/0146/0122 checked: not needed here.

## Priority 2: app compatibility

- [x] 0214 + 0215 -> our **0189**, booted in k0195-all-1121 (limits line seen)
- [~] 0142 -> **0190**, 0143 -> **0194**, booted in 0195 but WebKit not exercised yet; 0141 skipped (guacamole dropped that mode later)
- [ ] 0201/0202 sandbox container paths / by identity
- [x] 0263 -> **0191**, booted: bad pages in chronod/contextstored/callservicesd allowed
- [~] 0265 + 0266 -> **0193** (unchanged), booted in 0195, Files app not opened yet
- [ ] 0144-0146, 0174/0175 Metal2/Turnip fixes: diff against what METAL-NEXT.md already has

## Priority 3: hardware features

- [ ] Audio: IOAudio2 stand-in 0226-0235, 0254 + `userland/audio` (HakiosAudio.driver, MIT);
      only BT A2DP works there. ADSP/amp notes in docs/guacamole.md "Audio"
- [ ] Haptics 0248-0258 (AW8697): the 7T DTBO *does* list aw8697_haptic@5A (fragment 101), but our sensord vib.c
      drives PM8150B haptics: find out which one actually moves the motor before porting
- [ ] 0264 alert slider: NOT a port on the 7T - it has two I2C hall sensors (MXM1120 / IST8801, irq GPIO 10 and 36),
      not three GPIO pins; needs a new I2C driver + calibration on the phone
- [ ] 0197 AppleARMBacklight + `sweet-displayd` (Settings brightness slider, Night Shift);
      compare to our 0152-0156 backlight
- [-] 0253 AppleIPAppender: skipped (only CommCenter needs it; ours is off)
- [~] 0262 M2Scaler CPU stand-in -> **0195**, booted: SpringBoard opens it (0x0)

## Skip

Shim, S6SY761 touch (0085-0087), guacamole panel geometry (0082/0083), 90 Hz (we have 0157/0158),
Wi-Fi / Bluetooth / battery (we have our own).
