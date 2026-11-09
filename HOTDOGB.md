# OnePlus 7T (hotdogb) bring-up

Port of ios-flame to the OnePlus 7T (HD1901, `hotdogb`, SM8150+, 8 GB). Same SoC as the
Pixel 4; everything the shim touches turned out to be laid out the same way except where
noted. Source: live FDT, /proc/iomem and the shim probe log of 2026-11-05.

## Status

- `fastboot boot` of the shim: works (OnePlus ABL, unlocked, no AVB footer needed).
- Shim console on the splash framebuffer: works.
- PMIC (SPMI) + PON reboot reason: works, every reset lands in fastboot.
- UFS reads/writes: works; logs go to `rawdump` (see below).
- RAM 0x1_00000000-0x1_fff00000 read/write checked from the shim.
- XNU 0126: boots to the root mount (2026-11-06, `tools/hotdogb-run.sh` from WSL; HANDOFF.md
  "HOTDOGB: kernel 0126 boots"). S3706 touch stops (SE FIFO interface disabled; 0127), the
  Windows host has no CDC-ECM driver (0128: NCM). The kernel's console log (with `rd=ufsroot0`)
  lands at the end of rawdump.

## Build and test (no Mac needed for the shim)

    shim/build-hotdogb.sh STOCK_DTB          # clang/lld from Linux or WSL
    tools/mkadt.py OUT.adt --hotdogb         # ADT: high-mem layout, 1080x2400 fb

STOCK_DTB is the dtb section of the phone's own boot.img (20 SM8150 DTBs; ABL picks
dtb_idx 8, dtbo_idx 3). `tools/mkboot_hotdogb.py` packs a header-v2 image like stock:
kernel with Qualcomm's `UNCOMPRESSED_IMG` + le32 size prefix (no LZ4), base 0, kernel
0x8000, ramdisk 0x1000000, tags 0x100, dtb 0x1f00000, page 4096.

## Differences from flame

| | flame (Pixel 4) | hotdogb (OnePlus 7T) |
|---|---|---|
| Boot image kernel | LZ4 frame, AVB footer required | `UNCOMPRESSED_IMG` prefix, no footer |
| ABL load addresses | kernel 0x80080000, DTB 0x84500000, rd 0x84700000 | kernel 0x80080000, DTB 0x854fd000, rd 0x856fe000 |
| max-download-size | 256 MiB | 768 MiB |
| RAM | 6 GB | 8 GB: <0x80000000 0x3bb00000>, <0xc0000000 0xc0000000>, <0x1_80000000 0x1_00000000>; 0xc0000000-0x27fffffff free of carve-outs |
| Panel | NT37280 cmd DSC 1080x2280 | Samsung sofef03f cmd DSC 1080x2400 |
| Splash fb | 0x9c400000 | 0x9c000000 (stride 4320, a8r8g8b8) |
| SDE path | CTL0, VIG0+VIG1 540 wide, LM0/1, DSC0/1, INTF1, ABL autorefresh on | identical (autorefresh 0x80000001 left on by ABL) |
| Touch | ST FTS (ftm5), GENI SPI 0x890000 | Synaptics S3706, GENI I2C 0xc80000 addr 0x20; irq GPIO122, reset GPIO80, 1.8 V enable GPIO119, vdd 2.8 V L17A |
| Vol+ | PM8150 GPIO6 | PM8150 GPIO6 (same) |
| Vol- | PON RESIN | PM8150 GPIO7 (gpio-keys) |
| Extra | | alert slider (hall sensors, tri-state key) |
| userdata | sda15, LBA 0x25e000 | sda22, LBA 0x475676, ext4 over dm-default-key filling the partition |
| Log scratch area | end of userdata | end of `rawdump` (sda21, LBA 0x46d676, 32768 blocks) |

Same as flame: MIDR 0x51df805e, PSCI 1.1 (CPU_ON + SYSTEM_RESET, no RESET2/SUSPEND),
GIC-600 at 0x17a00000/0x17a60000, PMIC arbiter v5 (512 APIDs), PON at SID0 0x800 with
SOFT_RB_SPARE 0x88f, APSS WDT 0x17c10000, UFSHCI 3.0 at 0x1d84000, IMEM restart reason.

