#!/bin/bash
# Runs inside WSL (build-root.bat). Builds the iOS root image for userdata:
#   1. installs apfs-fuse + hfsprogs if missing (asks for your sudo password)
#   2. tools/mkiosroot.sh  -> ~/ios-flame-ipsw/ios16-root-20H392.img (16 GB, HFS+)
#   3. tools/sweet-root-tweaks.py on the mounted image
#   4. fsck, SHA-256; copies DeviceTree.d22ap.im4p to the Windows folder (for mkadt /product)
set -euo pipefail
WIN=/mnt/c/Users/Peketr/ios-flame
LOG=$WIN/hotdogb/build-root.log
exec > >(tee "$LOG") 2>&1
echo "=== $(date)"
D=$HOME/ios-flame-ipsw
IMG=$D/ios16-root-20H392.img
cd "$D"
read -r _ OS < <(grep '^OS ' parts.txt)
read -r _ SYS < <(grep '^Cryptex1,SystemOS ' parts.txt)
read -r _ APP < <(grep '^Cryptex1,AppOS ' parts.txt)
echo "OS=$OS SystemOS=$SYS AppOS=$APP"

need=()
command -v mkfs.hfsplus >/dev/null || need+=(hfsprogs)
command -v rsync >/dev/null || need+=(rsync)
# Ubuntu 26.04 has no apfs-fuse package: build it from source (sgan81/apfs-fuse).
command -v apfs-fuse >/dev/null || need+=(git cmake g++ make pkg-config libfuse3-dev fuse3 zlib1g-dev libbz2-dev libattr1-dev)
if ((${#need[@]})); then
	echo "== installing ${need[*]} (sudo password)"
	sudo apt-get update -qq
	sudo apt-get install -y "${need[@]}"
fi
sudo -v
if ! command -v apfs-fuse >/dev/null; then
	echo "== building apfs-fuse"
	src=$HOME/src/apfs-fuse
	[[ -d $src ]] || git clone --recursive https://github.com/sgan81/apfs-fuse.git "$src"
	git -C "$src" submodule update --init --recursive
	cmake -S "$src" -B "$src/build" -DCMAKE_BUILD_TYPE=Release -DUSE_FUSE3=ON \
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS="-include cstdint -include cstring"
	cmake --build "$src/build" -j"$(nproc)"
	sudo install -m 0755 "$src/build/apfs-fuse" /usr/local/bin/apfs-fuse
	command -v apfs-fuse
fi

cp -f Firmware/all_flash/DeviceTree.d22ap.im4p "$WIN/hotdogb/"

# The image is filled through a Linux hfsplus mount. WSL kernels ship most
# filesystems as modules (modules.vhdx); stop early if hfsplus is not one of them.
echo "== hfsplus in kernel $(uname -r)"
sudo modprobe hfsplus 2>&1 || true
if ! grep -qw hfsplus /proc/filesystems; then
	echo "== building hfsplus.ko for this kernel"
	bash "$WIN/hotdogb/hfsplus-module.sh" || true
fi
if ! grep -qw hfsplus /proc/filesystems; then
	echo "NO_HFSPLUS: this WSL kernel cannot mount HFS+ (modprobe failed)"
	ls /lib/modules/"$(uname -r)"/kernel/fs 2>/dev/null | head -50 || echo "no module tree"
	exit 3
fi
grep -w hfsplus /proc/filesystems
# A finished copy is one whose last mkiosroot step (the system keybag) is there.
complete() {
	local m; m=$(mktemp -d)
	sudo mount -t hfsplus -o loop,ro,force "$1" "$m" 2>/dev/null || { rmdir "$m"; return 1; }
	local ok=1; sudo test -s "$m/private/var/keybags/systembag.kb" && ok=0
	sudo umount "$m"; rmdir "$m"; return $ok
}
# Repair with fsck -fy until it reports the volume OK. (fsck -n keeps saying "Volume
# header needs minor repair" even right after a clean -fy pass: its read-only
# mode cannot update the header's state bits, so -n is not used as the verdict.)
repair() {
	for i in 1 2 3; do
		local out; out=$(sudo fsck.hfsplus -f -y "$1" 2>&1 || true)
		echo "$out" | grep -E "appears to be OK|repaired successfully|corrupt|could not" || true
		echo "$out" | grep -q "appears to be OK" && { echo "fsck: clean (pass $i)"; return 0; }
	done
	echo "fsck: not clean after 3 passes"; return 1
}
if [[ -e $IMG.tmp && ! -e $IMG ]]; then
	if complete "$IMG.tmp"; then
		echo "== $IMG.tmp is a finished copy: repairing and keeping it"
		repair "$IMG.tmp"
		mv "$IMG.tmp" "$IMG"
	else
		rm -f "$IMG.tmp"	# an unfinished copy; mkiosroot refuses to overwrite
	fi
fi

if [[ -e $IMG ]]; then
	echo "== $IMG exists, skipping mkiosroot (delete it to rebuild)"
else
	echo "== mkiosroot ($(date +%T)), copies ~10 GB through apfs-fuse"
	bash "$WIN/src/tools/mkiosroot.sh" "$OS" "$SYS" "$APP" "$IMG.tmp" 16G || echo "mkiosroot exit $?"
	complete "$IMG.tmp" || { echo "image incomplete, stopping"; exit 1; }
	repair "$IMG.tmp"
	mv "$IMG.tmp" "$IMG"
fi

echo "== sweet-root-tweaks ($(date +%T))"
M=$(mktemp -d)
sudo fsck.hfsplus -f -y "$IMG" | tail -3 || true
sudo mount -t hfsplus -o loop,force "$IMG" "$M"
trap 'mountpoint -q "$M" && sudo umount "$M"; rmdir "$M" 2>/dev/null || true' EXIT
sudo python3 "$WIN/src/tools/sweet-root-tweaks.py" "$M"
df -h "$M" | tail -1
sudo umount "$M"
repair "$IMG"

echo "== sha256 ($(date +%T))"
sha256sum "$IMG" | tee "$IMG.sha256"
ls -la "$IMG"
echo "== done $(date +%T)"
