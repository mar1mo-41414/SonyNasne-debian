#!/bin/bash
# SPI復元用ブロック(rcS_block_restore.sh)の論理テスト。MTD・マウント・mtdtool・再起動を模擬に差し替えて、dash で実行する(実機・実デバイスには触れない)。
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d /tmp/rssim.XXXXXX); trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok() { echo "PASS: $1"; pass=$((pass + 1)); }
ng() { echo "FAIL: $1"; fail=$((fail + 1)); }
SZ=13631488
mkdir -p $T/mnt/rs_sys1/spi_backup $T/mnt/rs_p3
head -c $SZ /dev/urandom > $T/backup; cp $T/backup $T/mtd0
printf 'dev:    size   erasesize  name\nmtd0: 00d00000 00010000 "user1"\nmtd1: 00010000 00010000 "user2"\n' > $T/proc_mtd
cp $T/backup $T/mnt/rs_sys1/spi_backup/spi_mtd0.bin; ( cd $T/mnt/rs_sys1/spi_backup && sha1sum spi_mtd0.bin > SHA1SUMS )
touch $T/sda1 $T/sda3
# 模擬 mtdtool: dump/flash だけ。FAILKNL=1 なら KNL への flash を失敗させる。呼ばれたコマンドは $T/tool.log に残す
cat > $T/mtdtool <<EOT
#!/bin/sh
echo "\$*" >> $T/tool.log
cmd=\$1; off=\$((\$3)); 
case \$cmd in
  dump) len=\$((\$4)); dd if=$T/mtd0 of=\$5 bs=65536 skip=\$((off / 65536)) count=\$((len / 65536)) 2>/dev/null ;;
  flash) [ "\${FAILKNL:-0}" = 1 ] && [ \$off = 1048576 ] && exit 4
         sz=\$(wc -c < \$4); dd if=\$4 of=$T/mtd0 bs=65536 seek=\$((off / 65536)) count=\$((sz / 65536)) conv=notrunc 2>/dev/null ;;
esac
EOT
chmod +x $T/mtdtool
mk_block() {
    sed -e "s#/proc/mtd#$T/proc_mtd#g" -e "s#/dev/mtd0#$T/mtd0#g" -e "s#RS_TOOL=/usr/bin/mtdtool#RS_TOOL=$T/mtdtool#" \
        -e "s#/dev/sda1#$T/sda1#g" -e "s#/dev/sda3#$T/sda3#g" -e "s#/mnt/rs_#$T/mnt/rs_#g" -e 's#-b \$dev#-e $dev#g' \
        -e 's#^    /opt/dtvtuner/etc/startdtvtuner#    echo STARTED >> '"$T"'/started#' -e "s#mknod [^;]*#:#" -e 's#sleep 60#:#' \
        -e "s#RS_LOG=/tmp/spi_restore.log#RS_LOG=$T/rs.log#" -e "s#/tmp/rs_#$T/rs_#g" -e 's#^if \[ -f '"$T"'/rs_reboot \]; then sync; reboot;#if [ -f '"$T"'/rs_reboot ]; then sync; echo REBOOT >> '"$T"'/reboot;#' \
        "$HERE/rcS_block_restore.sh"
}
run() { { echo 'mount() { return 0; }'; echo 'umount() { return 0; }'; mk_block; } > $T/run.sh; : > $T/started; rm -f $T/reboot $T/rs_reboot $T/tool.log; sh $T/run.sh >/dev/null 2>&1; }
KNL=1048576; BKNL=3670016; LEN=2621440
echo "== 1: KNL だけ違う(カスタムカーネルが入っている状態)"
head -c $LEN /dev/urandom | dd of=$T/mtd0 bs=65536 seek=$((KNL / 65536)) conv=notrunc 2>/dev/null
run
cmp -s $T/mtd0 $T/backup && ok "KNL がバックアップの内容に戻り、mtd0 全体が一致した" || ng "mtd0 が戻っていない"
grep -q "flash .* 0x100000 " $T/tool.log && ok "KNL を flash した" || ng "KNL を flash していない"
grep -q "flash .* 0x380000 " $T/tool.log && ng "同じ BKNL まで書いた" || ok "BKNL は一致していたので書かない"
[ -f $T/reboot ] && ok "書き換えたので再起動に進んだ" || ng "再起動に進まない"
echo "== 1b: バックアップのスロットが 0x00 で始まる(実機のカスタムカーネルがそうだった。\$(...) は NUL を捨てる)"
python3 - "$T" $KNL <<'PY'
import sys
t,k=sys.argv[1],int(sys.argv[2])
for f in (t+"/backup", t+"/mnt/rs_sys1/spi_backup/spi_mtd0.bin"):
    d=bytearray(open(f,"rb").read()); d[k:k+4]=b"\x00\x00\x00\x00"; open(f,"wb").write(d)
