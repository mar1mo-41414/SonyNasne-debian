#!/usr/bin/env bash
# Debian 12 bookworm (mipsel) の rootfs を debootstrap + qemu-user-static で作る。
# nasne の新カーネル(6.12)用。systemd ではなく sysvinit-core を使う。
#   使い方: sudo scripts/newkernel/build_rootfs12.sh <出力ディレクトリ>
# 注意: bookworm の mipsel はセキュリティ更新の対象外(docs/19 §6)。
set -euo pipefail
OUT="${1:?使い方: $0 <出力ディレクトリ>}"
SSH_PUBKEY="${SSH_PUBKEY:-$(getent passwd "${SUDO_USER:-$USER}" | cut -d: -f6)/.ssh/id_rsa.pub}"
MIRROR="${MIRROR:-http://deb.debian.org/debian}"
[ "$(id -u)" -eq 0 ] || { echo "root が必要です" >&2; exit 1; }
for c in debootstrap qemu-mipsel-static; do command -v $c >/dev/null || { echo "$c が必要です" >&2; exit 1; }; done
umount_all() { for m in dev/pts dev proc sys; do while mountpoint -q "$OUT/$m"; do umount "$OUT/$m" || break; done; done; }
trap umount_all EXIT

echo "=== 1st stage ==="
debootstrap --foreign --arch=mipsel --variant=minbase \
  --include=sysvinit-core,sysv-rc,initscripts,openssh-server,ifupdown,isc-dhcp-client,iproute2,iputils-ping,procps,kmod,udev,ethtool,less,vim-tiny,ca-certificates,kexec-tools \
  --exclude=systemd-sysv,init \
  bookworm "$OUT" "$MIRROR"
cp "$(command -v qemu-mipsel-static)" "$OUT/usr/bin/"
echo "=== 2nd stage ==="
chroot "$OUT" /debootstrap/debootstrap --second-stage
echo "=== 設定 ==="
mount -t proc proc "$OUT/proc"; mount -t sysfs sysfs "$OUT/sys"
echo nasne > "$OUT/etc/hostname"
cat > "$OUT/etc/network/interfaces" <<'EON'
auto lo
iface lo inet loopback
auto eth0
iface eth0 inet dhcp
EON
mkdir -p "$OUT/root/.ssh"; [ -f "$SSH_PUBKEY" ] && cp "$SSH_PUBKEY" "$OUT/root/.ssh/authorized_keys"
chmod 700 "$OUT/root/.ssh"
# fstab は最小(ルートはカーネル引数 root= で渡す)
printf 'proc /proc proc defaults 0 0\nsysfs /sys sysfs defaults 0 0\ntmpfs /tmp tmpfs defaults 0 0\n' > "$OUT/etc/fstab"
# 起動確認用: root ログインを鍵のみに(sshd 既定は prohibit-password)
umount_all
rm -f "$OUT/usr/bin/qemu-mipsel-static"
du -sh "$OUT"
