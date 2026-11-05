#!/bin/bash
# OnePlus 7T (hotdogb) probe build: system clang/lld (Linux or WSL),
# no Homebrew/Xcode/AOSP tools needed.
#   shim/build-hotdogb.sh STOCK_DTB [OUT]
# STOCK_DTB: the dtb section of the 7T's own boot.img (20 SM8150 DTBs).
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd .. && pwd)
DTB=$(realpath "$1")
CC="clang --target=aarch64-none-elf"
CFLAGS="-O2 -ffreestanding -nostdlib -fno-builtin -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align
	-fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -DSHIM_HOTDOGB ${SHIM_CFLAGS:-}"
SRCS="start.S shim.c console.c log.c util.c mmu.c fdt.c loader.c ufs.c spmi.c sde.c showlog.c diag.c disptest.c gpuprobe.c gpufw.S smmuprobe.c ramlog.c"
mkdir -p out-hotdogb
objs=
for f in $SRCS; do
	$CC $CFLAGS -c "$f" -o "out-hotdogb/${f%.*}.o"
	objs="$objs out-hotdogb/${f%.*}.o"
done
abs=$(llvm-readelf -r $(echo "$objs" | sed 's| out-hotdogb/start.o||') | grep -c R_AARCH64_ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
ld.lld -static -nostdlib -T shim.ld --build-id=none -N $objs -o out-hotdogb/shim.elf
llvm-objcopy -O binary out-hotdogb/shim.elf out-hotdogb/Image
size=$(llvm-nm out-hotdogb/shim.elf | awk '$3=="_end"{print $1}')
truncate -s $((16#$size)) out-hotdogb/Image
python3 "$ROOT/tools/mkboot_hotdogb.py" out-hotdogb/Image "$DTB" "${2:-out-hotdogb/shim-hotdogb.img}"