PY
( cd $T/mnt/rs_sys1/spi_backup && sha1sum spi_mtd0.bin > SHA1SUMS )
head -c $LEN /dev/urandom | dd of=$T/mtd0 bs=65536 seek=$((KNL / 65536)) conv=notrunc 2>/dev/null
run
cmp -s $T/mtd0 $T/backup && ok "先頭がNULのスロットでも書き戻せた" || ng "先頭NULのスロットを空と誤判定した"
echo "== 2: 全部一致(2回目)"
run
[ ! -f $T/reboot ] && ok "一致していれば再起動しない" || ng "また再起動した"
! grep -q flash $T/tool.log && ok "何も書かない" || ng "一致なのに書いた"
[ "$(cat $T/started)" = STARTED ] && ok "公式アプリの起動に進んだ" || ng "起動に進まない"
echo "== 3: KNL も BKNL も違う"
head -c $LEN /dev/urandom | dd of=$T/mtd0 bs=65536 seek=$((KNL / 65536)) conv=notrunc 2>/dev/null
head -c $LEN /dev/urandom | dd of=$T/mtd0 bs=65536 seek=$((BKNL / 65536)) conv=notrunc 2>/dev/null
run
cmp -s $T/mtd0 $T/backup && ok "両方戻った" || ng "両方は戻らなかった"
L=$(grep -n flash $T/tool.log | head -2 | cut -d' ' -f3 | tr '\n' ' '); [ "$L" = "0x100000 0x380000 " ] && ok "書く順番は KNL → BKNL" || ng "順番が違う: $L"
echo "== 4: KNL の書き込み失敗 → BKNL は触らない、再起動しない"
head -c $LEN /dev/urandom | dd of=$T/mtd0 bs=65536 seek=$((KNL / 65536)) conv=notrunc 2>/dev/null
head -c $LEN /dev/urandom | dd of=$T/mtd0 bs=65536 seek=$((BKNL / 65536)) conv=notrunc 2>/dev/null
B=$(dd if=$T/mtd0 bs=65536 skip=$((BKNL / 65536)) count=$((LEN / 65536)) 2>/dev/null | md5sum)
FAILKNL=1 run
[ "$(dd if=$T/mtd0 bs=65536 skip=$((BKNL / 65536)) count=$((LEN / 65536)) 2>/dev/null | md5sum)" = "$B" ] && ok "KNL が失敗したら BKNL は書かない" || ng "BKNL を書いた"
[ ! -f $T/reboot ] && ok "失敗なら再起動しない" || ng "失敗なのに再起動した"
[ "$(cat $T/started)" = STARTED ] && ok "失敗でも公式アプリの起動に進んだ" || ng "失敗時に起動しない"
echo "== 5: SHA1SUMS が合わない(バックアップが壊れている) → 何も書かない"
cp $T/backup $T/mtd0; head -c $LEN /dev/urandom | dd of=$T/mtd0 bs=65536 seek=$((KNL / 65536)) conv=notrunc 2>/dev/null; M=$(md5sum < $T/mtd0)
printf 'ff\n' | dd of=$T/mnt/rs_sys1/spi_backup/spi_mtd0.bin bs=1 seek=100 conv=notrunc 2>/dev/null
run
[ "$(md5sum < $T/mtd0)" = "$M" ] && ok "壊れたバックアップからは書かない" || ng "壊れたバックアップから書いた"
cp $T/backup $T/mnt/rs_sys1/spi_backup/spi_mtd0.bin
echo "== 6: バックアップのサイズが違う → 何も書かない"
head -c 1000 /dev/urandom > $T/mnt/rs_sys1/spi_backup/spi_mtd0.bin; ( cd $T/mnt/rs_sys1/spi_backup && sha1sum spi_mtd0.bin > SHA1SUMS )
run
[ "$(md5sum < $T/mtd0)" = "$M" ] && ok "サイズ違いからは書かない" || ng "サイズ違いから書いた"
cp $T/backup $T/mnt/rs_sys1/spi_backup/spi_mtd0.bin; ( cd $T/mnt/rs_sys1/spi_backup && sha1sum spi_mtd0.bin > SHA1SUMS )
echo "== 7: バックアップの KNL スロットが全部 0xff → 書かない"
python3 - "$T" $KNL $LEN <<'PY'
import sys
t,k,l=sys.argv[1],int(sys.argv[2]),int(sys.argv[3])
f=t+"/mnt/rs_sys1/spi_backup/spi_mtd0.bin"; d=bytearray(open(f,"rb").read()); d[k:k+l]=b"\xff"*l; open(f,"wb").write(d)
PY
( cd $T/mnt/rs_sys1/spi_backup && sha1sum spi_mtd0.bin > SHA1SUMS )
run
[ "$(md5sum < $T/mtd0)" = "$M" ] || ! grep -q "flash .* 0x100000 " $T/tool.log && ok "空スロットは書かない" || ng "空スロットを書いた"
grep -q "flash .* 0x100000 " $T/tool.log && ng "KNL を空で上書きした" || ok "KNL は触らない"
echo "== $pass 件成功 / $fail 件失敗"
[ $fail = 0 ]
