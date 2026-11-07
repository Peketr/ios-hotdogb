#!/bin/bash
# The current recommended hotdogb run: the latest kernel and boot-args we test.
# Updated with every new test. Run from WSL Ubuntu, anywhere:
#
#   /mnt/c/Users/Peketr/ios-flame/src/tools/hotdogb-run-current.sh [NAME]
#
# NAME defaults to the run tag below plus the date and time (logs/runs/NAME/),
# so earlier runs are never overwritten. The run records the live log itself into
# logs/runs/NAME/live.txt (192.168.7.1 on "Ethernet 2"; close other flame-live.py listeners first)
# and prints the #### milestones as they come. End early: python tools\flame-cmd.py reboot
#
# Current (2026-11-07): kernel 0152 (panel brightness: DCS 0x51 + AppleARMBacklight, sweetbl=0 = off) on 0151 (GPU present fills the panel, sweetfb_fill=0 = bars; a client's first 3
# GPU resets forgiven, sweetgpuforgive=N) with the Adreno 640 on (-G: sweetgpu=1 + the 7T's GPU firmware from
# ../hotdogb/fw/gpu; 0149 = the 7T's zap shader region, 0150 = margins cleared after CPU-copy frames). iOS root on userdata read-write, with wland
# (Wi-Fi at boot, log /var/local/wland.log) and oslog-wifid (/var/local/oslog-wifid.log) launchd jobs.
# No kernel text on screen once the GUI presents (-q), 900 s, live log only. Wi-Fi off: sweetwifi=0.
# Metal: sweetmtl=2 with SweetMetal2.bundle (metal2vulkan + Turnip) in /System/Library/Extensions, installed by
# tools/hotdogb-metal2-install.py; its log is /var/local/mtl2.log. Without the bundle: drop sweetmtl=2.
set -euo pipefail
cd "$(dirname "$0")/.."

KERNEL=work/boot/kernel-0152
TAG=k0152-blnode
SECS=900
OPTS=(-q -G)
ARGS="rd=ufsroot0 ufsrw=1 rootrw=1 sweetusb=2 sweetmtl=2"

name=${1:-$TAG-$(date +%m%d-%H%M)}
[ -f "$KERNEL" ] || { echo "no kernel $KERNEL (fetch it from the NAS first)" >&2; exit 1; }
sed -i 's/\r$//' tools/hotdogb-run.sh ../hotdogb/logpull.sh
export PATH="$PWD/tools/wsl-bin:$PATH"
echo "run $name: $KERNEL ${OPTS[*]} -s $SECS -a \"$ARGS\""
exec tools/hotdogb-run.sh "$name" "${OPTS[@]}" -k "$KERNEL" -s "$SECS" -a "$ARGS"
