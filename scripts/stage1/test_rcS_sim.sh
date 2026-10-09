#!/bin/bash
# 生成した 00550066.dlm の rcS ブロックを、公式 sys2 の busybox(ash)で、コマンドをモックにして実行する論理テスト。
# 実機・実マウントには一切触れない(chroot するが、mount/chroot/telnetd 等は全てモック)。要: sudo, qemu-user-static(mipsel)
#   test_rcS_sim.sh <生成した .dlm> <p3|embed> <公式パッケージ(ofw_tool/make_stage1_dlm が読める.dlm)>
set -u
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"      # 全体をrootで実行する(chroot/mknod/qemu展開に必要)
DLM=${1:?}; MODE=${2:-p3}; OFFICIAL=${3:?}
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
W=$(mktemp -d /tmp/rcsim.XXXXXX); trap 'rm -rf "$W"' EXIT
BB_MINI=$W/miniroot/bin/busybox
echo "== 公式rootfs(sys2) と miniroot を用意"
python3 -I "$HERE/make_stage1_dlm.py" list --official "$OFFICIAL" >/dev/null || exit 1
# miniroot の busybox(静的)で tar を展開する(実機のminirootと同じ)
python3 -I - "$OFFICIAL" "$W" "$ROOT" <<'PY'
import sys, os
off, W, root = sys.argv[1:4]
sys.path.insert(0, os.path.join(root, "scripts"))
from dlm_crypto import NasneBlowfish
d = open(off, "rb").read(); pos = 0
while pos < len(d):
    h = NasneBlowfish().chain_decrypt(d[pos:pos+64]); size = int.from_bytes(h[4:8], "big")
    if bytes(h[:4]).rstrip(b"\0") == b"RFS":
        open(os.path.join(W, "rfs_body.gz"), "wb").write(d[pos+64:pos+size])
    pos += size
PY
mkdir -p "$W/miniroot" && ( cd "$W/miniroot" && zcat ../rfs_body.gz | cpio -id --quiet 2>/dev/null )
mkdir -p "$W/sys2"
tail -c +65 "$DLM" > "$W/body.tar.gz"
env -i LC_ALL=C qemu-mipsel-static "$BB_MINI" tar zxf "$W/body.tar.gz" -C "$W/sys2" || { echo "miniroot busybox での tar 展開に失敗"; exit 1; }
echo "== rcS からブロックを切り出す"
awk '/^# ======== nasne-debian stage1 \(/{f=1} f{print} /^# ======== \/nasne-debian stage1/{f=0}' "$W/sys2/etc/init.d/rcS" > "$W/block.sh"
[ -s "$W/block.sh" ] || { echo "ブロックが無い"; exit 1; }
sh -n "$W/block.sh" && echo "構文OK"
echo "== モックを置く"
mkdir -p "$W/sys2/mock" "$W/sys2/mnt" "$W/sys2/debian/etc" "$W/sys2/debian/proc" "$W/sys2/debian/dev"
cp "$(command -v qemu-mipsel-static)" "$W/sys2/usr/bin/"
mknod "$W/sys2/dev/sda3" b 8 3 2>/dev/null || true
for c in ifconfig udhcpc telnetd umount sync; do
  printf '#!/bin/sh\necho "%s $*" >> /tmp/calls.log\nexit 0\n' "$c" | tee "$W/sys2/mock/$c" >/dev/null
done
# sleep は待たない。mount は ext3 の /dev/sda3 だけ成功させ、p3 が「準備済み」か「未準備」かをフラグで切り替える
printf '#!/bin/sh\necho "sleep $*" >> /tmp/calls.log\nexit 0\n' | tee "$W/sys2/mock/sleep" >/dev/null
cat <<'M' | tee "$W/sys2/mock/mount" >/dev/null
#!/bin/sh
echo "mount $*" >> /tmp/calls.log
case "$*" in
  *"/dev/sda3 /mnt/p3"*)
    if [ -f /tmp/p3_blank ] && [ ! -f /tmp/p3_formatted ]; then exit 32; fi    # 未初期化(ext3でない)
    mkdir -p /mnt/p3/usr/local/sbin /mnt/p3/etc /mnt/p3/proc /mnt/p3/sys /mnt/p3/dev/pts /mnt/p3/var/log
    if [ ! -f /tmp/p3_blank ] || [ -f /tmp/p3_formatted ]; then
        : > /mnt/p3/etc/nasne-direct-boot; printf '#!/bin/sh\n' > /mnt/p3/usr/local/sbin/nasne-stage1.sh; chmod +x /mnt/p3/usr/local/sbin/nasne-stage1.sh
    fi;;
esac
exit 0
M
cat <<'M' | tee "$W/sys2/mock/chroot" >/dev/null
#!/bin/sh
echo "chroot $*" >> /tmp/calls.log
case "$*" in
  *mkfs.ext3*) : > /tmp/p3_formatted;;
  *nasne-mcu-wd*) mkdir -p "$1/var/log"; echo "MCU watchdog disabled" >> "$1/var/log/nasne-mcu-wd.log";;   # 偽のウォッチドッグ停止
esac
exit 0
M
chmod +x "$W"/sys2/mock/*
run() {  # $1 = blank(p3未初期化)|ready
  rm -f "$W/sys2/tmp/calls.log" "$W/sys2/tmp/p3_blank" "$W/sys2/tmp/p3_formatted" "$W/sys2/tmp/nasne_stage1_rcs.log"; rm -rf "$W/sys2/mnt/p3"
  [ "$1" = blank ] && touch "$W/sys2/tmp/p3_blank"
  cp "$W/block.sh" "$W/sys2/tmp/block.sh"
  chroot "$W/sys2" /usr/bin/qemu-mipsel-static /bin/busybox sh -c 'PATH=/mock:/bin:/sbin:/usr/bin:/usr/sbin; mkdir -p /mnt/p3; . /tmp/block.sh; echo block-finished-rc=$?' 2>&1 | tail -3
  echo "--- 呼ばれたコマンド:"; cat "$W/sys2/tmp/calls.log" | sed 's/^/    /'
}
echo; echo "######## p3 が準備済み(Debian入り)のとき"; run ready
if [ "$MODE" = embed ]; then
  echo; echo "######## p3 が未初期化のとき(embed: mkfs → コピー → 段階1)"; run blank
fi
