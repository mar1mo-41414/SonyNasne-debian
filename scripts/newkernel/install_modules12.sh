#!/usr/bin/env bash
# newkernel/build.sh で作った、カーネルのモジュール(USB の ehci-xcode・usbcore・usb-storage など)を、実機の Debian 12 の /lib/modules/<リリース>/ へ入れて、depmod する。
#   使い方: scripts/newkernel/install_modules12.sh        (NASNE_IP か local.env で、実機の IP を指定)
#   xcode4drv.ko は、これとは別(install_tv12.sh。アウトオブツリー)。モジュールの読み込み: modprobe ehci-xcode  (依存する usbcore などは自動)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
[ -f "$ROOT/local.env" ] && . "$ROOT/local.env"
IP="${NASNE_IP:?nasne の IP アドレスを NASNE_IP で指定してください(例: NASNE_IP=192.168.x.x。local.env でも可)}"
O=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5)
K="$ROOT/newkernel/linux-6.12.112"; OUT="$ROOT/newkernel/out/new"; STAGE="$ROOT/newkernel/out/modroot"
rm -rf "$STAGE"
make -C "$K" O="$OUT" ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- INSTALL_MOD_PATH="$STAGE" INSTALL_MOD_STRIP=1 modules_install >/dev/null
REL=$(ls "$STAGE/lib/modules")
rm -f "$STAGE/lib/modules/$REL/build" "$STAGE/lib/modules/$REL/source"
echo "カーネルのリリース: $REL($(find "$STAGE/lib/modules/$REL" -name '*.ko' | wc -l) モジュール、$(du -sk "$STAGE/lib/modules/$REL" | cut -f1)KB)"
# 注意: Debian 12 の / は usrmerge で、/lib は /usr/lib へのシンボリックリンク。lib/ を含む tar をそのまま / に展開すると、GNU tar は
#   そのリンクを実ディレクトリに置き換えてしまい、ld.so など /lib にあるものが全部消えて、sshd も bash も動かなくなる(2026-10-18 に実際に起きた)。
#   なので、(1) ステージは usr/lib/modules に作り直し(/lib/modules は /usr/lib/modules と同じ)、(2) --keep-directory-symlink を付ける。
mkdir -p "$STAGE/usr/lib"; mv "$STAGE/lib/modules" "$STAGE/usr/lib/modules"; rmdir "$STAGE/lib"
tar -C "$STAGE" -cf - usr | ssh "${O[@]}" "root@$IP" 'test -L /lib || { echo "/lib がシンボリックリンクでない。中止" >&2; exit 1; }; tar --keep-directory-symlink --no-overwrite-dir -C / -xf - && depmod -a && ls /lib/modules && test -L /lib && echo "/lib のリンクは無事"'
