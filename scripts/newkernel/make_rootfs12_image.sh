#!/usr/bin/env bash
# build_rootfs12.sh で作った Debian 12 の rootfs ディレクトリから、ext4 のイメージ(ループ用)を作る。
#   使い方: sudo scripts/newkernel/make_rootfs12_image.sh <rootfsディレクトリ> <出力 root.img> [サイズ MiB(既定1024)]
set -euo pipefail
SRC="${1:?rootfs ディレクトリ}"; OUT="${2:?出力ファイル}"; MB="${3:-1024}"
[ "$(id -u)" -eq 0 ] || { echo "root が必要です" >&2; exit 1; }
rm -f "$OUT"
truncate -s "${MB}M" "$OUT"
mkfs.ext4 -q -F -L nasne-deb12 -O ^has_journal,^metadata_csum,^64bit -d "$SRC" "$OUT"
e2fsck -fn "$OUT" | tail -1
ls -ls "$OUT"
