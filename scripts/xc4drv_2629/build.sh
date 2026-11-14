#!/bin/sh
# xcode4drv.ko を、旧カーネル(2.6.29)向けに GPL ソースからビルドする。fw_secondary.patch(Issue #14 の対策)を当てる。
# ファーム(Sony 由来の配列)はソースの一部で、再配布しないため、リポジトリには入れず、公開の AVDriver7GSP47.zip から作る。
#   前提: scripts/fetch_gpl_sources.sh、docker build -t nasne-kcc:gcc432 scripts/kernel_build、
#         NASNE_CUSTOM=1 sh scripts/kernel_build/build.sh で gpl_src/build/mips-linux-2.6.29 をビルド済み(実機のカーネルと同じ設定)
#   使い方: sh scripts/xc4drv_2629/build.sh [出力ファイル(既定 gpl_src/build/xcode4drv.ko)]
#   読み込み: insmod xcode4drv.ko fw_secondary=24(付けなければ純正と同じ動作。推奨値は 24)。docs/26 の 3.10・3.11
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE=$ROOT/scripts/xc4drv_2629
KSRC=$ROOT/gpl_src/build/mips-linux-2.6.29
W=$ROOT/gpl_src/build/xc4_2629
OUT=${1:-$ROOT/gpl_src/build/xcode4drv.ko}
[ -f "$KSRC/Module.symvers" ] || [ -f "$KSRC/vmlinux" ] || { echo "先に 2.6.29 のカーネルをビルドしてください($KSRC)" >&2; exit 1; }
rm -rf "$W"; mkdir -p "$W"
python3 -I - "$ROOT/gpl_src/AVDriver7GSP47.zip" "$W" <<'PY'
import sys, zipfile, tarfile, io
z = zipfile.ZipFile(sys.argv[1])
name = [n for n in z.namelist() if n.endswith("XC4_AVDriver_Src_20140709.tar.bz2")][0]
t = tarfile.open(fileobj=io.BytesIO(z.read(name)), mode="r:bz2")
pre = [m.name for m in t.getmembers() if m.name.endswith("Kernel_Src/kdrv/drv/mips.c")][0].rsplit("kdrv/", 1)[0]
for m in t.getmembers():
    if m.name.startswith(pre) and (m.isfile() or m.isdir()) and m.name[len(pre):].split("/")[0] in ("kdrv", "commsdk"):
        m.name = m.name[len(pre):]
        t.extract(m, sys.argv[2])
PY
patch -p1 -s -d "$W" < "$HERE/fw_secondary.patch"
cp "$HERE/Kbuild" "$W/kdrv/Kbuild"
docker run --rm --user "$(id -u):$(id -g)" -v "$KSRC":/work -v "$W":/xc nasne-kcc:gcc432 sh -c '
  make ARCH=mips CROSS_COMPILE=mipsel-linux- -j8 modules_prepare >/dev/null 2>&1
  make ARCH=mips CROSS_COMPILE=mipsel-linux- M=/xc/kdrv -j8 modules 2>&1 | grep -E "error|LD \[M\]" || true'
mipsel-linux-gnu-strip --strip-debug -o "$OUT" "$W/kdrv/xcode4drv.ko" 2>/dev/null || cp "$W/kdrv/xcode4drv.ko" "$OUT"
ls -l "$OUT"