## Reading the shim log

`hotdogb/run-shim.bat` (Windows) boots the shim, waits for the reset into fastboot,
boots Android and pulls the end of rawdump with root (Magisk): the shim log is the 64
blocks before the last 1025 (`shim/ufs.h`).

## Kernel patches

One tree builds either phone: `SWEET_HOTDOGB` in `pexpert/pexpert/arm64/SWEET.h` (default 1
after 0125; set 0, or build with `-DSWEET_HOTDOGB=0`, for flame).

- **0125 hotdogb board**: early console / `SWEET_EARLY_FB_*` at 0x9c000000, 1080x2400;
  `SWEET_PANEL_W/H` used by the GPU present (`sweet_gpu.c`), touch mapping (`SweetHID.cpp`)
  and `A6_FB_PA` (`sweet_a6xx.h`; the userland GPU tool takes `-DSWEET_HOTDOGB=1`);
  corner radius default 112 (117 x 1080/1125: the iPhone X frame fits by width,
  1080x2338 at y 31); volume down on PM8150 GPIO7 (read-only, withheld unless the GPIO
  reads as an enabled input, like GPIO6); console log ring to the end of `rawdump`
  through its own bounded writer (works with a read-only root; flame unchanged).
- **0126 S3706 touch** (`bsd/dev/sweet_s3706.c`): GENI I2C on QUPv3 2 SE0 (0xc80000, XBL
  firmware, FIFO mode, polled, 400 kHz from XO), wrap2 clock votes (0x52014 bits 0-4,
  RCG 0x1e148), TLMM south pins 55/56 (qup17), reset 80, 1.8 V switch 119, attention 122,
  L17A 3.008 V; RMI4 PDT scan, F01 configured + F12 interrupt enables, F12 data1 located
  from the register descriptors, objects reported to `sweet_hid_touch` in panel pixels.
  Replaces the FTS driver on hotdogb (its SPI SE at 0x890000 is an I2C bus on the 7T).
  Boot-args: `hdtouch=0`, `hdtouchdelay=N` (default 20 s), `hdtouchlog=1`.

