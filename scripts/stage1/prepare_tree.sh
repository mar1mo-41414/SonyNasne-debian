#!/usr/bin/env bash
# prepare_tree.sh — nasneのp3に置く(または .dlm に同梱する)Debian wheezy (mipsel) のrootfsツリーを作る。
#
#   scripts/stage1/prepare_tree.sh --out /path/to/tree --knl knl_new.bin --official <KRST3101_xxxx_SECURE.dlm | 00550066.dlm>
#                                  [--ssh-pubkey ~/.ssh/id_rsa.pub] [--bins DIR]
#
# やること(すべて --out の中だけ。sudo を使う):
#   1. --out が無ければ build_debian_rootfs.sh で Debian wheezy を作る(debootstrap + qemu-user-static、sshd、root用RSA鍵)
#   2. 公式ファームから xcode4drv.ko / rc.xcode4 を取り出して置く(Sonyのファイルなので再配布しないこと)
#   3. MIPS用バイナリ(mcui2c nasne-recpt1 i2cx mtdtool)をビルドして置く(--bins にビルド済みがあればそれを使う)
#   4. 起動時サービス(nasne-mcu-wd, nasne-recpt1-server)を update-rc.d、直起動の印、起動成功の印(rc.local)、ネットワーク設定
#   5. 段階1スクリプト nasne-stage1.sh、公式アプリを1回だけ起動する nasne-boot-switch、自作カーネル knl_new.bin(+sha256)を置く
#   --extra-packages "pkg ..." を付けると、そのDebianパッケージも追加でインストールする(例: テスト用に xfsprogs)
set -euo pipefail
export LC_ALL=C
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
OUT=; KNL=; OFFICIAL=; PUBKEY=${SSH_PUBKEY:-$HOME/.ssh/id_rsa.pub}; BINS=; EXTRA=
while [ $# -gt 0 ]; do
  case $1 in
    --out) OUT=$2; shift 2;; --knl) KNL=$2; shift 2;; --official) OFFICIAL=$2; shift 2;;
    --ssh-pubkey) PUBKEY=$2; shift 2;; --bins) BINS=$2; shift 2;; --extra-packages) EXTRA=$2; shift 2;;
    *) echo "不明な引数: $1" >&2; exit 2;;
  esac
done
[ -n "$OUT" ] && [ -n "$KNL" ] && [ -n "$OFFICIAL" ] || { sed -n 2,14p "$0"; exit 2; }
[ -s "$KNL" ] || { echo "kernel segment $KNL が無い" >&2; exit 1; }
[ -s "$OFFICIAL" ] || { echo "$OFFICIAL が無い" >&2; exit 1; }
python3 -I "$ROOT/scripts/build_knl.py" verify "$KNL" >/dev/null || { echo "$KNL はKNLセグメントとして不正(build_knl.py verify)" >&2; exit 1; }
KSIZE=$(stat -c %s "$KNL"); [ "$KSIZE" -le $((0x280000)) ] || { echo "$KNL が大きすぎる($KSIZE > 0x280000)" >&2; exit 1; }
SUDO=; [ "$(id -u)" = 0 ] || SUDO=sudo

# ---- 1. Debian ----
if [ ! -d "$OUT/etc" ]; then
  echo "== Debian wheezy を作る(debootstrap。数分かかる)"
  $SUDO env SSH_PUBKEY="$PUBKEY" "$ROOT/scripts/build_debian_rootfs.sh" "$OUT"
fi

# ---- 2. 公式ドライバ ----
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
python3 -I "$HERE/make_stage1_dlm.py" extract-drivers --official "$OFFICIAL" --dest "$WORK"
$SUDO install -d -m 755 "$OUT/usr/local/sbin" "$OUT/usr/local/share/nasne-stage1"
$SUDO install -m 755 "$WORK/xcode4drv.ko" "$WORK/rc.xcode4" "$OUT/usr/local/sbin/"

