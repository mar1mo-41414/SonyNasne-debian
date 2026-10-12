#!/bin/bash
# SPIバックアップ用ブロック(rcS_block_backup.sh)の論理テスト。MTD・マウント・起動を模擬ファイルに差し替えて、dash で実行する(実機・実デバイスには触れない)。
#   test_backup_sim.sh      期待: 1回目にバックアップができ、2回目はスキップし、どちらも最後に公式アプリの起動に進む
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d /tmp/bksim.XXXXXX); trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok() { echo "PASS: $1"; pass=$((pass + 1)); }
ng() { echo "FAIL: $1"; fail=$((fail + 1)); }
mkdir -p $T/mnt/bk_sys1 $T/mnt/bk_p3
head -c 262144 /dev/urandom > $T/mtd0; head -c 65536 /dev/urandom > $T/mtd1
printf 'dev:    size   erasesize  name\nmtd0: 00040000 00010000 "user1"\nmtd1: 00010000 00010000 "user2"\n' > $T/proc_mtd
touch $T/sda1 $T/sda3
mk_block() {   # $1=dest
    sed -e "s#@DEST@#$1#g" -e "s#/proc/mtd#$T/proc_mtd#g" -e "s#/dev/mtd0#$T/mtd0#g" -e "s#/dev/mtd1#$T/mtd1#g" \
        -e "s#/dev/sda1#$T/sda1#g" -e "s#/dev/sda3#$T/sda3#g" -e "s#/mnt/bk_#$T/mnt/bk_#g" -e 's#-b \$dev#-e $dev#g' \
        -e 's#^    /opt/dtvtuner/etc/startdtvtuner#    echo STARTED >> '"$T"'/started#' -e "s#mknod [^;]*#:#" \
        -e "s#BKLOG=/tmp/spi_backup.log#BKLOG=$T/bk.log#" "$HERE/rcS_block_backup.sh"
}
run() { { echo 'mount() { return 0; }'; echo 'umount() { return 0; }'; mk_block "$1"; } > $T/run.sh; : > $T/started; sh $T/run.sh >/dev/null 2>&1; }
echo "== 1回目(sys1)"
run sys1
[ -f $T/mnt/bk_sys1/spi_backup/spi_mtd0.bin ] && cmp -s $T/mtd0 $T/mnt/bk_sys1/spi_backup/spi_mtd0.bin && ok "mtd0 が一致して保存された" || ng "mtd0 の保存"
[ -f $T/mnt/bk_sys1/spi_backup/spi_mtd1_nv.bin ] && cmp -s $T/mtd1 $T/mnt/bk_sys1/spi_backup/spi_mtd1_nv.bin && ok "mtd1 が一致して保存された" || ng "mtd1 の保存"
[ -s $T/mnt/bk_sys1/spi_backup/SHA1SUMS ] && ( cd $T/mnt/bk_sys1/spi_backup && sha1sum -c SHA1SUMS >/dev/null 2>&1 ) && ok "SHA1SUMS が検証できる" || ng "SHA1SUMS"
[ -f $T/mnt/bk_sys1/spi_backup/README.txt ] && [ -f $T/mnt/bk_sys1/spi_backup/backup.log ] && ok "README とログがある" || ng "README/ログ"
[ ! -e $T/mnt/bk_p3/spi_backup ] && ok "dest=sys1 なので p3 には置かない" || ng "p3に置いてしまった"
[ "$(cat $T/started)" = STARTED ] && ok "最後に公式アプリの起動に進んだ" || ng "公式アプリの起動に進んでいない"
echo "== mtd は読むだけ(元ファイルが変わっていない)"
[ "$(md5sum < $T/mtd0)" = "$(md5sum < $T/mnt/bk_sys1/spi_backup/spi_mtd0.bin)" ] && ok "元のmtd0は不変" || ng "元が変わった"
echo "== 2回目(スキップ)"
M1=$(stat -c %Y $T/mnt/bk_sys1/spi_backup/spi_mtd0.bin); sleep 1; head -c 262144 /dev/urandom > $T/mtd0   # SPIが変わっても上書きしない
run sys1
[ "$(stat -c %Y $T/mnt/bk_sys1/spi_backup/spi_mtd0.bin)" = "$M1" ] && ok "作成済みなら上書きしない" || ng "上書きした"
[ "$(cat $T/started)" = STARTED ] && ok "スキップでも公式アプリの起動に進んだ" || ng "2回目の起動"
echo "== both"
rm -rf $T/mnt/bk_sys1/spi_backup; run both
cmp -s $T/mnt/bk_sys1/spi_backup/spi_mtd0.bin $T/mnt/bk_p3/spi_backup/spi_mtd0.bin && cmp -s $T/mtd0 $T/mnt/bk_p3/spi_backup/spi_mtd0.bin && ok "both: sys1 と p3 の両方に同じものが置かれた" || ng "both"
echo "== mtd0 が見えない/サイズ不一致でも起動は続ける"
rm -rf $T/mnt/bk_sys1/spi_backup $T/mnt/bk_p3/spi_backup; head -c 100 /dev/urandom > $T/mtd0   # /proc/mtd の宣言(262144)と違う
run sys1
[ ! -e $T/mnt/bk_sys1/spi_backup/spi_mtd0.bin ] && ok "サイズ不一致のものは保存しない" || ng "不正サイズを保存した"
[ "$(cat $T/started)" = STARTED ] && ok "失敗しても公式アプリの起動に進んだ" || ng "失敗時の起動"
echo "== $pass 件成功 / $fail 件失敗"
[ $fail = 0 ]
