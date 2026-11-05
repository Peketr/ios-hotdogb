#!/bin/bash
# Runs inside WSL (fetch-ipsw.bat). Fetches only what mkiosroot.sh and mkadt.py need from
# the iPhone X iOS 16.7.16 (20H392) IPSW on Apple's CDN, via tools/remotezip.py
# (HTTP range requests, resumable, CRC-32 checked): BuildManifest.plist, the OS
# image, the SystemOS and AppOS cryptexes and DeviceTree.d22ap.im4p.
# Output: ~/ios-flame-ipsw in WSL (ext4: fast, and the 16 GB root image goes there too).
set -euo pipefail
WIN=/mnt/c/Users/Peketr/ios-flame
LOG=$WIN/hotdogb/ipsw.log
exec > >(tee "$LOG") 2>&1
echo "=== $(date)"
echo "== environment"
uname -srm; grep -E '^(PRETTY_NAME)=' /etc/os-release || true
df -h "$HOME" | tail -1
for t in curl python3 apfs-fuse mkfs.hfsplus fsck.hfsplus rsync; do
	printf '%-14s %s\n' "$t" "$(command -v $t || echo MISSING)"
done
for t in curl python3; do command -v $t >/dev/null || { echo "need $t"; exit 1; }; done

D=$HOME/ios-flame-ipsw
mkdir -p "$D"; cd "$D"
RZ=$WIN/src/tools/remotezip.py
echo "== IPSW URL (api.ipsw.me)"
URL=$(curl -sfL https://api.ipsw.me/v4/ipsw/iPhone10,3/20H392 |
	python3 -c 'import json,sys; j=json.load(sys.stdin); print(j["url"]); print(j["filesize"], j["sha256sum"], file=sys.stderr)')
echo "$URL"
case $URL in https://updates.cdn-apple.com/*|https://appldnld.apple.com/*) ;; *) echo "unexpected host, stopping"; exit 1;; esac
echo "$URL" > url.txt

echo "== BuildManifest"
python3 "$RZ" "$URL" > members.txt
python3 "$RZ" "$URL" BuildManifest.plist .
python3 - > parts.txt <<'PY'
import plistlib
m = plistlib.load(open('BuildManifest.plist', 'rb'))
for bi in m['BuildIdentities']:
    info = bi['Info']
    if info.get('DeviceClass', '').lower() == 'd22ap' and 'Erase' in info.get('Variant', ''):
        man = bi['Manifest']
        for k in ('OS', 'Cryptex1,SystemOS', 'Cryptex1,AppOS', 'DeviceTree'):
            print(k, man[k]['Info']['Path'])
        break
else:
    raise SystemExit('no d22ap Erase identity')
PY
cat parts.txt
while read -r key path; do
	size=$(awk -v p="$path" '$2 == p {print $1}' members.txt)
	echo "== $key: $path ($((size >> 20)) MiB)"
	python3 "$RZ" "$URL" "$path" . | awk 'NR % 8 == 0 || /CRC|complete/'
done < parts.txt
echo "== done"
ls -la "$D" "$D"/Firmware/all_flash 2>/dev/null || true
df -h "$HOME" | tail -1