- **0127 S3706 FIFO mode**: kernel 0126 found the SE with I2C firmware (0x303) and 32-bit FIFOs
  but GENI_IF_DISABLE_RO bit 0 set (GSI-only, Android uses GPI DMA). The driver now clears
  SE_FIFO_IF_DISABLE (SE + 0x2008, the register Linux's GENI firmware loader programs) and checks
  the read-only mirror again. **Result (kernel 0128): any access to SE + 0x2000.. resets the
  phone at once (TrustZone-protected), reads included.** Undone by 0129.
- **0129 S3706 no FIFO switch**: a GSI-only SE is a logged stop ("touch needs GPI DMA"), never
  an access to SE + 0x2000... Touch on the 7T needs a GSI/GPI DMA path.
- **0130 S3706 over GPI DMA**: polled GPI/GSI I2C client (qcom,gpi-dma@0xc00000 GPII 1, from the
  live FDT): event + TX/RX channel rings in one contiguous DMA block, apps-SMMU bypass for stream
  0x7b6, SE put in GSI mode (regs below SE+0x2000 only), I2C config0/go/dma TREs. Replaces the FIFO
  s3_msg path when the SE is GSI-only. Model from Linux gpi.c + i2c-qcom-geni.c. `hdtouchgpi=0`
  reverts to the 0129 stop. Logic host-tested (TRE layout, ring wrap, event parse). Kernel 0130 ran
  (no reset) and reached the SE read, then bailed at a >4 GiB DMA-address guard.
- **0131 GPI 64-bit DMA**: the contiguous DMA block can land above 4 GiB (RAM is), so carry the high
  word in the DMA TRE dword1 and drop the bail (the engine is 64-bit, as the UFS driver relies on).
- **0132 GPI step markers** (`hdtouchlog=1`): kernel 0132 showed the reset is in the touch SMMU setup.
- **0133 GPI SMMU shares USB's bank**: stream 0x7b6 gets its own SMR but points at the translation-off
  bank USB already set up, instead of writing a fresh bank (`hdgpismmu=2` old way, `0` none).
- **0134 log kick + finer SMMU markers**: GPIDBG wakes the log thread so each marker reaches disk.
- **0135 cross-check fixes**: GPI SE index 0 (touch = qupv3_se17_i2c, `dmas = <&gpi_dma2 ch 0 3 64 0>`),
  Linux 400 kHz I2C timing (div 2, t_high 5, t_low 12, t_cycle 24), 12-bit x/y, F01 device status.
- **0136 GPI SMMU via USB**: sweet_usb.c adds the touch stream's SMR through its own SMMU mapping
  (`hdgpismmu=3` default); the touch driver no longer maps 0x15000000 itself.
- **0137 panic log to disk**: at panic, the console ring (panic string + backtraces, via 0098) is written
  to rawdump synchronously; hooks in osfmk/arm/model_dep.c SavePanicInfo / paniclog_flush.
- **0138 GPI START debug**: dumps around channel START; `hdgpistart=0` stop before it, `hdgpipanic=1`
  deliberate panic there (tests 0137).
- **0139 GPI I2C TREs as Linux + transfer trace**: one message per transfer (read GO link_rx|IEOB, write
  DMA IEOT), first 3 transfers traced with GPII dumps after the doorbell.
- **0140 QUP2 bus votes**: BCM QUP0 + SN11 ACTIVE votes (`hdgpibcm=N`, 0 = off) for the GPI's path to DDR.
  Kernel 0140: fixed the reset on the first TX doorbell.
- **0141 GPI event ring WP**: event doorbell carries the recycled WP (as Linux), not the RP; wait traced.
- **0142 GPI DMA below 4 GiB** (`hdgpilow=1` default): Linux only gives the GPI 32-bit IOVAs.
- **0143 GPI DMA fixed low block** (`hdgpilowpa`, default 0xf0000000): XNU has no RAM below 4 GiB here
  (0142's allocation failed), so the block is unmanaged DDR mapped write-combining; doorbells are 32-bit.
- **0144 GPI doorbell probes** (`hdgpiprobe=1|2|3`, default 0): empty TX doorbell / RX DMA TRE alone /
  CONFIG0 alone, each followed by GPI RP/WP and SMMU global fault dumps; the touch thread then stops.
- **0145 GPI fast trace** (`hdgpiprobe=4|5`): the real first write / CONFIG0 + GO only, with the log synced
  to disk (`sweet_log_sync`) before the doorbell and after each dump; GPI PC/QSB and SE status in every dump.
  Milestones in the log start with `####`.
- **0146 QUP2 data stream** (`hdqupsid=1` default): the wrapper's SE DMA reads/writes the I2C buffers on
  SMMU stream 0x7a3 (DT qcom,iommu_qupv3_2_geni_se_cb), not the GPI's 0x7b6; both now map to the bank.
  Kernel 0146: the touchscreen works (PDT, F12 10 objects, reports into SweetHID).
- **0147 touch start-up / multi-touch / HID**: `hdgpidbg=1` brings back the 2.5 s debug pauses; `####`
  lines for first touch, max fingers down, HID service opened by iOS and first HID touch event.
- **0148 raw partition reads**: the file server's `PARTS` lists every GPT partition on LUN 0-7, `PART
  name offset len` streams one read-only (`flame-files.py parts` / `part NAME LOCAL`). sweet_ufs.c moves
  su.lun only inside these reads, under its lock; writes are refused unless su.lun is the home LUN 0, and
  the panic flush resets it. `tools/hotdogb-fw-pull.py` uses it for the modem firmware and EFS partitions.
- **0149 GPU zap region**: 0x99515000 on hotdogb (dtbo overlay 3 moved pil_gpu_region); with `hotdogb-run.sh -G`
  (sweetgpu=1 + the 7T's own GPU firmware from vendor_a) the A640 presents every frame (run k0149-gpu-1107-1336).
- **0150 GPU present margins**: multi-layer frames fall back to the CPU copy, which crops 1125x2436 to the
  panel and writes the bands outside the GPU's fitted 1080x2339 (status bar, home indicator); GPU frames never
  repainted them (double home indicator, seen in a panel capture). Now the next GPU frame clears them.

Reference sources for the touch path (read-only, not vendored):
- OnePlusOSS/android_kernel_oneplus_sm8150 `oneplus/SM8150_R_10.0`: arch/arm64/boot/dts/qcom/sm8150-qupv3.dtsi
  (qupv3_se17_i2c), sm8150.dtsi (gpi_dma2 @0xc00000, gpii-mask 0xfa, iommus 0x7b6, smmu-cfg 1),
  sm8150-oem.dtsi + hotdogb.dtsi (synaptics-s3706@20, reset GPIO80, attn GPIO122 0x2008, L17A 3.008 V,
  1.8 V via GPIO119 pull-up), drivers/dma/qcom/gpi.c, drivers/i2c/busses/i2c-qcom-geni.c.
- LineageOS/android_kernel_oneplus_sm8150: drivers/input/touchscreen/oplus_touchscreen/Synaptics/S3706/
  (synaptics_get_chip_info: PDT at 0xDD/0xE3/0xE9 page 0, F51 page 4; get_touch_points: attention word
  at F12 data15 = data base + 3, then 8 bytes/finger from data base; trigger_reason: F01 data word,
  status byte non-zero = exception, irq bit 0x04 = touch; reset 10 ms low, 80 ms after).
- **0128 USB NCM**: the Windows host had no driver for the CDC-ECM gadget. On hotdogb the gadget
  is now CDC-NCM: device class EF/02/01 + IAD (Windows' composite driver enumerates the
  function), MS OS 1.0 string 0xEE + extended compatible ID "WINNCM" (inbox UsbNcm),
  bcdDevice 2.00 (Windows caches OS descriptors per VID/PID/bcdDevice), NTB16 framing (one
  datagram per IN block, any number per OUT block, 2 KiB blocks), speed-change + connect
  notifications. macOS and Linux drive NCM as well. `sweetusbncm=0` brings back ECM.
  The framing, descriptors and parser were unit-tested on the host (not yet on the phone).

### Building on the Mac (tools/mac-build.sh)

    cd ~/gitrepos/ios-hotdogb && git pull && tools/mac-build.sh
    # first time only, with a tree that already has everything through 0141:
    #   tools/mac-build.sh --applied-through 0141

It applies every xnu/patches/NNNN-*.patch the work/dxb/xnu tree does not have yet (recorded in
work/dxb/applied-patches; a patch already in the tree is detected and recorded, one that does not apply
stops the run), builds with tools/build-xnu.sh (log work/build-NNNN.log), checks the kernel for each
applied patch's string from tools/mac-build-check.txt, and copies it to work/out/kernel-NNNN.sweet -
the file to upload. `--dry-run` shows what it would apply. Add a check line for every new patch.


(0127-0128 are already in a tree built for kernel 0128; a fresh tree needs 0127 0128 0129. A clean `build.sh` run copies every patch from xnu/patches.)

### Building in WSL (tools/wsl-build.sh, no Mac)

From PowerShell:

    wsl.exe -- bash /mnt/c/Users/Peketr/ios-flame/src/tools/wsl-build.sh

The same tools/mac-build.sh run (pending patches, build, check strings) on the ~/xnu-linux tree,
cross-compiled on Linux. The kernel lands in work/out/kernel-NNNN.sweet and work/boot/kernel-NNNN
(an existing, different work/boot file is kept; `WSL_BUILD_FORCE=1` replaces it). A full build takes
about 2 minutes; the full make log is ~/xnu-linux/dxb/build-NNNN.log (work/build-NNNN.log is a summary).

One-time setup: `tools/wsl-xnu-setup.sh` (see `--help`: apt packages, Theos' Linux ld64/cctools, and
four seed tarballs made once on the Mac: SDK, fakeroot, xnu tree, build tree for the iig outputs).
It builds clang (the apt LLVM version) with Apple's XNU kalloc_type builtins and the
`xnu_usage_semantics` attribute added (tools/wsl-xnu/clang-xnu-builtins.py, ~40 min), mig from
bootstrap_cmds-129 (Xcode 27.2's), unifdef, compiler-rt's cc_kext profile runtime, and stand-ins for
xcrun, iig, plutil. iig is Xcode-only: its outputs come from the seed, and the build stops if a patch
changes an .iig (then refresh dxb-obj.tgz from a Mac build and rerun setup with `--tools`).

Checked against Mac builds of 0165 and 0169: identical kalloc_type views (1554: type signatures,
flags, sizes), kalloc_var views (465), global symbols and MIG sources (79 files); data sections
match in size. The code differs (upstream clang, not Apple's fork): __TEXT_EXEC,__text is 3.8%
smaller. `tools/wsl-xnu/kalloc-type-sigs.py MAC_KERNEL WSL_KERNEL` repeats the allocator check;
run it after a clang or toolchain update, since a wrong signature puts types in other heaps than
a Mac build would. Not yet booted on the phone.

### Running a kernel from the Windows PC (WSL)

Copy the kernel to `work/boot/kernel-NNNN`, then from WSL Ubuntu:

    PATH=$PWD/tools/wsl-bin:$PATH tools/hotdogb-run.sh NAME -k work/boot/kernel-NNNN -s 300 \
        -a "rd=ufsroot0 ufsroot=rawdump sweetusb=2 sweetmtl=2"

`tools/hotdogb-run-current.sh [NAME]` runs the currently recommended kernel and boot-args (updated
with every new test).

`tools/wsl-bin` runs Windows' fastboot.exe/adb.exe (WSL has no USB). Only `fastboot boot`; the
console log reaches rawdump only with `rd=ufsroot0` (`ufsroot=rawdump`: read-only root on rawdump,
no userland; `rd=ufsroot0 ufsrw=1 rootrw=1` without `ufsroot=` mounts the iOS root on userdata).

**Since 2026-11-07 userdata holds the iOS root**, so Android must not boot: its /data mount fails and
recovery offers "factory data reset", which would wipe the root. If the phone ever shows that screen,
pick "Try again" or power off, never the reset. The run script records the live log (UDP 5140 over
the NCM link, Windows Python through WSL interop) into logs/runs/NAME/live.txt and leaves the phone in
fastboot. `-T` also reads the rawdump ring through TWRP (`fastboot boot` of
../hotdogb/downloads/twrp/twrp-3.7.0_11-0-hotdog.img, official 7T build, SHA-256 96a15ec1...6ba80):
worked once by hand on 2026-11-07, then not in the user's runs - not the default until fixed.
`-A` is the old Android path. For ue0 over NCM, the Windows adapter needs 192.168.7.1/24 (the phone is .2); WSL's
default NAT network cannot receive the live log, so run `tools/flame-live.py` with Windows' Python.

Unverified assumptions to check in the first kernel log: wrap2 core/core_2x vote bits
(0 and 3, from downstream), GPIO7 reads as an enabled input, the SE reports protocol 3
(I2C) firmware, F12 objects are 8 bytes (type, x, y, z, wx, wy).

Host tools still default to flame: `tools/flame-shot --pa 0x9c000000 --h 2400`.

## iOS root image (WSL, Ubuntu 26.04)

Built 2026-11-05 with the scripts in the Windows folder `hotdogb/` (run from `.bat` files):
`fetch-ipsw.sh` (remotezip: OS 087-86698-044.dmg, SystemOS 087-86593-047.dmg, AppOS
087-86715-047.dmg, DeviceTree.d22ap.im4p, all CRC-checked) and `build-root.sh`
(mkiosroot.sh + sweet-root-tweaks.py) -> `~/ios-flame-ipsw/ios16-root-20H392.img`, 16 GiB
HFS+ (12 GB used), SHA-256 `d699e211e851668a31a5cf525d20df02021e027bb3f79eba42d494a257ec3502`.

WSL quirks met on the way: no `apfs-fuse` package (built from sgan81/apfs-fuse with
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5`); no hfsplus in the WSL kernel (`hfsplus-module.sh`
builds `hfsplus.ko` from microsoft/WSL2-Linux-Kernel at the running tag and insmods it,
nothing persistent); uutils `install` rejects numeric owners (mkiosroot.sh now uses
mkdir/chown/chmod); `fsck.hfsplus -n` always reports "volume header needs minor repair"
on these images while `-fy` reports OK, so `-fy` is the verdict.

`research/ipsw-x/d22ap.adt` is the LZFSE-decoded IM4P payload; `mkadt.py --hotdogb`
then includes the real `/product` node.

**Writing it**: only to `/dev/block/by-name/userdata` (sda22). The README's `sda15` is
flame's userdata; on the 7T sda15 is `super`.

Written 2026-11-07 from bootloader fastboot (userdata 0x1850185000 bytes, reported as f2fs):
`img2simg` in WSL (12.1 GB sparse, SHA-256 41901d97...ed63), copied to
`hotdogb/downloads/root/` on C:, then `fastboot -S 512M flash userdata <that copy>` from PowerShell
(24 transfers, 394 s). Reading the image straight from `\\wsl.localhost` failed mid-flash ("Error
reading sparse file") and left the bootloader answering "no link" until a restart from its menu.
**Check after flashing:** the OnePlus ABL acknowledged all 24 transfers, but the device's last
backup-header block was missing (16 MiB windows compared from TWRP: all 744 data windows matched
except the last). Fixed by writing that 4 KiB block (LBA 4194303 of the image) with dd from TWRP.
Zero-filled free space after ~15.1 GiB still holds old userdata bytes (harmless for HFS+). Compare
the image against sda22 after any reflash.
Android's data is gone. To get Android back: `fastboot erase userdata` (or format in recovery).

## Wi-Fi (wland, phone/wifi)

Same WLAN path as flame (HANDOFF "WiFi, part 1"): WCSS 0x18800000, SMMU stream 0x640, MSA 0x8bc00000 (1 MB),
regulators pm8150 l1/l7 + pm8150l l2/l11. **Reserved memory: the base DTBs are not what Linux saw** - ABL
applies dtbo_a overlay 3 (`dtbo_idx 3`), which moves modem_region to 0x8dc00000 + 0xa000000 (fragment@85;
base DTBs: 0x8d800000 + 0x9600000), adsp to 0x8be00000 + 0x1e00000 and video/slpi/ipa/... up to 0x9ab00000.
The OnePlus modem.mdt spans exactly 0x8dc00000-0x97c00000. Check the overlay before trusting any carve-out.

Firmware (kernel 0148, `python tools/hotdogb-fw-pull.py` during a run, 2026-11-07; ../hotdogb/fw, not in git):
modem_a (LUN 4, vfat, MPSS.HE build in modem-image/verinfo) -> modem.mdt + b00-b31, wlanmdsp.mbn,
modemr.jsn + modemuw.jsn (the adsp/cdsp/slpi .jsn are left out, as on flame), bdwlan.bin (default board
data; 34 board-specific bdwlan.bNN also there); modemst1/modemst2/fsg/fsc from LUN 5. Installed on the
iOS root with `python tools/hotdogb-wifi-install.py --no-job` (read back and compared).

Working (run k0148-wifi-3): the launchd job starts `wland auto` (~33 s), the radio scans ~24 s later; join from
Settings, configd's DHCP on en0 (needs a Wi-Fi network service in configd's preferences.plist: the root image
has none - HANDOFF "Wi-Fi works end to end"). Network names show up in wland.log/live.txt: keep logs out of git.

## Next

1. Build 0126 on macOS (darwin-xnu-build once, then `tools/build-xnu.sh`), boot with
   `mkadt.py --hotdogb` and a hotdogb payload; read the log from rawdump.
2. Wi-Fi: check the 7T's WLAN chip and firmware against flame's wcn3990 path.
3. Alert slider (hall sensors) as a ringer switch.
