#!/usr/bin/env bash
# Debian wheezy (mipsel) の rootfs を debootstrap + qemu-user-static で作る。
#
# nasneのカーネルは2.6.29(2009年ベース)と非常に古いため、glibcの最小カーネル要求が緩い
# Debian 7 "wheezy"(glibc 2.13)をベースにしている。これより新しいDebianは起動しない可能性が高い
# (udev 175以降はカーネル2.6.32以上が必要、など)。
#
# 使い方:
#   sudo ./scripts/build_debian_rootfs.sh <出力先ディレクトリ>
#   (必要: debootstrap, qemu-user-static。SSH_PUBKEY環境変数でroot用の公開鍵を指定、既定 ~/.ssh/id_rsa.pub)
#
# 出来上がった <出力先>/ を、nasneのHDDのp3(ext3)にそのままコピーして使う(docs/05_kernel_and_direct_boot.md)。
# 注意: wheezyのリポジトリは archive.debian.org にあり、署名鍵が期限切れのため --no-check-gpg を使っている。

set -euo pipefail

OUT="${1:?使い方: $0 <出力先ディレクトリ>}"
SSH_PUBKEY="${SSH_PUBKEY:-$HOME/.ssh/id_rsa.pub}"
# 注意: 実機のsshd(OpenSSH 6.0、Wheezy)はed25519鍵・rsa-sha2署名に非対応。
# 必ずRSA鍵を使うこと(クライアント側でも `-o PubkeyAcceptedAlgorithms=+ssh-rsa` が必要)。

if [ "$(id -u)" -ne 0 ]; then
    echo "root権限が必要です(sudoで実行してください)" >&2
    exit 1
fi

if ! command -v debootstrap >/dev/null; then
    echo "debootstrapが必要です: apt-get install debootstrap" >&2
    exit 1
fi
if ! command -v qemu-mipsel-static >/dev/null; then
    echo "qemu-mipsel-staticが必要です: apt-get install qemu-user-static" >&2
    exit 1
fi

echo "=== 1st stage: debootstrap --foreign (wheezy, mipsel) ==="
debootstrap --no-check-gpg --arch=mipsel --foreign wheezy "$OUT" \
    http://archive.debian.org/debian/

echo "=== qemu-mipsel-static を配置、2nd stageを実行 ==="
cp "$(command -v qemu-mipsel-static)" "$OUT/usr/bin/"
chroot "$OUT" /debootstrap/debootstrap --second-stage

echo "=== proc/sysをマウントしopensssh-server等をインストール ==="
mount -t proc proc "$OUT/proc"
mount -t sysfs sysfs "$OUT/sys"
cp /etc/resolv.conf "$OUT/etc/resolv.conf"
trap 'umount "$OUT/proc" "$OUT/sys" 2>/dev/null || true' EXIT

chroot "$OUT" /bin/bash -c "
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --force-yes openssh-server
"

echo "=== root用SSH鍵認証を設定(RSA必須、ed25519は実機のOpenSSH 6.0が非対応) ==="
mkdir -p "$OUT/root/.ssh"
cp "$SSH_PUBKEY" "$OUT/root/.ssh/authorized_keys"
chmod 700 "$OUT/root/.ssh"
chmod 600 "$OUT/root/.ssh/authorized_keys"
sed -i 's/^PermitRootLogin.*/PermitRootLogin yes/' "$OUT/etc/ssh/sshd_config"
echo debian-mipsel > "$OUT/etc/hostname"

umount "$OUT/proc" "$OUT/sys"
trap - EXIT

echo "=== サイズ削減: apt cache/lists、doc、man、非英語ロケール、qemuバイナリを除去 ==="
rm -rf "$OUT/var/lib/apt/lists"/*
rm -f "$OUT/var/cache/apt/archives"/*.deb
rm -f "$OUT/var/cache/apt/srcpkgcache.bin" "$OUT/var/cache/apt/pkgcache.bin"
rm -rf "$OUT/var/cache/man"/*
rm -rf "$OUT/usr/share/doc"/*
rm -rf "$OUT/usr/share/man"/*
rm -rf "$OUT/usr/share/info"/*
rm -rf "$OUT/usr/share/lintian" "$OUT/usr/share/calendar" "$OUT/usr/share/X11"
find "$OUT/usr/share/locale" -maxdepth 1 -mindepth 1 -type d \
    ! -name 'en*' ! -name 'C' -exec rm -rf {} +
rm -f "$OUT/usr/bin/qemu-mipsel-static"

echo "=== 不要なデバイスノードを削除(起動時にnasne_initがtmpfsの/devを作る) ==="
find "$OUT/dev" -mindepth 1 -delete

echo "=== ハードリンクを独立ファイルに変換(tar移植性のため、念のため) ==="
python3 - "$OUT" <<'PYEOF'
import os, sys
base = sys.argv[1]
seen = {}
for root, dirs, files in os.walk(base):
    for name in files:
        path = os.path.join(root, name)
        try:
            st = os.lstat(path)
        except FileNotFoundError:
            continue
        if not os.path.isfile(path) or os.path.islink(path):
            continue
        key = (st.st_dev, st.st_ino)
        if st.st_nlink > 1:
            if key in seen:
                # 2つ目以降の出現: 独立コピーに置き換え
                import shutil
                os.remove(path)
                shutil.copy2(seen[key], path)
                print(f"hardlink解消: {path}")
            else:
                seen[key] = path
PYEOF

du -sh "$OUT"
echo "完了: $OUT"
