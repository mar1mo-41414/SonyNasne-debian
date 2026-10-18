#!/usr/bin/env bash
# 実機の Debian 12(6.12)に、スワップファイルを作って有効にする(RAM が約240MBなので)。
#   使い方: NASNE_IP=<nasne の IP> scripts/newkernel/setup_swap12.sh [サイズ(MB。既定 512)]
# スワップは、HDD の p3(XFS。起動用の init が /data にマウント済み)の /data/swapfile に置く。/etc/fstab に書くので、再起動後も有効。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
[ -f "$ROOT/local.env" ] && . "$ROOT/local.env"
IP="${NASNE_IP:?nasne の IP アドレスを NASNE_IP で指定してください(例: NASNE_IP=192.168.x.x。local.env でも可)}"
MB="${1:-512}"
O=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5)
ssh "${O[@]}" "root@$IP" "set -e
mountpoint -q /data || { echo '/data がマウントされていない。中止' >&2; exit 1; }
swapoff /data/swapfile 2>/dev/null || true
dd if=/dev/zero of=/data/swapfile bs=1M count=$MB 2>/dev/null
chmod 600 /data/swapfile; mkswap /data/swapfile >/dev/null
grep -q '^/data/swapfile' /etc/fstab || echo '/data/swapfile none swap sw,pri=-2 0 0' >> /etc/fstab
swapon /data/swapfile
swapon --show; free -m | head -3"
