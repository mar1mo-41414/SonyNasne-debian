#!/bin/sh
# nasneカーネル(GPLソース)を Docker コンテナ内でビルドする。
#   事前: docker build -t nasne-kcc:gcc432 scripts/kernel_build   (GCC 4.3.2 + binutils 2.19.1 の mipsel クロス環境)
#         scripts/fetch_gpl_sources.sh でソース取得、gpl_src/build/mips-linux-2.6.29 に展開済みであること
#   使い方: scripts/kernel_build/build.sh [出力ディレクトリ]   -> vmlinux_new.bin (生バイナリ) を出力
#   環境変数 NASNE_CUSTOM=1 を付けると、内蔵initramfs(initramfs/nasne_init)と rdinit既定の変更(/init -> /nasne_init)を入れる。
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE=$ROOT/scripts/kernel_build
SRC=$ROOT/gpl_src/build/mips-linux-2.6.29
OUT=${1:-$ROOT/gpl_src/build/out}
mkdir -p "$OUT"
if [ -n "$NASNE_CUSTOM" ]; then
    rm -rf "$SRC/nasne_initramfs"; cp -r "$HERE/initramfs" "$SRC/nasne_initramfs"
    sed -i 's|ramdisk_execute_command = "/init";|ramdisk_execute_command = "/nasne_init";|' "$SRC/init/main.c"
    sed -i 's|^CONFIG_INITRAMFS_SOURCE=.*|CONFIG_INITRAMFS_SOURCE="/work/nasne_initramfs/initramfs.list"|' "$SRC/.config"
    # Sonyのソースは populate_rootfs() の内蔵initramfs展開を #if 0 で無効化している。有効に戻す。
    sed -i '/^static int __init populate_rootfs/,/^rootfs_initcall/ s/^#if 0$/#if 1/' "$SRC/init/initramfs.c"
    touch "$SRC/init/initramfs.c"
    # 内蔵initramfsを変えても再ビルドされるようスクリプトの変更を反映させる
    touch "$SRC/nasne_initramfs/nasne_init" "$SRC/init/main.c"
else
    sed -i 's|ramdisk_execute_command = "/nasne_init";|ramdisk_execute_command = "/init";|' "$SRC/init/main.c"
    sed -i 's|^CONFIG_INITRAMFS_SOURCE=.*|CONFIG_INITRAMFS_SOURCE=""|' "$SRC/.config"
    sed -i '/^static int __init populate_rootfs/,/^rootfs_initcall/ s/^#if 1$/#if 0/' "$SRC/init/initramfs.c"
fi
docker run --rm --user "$(id -u):$(id -g)" -v "$SRC":/work -v "$OUT":/out nasne-kcc:gcc432 sh -c '
  make ARCH=mips CROSS_COMPILE=mipsel-linux- oldconfig </dev/null >/dev/null 2>&1
  make ARCH=mips CROSS_COMPILE=mipsel-linux- -j6 vmlinux
  mipsel-linux-objcopy -O binary -S -R .reginfo -R .note -R .comment -R .mdebug -R .MIPS.abiflags vmlinux /out/vmlinux_new.bin'
echo "出力: $OUT/vmlinux_new.bin  (KNLセグメント化: python3 scripts/build_knl.py build <spi.bin> KNL_new.bin --slot KNL --kernel $OUT/vmlinux_new.bin)"
