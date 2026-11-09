#!/opt/homebrew/bin/bash
# One-step Mac build after `git pull`: apply every xnu/patches/NNNN-*.patch the
# work/dxb/xnu tree does not have yet, build with tools/build-xnu.sh, check the
# kernel contains the new code, and copy it to work/out/kernel-NNNN.sweet.
#
#   tools/mac-build.sh                        normal use
#   tools/mac-build.sh --applied-through 0141 first run only: the tree already has
#                                             every patch up to and including 0141
#   tools/mac-build.sh --dry-run              show what would be applied, change nothing
#
# Applied patches are recorded in work/dxb/applied-patches (one file name per
# line). A pending patch that turns out to be in the tree already (it reverses
# cleanly) is recorded without being applied again. Any other patch failure
# stops before the build, with the tree unchanged by that patch.
#
# Checks: tools/mac-build-check.txt lists "NNNN <string>" pairs; for every
# applied patch NNNN, the kernel must contain <string>.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DXB=${DXB:-$ROOT/work/dxb}   # tools/wsl-build.sh points this at ~/xnu-linux/dxb
TREE=$DXB/xnu
STATE=$DXB/applied-patches
CHECKS=$ROOT/tools/mac-build-check.txt
OUT=$ROOT/work/out

through= dry=0
while [ $# -gt 0 ]; do
	case $1 in
	--applied-through) through=${2:?--applied-through needs a patch number}; shift 2 ;;
	--dry-run) dry=1; shift ;;
	-h|--help) sed -n '2,19p' "$0"; exit 0 ;;
	*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

[ -d "$TREE" ] || { echo "no XNU tree at $TREE (run the full build.sh once first)" >&2; exit 1; }
cd "$ROOT"

patches=()
while IFS= read -r p; do patches+=("$p"); done < <(cd xnu/patches && ls | LC_ALL=C sort | grep -E '^[0-9]{4}-.*\.patch$')
[ ${#patches[@]} -gt 0 ] || { echo "no patches in xnu/patches" >&2; exit 1; }

if [ ! -f "$STATE" ]; then
	if [ -z "$through" ]; then
		echo "first run: tell me what the tree already has, e.g." >&2
		echo "  tools/mac-build.sh --applied-through 0141" >&2
		exit 1
	fi
	[ $dry -eq 1 ] || : > "$STATE"
	for p in "${patches[@]}"; do
		if [ "${p%%-*}" \< "$through" ] || [ "${p%%-*}" = "$through" ]; then
			[ $dry -eq 1 ] || echo "$p" >> "$STATE"
		fi
	done
	echo "seeded: patches up to $through recorded as already applied"
elif [ -n "$through" ]; then
	echo "$STATE exists already; --applied-through ignored" >&2
fi

applied() { [ -f "$STATE" ] && grep -qxF "$1" "$STATE"; }
pending=()
for p in "${patches[@]}"; do
	if [ -n "$through" ] && [ $dry -eq 1 ] && { [ "${p%%-*}" \< "$through" ] || [ "${p%%-*}" = "$through" ]; }; then
		continue
	fi
	applied "$p" || pending+=("$p")
done

if [ ${#pending[@]} -eq 0 ]; then
	echo "no new patches; rebuilding the current tree"
else
	echo "new patches: ${pending[*]}"
fi
[ $dry -eq 0 ] || exit 0

for p in "${pending[@]}"; do
	f=$ROOT/xnu/patches/$p
	if (cd "$TREE" && patch -p1 --dry-run -s -f < "$f" >/dev/null 2>&1); then
		(cd "$TREE" && patch -p1 -s -f < "$f")
		echo "applied $p"
	elif (cd "$TREE" && patch -p1 -R --dry-run -s -f < "$f" >/dev/null 2>&1); then
		echo "already in the tree: $p"
	else
		echo "patch $p does not apply; stopping (tree untouched by it). Details:" >&2
		(cd "$TREE" && patch -p1 --dry-run -f < "$f") >&2 || true
		exit 1
	fi
	echo "$p" >> "$STATE"
done

last=$(tail -1 "$STATE"); num=${last%%-*}
LOG=$ROOT/work/build-$num.log
echo "building (log: $LOG) ..."
if [ "$(uname -s)" = Linux ]; then
	tools/wsl-build-xnu.sh "$LOG"
else
	tools/build-xnu.sh "$LOG"
fi

K=$DXB/build/xnu.obj/kernel.development.sweet
K2=$DXB/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet
if [ -f "$K2" ] && { [ ! -f "$K" ] || [ "$K2" -nt "$K" ]; }; then
	K=$K2   # the newer of the two build outputs
fi
[ -f "$K" ] || { echo "no kernel found after the build" >&2; exit 1; }
echo "kernel: $K"

ok=1
if [ -f "$CHECKS" ]; then
	strs=$(strings -a "$K")
	while read -r n s; do
		s=${s%$'\r'}
		case $n in ''|\#*) continue ;; esac
		grep -q "^$n-" "$STATE" || continue
		# Here-string, not a pipe: grep -q stops at the first match, and with
		# pipefail the writer's SIGPIPE would turn every hit into a failure.
		if grep -qF -- "$s" <<< "$strs"; then
			echo "check $n ok: $s"
		else
			echo "check $n FAILED: '$s' not in the kernel" >&2
			ok=0
		fi
	done < "$CHECKS"
fi
[ $ok -eq 1 ] || { echo "kernel is missing patched code; not copying it" >&2; exit 1; }

mkdir -p "$OUT"
cp "$K" "$OUT/kernel-$num.sweet"
echo
echo "kernel with patches through $num:"
if command -v shasum >/dev/null; then shasum -a 256 "$OUT/kernel-$num.sweet"; else sha256sum "$OUT/kernel-$num.sweet"; fi
[ "$(uname -s)" = Linux ] || echo "upload this file: $OUT/kernel-$num.sweet"
