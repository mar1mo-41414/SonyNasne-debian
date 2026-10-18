#!/usr/bin/env bash
# xcode4drv(GPL 版)を 6.12 向けにビルドして、エラーの要約を出す。使い方: xc4drv/xc.sh [make の追加引数...]
HERE="$(cd "$(dirname "$0")/.." && pwd)"
K="$HERE/xc4src/xc4drv/xcodedriver_src/Kernel_Src/kdrv"
sed "s#@XC_COMPAT_DIR@#$HERE/xc4drv#; s#@XC_EXTRA@#${XC_EXTRA:-}#" "$HERE/xc4drv/Kbuild.in" > "$K/Kbuild"
make -C "$HERE/linux-6.12.112" O="$HERE/out/new" M="$K" ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- -k -j"$(nproc)" modules "$@" > /tmp/xc4build.log 2>&1
echo "error: $(grep -c 'error:' /tmp/xc4build.log)  warning: $(grep -c 'warning:' /tmp/xc4build.log)  (ログ /tmp/xc4build.log)"
grep 'error:' /tmp/xc4build.log | sed -E 's/^.*Kernel_Src\///' | awk -F: '{print $1}' | sort | uniq -c | sort -rn | head -${TOPN:-15}
