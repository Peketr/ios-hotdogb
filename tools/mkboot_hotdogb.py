#!/usr/bin/env python3
"""Pack a boot image v2 the way the OnePlus 7T (hotdogb) stock boot.img is laid out.

  mkboot_hotdogb.py IMAGE DTB OUT [RAMDISK]

The kernel gets Qualcomm's "UNCOMPRESSED_IMG" + le32 size prefix (what the
stock 7T kernel uses), so ABL takes it without decompression. Layout: base 0,
kernel 0x8000, ramdisk 0x1000000, tags 0x100, dtb 0x1f00000, page 4096.
No ramdisk -> an empty newc cpio (gzip), like shim/build.sh.
"""
import gzip, hashlib, struct, sys

PAGE = 4096
def pad(b): return b + b'\0' * (-len(b) % PAGE)

def empty_cpio():
    name = b'TRAILER!!!\0'
    hdr = b'070701' + b''.join(b'%08X' % v for v in
          (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0))
    d = hdr + name
    d += b'\0' * (-len(d) % 4)
    d += b'\0' * (-len(d) % 512)
    return gzip.compress(d, 9, mtime=0)

img, dtbf, out = sys.argv[1:4]
image = open(img, 'rb').read()
kernel = b'UNCOMPRESSED_IMG' + struct.pack('<I', len(image)) + image
ramdisk = open(sys.argv[4], 'rb').read() if len(sys.argv) > 4 else empty_cpio()
dtb = open(dtbf, 'rb').read()
os_version = 0x1e00018c          # copied from the stock 7T boot_a header
cmdline = b'console=null'

sha = hashlib.sha1()
for blob in (kernel, ramdisk, b'', b'', dtb):
    sha.update(blob); sha.update(struct.pack('<I', len(blob)))
h = struct.pack('<8s10I', b'ANDROID!', len(kernel), 0x8000, len(ramdisk), 0x1000000,
                0, 0, 0x100, PAGE, 2, os_version)
h += b'\0' * 16                                   # name
h += cmdline.ljust(512, b'\0')
h += sha.digest().ljust(32, b'\0')
h += b'\0' * 1024                                 # extra cmdline
h += struct.pack('<IQI', 0, 0, 1660)              # recovery dtbo size/offset, header size
h += struct.pack('<IQ', len(dtb), 0x1f00000)
assert len(h) == 1660, len(h)
open(out, 'wb').write(pad(h) + pad(kernel) + pad(ramdisk) + pad(dtb))
print(out, 'kernel', len(kernel), 'ramdisk', len(ramdisk), 'dtb', len(dtb))
