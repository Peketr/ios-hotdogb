#!/bin/bash
# One perf test run (Git Bash on Windows): boots hotdogb-run-current.sh as NAME for SECS (default 300) in WSL,
# waits for the home screen, runs hotdogb-swipebench.py twice, prints the frame stats summary.
#   tools/hotdogb-perftest.sh NAME [EXTRA_BOOT_ARGS]
cd "$(dirname "$0")/.."
name=$1; extra=${2:-}; secs=${SECS:-300}
MSYS_NO_PATHCONV=1 wsl.exe -- env SECS=$secs EXTRA="$extra" bash /mnt/c/Users/Peketr/ios-flame/src/tools/hotdogb-run-current.sh "$name" > logs/perftest-$name.out 2>&1 &
L=logs/runs/$name/live.txt
for i in $(seq 1 200); do grep -aq "5s: [1-9][0-9] swaps" $L 2>/dev/null && break; sleep 3; done
sleep 25
for k in 1 2; do timeout 150 python tools/hotdogb-swipebench.py 30 2>&1 | grep -E "fps:|GPU busy|vsync late"; sleep 5; done
grep -a "####" $L | grep -E "REFRESH|FRAME STATS|COMPOS|PRESENT|LAYER" | cut -c1-160 | sort -u | head
wait
grep -E "####|phone not" logs/perftest-$name.out | cut -c1-160 | tail -5