# ---- 3. バイナリ ----
if [ -z "$BINS" ]; then
  BINS=$WORK/bin; mkdir -p "$BINS"
  command -v docker >/dev/null || { echo "dockerが無い: --bins でビルド済みバイナリのディレクトリを指定してください" >&2; exit 1; }
  docker image inspect nasne-kcc:gcc432 >/dev/null 2>&1 || { echo "nasne-kcc:gcc432 が無い(docs/05 ①)" >&2; exit 1; }
  for p in "watchdog/mcui2c.c:mcui2c" "tools/nasne_recpt1.c:nasne-recpt1" "tools/i2cx.c:i2cx" "tools/mtdtool.c:mtdtool"; do
    src=${p%%:*}; bin=${p##*:}
    docker run --rm -v "$ROOT":/w -v "$BINS":/o -w "/w/scripts/$(dirname "$src")" nasne-kcc:gcc432 \
      mipsel-linux-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -e __start -o "/o/$bin" "$(basename "$src")"
  done
fi
for b in mcui2c nasne-recpt1 i2cx mtdtool; do
  [ -s "$BINS/$b" ] || { echo "$BINS/$b が無い" >&2; exit 1; }
  file "$BINS/$b" | grep -q 'MIPS' || { echo "$BINS/$b はMIPSバイナリではない" >&2; exit 1; }
  $SUDO install -m 755 "$BINS/$b" "$OUT/usr/local/sbin/$b"
done

# ---- 4. サービス・設定 ----
$SUDO install -m 755 "$ROOT/scripts/watchdog/nasne-mcu-wd" "$ROOT/scripts/watchdog/nasne-recpt1-server" "$OUT/etc/init.d/"
$SUDO touch "$OUT/etc/nasne-direct-boot"
$SUDO tee "$OUT/etc/rc.local" >/dev/null <<'RC'
#!/bin/sh -e
# 起動成功の印(自作カーネルの /nasne_init が起動カウンタとして見る。空にする=成功)
: > /var/lib/nasne-boot-count
exit 0
RC
$SUDO chmod 755 "$OUT/etc/rc.local"
$SUDO install -d "$OUT/var/lib"
$SUDO tee "$OUT/etc/network/interfaces" >/dev/null <<'NET'
auto lo
iface lo inet loopback

# カーネルにSynopGMACが組み込み済み。udev(2.6.29では動かない)に頼らず auto で上げる
auto eth0
iface eth0 inet dhcp
NET
echo nasne-debian | $SUDO tee "$OUT/etc/hostname" >/dev/null

# update-rc.d は MIPS のDebian側のコマンドなので qemu-user で chroot して実行する
$SUDO cp "$(command -v qemu-mipsel-static)" "$OUT/usr/bin/"
$SUDO mount -t proc proc "$OUT/proc"; $SUDO mount -t sysfs sysfs "$OUT/sys"
cleanup_mounts() { $SUDO umount "$OUT/proc" "$OUT/sys" 2>/dev/null || $SUDO umount -l "$OUT/proc" "$OUT/sys" 2>/dev/null || true; $SUDO rm -f "$OUT/usr/bin/qemu-mipsel-static"; }
trap 'cleanup_mounts; rm -rf "$WORK"' EXIT
if [ -n "$EXTRA" ]; then
  $SUDO cp /etc/resolv.conf "$OUT/etc/resolv.conf"
  $SUDO chroot "$OUT" env DEBIAN_FRONTEND=noninteractive apt-get update
  $SUDO chroot "$OUT" env DEBIAN_FRONTEND=noninteractive apt-get install -y --force-yes $EXTRA
  $SUDO rm -rf "$OUT/var/lib/apt/lists"/* "$OUT/var/cache/apt/archives"/*.deb
fi
$SUDO chroot "$OUT" update-rc.d nasne-mcu-wd defaults 05
$SUDO chroot "$OUT" update-rc.d nasne-recpt1-server defaults 20
cleanup_mounts
if mount | grep -q " $OUT/\(proc\|sys\) "; then echo "umountに失敗: $OUT/proc|sys が残っている" >&2; exit 1; fi
ls "$OUT"/etc/rc2.d/ | grep -q nasne-mcu-wd || { echo "update-rc.d が効いていない" >&2; exit 1; }

# ---- 5. 段階1 ----
$SUDO install -m 755 "$HERE/nasne-stage1.sh" "$HERE/nasne-boot-switch" "$OUT/usr/local/sbin/"
$SUDO install -d "$OUT/data"
$SUDO install -m 644 "$KNL" "$OUT/usr/local/share/nasne-stage1/knl_new.bin"
( cd "$(dirname "$KNL")" && sha256sum "$(basename "$KNL")" | sed 's/  .*/  knl_new.bin/' ) | $SUDO tee "$OUT/usr/local/share/nasne-stage1/knl_new.bin.sha256" >/dev/null

echo "== 完了: $OUT"
$SUDO du -sh "$OUT" | tail -1
