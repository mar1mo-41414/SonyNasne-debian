#!/bin/sh
# nasneのGPLソース(Sony Interactive Entertainmentが公開)を取得する。サイズが大きいためgitには入れず gpl_src/ に置く。
#   カーネル(2.6.29, Viper/XCode対応、.config付き) / U-Boot 1.1.3 / AVDriver(xcode4drvの全ソース+SDKヘッダ+rc.xcode4)
# 公開ページ(現在は個別ページが消えているが、旧URLのファイル本体は取得できる):
#   https://doc.dl.playstation.net/doc/nasne-oss/   (Wayback: web.archive.org/web/20240316140143/…)
set -e
cd "$(dirname "$0")/.."
mkdir -p gpl_src && cd gpl_src
B="https://doc.dl.playstation.net/content/dam/corporate/eula/oss/nasne-oss"
for f in mips-linux-2.6.29.tar.bz2 u-boot-1.1.3.tar.bz2 AVDriver7GSP47.zip; do
  [ -f "$f" ] || curl -fL -o "$f" "$B/$f"
done
# AVDriverのバージョン別(ファームウェア版数対応): AVDriver7GSP28/33/40/44/46/47.zip (47がfirmware 2.50〜用)
sha256sum -c <<'SUM'
cd1d4ffd704687c36baea1679956d295ab95f1ace16f87960a6526c26f08aa89  mips-linux-2.6.29.tar.bz2
da6d0a30751c274a6d0dfde7ec99ae400b3f4d576c904b3001569960a84266f2  u-boot-1.1.3.tar.bz2
412121ec286b74213fed7f2a8f9390d1505a2b6ef88bbacbee1252f5392c12c3  AVDriver7GSP47.zip
SUM
