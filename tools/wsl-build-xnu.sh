#!/bin/bash
# XNU build in WSL with the toolchain from tools/wsl-xnu-setup.sh: the Linux
# counterpart of tools/build-xnu.sh (same make invocation, every Xcode tool
# replaced by its Linux stand-in on the command line).
#
#   tools/wsl-build-xnu.sh [LOG]   -> $XNU_LINUX/dxb/build/xnu.obj/kernel.development.sweet
#
# The make output goes to the Linux filesystem ($XNU_LINUX/dxb/<name of LOG>):
# hundreds of MB of upstream clang warnings, which written to /mnt/c slow the
# build to a crawl. A LOG outside $XNU_LINUX gets a summary (errors, the
# failing make lines, the last 200 lines) and the path of the full log.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
L=${XNU_LINUX:-$HOME/xnu-linux}
DXB=$L/dxb TC=$L/toolchain
LOG=${1:-$DXB/build-xnu.log}
case $LOG in
"$L"/*) FULL=$LOG ;;
*) FULL=$DXB/$(basename "$LOG") ;;
esac
[ -x "$TC/bin/xcrun" ] || { echo "no toolchain in $TC: run tools/wsl-xnu-setup.sh first" >&2; exit 1; }

export PATH="$TC/bin:/usr/bin:/bin"
export DEVELOPER_DIR=$TC
KERNEL_BUILD_DATE=${KERNEL_BUILD_DATE:-$(TZ=UTC git -C "$DXB/xnu" log -1 --format=%cd --date=format-local:'%a %b %e %T %Z %Y')}
export KERNEL_BUILD_DATE KERNEL_BUILD_OBJROOT=${KERNEL_BUILD_OBJROOT:-xnu-8796.141.3}
mkdir -p "$L/fake-kdk" "$DXB/build"
NCPU=$(nproc)
MEM=$(awk '/MemTotal/ {print $2 * 1024}' /proc/meminfo)

cd "$DXB/xnu"
set +e
make install -j"$NCPU" -k VERBOSE=YES \
	XCRUN="$TC/bin/xcrun" CC="$TC/bin/clang" CXX="$TC/bin/clang++" \
	MIG="$TC/bin/mig" MIGCOM="$TC/libexec/migcom" MIGCC="$TC/bin/clang" IIG="$TC/bin/iig" \
	STRIP="$TC/bin/strip" LIPO="$TC/bin/lipo" LIBTOOL="$TC/bin/libtool" OTOOL="$TC/bin/otool" \
	NM="$TC/bin/nm" NMEDIT="$TC/bin/nmedit" UNIFDEF="$TC/bin/unifdef" DSYMUTIL="$TC/bin/dsymutil" \
	CTFINSERT="$TC/bin/ctf_insert" CTFCONVERT= CTFMERGE= CTFDUMP= DO_CTFMERGE=0 \
	GIT=/usr/bin/git PLUTIL="$TC/bin/plutil" SED=/usr/bin/sed \
	HOST_CC="$TC/bin/host-cc" HOST_FLEX=/usr/bin/flex HOST_BISON=/usr/bin/bison HOST_GM4=/usr/bin/m4 \
	HOST_CODESIGN=/bin/true HOST_CODESIGN_ALLOCATE=/bin/true HOST_OS_VERSION=13.0 \
	SDKROOT=macosx TARGET_CONFIGS="DEVELOPMENT ARM64 SWEET" CONCISE=0 LOGCOLORS=n \
	BUILD_WERROR=0 BUILD_LTO=0 SRCROOT="$DXB/xnu" OBJROOT="$DXB/build/xnu.obj" \
	SYMROOT="$DXB/build/xnu.sym" DSTROOT="$DXB/fakeroot" FAKEROOT_DIR="$DXB/fakeroot" \
	KDKROOT="$L/fake-kdk" TIGHTBEAMC=tightbeamc-not-supported \
	RC_DARWIN_KERNEL_VERSION=22.6.0 MEMORY_SIZE="$MEM" \
	SYSCTL_HW_PHYSICALCPU="$NCPU" SYSCTL_HW_LOGICALCPU="$NCPU" KERNEL_BUILDS_IN_PARALLEL=1 \
	> "$FULL" 2>&1
rc=$?
set -e
if [ "$FULL" != "$LOG" ]; then
	{
		echo "WSL build summary (make rc=$rc); full log: $FULL"
		echo "== errors"
		grep -a -o -E '[A-Za-z0-9_./+-]+:[0-9]+:[0-9]+: (fatal )?error: .*' "$FULL" | sort -u || true
		echo "== make failures"
		grep -a -E '^make.*(Error|\*\*\*)|No rule to make' "$FULL" || true
		echo "== last 200 lines"
		tail -200 "$FULL"
	} > "$LOG"
fi
grep -a -o -E '[A-Za-z0-9_./+-]+:[0-9]+:[0-9]+: (fatal )?error: .*' "$FULL" | sort -u | head -40 || true
K=$DXB/build/xnu.obj/kernel.development.sweet
if [ $rc -eq 0 ] && [ -f "$K" ]; then
	ls -la "$K"
	sha256sum "$K"
else
	grep -a -E '^make.*(Error|\*\*\*)|No rule to make' "$FULL" | head -20 >&2 || true
	echo "build failed (make rc=$rc), log: $FULL" >&2
	exit 1
fi
