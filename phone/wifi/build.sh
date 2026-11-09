#!/bin/sh
# Build wland for iOS 16 arm64 into phone/wifi/out (signed ad hoc).
set -e
cd "$(dirname "$0")"
mkdir -p out
# BOARD=hotdogb (default, OnePlus 7T) or flame (Pixel 4): board.h
case "${BOARD:-hotdogb}" in hotdogb) B=1 ;; flame) B=0 ;; *) echo "BOARD=hotdogb|flame" >&2; exit 2 ;; esac
echo "wland for ${BOARD:-hotdogb}"
../../work/userland/bin/ios-cc -DSWEET_HOTDOGB=$B -O2 -Wall -Wextra -Wno-unused-parameter -o out/wland wland.c smem.c pmem.c glink.c link.c qmi.c rmtfs.c pdmapper.c tftp.c scm.c wlfw.c radio.c ipnet.c bridge.c gnss.c
codesign -f -s - out/wland 2>/dev/null
ls -la out/wland
