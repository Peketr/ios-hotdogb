#!/bin/sh
# Build audiod for iOS 16 arm64 into phone/audio/out (signed ad hoc). phone/wifi's SMEM/GLINK layers with
# the ADSP's edge: SMEM host 2, APCS IPC bit 8, the raw "apr_audio_svc" channel instead of QRTR.
set -e
cd "$(dirname "$0")"
mkdir -p out
W=../wifi
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter \
	-DGLINK_HOST=2 -DIPC_GLINK=8 -DLINK_CHAN='"apr_audio_svc"' -DLINK_RAW \
	-o out/audiod audiod.c i2c.c $W/smem.c $W/pmem.c $W/glink.c $W/link.c
codesign -f -s - out/audiod 2>/dev/null
ls -la out/audiod
