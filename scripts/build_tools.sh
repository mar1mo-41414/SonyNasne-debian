#!/bin/sh
# nasne(MIPS32r2 リトルエンディアン)用の小さな静的ツール mcui2c / nasne-recpt1 / i2cx / mtdtool をビルドする(libc不使用、-nostdlib -static)。
#   使い方: sh scripts/build_tools.sh <出力ディレクトリ>
# 使うコンパイラ(先に見つかったもの):
#   1. ホストの mipsel-linux-gnu-gcc  (Debian/Ubuntu: apt install gcc-mipsel-linux-gnu)   ← Docker不要。新しいgccは memset/strlen 呼び出しを生成するので -ffreestanding 等を付ける
#   2. Docker イメージ nasne-kcc:gcc432 の mipsel-linux-gcc(古いgcc 4.3.2。docs/05)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?出力ディレクトリ}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
find_src() {   # $1=ファイル名。公開版は scripts/tools、開発版は scripts/userspace_tools にある
    for d in tools userspace_tools watchdog; do [ -f "$ROOT/scripts/$d/$1" ] && { echo "$ROOT/scripts/$d/$1"; return; }; done
    echo "ソースが無い: $1" >&2; exit 1
}
if command -v mipsel-linux-gnu-gcc >/dev/null 2>&1; then
    CC="mipsel-linux-gnu-gcc"; MODE=host
    FLAGS="-nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -ffreestanding -fno-tree-loop-distribute-patterns -fno-stack-protector -e __start"
elif command -v docker >/dev/null 2>&1 && docker image inspect nasne-kcc:gcc432 >/dev/null 2>&1; then
    MODE=docker; FLAGS="-nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -e __start"
else
    echo "MIPS用のコンパイラが無い。次のどちらかを用意してください:" >&2
    echo "  sudo apt install gcc-mipsel-linux-gnu        (推奨。Docker不要)" >&2
    echo "  docker build -t nasne-kcc:gcc432 scripts/kernel_build   (docs/05)" >&2
    exit 1
fi
for t in mcui2c:mcui2c.c nasne-recpt1:nasne_recpt1.c i2cx:i2cx.c mtdtool:mtdtool.c nasne-ledctl:nasne_ledctl.c; do
    bin=${t%%:*}; src=$(find_src "${t##*:}")
    if [ $MODE = host ]; then
        ( cd "$(dirname "$src")" && $CC $FLAGS -o "$OUT/$bin" "$(basename "$src")" )
    else
        docker run --rm -v "$(dirname "$src")":/s -v "$OUT":/o -w /s nasne-kcc:gcc432 mipsel-linux-gcc $FLAGS -o "/o/$bin" "$(basename "$src")"
    fi
    echo "OK: $OUT/$bin ($MODE)"
done
