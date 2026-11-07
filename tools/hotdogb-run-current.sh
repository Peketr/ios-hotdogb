#!/bin/bash
# The current recommended hotdogb run: the latest kernel and boot-args we test.
# Updated with every new test. Run from WSL Ubuntu, anywhere:
#
#   /mnt/c/Users/Peketr/ios-flame/src/tools/hotdogb-run-current.sh [NAME]
#
# NAME defaults to the run tag below plus the date and time (logs/runs/NAME/),
# so earlier runs are never overwritten. The live log: run
#   python tools\flame-live.py logs\live.txt
# in a Windows PowerShell in src (192.168.7.1 on "Ethernet 2"), before or during the run.
#
# Current (2026-11-07): kernel 0147 with the iOS root on userdata mounted read-write,
# no kernel text on screen once the GUI presents (-q), 600 s, log through TWRP.
set -euo pipefail
cd "$(dirname "$0")/.."

KERNEL=work/boot/kernel-0147
TAG=k0147-root
SECS=600
OPTS=(-q)
ARGS="rd=ufsroot0 ufsrw=1 rootrw=1 sweetusb=2 sweetmtl=2"

name=${1:-$TAG-$(date +%m%d-%H%M)}
[ -f "$KERNEL" ] || { echo "no kernel $KERNEL (fetch it from the NAS first)" >&2; exit 1; }
sed -i 's/\r$//' tools/hotdogb-run.sh ../hotdogb/logpull.sh
export PATH="$PWD/tools/wsl-bin:$PATH"
echo "run $name: $KERNEL ${OPTS[*]} -s $SECS -a \"$ARGS\""
exec tools/hotdogb-run.sh "$name" "${OPTS[@]}" -k "$KERNEL" -s "$SECS" -a "$ARGS"
