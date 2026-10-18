#!/usr/bin/env bash
# 作業ツリー(xc4src。ここを編集して 6.12 向けに直す)と、元の GPL ソース(xc4src_orig)の差分を、xc4patches/xcode4drv-6.12.patch に書き出す。
# ファーム(Sony 由来のバイナリ配列)は変更していないので、パッチには入らない(再配布しない)。
#   使い方: newkernel/xc4drv/make_patch.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SUB=xc4drv/xcodedriver_src/Kernel_Src
OUT="$HERE/xc4patches/xcode4drv-6.12.patch"
mkdir -p "$HERE/xc4patches"
cd "$HERE"
diff -ruN \
    --exclude=Kbuild --exclude='*.o' --exclude='*.ko' --exclude='*.mod' --exclude='*.mod.c' --exclude='.*.cmd' \
    --exclude=modules.order --exclude='*.symvers' \
    "xc4src_orig/$SUB" "xc4src/$SUB" \
  | sed -E "s#^(---) xc4src_orig/$SUB/([^[:space:]]*).*#\1 a/\2#; s#^(\+\+\+) xc4src/$SUB/([^[:space:]]*).*#\1 b/\2#; s#^diff .*#diff -ruN a b#" > "$OUT" || true
echo "$OUT: $(grep -c '^--- a/' "$OUT") ファイル、$(wc -l < "$OUT") 行"
grep '^--- a/' "$OUT" | sed 's/^--- a\///'
