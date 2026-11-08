# iOS on Pixel 4 (flame) - working notes

Port of [ios-sweet](https://github.com/dipa-studio/ios-sweet) (XNU xnu-8796 + iPhone X iOS 16.7.16 userland on
a Redmi Note 10 Pro, SM7150) to the Pixel 4 (`flame`, SM8150). Read `docs/ios-sweet/NOTES.md`
for the history of ios-sweet's kernel patches (0001-0081); this file records what differs on flame.

What works now is in [README.md](README.md). [HANDOFF.md](HANDOFF.md) is the engineering log, newest
first: every boot, what broke and why. Paths under `work/`, `logs/`, `backup/`, `dtb/`, `stock/`,
`downloads/`, `images/` and `research/` in either file are local and not in git (see the README).

## Test loop (no buttons, no screen)

1. Phone in fastboot (`adb reboot bootloader` from TWRP).
2. `fastboot boot shim/out/shim-boot.img` (shim alone = probe; with a payload = XNU).
3. The shim sets the PM8150 PON reason to bootloader, so a reset lands in fastboot.
4. `fastboot boot downloads/twrp/twrp-3.7.1_12-0-flame.img`, then
   `tools/flame-log-pull.sh shim logs/X.txt` (and `xnu` for the XNU ring).
- `boot_a` holds TWRP (flashed 2026-10-19) so any reset without the PON reason boots TWRP
  (adb root), not Android. `boot_b` is stock. Restore everything with `images/.../flash-all.sh`.
- macOS asks "Allow accessory to connect?" the first time a new USB identity appears; until
  then `ioreg -p IOUSB` shows the phone `!registered`.

## Google ABL boot image rules (found the hard way)

`fastboot boot` fails with "Error verifying the received boot.img: ..." unless:
- the kernel is **LZ4 (frame) compressed** (uncompressed -> `Unsupported`),
- the Image header has **text_offset 0x80000** (0 -> `Invalid Parameter`),
- the image has an **AVB hash footer** (unsigned, algorithm NONE, padded to 64 MiB),
  `avbtool add_hash_footer --partition_size 67108864 --partition_name boot`.
