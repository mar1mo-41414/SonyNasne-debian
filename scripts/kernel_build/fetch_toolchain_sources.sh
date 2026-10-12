#!/bin/sh
# カーネルビルド用 Docker イメージ(nasne-kcc:gcc432)の素材 binutils 2.19.1 / gcc-core 4.3.2 を GNU から取得し、sha256 を確認する。
#   使い方: sh scripts/kernel_build/fetch_toolchain_sources.sh   → scripts/kernel_build/dl/ に置く(その後 docker build -t nasne-kcc:gcc432 scripts/kernel_build)
set -e
cd "$(dirname "$0")"
mkdir -p dl && cd dl
get() {   # $1=ファイル名 $2=URL $3=sha256
    if [ ! -f "$1" ]; then
        curl -fL --retry 3 -o "$1.part" "$2"
        mv "$1.part" "$1"
    fi
    echo "$3  $1" | sha256sum -c - >/dev/null || { echo "sha256 が合わない: $1(消して取り直してください)" >&2; exit 1; }
    echo "OK: $1"
}
get binutils-2.19.1.tar.bz2 https://ftp.gnu.org/gnu/binutils/binutils-2.19.1.tar.bz2 2dbd2c554b70d915c5b32fcfd401d03afcbf202529059c7a80af3993885ad1c2
get gcc-core-4.3.2.tar.bz2  https://ftp.gnu.org/gnu/gcc/gcc-4.3.2/gcc-core-4.3.2.tar.bz2 0687e7279d78eb963ac34a3b06a0a6a1d1e9a26807ef4939cf4a2ad5f062e75a
