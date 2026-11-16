#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$repo"
case "$repo" in *[[:space:]]*) echo 'Use a source path without whitespace' >&2; exit 2;; esac
case "${1:-}" in ""|--standalone-only) ;; *) echo "Usage: $0 [--standalone-only]" >&2; exit 2;; esac
inputs="${T5_INPUTS:-$repo/local-inputs}"
inputs=$(realpath "$inputs")
case "$inputs" in *[[:space:]]*) echo 'Use an input path without whitespace' >&2; exit 2;; esac
: "${T5_EMMC_CID:?Set T5_EMMC_CID to your device eMMC CID (32 hex digits)}"
: "${T5_DTS_SHA256:?Set T5_DTS_SHA256 to the SHA-256 of the verified Wi-Fi-only DTS partition}"
printf '%s\n' "$T5_EMMC_CID" | grep -Eq '^[0-9a-f]{32}$'
printf '%s\n' "$T5_DTS_SHA256" | grep -Eq '^[0-9a-f]{64}$'
export ARCH=arm64 LLVM=1
mkdir -p build/kernel build/ramdisk out
for path in $(awk '$1=="file" && $3 ~ /^@INPUTS@\// {sub(/^@INPUTS@\//,"",$3); print $3}' ramdisk/initramfs.list) kernel-android-original.img; do
    if [ ! -s "$inputs/$path" ]; then
        echo "Missing local input: $inputs/$path" >&2
        exit 1
    fi
done
sed "s|@T5_EMMC_CID@|$T5_EMMC_CID|g; s|@T5_DTS_SHA256@|$T5_DTS_SHA256|g" \
    ramdisk/init > build/ramdisk/init
sh -n build/ramdisk/init
for name in t5-touch-test t5-ext4-probe t5-root-control t5-emmc-readbench; do
    source="ramdisk/$name.c"
    if [ "$name" = t5-emmc-readbench ]; then source=tools/readbench.c; fi
    clang --target=aarch64-linux-gnu -O2 -Wall -Wextra -Werror \
        -ffreestanding -nostdlib -static -fuse-ld=lld -fno-stack-protector -fno-builtin \
        -Wl,-e,_start -Wl,--build-id=none "$source" -o "build/ramdisk/$name"
done
T5_REPO="$repo" T5_INPUTS_ABS="$inputs" python3 - <<'PY'
import os
from pathlib import Path
entries = Path("ramdisk/initramfs.list").read_text()
for name, value in {
    "@REPO@": os.environ["T5_REPO"],
    "@INPUTS@": os.environ["T5_INPUTS_ABS"],
}.items():
    entries = entries.replace(name, value)
Path("build/initramfs.list").write_text(entries)
PY
if [ ! -f build/kernel/.config ]; then cp kernel.config build/kernel/.config; fi
kernel/scripts/config --file build/kernel/.config --set-str INITRAMFS_SOURCE "$repo/build/initramfs.list"
make -C kernel O="$repo/build/kernel" olddefconfig
make -C kernel O="$repo/build/kernel" -j"${JOBS:-4}" \
    CONFIG_T5_HI1102_LEGACY_TEST=y CONFIG_HI110X_DRIVER_PATH=drivers/connectivity/hisi \
    Image hisilicon/hi6250-huawei-agassi2.dtb
cp build/kernel/arch/arm64/boot/Image out/
cp build/kernel/arch/arm64/boot/dts/hisilicon/hi6250-huawei-agassi2.dtb out/board.dtb
python3 build-dualboot.py --linux-dir out --android-template "$inputs/kernel-android-original.img" "$@"
