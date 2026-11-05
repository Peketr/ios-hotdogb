#!/bin/bash
# Build and load hfsplus.ko for the running WSL kernel, from Microsoft's source
# at the matching tag. Nothing persistent: the module is gone after `wsl --shutdown`
# (build-root.sh loads it again from ~/src). WSL's own config is not changed.
set -euo pipefail
KV=$(uname -r)                       # 6.18.54.1-microsoft-standard-WSL2
TAG=linux-msft-wsl-${KV%%-*}
K=$HOME/src/wsl-kernel-${KV%%-*}
KO=$K/fs/hfsplus/hfsplus.ko
if [[ ! -f $KO ]]; then
	sudo apt-get install -y build-essential flex bison bc libelf-dev libssl-dev dwarves cpio kmod
	[[ -d $K ]] || git clone --depth 1 --branch "$TAG" https://github.com/microsoft/WSL2-Linux-Kernel.git "$K"
	cd "$K"
	if [[ -r /proc/config.gz ]]; then zcat /proc/config.gz > .config; echo "config from /proc/config.gz";
	else cp Microsoft/config-wsl .config; echo "config from Microsoft/config-wsl"; fi
	./scripts/config --module HFSPLUS_FS --disable LOCALVERSION_AUTO
	make olddefconfig >/dev/null
	grep -E '^CONFIG_(HFSPLUS_FS|NLS|NLS_UTF8|MODVERSIONS|MODULE_SIG_FORCE)=' .config || true
	make -j"$(nproc)" modules_prepare
	# vermagic must be exactly the running release.
	echo "$KV" > include/config/kernel.release
	printf '#define UTS_RELEASE "%s"\n' "$KV" > include/generated/utsrelease.h
	make -j"$(nproc)" M=fs/hfsplus KBUILD_MODPOST_WARN=1 modules
fi
modinfo "$KO" | grep -E 'vermagic|depends'
sudo modprobe nls_utf8 2>/dev/null || true
sudo insmod "$KO" || { sudo dmesg | tail -5; exit 1; }
grep -w hfsplus /proc/filesystems