Header v2, base 0, kernel 0x8000, ramdisk 0x1000000, tags 0x100, dtb 0x1f00000, page 4096,
stock `dtb` (two SM8150 base DTBs; ABL picks v2 and applies dtbo idx 11).
ABL loads the kernel at 0x80080000, the DTB at 0x84500000, the ramdisk at 0x84700000
(TWRP's 30 MB ramdisk went to 0xc0000000). max-download-size 256 MiB.

## Hardware facts (live FDT `dtb/live.dtb` + shim probe)

| | flame | (sweet) |
|---|---|---|
| CPU | 4x A55 (cpu0-3) + 3x A76 + 1x A76 prime, MPIDR 0x81000n00, MIDR 0x51df805e | 6+2 |
| RAM | <0x80000000 0x3cc00000>, <0xc0000000 0x140000000> (6 GB) | |
| GIC | GIC-600, GICD 0x17a00000, GICR 0x17a60000 stride 0x20000 | same |
| Timer | 19.2 MHz, vtimer INTID 19 | same |
| PSCI | 1.1 smc; CPU_ON, SYSTEM_RESET yes; RESET2, SYSTEM_SUSPEND no | |
| UFS | UFSHCI 3.0 @0x1d84000, LUN0 sda, userdata sda15 first 4K LBA 0x25e000, 0xc80ffb blocks | 2.1, same addr |
| Display | NT37280 cmd-mode DSC panel 1080x2280; ABL fb 0x9c400000 stride 4320 a8r8g8b8; CTL0, VIG0+VIG1 (540 each), LM0/1, DSC0/1, INTF1; **ABL enables INTF1 autorefresh** | video mode, 0x9c000000 1080x2400 |
| Touch | ST FTS (ftm5) on GENI SPI @0x890000 CS0, irq GPIO122, reset GPIO127, vdd L14A 1.8V, avdd L17A | Goodix I2C |
| Keys | power KPDPWR + vol- RESIN on PM8150 PON (SID0 0x800); vol+ PM8150 GPIO6 (SID0 0xc500), active low | vol+ PM6150L GPIO2 |
| PMIC arb | v5 @0xc440000, chnls 0xc600000, obs 0xe600000, cnfg 0xc40a000, EE 0, 512 APIDs | same |
| Reboot reason | PM8150 PON SOFT_RB_SPARE 0x88f bits [7:1] = 2 (bootloader) -> fastboot; Linux clears it to 0 before every reset; 0x10 (= WDT) at boot | IMEM ignored |
| Watchdog | APSS WDT 0x17c10000, 32765 Hz, ABL leaves it off; bite register 20 bits (~32 s max) | |
| USB | DWC3 @0xa600000, SNPS femto HS PHY 0x88e2000, QMP SS PHY 0x88e8000 | QUSB2 |
| ramoops | 0xa47ff000 (2 MB), alt 0xa49ff000 (TWRP /dev/access-ramoops) | |

DRAM partly survives resets (shim saw TWRP's kernel pointers at 0x100000000), but a marker
written to the alt ramoops region via /dev/access-ramoops did not survive reboot -> bootloader -> TWRP.

## Layout

- `shim/` flame shim (`build.sh`, Homebrew llvm + lld), `tools/` host tools, `phone/` code that runs in
  iOS on the phone (each directory has a `build.sh`; they use `work/userland/bin/ios-cc`)
- `xnu/patches/` ios-sweet 0001-0081 + flame patches from 0082
- `work/dxb/` blacktop darwin-xnu-build checkout; `work/dxb/xnu` is a git repo whose HEAD is
  "baseline + applied patches", new patches come from `git diff`
- Not for git: `backup/` (partition dumps, serials), `dtb/`, `stock/`, `downloads/` (TWRP,
  IPSW parts, AOSP tools), `work/`, `logs/`

## Flame patches

- 0082 IORPC.kernelContent: Xcode 16+ iig emits it; field added (kernel only) and set at the
  two user->kernel IOUserServer dispatch sites, as in newer XNU.
- 0083 flame basics: early console on the 0x9c400000 / 2280-row framebuffer (SWEET.h
  constants used by start.s, sweet_early.c, arm_vm_init.c), vol+ on PM8150 GPIO6, the Redmi
  touch driver off by default (sweettouch=1 forces it; it would vote L11C to 3.2 V), APSS
  watchdog keepalive in SweetPlatformExpert (flamewdt=0 off, flamereboot=N stop after N s).
- 0084 early framebuffer block mapped Device-GRE, not Normal-NC (no speculative reads into pil_slpi).
- 0085 ST FTS touch (ftm5) over GENI SPI; 0086 its HW clock gating + cpufreq for all 8 cores.
- 0087 fixed-rate vsync + fps stats. 0088 flame USB offsets.
- 0089 log timestamps, RAM mirror of the console ring, poke scripts; with 0090: USB through the apps SMMU
  the way ABL does it (own context bank, translation off) and the UDP command server.
- 0091 USB ARP fixes, TCP file server, `sync` command (0092-0093 included).
- 0094 /dev/pmem (physical memory and SCM calls for userland drivers), file server READ.
- 0095 the A640 core in the kernel, GPU present (frames scaled to fit by the GPU).
- 0096 SweetAccelerator (IOAcceleratorES for Metal, `sweetmtl=1`); 0097 its user client.
- 0098 panic logs into the persistent log ring. 0099 PM8150 RTC time of day (`sweetrtcoff`).
- 0100 clonefile copy fallback on HFS, tap/swipe commands (0101 included).
- 0102 RPMh ARC/VRM votes from userland (PMEM_RPMH). 0103 AMFI stand-in answers trust cache queries.
- 0104 planar IOSurfaces report summed bytes-per-row/element (blank Safari pages).
- 0105-0109 GPU: VA reuse, more page tables, perf vote (`sweetgpufreq`), recovery, async submit.
- 0110-0112 Apple continuous display corners (final: 0112, GPU scissored bands + CPU row masks).
- 0113 wait for a client's last fence before unmapping its memory.
- 0114 the AMFI policy answers dyld's policy query: @rpath frameworks of third-party apps load.
- 0115 v2 GPU user client (NativeAdreno ABI) for Turnip: GPU buffers from RAM XNU does not manage
  (no cacheable physmap alias), multi-IB submits, progress-based hang detection.
- 0116 `sweetmtl=2` selects SweetMetal2 (metal2vulkan + Turnip). 0117-0118 pool accounting, submit timing.
- 0119 present not waited for. 0120 per-process GPU stats, BCM (bus/DDR) votes over PMEM_RPMH.
- 0121 vsync notifications carry the refresh period (CA's frame deadlines were always past).
- 0122 DDR + memory NoC voted up at GPU start (`sweetddr`), GPU pool caps (`sweetgpucap`, `sweetgpureserve`).
- 0123 en0 + Apple80211 for wifid (ios-topaz 0900 + 0128, from HackiOS), the station over /dev/wlan to
  wland in userland.
- 0124 10-bit surfaces as BGRA only for WebContent (`sweetsurf10`); the joined network keeps its scan record.
- 0125 hotdogb board, 0126 S3706 touch (OnePlus 7T; HOTDOGB.md).
- 0127 hotdogb: S3706 SE left GSI-only by Android is switched to FIFO mode (`hdtouchfifo=0` keeps it).
- 0128 hotdogb: USB gadget as CDC-NCM (IAD, MS OS "WINNCM", NTB16) so Windows' inbox UsbNcm binds;
  `sweetusbncm=0|1` picks ECM/NCM (hotdogb default NCM, flame ECM). Works: Windows binds UsbNcm.sys.
- 0129 hotdogb: 0127 undone (any access to SE + 0x2000.. resets the phone: TrustZone); GSI-only SE = stop.
- 0130 hotdogb: S3706 touch over GPI DMA (GSI mode) through qcom,gpi-dma@0xc00000 GPII 1: polled
  channel/event rings, I2C config0/go/dma TREs, apps-SMMU bypass for stream 0x7b6. `hdtouchgpi=0` reverts
  to the 0129 stop. Register/command model from Linux gpi.c + i2c-qcom-geni.c; logic host-tested.
- 0131 hotdogb: GPI DMA buffers above 4 GiB (the DMA block can land there) - carry the high word in the
  TRE dword1 and drop the <4 GiB bail; ring CNTXT/doorbell already 64-bit. (Kernel 0130 bailed on this.)
- 0132 hotdogb: GPIDBG step markers in GPI setup (hdtouchlog=1, each waits past a 2 s log flush). Showed the
  reset is in s3_gpi_smmu (fresh SMR + context bank for stream 0x7b6).
- 0133 hotdogb: GPI stream 0x7b6 shares USB's translation-off SMMU bank (`hdgpismmu=1` default; 2 = own bank,
  0 = none) and logs the SMMU state/chosen smr+cb with step markers. (Kernel 0133: still resets, no marker.)
- 0134 hotdogb: sweet_log_kick() (sweet_ufs.c) flushes the console log on demand; GPIDBG kicks + waits 2.5 s;
  finer markers through s3_gpi_smmu (enter, mapped, idr read, every 16 SMRs, scan done).
- 0135 hotdogb: cross-check vs OnePlus/LineageOS S3706 + Linux GENI/GPI: GPI SE index 0 (was 3, read from the
  wrong DT node), I2C 400 kHz timing 5/12/24 as Linux, 12-bit coordinates, F01 device-status logging.
- 0136 hotdogb: GPI stream 0x7b6's SMR is written by sweet_usb.c (sweet_usb_smmu_share) through USB's SMMU
  mapping onto its translation-off bank; kernel 0135 reset on the touch driver's own map of 0x15000000,
  and with hdgpismmu=0 the whole GPI setup ran (reset only at the first DMA, stream unmatched).
- 0137 hotdogb: panic-time console-log flush - sweet_log_panic_flush() (polled UFS, no mutex) called from
  SavePanicInfo and paniclog_flush (osfmk/arm/model_dep.c), so panics land in rawdump (7T RAM is lost).
- 0138 hotdogb: GPI channel START debug - register dumps around TX/RX START, `hdgpistart=0` (stop before),
  `hdgpipanic=1` (test panic before START). Kernel 0137 reset right after "ch ring".
- 0139 hotdogb: GPI I2C TREs as Linux (one message per transfer, read GO with link_rx|IEOB, IEOT on the
  write DMA); trace of the first 3 transfers and the touch reset release. Kernel 0138: START ok, hardware
  reset after it; hdgpipanic=1 proved the 0137 panic capture.
- 0140 hotdogb: RPMh BCM votes QUP0 + SN11 (QUP2 master path to DDR) before the touch SE/GPI is used
  (`hdgpibcm`). Kernel 0139 reset on the first GPI TX doorbell (first GPI memory access).
- 0141 hotdogb: GPI event ring as Linux (WP primed base+len-16, advanced per consumed event, doorbell + IEOB
  clear only after events), wait traced. Kernel 0140: votes fixed the doorbell reset; reset in the wait.
- 0142 hotdogb: GPI DMA block below 4 GiB (`hdgpilow`). Kernel 0141 reset within ms of the first TX
  doorbell (0140's extra line was flush timing), i.e. on the GPI's first TRE fetch from 0x1_049xxxxx.
- 0143 hotdogb: GPI DMA block at a fixed unmanaged PA below 4 GiB (`hdgpilowpa`, default 0xf0000000),
  write-combining. Kernel 0142 found no XNU RAM below 4 GiB (highmem layout) and stopped before the GPI.
- 0144 hotdogb: `hdgpiprobe` 1-3 isolates the first-doorbell reset (empty TX doorbell, RX TRE alone,
  CONFIG0 alone) and logs the apps SMMU global fault registers. Kernel 0143 reset at the same doorbell.
- 0145 hotdogb: synchronous log flush (`sweet_log_sync`), GPI/SE debug dumps, probes 4 (real first write,
  fast synced trace) and 5 (CONFIG0 + GO, no DMA), `####` milestone lines. 0144 probes 1-3 all survived;
  the GPI never moved a read pointer.
- 0146 hotdogb: SMR for the QUP wrapper 2 data stream 0x7a3 (I2C buffers), shared onto the USB bank
  (`hdqupsid`). Kernel 0145 probe 5 (no DMA TRE) worked; probe 4 (DMA TRE) reset.
- 0147 hotdogb: GPI debug pauses only with `hdgpidbg=1` (fast touch start-up), multi-touch and HID
  milestones (`####`). Kernel 0146: touchscreen works over GPI DMA, 443 reports, HID events sent.
- 0148 hotdogb: read-only raw partition reads over the file server (`PARTS`, `PART name off len`; any
  GPT partition on LUN 0-7), writes refused off the home LUN. For firmware without Android/TWRP.
- 0149 hotdogb: zap shader region 0x99515000 (dtbo overlay 3); `zap: region` logged.
- 0150 GPU present: after a CPU-copy frame (multi-layer), the next GPU frame clears the panel outside its
  fitted rectangle (stale status bar/home indicator in the margins); CPU fallbacks counted per 5 s.
- 0151 GPU present fills the panel (`sweetfb_fill`, default 1; 0 = fit with bars): scaled by the larger factor,
  overhang cropped (hotdogb: source columns 14..1109 -> 1080x2400), touch mapped through the whole frame's
  rectangle (-14,0 1109x2400). v2 clients: the first `sweetgpuforgive` (default 3) GPU resets that lost their
  work are not reported as faults (Turnip's DeviceLost froze backboardd for good).
- 0152 hotdogb panel brightness (after ios-topaz 0104/0105): DCS 0x51 (10-bit, high 2 bits first) through
  DSI0's command DMA (0xae94000), packet in ABL's display carve-out (0x9e300000; kernel RAM is above 4 GiB);
  AppleARMBacklight (backlight-control) for CoreBrightness; IOMFBBrightnessLevel nits 16.16 -> 1..0x3ff at
  500 nits; level 0 while the display is off. Boot-arg `sweetbl` (default 1 with sweetmtl, else 0).
- 0153 framebuffer and backlight setProperties: every key logged (`SWEET BL: PROC iomfb|backlight set KEY`, first
  4 per key then every 200th), stored and accepted (0152 refused all but IOMFBBrightnessLevel, unlogged; with the
  backlight service the UI fell to 0.2-0.4 fps and no brightness arrived).
- 0154 `sweetbl=2` publishes AppleARMBacklight; the default 1 only darkens the panel while the display is off
  (0153: CoreBrightness set no key at all, and the service alone still caused the 0.2-0.4 fps).
- 0155 AppleARMBacklight gets IODisplayParameters {brightness {min 0, max 65536, value}} and the ADT /backlight
  node's properties: CoreBrightness's DisplayCreateWithID failed without IODisplayParameters ("failed to create
  DisplayRef"), so libEDR's server never started and CA's EDRClient stalled every frame. A set IODisplayParameters
  brightness value (0..65536) maps to 0..500 nits. Still only with sweetbl=2.
- 0156 brightness slider: CoreBrightness sets "backlight-level" (= "brightness") 0..65536 of its hardware maximum
  per slider step (slider 0.505 -> 10709): mapped linearly to panel level 1..0x3ff; the framebuffer's
  IOMFBBrightnessLevel (it sets 0 there) is ignored while the service exists. The service is now the default
  with sweetmtl (sweetbl default 2; 1 = panel off/on only, 0 = nothing).
- (0157/0158 dropped: 0157 swapped the hotdogb volume keys on a misread, 0158 swapped them back; both patches were
  removed. 0156 is right: GPIO6 = up, GPIO7 = down; iOS screenshot = side + volume UP.)
- 0157 (panel-90hz; reuses the number) 90 Hz: boot-arg sweet90=1 sends the 7T panel's timing switch over DSI0 DCS
  (F0 5A 5A, 60 10, F0 A5 A5: dtbo sofef03f_m 90 Hz timing, same DSI clock) and runs vsync every 11111 us; the
  5 s stats follow the rate. Needs DSI0 mapped (sweetbl >= 1, the default with sweetmtl).
- 0158 90 Hz froze the picture (panel at 90, INTF_1's tear check still counting for 60: the next TE came before
  the start position): vsync_count (INTF_1 0xae6b800 + 0x284, bits 18:0) scaled by 60/90 if plausible (50..1000);
  sweet90 is a bit mask now: 1 = panel + tear check, 2 = vsync timer, 3 = both.
- 0159 sample bursts: USB command "sample NAMES COUNT MS" samples the running (not waiting) threads of NAMES
  every MS ms (SMP name tN tid T pc ... bt ...; header once); tools/hotdogb-swipebench.py --sample triggers it
  during the swipes, tools/sweet-profile.py counts busy threads / hot pcs, tools/sweet-crash.py symbolicates.
