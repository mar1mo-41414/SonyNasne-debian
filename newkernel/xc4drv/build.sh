#!/usr/bin/env bash
# xcode4drv.ko(Linux 6.12 向け)を、GPL ソースから作る。ファーム(Sony 由来の配列)はソースの一部で、再配布しないため、リポジトリには入れず、
# 公開されている AVDriver7GSP47.zip(scripts/fetch_gpl_sources.sh で取得)を展開して、パッチ(newkernel/xc4patches/)を当ててビルドする。
#   使い方: newkernel/xc4drv/build.sh [カーネルのビルドディレクトリ(既定 newkernel/out/new)]
#   前提: newkernel/build.sh でカーネルを作ってあること(MODULES=y、modules_prepare 済み、Module.symvers あり)
#   できるもの: newkernel/out/xcode4drv.ko
#   環境変数: XC_EXTRA(ccflags に足す定義。例 -DDEBUG_LINUX でドライバの診断出力)
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"; ROOT="$(cd "$HERE/.." && pwd)"
KOUT="${1:-$HERE/out/new}"
KSRC="$HERE/linux-6.12.112"
ZIP="$ROOT/gpl_src/AVDriver7GSP47.zip"
W="$HERE/xc4build"
SUB=xc4drv/xcodedriver_src/Kernel_Src
[ -f "$ZIP" ] || bash "$ROOT/scripts/fetch_gpl_sources.sh" >/dev/null
if [ ! -d "$W/$SUB/kdrv" ]; then
  rm -rf "$W"; mkdir -p "$W"
  ( cd "$W" && unzip -oq "$ZIP" 'xc4drv/xcodedriver_src/*' && cd xc4drv/xcodedriver_src && tar xjf XC4_AVDriver_Src_20140709.tar.bz2 )
  patch -p1 -s -d "$W/$SUB" < "$HERE/xc4patches/xcode4drv-6.12.patch"
fi
K="$W/$SUB/kdrv"
sed "s#@XC_COMPAT_DIR@#$HERE/xc4drv#; s#@XC_EXTRA@#${XC_EXTRA:-}#" "$HERE/xc4drv/Kbuild.in" > "$K/Kbuild"
[ -f "$KOUT/Module.symvers" ] || make -C "$KSRC" O="$KOUT" ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- -j"$(nproc)" modules >/dev/null
make -C "$KSRC" O="$KOUT" M="$K" ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- -j"$(nproc)" modules 2>&1 | grep -E "error|ERROR|LD \[M\]" || true
cp "$K/xcode4drv.ko" "$HERE/out/xcode4drv.ko"
ls -l "$HERE/out/xcode4drv.ko"
