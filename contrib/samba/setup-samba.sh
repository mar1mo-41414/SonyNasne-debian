#!/bin/sh
# nasne(Debian直起動)を Samba のファイルサーバーにする(オプション)。nasne上で root で実行する。
#   使い方: sh setup-samba.sh [共有名 [共有するディレクトリ [ユーザー名]]]
#          既定: 共有名 nasne、ディレクトリ /data/share、ユーザー nasne(Samba のパスワードを聞かれる)
# やること: samba を入れる(推奨パッケージなし)、/etc/samba/smb.conf を作る(元は smb.conf.bak へ)、共有用のユーザーとディレクトリを作る、smbd を起動。
# nmbd(NetBIOS名の応答)は起動しない。Windows・macOS・Linux からは IPアドレスまたはホスト名で繋ぐ(\\<nasne>\共有名 / smb://<nasne>/共有名)。
# 注意: 認証はユーザー名+パスワード(ゲストは不可)。LAN内で使い、インターネットには出さないこと。
set -e
SHARE=${1:-nasne}; DIR=${2:-/data/share}; USERNAME=${3:-nasne}
[ "$(id -u)" = 0 ] || { echo "root で実行してください" >&2; exit 1; }
export DEBIAN_FRONTEND=noninteractive LC_ALL=C
if ! command -v smbd >/dev/null 2>&1 || ! command -v smbpasswd >/dev/null 2>&1; then
    apt-get update -qq || true
    apt-get install -y --force-yes --no-install-recommends samba samba-common-bin
fi
mkdir -p "$DIR"
id "$USERNAME" >/dev/null 2>&1 || adduser --system --no-create-home --shell /usr/sbin/nologin --group "$USERNAME"
chown -R "$USERNAME:$USERNAME" "$DIR"; chmod 775 "$DIR"
[ -f /etc/samba/smb.conf.bak ] || cp -a /etc/samba/smb.conf /etc/samba/smb.conf.bak 2>/dev/null || true
cat > /etc/samba/smb.conf <<CONF
[global]
   workgroup = WORKGROUP
   server string = nasne
   security = user
   map to guest = Never
   guest ok = no
   disable netbios = yes
   smb ports = 445
   max protocol = SMB2
   load printers = no
   printing = bsd
   printcap name = /dev/null
   disable spoolss = yes
   log file = /var/log/samba/log.%m
   max log size = 200
   socket options = TCP_NODELAY
   # 低メモリ(約240MB)向け
   use sendfile = yes

[$SHARE]
   path = $DIR
   read only = no
   valid users = $USERNAME
   force user = $USERNAME
   create mask = 0664
   directory mask = 0775
CONF
echo "Samba のパスワードを設定します(ユーザー: $USERNAME)"
if [ -n "$SMB_PASSWORD" ]; then printf '%s\n%s\n' "$SMB_PASSWORD" "$SMB_PASSWORD" | smbpasswd -s -a "$USERNAME"; else smbpasswd -a "$USERNAME"; fi
# nmbd は止める(NetBIOS は使わない)。smbd だけ
[ -x /etc/init.d/nmbd ] && { /etc/init.d/nmbd stop >/dev/null 2>&1 || true; update-rc.d nmbd disable >/dev/null 2>&1 || true; }
/etc/init.d/samba restart 2>/dev/null || { /etc/init.d/smbd restart; }
printf '%s\n' "完了: smb://$(hostname)/$SHARE  ユーザー: $USERNAME(Windows は エクスプローラーに \\\\<nasneのIP>\\$SHARE を入力)"
