#!/bin/bash
# Set up the build trees SweetMetal2.bundle needs (not in git), on the Mac:
#   work/turnip/mesa      Mesa 85fda3b3 + UniXNU's patches/mesa (UniXNU as of 2026-10-22, when flame's Turnip
#                         was built: the list in its scripts/data/adreno-compiler-inputs.json) + phone/turnip/patches
#   work/turnip/venv      meson, mako, pyyaml, packaging
#   work/metal2vulkan     github.com/steelbrain/metal2vulkan 720ef1ce + phone/metal2/bridge/patches
#   work/UniXNU           github.com/UniXNU/UniXNU (source of the Mesa patches)
# Idempotent: a patch already applied is skipped, one that does not apply stops the script.
#
#   tools/setup-metal2-trees.sh
#   then: tools/build-turnip-ios.sh && tools/build-m2v-bridge.sh && phone/metal2/build.sh
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
W=$ROOT/work
T=$W/turnip
MESA_COMMIT=85fda3b36405bcaea8bffc64aa4f2651dc90770d
M2V_COMMIT=720ef1ce
UNIXNU_DATE=2026-10-22T04:00
echo "== tree $ROOT"
ls -ld "$W" 2>&1 | sed 's/^/   /'
if [ -L "$W" ] && [ ! -d "$W" ]; then
	echo "work is a symlink to $(readlink "$W"), which is not there: create that folder or remove the link" >&2
	exit 1
fi
[ ! -e "$W" ] || [ -d "$W" ] || { echo "$W exists but is not a folder" >&2; exit 1; }
mkdir -p "$T" || { echo "cannot create $T (see the work line above)" >&2; exit 1; }

echo "== host tools"
missing=()
for f in llvm glslang bison flex; do
	brew list --versions "$f" >/dev/null 2>&1 || missing+=("$f")
done
if ((${#missing[@]})); then
	echo "missing Homebrew packages: ${missing[*]}  ->  brew install ${missing[*]}" >&2
	exit 1
fi
command -v rustup >/dev/null || { echo "no rustup (https://rustup.rs)" >&2; exit 1; }
rustup toolchain install 1.98.0 --profile minimal >/dev/null
rustup target add aarch64-apple-ios aarch64-apple-darwin --toolchain 1.98.0 >/dev/null

# apply DIR PATCH...: git apply each patch unless it is already in
apply() {
	local dir=$1; shift
	for p in "$@"; do
		if git -C "$dir" apply --reverse --check "$p" >/dev/null 2>&1; then
			echo "   already in: $(basename "$p")"
		elif git -C "$dir" apply --check "$p" >/dev/null 2>&1; then
			git -C "$dir" apply "$p"
			echo "   applied:    $(basename "$p")"
		else
			echo "   DOES NOT APPLY: $p" >&2
			git -C "$dir" apply --check "$p" || true
			exit 1
		fi
	done
}

echo "== UniXNU (patch source, as of $UNIXNU_DATE)"
[ -d "$W/UniXNU/.git" ] || git clone -q https://github.com/UniXNU/UniXNU.git "$W/UniXNU"
git -C "$W/UniXNU" fetch -q origin
U=$(git -C "$W/UniXNU" rev-list -1 --before="$UNIXNU_DATE" origin/HEAD)
git -C "$W/UniXNU" checkout -q "$U"
echo "   UniXNU at $U ($(git -C "$W/UniXNU" log -1 --format=%cd --date=short))"
J=$W/UniXNU/scripts/data/adreno-compiler-inputs.json
pin=$(python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(d['mesa']['commit'])" "$J")
[ "$pin" = "$MESA_COMMIT" ] || { echo "UniXNU pins Mesa $pin, flame used $MESA_COMMIT" >&2; exit 1; }
mesa_patches=()	# (macOS bash 3.2: no mapfile)
while IFS= read -r p; do mesa_patches+=("$p"); done < <(python3 -c "import json,sys; [print(p) for p in json.load(open(sys.argv[1]))['mesa']['patches']]" "$J")
echo "   ${#mesa_patches[@]} Mesa patches listed"

echo "== Mesa $MESA_COMMIT"
if [ ! -d "$T/mesa/.git" ]; then
	git init -q "$T/mesa"
	git -C "$T/mesa" remote add origin https://gitlab.freedesktop.org/mesa/mesa.git
	# one commit, not the whole history (falls back to a full fetch if the server refuses)
	git -C "$T/mesa" fetch -q --depth 1 origin "$MESA_COMMIT" || git -C "$T/mesa" fetch -q origin
	git -C "$T/mesa" checkout -q "$MESA_COMMIT"
fi
[ "$(git -C "$T/mesa" rev-parse HEAD)" = "$MESA_COMMIT" ] || { echo "work/turnip/mesa is not at $MESA_COMMIT" >&2; exit 1; }
apply "$T/mesa" $(printf "$W/UniXNU/%s " "${mesa_patches[@]}") "$ROOT"/phone/turnip/patches/*.patch

echo "== Python venv for Mesa's build"
[ -x "$T/venv/bin/python" ] || python3 -m venv "$T/venv"
"$T/venv/bin/pip" install -q meson mako pyyaml packaging

echo "== metal2vulkan $M2V_COMMIT"
[ -d "$W/metal2vulkan/.git" ] || git clone -q https://github.com/steelbrain/metal2vulkan.git "$W/metal2vulkan"
if [ "$(git -C "$W/metal2vulkan" rev-parse --short=8 HEAD)" != "$M2V_COMMIT" ]; then
	[ -z "$(git -C "$W/metal2vulkan" status --porcelain)" ] || { echo "work/metal2vulkan has changes, not at $M2V_COMMIT" >&2; exit 1; }
	git -C "$W/metal2vulkan" checkout -q "$M2V_COMMIT"
fi
apply "$W/metal2vulkan" "$ROOT"/phone/metal2/bridge/patches/*.patch

echo "== done. Next: tools/build-turnip-ios.sh && tools/build-m2v-bridge.sh && phone/metal2/build.sh"
