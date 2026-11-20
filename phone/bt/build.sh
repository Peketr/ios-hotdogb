#!/bin/sh
# Build btd (Bluetooth: WCN3998 bring-up, scan, and serving bluetoothd through the kernel bridge) for iOS 16 arm64 into phone/bt/out (signed ad hoc).
# Mac: ../../work/userland/bin/ios-cc. PC: in WSL with theos (~/theos, iPhoneOS16.5.sdk):
#   wsl.exe -- sh /mnt/c/Users/Peketr/ios-flame/src/phone/bt/build.sh
set -e
cd "$(dirname "$0")"
mkdir -p out
T=$HOME/theos/toolchain/linux/iphone/bin
if command -v xcrun >/dev/null 2>&1; then
	CC=../../work/userland/bin/ios-cc
	SIGN="codesign -f -s -"
else
	CC="$T/clang -target arm64-apple-ios16.0 -isysroot $HOME/theos/sdks/iPhoneOS16.5.sdk"
	SIGN="$T/ldid -S"
fi
$CC -O2 -Wall -Wextra -Wno-unused-parameter -o out/btd btd.c uart.c pipe.c ../wifi/pmem.c
$SIGN out/btd 2>/dev/null
ls -la out/btd
