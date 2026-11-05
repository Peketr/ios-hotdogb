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
- XNU: patches 0125-0126 written and checked to apply (0001-0126 on xnu-8796.141.3);
  not built yet (needs macOS + Xcode), so nothing below has run on the phone.

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
