#!/usr/bin/env bash
# nasne の新カーネル(Linux 6.12 LTS)をビルドする。
#   使い方: newkernel/build.sh [--clean]
#   できるもの: newkernel/out/new/vmlinux(ELF)、vmlinux.bin(生バイナリ。kexec で読み込む)、KNL 向けの gzip 後サイズの表示
#   環境変数: KVER(既定 6.12.112)、INITRAMFS(initramfs のディレクトリ。既定 newkernel/initramfs/root。無ければ init.c から最小のを作る)、JOBS
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
KVER="${KVER:-6.12.112}"
SRC="$HERE/linux-$KVER"
OUT="$HERE/out/new"
CC_PREFIX="${CROSS_COMPILE:-mipsel-linux-gnu-}"
JOBS="${JOBS:-$(nproc)}"
mkdir -p "$HERE/dl" "$OUT"

if [ ! -d "$SRC" ]; then
  [ -f "$HERE/dl/linux-$KVER.tar.xz" ] || curl -fL -o "$HERE/dl/linux-$KVER.tar.xz" "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KVER.tar.xz"
  tar xf "$HERE/dl/linux-$KVER.tar.xz" -C "$HERE"
fi
# パッチ(Kconfig などの小さな差分)。適用済みなら飛ばす
for p in "$HERE"/patches/*.patch; do
  if patch -p1 -R --dry-run -s -d "$SRC" < "$p" >/dev/null 2>&1; then :; else patch -p1 -s -d "$SRC" < "$p"; fi
done
# 新規ファイル(プラットフォーム一式、DTS、defconfig)
cp -a "$HERE/overlay/." "$SRC/"
# カーネルの追加の引数(DT の bootargs の @EXTRA_BOOTARGS@ に入る)。例: ip=…(固定 IP)、netconsole=…。環境依存なので、リポジトリには入れず、local.env か環境変数で渡す
[ -f "$HERE/../local.env" ] && . "$HERE/../local.env"
sed -i "s|@EXTRA_BOOTARGS@|${NASNE_EXTRA_BOOTARGS:-}|" "$SRC/arch/mips/boot/dts/xcode/nasne.dts"

# initramfs(busybox + /init スクリプト。INIT=c なら最小の C 版 init)
INITRAMFS="${INITRAMFS:-$HERE/initramfs/root}"
[ -d "$INITRAMFS" ] || "$HERE/initramfs/mk_root.sh" "$INITRAMFS" "${INIT:-}"

MK=(make -C "$SRC" O="$OUT" ARCH=mips CROSS_COMPILE="$CC_PREFIX" -j"$JOBS")
[ "${1:-}" = "--clean" ] && rm -rf "$OUT" && mkdir -p "$OUT"
[ -f "$OUT/.config" ] || "${MK[@]}" nasne_defconfig
"$SRC/scripts/config" --file "$OUT/.config" --set-str INITRAMFS_SOURCE "$INITRAMFS"
# xcode4drv.ko(モジュール)を使うため MODULES=y。ただし輸出シンボルの表(約90KB gzip)を、xcode4drv が使う分だけに絞る(KNL スロットに収めるため)
"$SRC/scripts/config" --file "$OUT/.config" -e MODULES -e TRIM_UNUSED_KSYMS --set-str UNUSED_KSYMS_WHITELIST "$HERE/xc4drv/ksyms_whitelist.txt"
"${MK[@]}" olddefconfig
# vmlinux と、モジュール(USB など。TRIM_UNUSED_KSYMS は、同時に作るモジュールが使うシンボルを残す)
"${MK[@]}" vmlinux modules
"${CC_PREFIX}objcopy" -O binary -S -R .reginfo -R .note -R .comment -R .mdebug -R .MIPS.abiflags "$OUT/vmlinux" "$OUT/vmlinux.bin"
ENTRY=$(LC_ALL=C "${CC_PREFIX}readelf" -h "$OUT/vmlinux" | awk '/Entry point/{print $4}')
echo "エントリ: $ENTRY"
ls -l "$OUT/vmlinux.bin"
{ head -c $((0x40000)) /dev/zero; cat "$OUT/vmlinux.bin"; } > "$OUT/vmlinux.knl.bin"   # KNL スロットの中身(4段目ブートは 0x10000000 へ展開し、0xB0040000 へ飛ぶ)
GZ=$(gzip -9 -c "$OUT/vmlinux.knl.bin" | wc -c)
echo "KNL に入れる gzip 後サイズ(先頭 0x40000 のゼロ埋め込み): $GZ バイト / 上限 $((0x280000 - 64)) バイト"
"${CC_PREFIX}size" "$OUT/vmlinux"
