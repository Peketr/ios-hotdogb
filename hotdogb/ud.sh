#!/system/bin/sh
O=/data/local/tmp/ud; rm -rf $O; mkdir -p $O
for p in sda sda22; do echo "$p start=$(cat /sys/class/block/$p/start 2>/dev/null) size=$(cat /sys/class/block/$p/size) lbs=$(cat /sys/block/sda/queue/logical_block_size)"; done > $O/info.txt
grep -E " /data | /metadata " /proc/mounts >> $O/info.txt
df /data >> $O/info.txt
ls -l /dev/block/by-name/userdata /dev/block/by-name/metadata >> $O/info.txt
ls -l /dev/block/dm-* >> $O/info.txt 2>&1
dmsetup table >> $O/info.txt 2>&1
dd if=/dev/block/by-name/userdata of=$O/ud_head.bin bs=4096 count=4 2>/dev/null
dd if=/dev/block/sda of=$O/gpt.bin bs=4096 count=6 2>/dev/null
cat /vendor/etc/fstab.qcom 2>/dev/null | grep -E "userdata|metadata" >> $O/info.txt
chmod -R a+rX $O
cat $O/info.txt
