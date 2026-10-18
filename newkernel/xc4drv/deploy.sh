#!/usr/bin/env bash
# xcode4drv.ko と実験用ツール(physio・i2cx・i2cscan・boxioctl)を実機の /tmp に送り、デバイスノードを作る。--load で insmod まで行う。
#   使い方: xc4drv/deploy.sh [--load]
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"; ROOT="$(cd "$HERE/.." && pwd)"
[ -f "$ROOT/local.env" ] && . "$ROOT/local.env"
IP="${NASNE_IP:?nasne の IP アドレスを NASNE_IP で指定してください(例: NASNE_IP=192.168.x.x。local.env でも可)}"
O=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5)
TOOLS=/tmp/xtools_deploy
[ -x $TOOLS/i2cx ] || bash "$ROOT/scripts/build_tools.sh" $TOOLS >/dev/null
[ -x $TOOLS/physio ] || mipsel-linux-gnu-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -ffreestanding -fno-tree-loop-distribute-patterns -fno-stack-protector -e __start -o $TOOLS/physio "$ROOT/scripts/userspace_tools/physio.c"
scp -O "${O[@]}" $TOOLS/i2cx $TOOLS/physio $TOOLS/mcui2c $TOOLS/mtdtool "$ROOT/scripts/userspace_tools/i2cscan" "$ROOT/scripts/userspace_tools/boxioctl" \
    "$HERE/xc4src/xc4drv/xcodedriver_src/Kernel_Src/kdrv/xcode4drv.ko" "root@$IP:/tmp/" >/dev/null
ssh "${O[@]}" "root@$IP" 'chmod +x /tmp/i2cx /tmp/physio /tmp/mcui2c /tmp/mtdtool /tmp/i2cscan /tmp/boxioctl'
if [ "${1:-}" = "--load" ]; then
  ssh "${O[@]}" "root@$IP" 'insmod /tmp/xcode4drv.ko; mkdir -p /dev/vixs; for i in 0 1 2 3; do n=xcodedrv; [ $i -gt 0 ] && n=xcodedrv$((i+1)); [ -e /dev/vixs/$n ] || mknod /dev/vixs/$n c 120 $i; done; chmod 666 /dev/vixs/*; ls /dev/vixs'
fi
