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

### Running a kernel from the Windows PC (WSL)

Copy the kernel to `work/boot/kernel-NNNN`, then from WSL Ubuntu:

    PATH=$PWD/tools/wsl-bin:$PATH tools/hotdogb-run.sh NAME -k work/boot/kernel-NNNN -s 300 \
        -a "rd=ufsroot0 ufsroot=rawdump sweetusb=2 sweetmtl=2"

`tools/wsl-bin` runs Windows' fastboot.exe/adb.exe (WSL has no USB). Only `fastboot boot`; the
console log reaches rawdump only with `rd=ufsroot0` (`ufsroot=rawdump`: read-only, userdata
untouched). For ue0 over NCM, the Windows adapter needs 192.168.7.1/24 (the phone is .2); WSL's
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

## Next

1. Build 0126 on macOS (darwin-xnu-build once, then `tools/build-xnu.sh`), boot with
   `mkadt.py --hotdogb` and a hotdogb payload; read the log from rawdump.
2. Wi-Fi: check the 7T's WLAN chip and firmware against flame's wcn3990 path.
3. Alert slider (hall sensors) as a ringer switch.
