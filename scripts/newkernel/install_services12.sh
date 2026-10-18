#!/usr/bin/env bash
# 実機の Debian 12(6.12 カーネル)に、常駐サービス一式(LED・状態ページ・電源ボタン・電源断と起床・チューナー配信)を入れる。
# Debian 11 版(scripts/stage1/prepare_tree.sh)と同じサービスだが、MCU の操作は xcode4drv の I2C ioctl ではなく、カーネルの nasne-mcu ドライバの
# sysfs(互換の mcui2c: scripts/newkernel/debian12/mcui2c)で行う。ウォッチドッグの停止と PWR LED の点灯は、カーネルが起動時に済ませる(debian12/nasne-mcu-wd は確認と、他サービスの依存名のため)。
#   使い方: scripts/newkernel/install_services12.sh [--apt]     (--apt: smartmontools などの足りないパッケージを apt で入れる)
#   前提: scripts/newkernel/install_tv12.sh を先に実行してある(xcode4drv・nasne-recpt1)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
[ -f "$ROOT/local.env" ] && . "$ROOT/local.env"
IP="${NASNE_IP:?nasne の IP アドレスを NASNE_IP で指定してください(例: NASNE_IP=192.168.x.x。local.env でも可)}"
O=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5)
W="$ROOT/scripts/watchdog"; D="$ROOT/scripts/newkernel/debian12"
T=/tmp/xtools_install
[ -x $T/nasne-ledctl ] || bash "$ROOT/scripts/build_tools.sh" $T >/dev/null
S() { ssh "${O[@]}" "root@$IP" "$@"; }
C() { scp -O "${O[@]}" "$@" >/dev/null; }

S 'mkdir -p /usr/local/sbin /var/lib/nasne-wake'
# /usr/local/sbin: 互換 mcui2c(シェル)、rc.xcode4(スタブ)、nasne-ledctl(ビルドしたバイナリ)、各デーモン
C "$D/if-up-nasne-time" "root@$IP:/etc/network/if-up.d/nasne-time"
C "$D/mcui2c" "$D/rc.xcode4" $T/nasne-ledctl "$W/nasne-ledd" "$W/nasne-statusd" "$W/nasne-waked" "$W/nasne-report" "$W/nasne-nvreset" "root@$IP:/usr/local/sbin/"
# /etc/init.d
C "$D/nasne-mcu-wd" "$W/nasne-led" "$W/nasne-status" "$W/nasne-wake" "$W/nasne-powerbtn" "$W/a-nasne-poweroff" "$W/nasne-recpt1-server" "root@$IP:/etc/init.d/"
if [ "${1:-}" = "--apt" ]; then
  # 起動直後は時計が 1970 年で、apt が「Release が未来」と言うので、日付の検査を切る
  S 'A="-o Acquire::Check-Valid-Until=false -o Acquire::Check-Date=false"; apt-get $A update >/dev/null 2>&1; DEBIAN_FRONTEND=noninteractive apt-get $A install -y --no-install-recommends smartmontools hdparm ntpdate fake-hwclock >/dev/null 2>&1; which smartctl hdparm ntpdate'
fi
S 'chmod +x /etc/network/if-up.d/nasne-time /usr/local/sbin/* /etc/init.d/nasne-mcu-wd /etc/init.d/nasne-led /etc/init.d/nasne-status /etc/init.d/nasne-wake /etc/init.d/nasne-powerbtn /etc/init.d/a-nasne-poweroff /etc/init.d/nasne-recpt1-server
  update-rc.d nasne-mcu-wd defaults 05 >/dev/null; update-rc.d nasne-recpt1-server defaults 20 >/dev/null; update-rc.d nasne-status defaults 25 >/dev/null; update-rc.d nasne-led defaults 26 >/dev/null
  update-rc.d nasne-wake defaults 27 >/dev/null; update-rc.d a-nasne-poweroff defaults >/dev/null; update-rc.d nasne-powerbtn defaults >/dev/null
  ls /etc/rc2.d | grep -i nasne | tr "\n" " "; echo; ls /etc/rc0.d | grep -i nasne'
