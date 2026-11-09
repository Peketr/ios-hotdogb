#!/bin/bash
# Build the hotdogb kernel in WSL: tools/mac-build.sh (apply pending
# xnu/patches, build, check strings) on the ~/xnu-linux tree, then put the
# kernel where tools/hotdogb-run.sh takes it from.
#
# From PowerShell:
#   wsl.exe -- bash /mnt/c/Users/Peketr/ios-flame/src/tools/wsl-build.sh
#
#   tools/wsl-build.sh [mac-build.sh options]   e.g. --dry-run
#   WSL_BUILD_FORCE=1 tools/wsl-build.sh        replace an existing, different
#                                               work/boot/kernel-NNNN
#
# Output: work/out/kernel-NNNN.sweet and work/boot/kernel-NNNN (same file).
# One-time setup: tools/wsl-xnu-setup.sh.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
L=${XNU_LINUX:-$HOME/xnu-linux}
[ "$(uname -s)" = Linux ] || { echo "run this in WSL" >&2; exit 1; }
[ -x "$L/toolchain/bin/xcrun" ] || { echo "no toolchain in $L: run tools/wsl-xnu-setup.sh first" >&2; exit 1; }

cd "$ROOT"
DXB=$L/dxb bash tools/mac-build.sh "$@"
case " $* " in *" --dry-run "*) exit 0 ;; esac

num=$(tail -1 "$L/dxb/applied-patches"); num=${num%%-*}
K=$ROOT/work/out/kernel-$num.sweet
B=$ROOT/work/boot/kernel-$num
mkdir -p "$ROOT/work/boot"
if [ -f "$B" ] && ! cmp -s "$K" "$B" && [ "${WSL_BUILD_FORCE:-0}" != 1 ]; then
	echo "$B exists and differs (a Mac build?); kept it. WSL_BUILD_FORCE=1 replaces it." >&2
	echo "the WSL kernel is $K" >&2
	exit 0
fi
cp "$K" "$B"
echo "for tools/hotdogb-run.sh: -k work/boot/kernel-$num"
