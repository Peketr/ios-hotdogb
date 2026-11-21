# Porting from ios-guacamole (OnePlus 7 Pro)

Source: https://github.com/coreycusick/ios-guacamole/tree/ios16 (commit 29e3cf3, 2026-11-20, MIT,
retired upstream). Same SM8150 / Adreno 640 and same ios-sweet lineage as ours, so kernel work
mostly carries over. Patches live in `xnu/patches/`, notes in `docs/guacamole.md` and `NOTES.md`.
Get it again with: `git clone --depth 1 -b ios16 https://github.com/coreycusick/ios-guacamole`

Our hotdogb series ended at 0186 when this list was written; ported patches get new numbers
from 0187 onward. Mark each item done (with our patch number) as it lands.

## Priority 1: performance (display / GPU)

- [ ] Stats first, so gains are measurable: 0151 fb-frame-stats, 0153 fb-protocol-stats,
      0155 fb-gpu-busy-stat
- [ ] Present at swap / on GPU 2D: 0149 present-on-gpu-2d, 0150 present-at-swap,
      0157 fb-at-swap-toggle (fit onto our 0175 present-thread)
- [ ] GPU layer composition: 0169 frame-handover-sync, 0170 compose-layers, 0171 layer-order-top-only,
      0172 two-layers-on-gpu, 0183 black-frame-layout, 0184 layers-persist, 0185 higher-layer-on-top,
      0225 top-layer-edge-bands (removes the CPU blend fallback that costs frame rate)
- [ ] 0131 zero-copy surfaces (+ 0132 wrap-offset / fb timing)
- [ ] 0147 gpu-full-power, 0148 gpu-va-64gb (compare to our 0170 governor / 0179 675 MHz)
- [ ] 0238 gpu-hang-recovery (+ 0176/0177/0179 hang reports)
- [ ] 0152 touch 4 ms / 400 kHz: adapt to S3706 on QUP 0xc80000
- [ ] 0156 big-cores-and-cpu-clocks: compare to our 0172/0173
- [ ] 0240/0241 fb-scale2: check if a 2x mode suits our 1080x2400 panel

## Priority 2: app compatibility

- [ ] 0214 iosurface-limits-maxima, 0215 iosurface-plane-row-offset
- [ ] 0141 webkit-gpu-keeps-10bit, 0142 webkit-memlimit, 0143 webkit-b3a8-one-plane
- [ ] 0201/0202 sandbox container paths / by identity
- [ ] 0263 cs-keep-valid
- [ ] 0265/0266 Files app (fpfs syncroot on HFS, placeholder ioctl)
- [ ] 0144-0146, 0174/0175 Metal2/Turnip fixes: diff against what METAL-NEXT.md already has

## Priority 3: hardware features

- [ ] Audio: IOAudio2 stand-in 0226-0235, 0254 + `userland/audio` (HakiosAudio.driver, MIT);
      only BT A2DP works there. ADSP/amp notes in docs/guacamole.md "Audio"
- [ ] Haptics 0248-0258 (AW8697): first confirm the 7T's haptic chip from its DT
- [ ] 0264 alert slider as ringer switch: our hall-sensor GPIOs differ
- [ ] 0197 AppleARMBacklight + `sweet-displayd` (Settings brightness slider, Night Shift);
      compare to our 0152-0156 backlight
- [ ] 0253 AppleIPAppender stub, 0262 M2Scaler CPU stand-in

## Skip

Shim, S6SY761 touch (0085-0087), guacamole panel geometry (0082/0083), 90 Hz (we have 0157/0158),
Wi-Fi / Bluetooth / battery (we have our own).
