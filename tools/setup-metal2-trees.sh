#!/bin/bash
# Set up the build trees SweetMetal2.bundle needs (not in git), on the Mac:
#   work/turnip/mesa      Mesa 85fda3b3 + UniXNU's patches/mesa (UniXNU as of 2026-10-22, when flame's Turnip
#                         was built: the list in its scripts/data/adreno-compiler-inputs.json) + phone/turnip/patches
#   work/turnip/venv      meson, mako, pyyaml, packaging
#   work/metal2vulkan     github.com/steelbrain/metal2vulkan 720ef1ce + phone/metal2/bridge/patches
#   work/UniXNU           github.com/UniXNU/UniXNU (source of the Mesa patches)
# Idempotent: a patch already applied is skipped, one that does not apply stops the script (Mesa: while the patch
# set and the tree match work/turnip/mesa-patches.stamp it is left alone, else it starts again from the commit,
# its changes kept in work/turnip/mesa-before-<time>.diff).
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
# work/ and the trees in it may be symlinks to another clone's (the old flame checkout): a link whose target
# is gone makes mkdir fail with "No such file or directory" on the link itself
for d in "$W" "$T" "$W/UniXNU" "$W/metal2vulkan"; do
	if [ -L "$d" ]; then
		echo "   ${d#$ROOT/} -> $(readlink "$d")"
		[ -d "$d" ] || { echo "${d#$ROOT/} is a symlink to $(readlink "$d"), which is not there: point it at the tree or remove the link (rm ${d#$ROOT/})" >&2; exit 1; }
	fi
	[ ! -e "$d" ] || [ -d "$d" ] || { echo "$d exists but is not a folder" >&2; exit 1; }
done
mkdir -p "$T" || { ls -ld "$W" "$T" >&2; exit 1; }

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
# the patch list is top-level "patches" (paths relative to the UniXNU root)
list=$(python3 -c "import json,sys; d=json.load(open(sys.argv[1])); [print(p) for p in d.get('patches') or d['mesa']['patches']]" "$J")
mesa_patches=()	# (macOS bash 3.2: no mapfile)
while IFS= read -r p; do [ -n "$p" ] && mesa_patches+=("$p"); done <<EOF
$list
EOF
((${#mesa_patches[@]})) || { echo "no Mesa patches listed in $J" >&2; exit 1; }
echo "   ${#mesa_patches[@]} Mesa patches listed"

echo "== Mesa $MESA_COMMIT"
[ -d "$T/mesa/.git" ] || git init -q "$T/mesa"
if ! git -C "$T/mesa" cat-file -e "$MESA_COMMIT^{commit}" 2>/dev/null; then
	# one commit, not the whole history: freedesktop first, then the Chromium mirror UniXNU lists
	ok=
	for url in https://gitlab.freedesktop.org/mesa/mesa.git \
	    https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa; do
		echo "   fetching from $url"
		if git -C "$T/mesa" fetch -q --depth 1 "$url" "$MESA_COMMIT"; then ok=1; break; fi
	done
	[ -n "$ok" ] || { echo "could not fetch Mesa $MESA_COMMIT" >&2; exit 1; }
fi
if [ "$(git -C "$T/mesa" rev-parse -q --verify HEAD || true)" != "$MESA_COMMIT" ]; then
	[ -z "$(git -C "$T/mesa" status --porcelain 2>/dev/null)" ] || { echo "work/turnip/mesa has changes, not at $MESA_COMMIT" >&2; exit 1; }
	git -C "$T/mesa" checkout -q "$MESA_COMMIT"
fi
mesa_list=($(printf "$W/UniXNU/%s " "${mesa_patches[@]}") "$ROOT"/phone/turnip/patches/*.patch)
# Patches that append to the same file (meson.options: offline-adreno-compiler-probe, then turnip-darwin-kmd and
# turnip-builtin-shader-pack) stack: in a patched tree an earlier one neither reverses (its context moved) nor
# applies, so they cannot be checked one by one. Instead work/turnip/mesa-patches.stamp holds a hash of the patch
# set and one of the tree's changes after applying it; while both match the tree is left alone. Otherwise its
# changes (if any) go to work/turnip/mesa-before-<time>.diff and it starts again from the Mesa commit.
tree_hash() { git -C "$T/mesa" add -A . && git -C "$T/mesa" diff --cached --binary HEAD | git hash-object --stdin; }
want=$(cat "${mesa_list[@]}" | git hash-object --stdin)
stamp=$T/mesa-patches.stamp
if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want $(tree_hash)" ]; then
	echo "   all ${#mesa_list[@]} patches already in"
else
	if [ -n "$(git -C "$T/mesa" status --porcelain)" ]; then
		keep=$T/mesa-before-$(date +%Y%m%d-%H%M%S).diff
		git -C "$T/mesa" add -A . && git -C "$T/mesa" diff --cached --binary HEAD > "$keep"
		echo "   patch set changed: tree changes saved to ${keep#$ROOT/}, starting from $MESA_COMMIT"
		git -C "$T/mesa" reset -q --hard "$MESA_COMMIT"
		git -C "$T/mesa" clean -q -fd
	fi
	apply "$T/mesa" "${mesa_list[@]}"
	echo "$want $(tree_hash)" > "$stamp"
fi

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
