#!/usr/bin/env bash
# initramfs のディレクトリ(busybox + /init スクリプト)を作る。busybox は Debian 12 の busybox-static(mipsel)。
#   使い方: mk_root.sh <出力ディレクトリ> [boot|c]   boot=busybox なしの本番用 init(init_boot.c)、c=最小の確認用 init(init.c)、省略=busybox+init.sh(デバッグ用)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:?出力ディレクトリ}"
if [ "${2:-}" = "boot" ]; then   # 本番用: busybox なしの小さな init(約4KB)だけ
  rm -rf "$OUT"; mkdir -p "$OUT"/{dev,proc,sys,p3,newroot}
  mipsel-linux-gnu-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -Os -fno-builtin -e __start -o "$OUT/init" "$HERE/init_boot.c"
  echo "initramfs(boot): $OUT ($(du -sh "$OUT" | cut -f1))"; exit 0
fi
BB="${BUSYBOX:-$HERE/../dl/bb/bin/busybox}"
if [ ! -x "$BB" ]; then
  mkdir -p "$HERE/../dl/bb" && cd "$HERE/../dl/bb"
  curl -fsSLO "https://deb.debian.org/debian/pool/main/b/busybox/busybox-static_1.35.0-4+deb12u1+b1_mipsel.deb"
  ar x busybox-static_*_mipsel.deb && tar xf data.tar.xz ./bin/busybox
fi
rm -rf "$OUT"; mkdir -p "$OUT"/{bin,sbin,dev,proc,sys,tmp,p3,newroot,usr/bin,usr/sbin,etc}
install -m755 "$BB" "$OUT/bin/busybox"
# アプリレットの symlink(qemu-user で busybox 自身に一覧を出させる)
for a in $(qemu-mipsel-static "$BB" --list-full 2>/dev/null); do
  [ -e "$OUT/$a" ] || { mkdir -p "$OUT/$(dirname "$a")"; ln -s /bin/busybox "$OUT/$a"; }
done
mipsel-linux-gnu-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -Os -fno-builtin -e __start -o "$OUT/bin/mapsum" "$HERE/mapsum.c"   # 診断用
if [ "${2:-}" = "c" ]; then
  mipsel-linux-gnu-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -fno-builtin -e __start -o "$OUT/init" "$HERE/init.c"
else
  install -m755 "$HERE/init.sh" "$OUT/init"
fi
ls "$OUT" | head -20 >/dev/null
echo "initramfs: $OUT ($(du -sh "$OUT" | cut -f1))"
