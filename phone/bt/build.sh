#!/bin/sh
# Build btd (Bluetooth: WCN3998 bring-up, scan, and serving bluetoothd through the kernel bridge) for iOS 16 arm64 into phone/bt/out (signed ad hoc).
set -e
cd "$(dirname "$0")"
mkdir -p out
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter -o out/btd btd.c uart.c pipe.c ../wifi/pmem.c
codesign -f -s - out/btd 2>/dev/null
ls -la out/btd
