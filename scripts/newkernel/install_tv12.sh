#!/usr/bin/env bash
# 実機の Debian 12(6.12 カーネル)に、チューナー関連を入れる: xcode4drv.ko、起動時の読み込みスクリプト、ツール(nasne-recpt1・mcui2c・i2cx)。
#   使い方: scripts/newkernel/install_tv12.sh [--now]     (--now: 入れたあと、すぐ起動スクリプトを走らせて確認する)
#   前提: newkernel/xc4drv/build.sh で newkernel/out/xcode4drv.ko を作ってある。sshd が動いている(鍵は ~/.ssh の既定)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
[ -f "$ROOT/local.env" ] && . "$ROOT/local.env"
IP="${NASNE_IP:?nasne の IP アドレスを NASNE_IP で指定してください(例: NASNE_IP=192.168.x.x。local.env でも可)}"
O=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5)
KO="$ROOT/newkernel/out/xcode4drv.ko"
[ -f "$KO" ] || { echo "$KO が無い。newkernel/xc4drv/build.sh を先に" >&2; exit 1; }
T=/tmp/xtools_install
[ -x $T/nasne-recpt1 ] || bash "$ROOT/scripts/build_tools.sh" $T >/dev/null
ssh "${O[@]}" "root@$IP" 'mkdir -p /usr/local/lib/nasne /usr/local/sbin'
scp -O "${O[@]}" "$KO" "root@$IP:/usr/local/lib/nasne/xcode4drv.ko" >/dev/null
scp -O "${O[@]}" $T/nasne-recpt1 $T/mcui2c $T/i2cx "root@$IP:/usr/local/sbin/" >/dev/null
scp -O "${O[@]}" "$ROOT/scripts/newkernel/debian12/nasne-xcode4drv" "root@$IP:/etc/init.d/nasne-xcode4drv" >/dev/null
ssh "${O[@]}" "root@$IP" 'chmod +x /etc/init.d/nasne-xcode4drv /usr/local/sbin/*; update-rc.d nasne-xcode4drv defaults 02 >/dev/null; ls /etc/rc2.d | grep xcode4drv'
if [ "${1:-}" = "--now" ]; then
  ssh "${O[@]}" "root@$IP" '/etc/init.d/nasne-xcode4drv start; /etc/init.d/nasne-xcode4drv status; tail -3 /var/log/nasne-xcode4drv.log'
fi
