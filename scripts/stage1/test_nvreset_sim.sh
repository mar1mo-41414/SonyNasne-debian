#!/bin/bash
# NVリセット用ブロック(rcS_block_nvreset.sh)の論理テスト。MTD・マウント・mtdtool・再起動を模擬に差し替えて dash で実行する(実機・実デバイスには触れない)。
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d /tmp/nvsim.XXXXXX); trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok() { echo "PASS: $1"; pass=$((pass + 1)); }
ng() { echo "FAIL: $1"; fail=$((fail + 1)); }
SZ=3080192
mkdir -p $T/mnt; touch $T/sda1
head -c $SZ /dev/urandom > $T/mtd1; cp $T/mtd1 $T/mtd1.orig
cat > $T/mtdtool <<EOT
#!/bin/sh
echo "\$*" >> $T/tool.log
case \$1 in
  dump)  [ "\${FAILDUMP:-0}" = 1 ] && exit 5; cp $T/mtd1 \$5 ;;
  flash) [ "\${FAILFLASH:-0}" = 1 ] && exit 6; cp \$4 $T/mtd1 ;;
esac
EOT
chmod +x $T/mtdtool
mk_block() {
    sed -e "s#NV_TOOL=/usr/bin/mtdtool#NV_TOOL=$T/mtdtool#" -e "s#NV_LOG=/tmp/nv_reset.log#NV_LOG=$T/nv.log#" \
        -e "s#/dev/mtd1#$T/mtd1#g" -e "s#/dev/sda1#$T/sda1#g" -e 's#-b \$T#-e $T#' -e "s#\[ ! -b $T/sda1 \]#[ ! -e $T/sda1 ]#" \
        -e "s#/mnt/nv_sys1#$T/mnt#g" -e "s#/tmp/nv_ff.bin#$T/nv_ff.bin#g" -e "s#/tmp/nv_reboot#$T/nv_reboot#g" \
        -e "s#\[ -c $T/mtd1 \] || mknod [^;]*#:#" -e 's#^if \[ -f '"$T"'/nv_reboot \]; then sync; reboot; sleep 60; fi#if [ -f '"$T"'/nv_reboot ]; then sync; echo REBOOT >> '"$T"'/reboot; fi#' \
        -e 's#^    /opt/dtvtuner/etc/startdtvtuner#    echo STARTED >> '"$T"'/started#' "$HERE/rcS_block_nvreset.sh"
}
run() { { echo 'mount() { return 0; }'; echo 'umount() { return 0; }'; mk_block; } > $T/run.sh; : > $T/started; rm -f $T/reboot $T/nv_reboot $T/tool.log $T/nv.log; sh $T/run.sh >/dev/null 2>&1; }
echo "== 1: 初回(NVが入っている)"
run
cmp -s $T/mnt/spi_backup/nv_before_reset_1.bin $T/mtd1.orig && ok "いまのNVが sys1 に保存された" || ng "NVの保存"
[ "$(wc -c < $T/mtd1)" = $SZ ] && [ -z "$(tr -d '\377' < $T/mtd1 | head -c 1)" ] && ok "NVが全部 0xff になった" || ng "NVが空になっていない"
[ -f $T/reboot ] && ok "再起動に進んだ" || ng "再起動に進まない"
[ -s $T/mnt/spi_backup/nv_reset.done ] && ok "nv_reset.done が残った" || ng "完了の印がない"
[ -s $T/mnt/spi_backup/nv_reset.log ] && ok "ログが sys1 に残った" || ng "ログがない"
[ "$(cat $T/started)" = STARTED ] && ok "公式アプリの起動に進んだ(再起動に失敗した場合の続行)" || ng "起動に進まない"
echo "== 2: 2回目(完了の印がある)"
cp $T/mtd1 $T/mtd1.afterreset
run
[ ! -f $T/reboot ] && ! grep -q flash $T/tool.log 2>/dev/null && ok "実施済みなら何もしない" || ng "また消した"
echo "== 3: 印を消してもう一度(バックアップは番号が増える)"
head -c $SZ /dev/urandom > $T/mtd1; cp $T/mtd1 $T/mtd1.orig2; rm -f $T/mnt/spi_backup/nv_reset.done
run
cmp -s $T/mnt/spi_backup/nv_before_reset_2.bin $T/mtd1.orig2 && cmp -s $T/mnt/spi_backup/nv_before_reset_1.bin $T/mtd1.orig && ok "2つ目のバックアップが _2 に保存され、_1 は残る" || ng "バックアップの番号"
echo "== 4: 保存に失敗 → 消さない"
head -c $SZ /dev/urandom > $T/mtd1; cp $T/mtd1 $T/mtd1.keep; rm -f $T/mnt/spi_backup/nv_reset.done
FAILDUMP=1 run
cmp -s $T/mtd1 $T/mtd1.keep && ok "保存できなければNVを消さない" || ng "保存失敗なのに消した"
[ ! -f $T/mnt/spi_backup/nv_reset.done ] && [ ! -f $T/reboot ] && ok "印も再起動も無し" || ng "印か再起動がある"
echo "== 5: 書き込みに失敗 → 印を付けず再起動しない"
FAILFLASH=1 run
[ ! -f $T/mnt/spi_backup/nv_reset.done ] && [ ! -f $T/reboot ] && ok "書き込み失敗なら印も再起動も無し" || ng "失敗なのに印/再起動"
[ "$(cat $T/started)" = STARTED ] && ok "失敗しても公式アプリの起動に進む" || ng "失敗時に起動しない"
echo "== 6: mtdtool が無い"
rm -f $T/mnt/spi_backup/nv_reset.done; mv $T/mtdtool $T/mtdtool.x
run
[ ! -f $T/reboot ] && cmp -s $T/mtd1 $T/mtd1.keep && ok "mtdtool が無ければ何もしない" || ng "mtdtool無しで動いた"
echo "== $pass 件成功 / $fail 件失敗"
[ $fail = 0 ]
