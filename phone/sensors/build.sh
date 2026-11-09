#!/bin/sh
# Build sensord for iOS 16 arm64 into phone/sensors/out (signed ad hoc). phone/wifi's SMEM/GLINK/QRTR layers on
# the SLPI's edge: SMEM host 3, APCS IPC bit 24, the raw "fastrpcglink-apps-dsp" channel (FastRPC) plus QRTR on
# IPCRTR (the SLPI's node is learned from its HELLO) with a pd-mapper of the SLPI's domains.
# Host test of the encoders first (any cc): cc -DFRPC_PURE -DSSC_PURE -o /tmp/t test_sensord.c fastrpc.c ssc.c && /tmp/t
set -e
cd "$(dirname "$0")"
mkdir -p out
W=../wifi
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter \
	-DGLINK_HOST=3 -DIPC_GLINK=24 -DLINK_CHAN='"fastrpcglink-apps-dsp"' -DLINK_RAW -DLINK_CHAN2='"sensord-unused"' \
	-DQRTR_NODE_REMOTE_VAR -DPDMAPPER_SLPI \
	-o out/sensord sensord.c fastrpc.c ssc.c $W/smem.c $W/pmem.c $W/glink.c $W/link.c $W/qmi.c $W/pdmapper.c
codesign -f -s - out/sensord 2>/dev/null
ls -la out/sensord
