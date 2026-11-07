#!/opt/homebrew/bin/bash
# Completely clean kernel build on the Mac, the way .github/workflows/xnu.yml builds it:
#   1. the old work/dxb is moved aside (work/dxb.old-DATE; delete it yourself once the new one works)
#   2. a fresh blacktop/darwin-xnu-build checkout at the pinned commit in work/dxb, with every
#      xnu/patches/*.patch and stage script (*.sh) next to blacktop's own patches
#   3. its build.sh: clones xnu-8796 (macOS 13.5), the tools, applies every patch, builds DEVELOPMENT ARM64 SWEET
#      (KDKROOT = an empty work/fake-kdk, as tools/build-xnu.sh: the SWEET config links no closed KDK library)
#   4. stops if build.sh skipped any of our patches (it only warns), commits the patched tree in work/dxb/xnu
#      ("baseline + patches", as AGENTS.md describes) and records every patch in work/dxb/applied-patches
#   5. tools/mac-build.sh: the string checks and work/out/kernel-NNNN.sweet, as after every normal build
# Takes an hour or more (network clones, the whole tree). Log: work/build-clean-NNNN.log
#
#   tools/mac-clean-build.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
DXB_REPO=https://github.com/blacktop/darwin-xnu-build
DXB_SHA=$(sed -n 's/^  DXB_SHA: //p' .github/workflows/xnu.yml)
[ -n "$DXB_SHA" ] || { echo "no DXB_SHA in .github/workflows/xnu.yml" >&2; exit 1; }

missing=()
for t in jq gum cmake ninja git; do command -v "$t" >/dev/null || missing+=("$t"); done
if ((${#missing[@]})); then
	echo "missing: ${missing[*]}  ->  brew install ${missing[*]}" >&2
	exit 1
fi
compgen -G "/Applications/Xcode*.app" >/dev/null || { echo "no Xcode in /Applications" >&2; exit 1; }
# build.sh insists on xcodes only to install Xcode, which is here already
BIN=$ROOT/work/dxb-bin
mkdir -p "$BIN"
if ! command -v xcodes >/dev/null; then
	printf '#!/bin/sh\nexit 0\n' > "$BIN/xcodes"
	chmod +x "$BIN/xcodes"
fi
export PATH="$BIN:$PATH"

last=$(cd xnu/patches && ls | LC_ALL=C sort | grep -E '^[0-9]{4}-.*\.patch$' | tail -1)
num=${last%%-*}
LOG=$ROOT/work/build-clean-$num.log

if [ -d work/dxb ]; then
	old=work/dxb.old-$(date +%Y%m%d-%H%M)
	mv work/dxb "$old"
	echo "old tree moved to $old"
fi
echo "== darwin-xnu-build $DXB_SHA"
git init -q work/dxb
git -C work/dxb fetch -q --depth 1 "$DXB_REPO" "$DXB_SHA"
git -C work/dxb checkout -q FETCH_HEAD
cp xnu/patches/*.patch xnu/patches/*.sh work/dxb/patches/
# libfirehose_kernel is built arm64e-only; the SWEET kernel is plain arm64
sed -i '' 's/ARCHS="x86_64 arm64e" VALID_ARCHS="x86_64 arm64e"/ARCHS="x86_64 arm64e arm64" VALID_ARCHS="x86_64 arm64e arm64"/' work/dxb/build.sh
grep -q 'VALID_ARCHS="x86_64 arm64e arm64"' work/dxb/build.sh || { echo "build.sh arch line not found" >&2; exit 1; }
mkdir -p work/fake-kdk

echo "== build.sh (log: $LOG) ..."
(cd work/dxb && MACOS_VERSION=13.5 KERNEL_CONFIG=DEVELOPMENT MACHINE_CONFIG=SWEET KDKROOT="$ROOT/work/fake-kdk" \
	bash build.sh) > "$LOG" 2>&1 || { tail -30 "$LOG"; echo "build.sh failed, log: $LOG" >&2; exit 1; }

skipped=$(grep -E 'Skipping patch .*/[0-9]{4}-' "$LOG" || true)
if [ -n "$skipped" ]; then
	echo "build.sh skipped some of our patches (the kernel would lack them):" >&2
	echo "$skipped" >&2
	exit 1
fi
K=work/dxb/build/xnu.obj/kernel.development.sweet
K2=work/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet
[ -f "$K" ] || [ -f "$K2" ] || { tail -30 "$LOG"; echo "no kernel built, log: $LOG" >&2; exit 1; }

echo "== recording the patched tree"
git -C work/dxb/xnu add -A
git -C work/dxb/xnu -c user.name=build -c user.email=build@localhost commit -qm "baseline + xnu/patches through $num"
(cd xnu/patches && ls | LC_ALL=C sort | grep -E '^[0-9]{4}-.*\.patch$') > work/dxb/applied-patches

echo "== checks and work/out"
tools/mac-build.sh
