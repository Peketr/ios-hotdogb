#!/system/bin/sh
# Shim logs live at the end of rawdump on hotdogb (shim/ufs.h layout):
# [.. rescue 64][shim 64][xnu 1025] = last 1153 4K blocks.
D=$(readlink -f /dev/block/by-name/rawdump)
B=$(( $(blockdev --getsize64 $D) / 4096 ))
O=/data/local/tmp/shimlog; rm -rf $O; mkdir -p $O
dd if=$D of=$O/shim.raw bs=4096 skip=$((B-1025-64)) count=64 2>/dev/null
dd if=$D of=$O/rescue.raw bs=4096 skip=$((B-1025-128)) count=64 2>/dev/null
dd if=$D of=$O/xnu.raw bs=4096 skip=$((B-1025)) count=1025 2>/dev/null
chmod -R a+rX $O
echo "rawdump $D blocks $B"
