# ======== nasne-debian stage1 (restore: SPIのKNL/BKNLをバックアップへ戻す。scripts/stage1/make_stage1_dlm.py --mode restore が startdtvtuner の直前に挿入) ========
# バックアップ専用 .dlm(--mode backup)が置いた /spi_backup/spi_mtd0.bin(sys1 または p3)から、KNL(0x100000〜)と BKNL(0x380000〜)だけを書き戻す。
# 触るのはこの2スロットだけ(ブートローダ・ECC・RFS などには触れない。mtdtool 自体が許可範囲を固定している)。
# すでにバックアップと同じスロットは書かない。2スロットとも違う場合は KNL を書いて検証してから BKNL を書く(途中で止まっても片方は無傷)。
# 終わったら(失敗しても)ふつうに公式アプリを起動する。成功して書き換えがあったときは、再起動して戻したカーネルで立ち上げる(再起動後は同じなので何もしない)。
RS_LOG=/tmp/spi_restore.log
RS_TOOL=/usr/bin/mtdtool
rs_log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> $RS_LOG; }
# rs_mount <デバイス> <マウント先>: ext3 → xfs の順に試す
rs_mount() {
    mkdir -p "$2"
    mount -t ext3 -o rw "$1" "$2" 2>/dev/null && return 0
    mount -t xfs  -o rw "$1" "$2" 2>/dev/null && return 0
    return 1
}
# rs_slot <名前> <オフセット> <長さ> <バックアップ>: 違うときだけ書く。戻り値 0=一致/書いて検証OK、1=失敗、2=書き換えた
rs_slot() {
    cur=/tmp/rs_cur.bin; new=/tmp/rs_new.bin
    skip=$(( $2 / 65536 )); cnt=$(( $3 / 65536 ))
    dd if="$4" of=$new bs=65536 skip=$skip count=$cnt 2>/dev/null
    if [ "$(wc -c < $new)" != "$3" ]; then rs_log "NG: $1 バックアップの切り出しサイズが違う"; rm -f $new; return 1; fi
    # 「全部 0xff / 全部 0x00」の検査。$(...) は NUL を捨てるので、バイト数(wc -c)で数える
    if [ "$(tr -d '\377' < $new | wc -c)" = 0 ] || [ "$(tr -d '\000' < $new | wc -c)" = 0 ]; then rs_log "NG: $1 バックアップのスロットが空(全部 0xff または 0x00)。書かない"; rm -f $new; return 1; fi
    if $RS_TOOL dump /dev/mtd0 $(printf '0x%x' $2) $(printf '0x%x' $3) $cur >/dev/null 2>&1 && cmp -s $cur $new; then
        rs_log "$1: すでにバックアップと同じ。書かない"; rm -f $cur $new; return 0
    fi
    rs_log "$1: 違うので書き戻す (flash: 消去→書き込み→読み戻し検証)"
    if $RS_TOOL flash /dev/mtd0 $(printf '0x%x' $2) $new >> $RS_LOG 2>&1; then
        rs_log "$1: 書き戻して検証OK"; rm -f $cur $new; return 2
    fi
    rs_log "NG: $1 の書き戻しに失敗(rc=$?)。残りのスロットは触らない"; rm -f $cur $new; return 1
}
RS_REBOOT=0
(
    rs_log "SPI復元開始"
    [ -c /dev/mtd0 ] || mknod /dev/mtd0 c 90 0
    SZ0=
    while read dev size erase name; do
        case "$dev" in mtd0:) SZ0=$((0x$size));; esac
    done < /proc/mtd
    rs_log "mtd0=$SZ0 (/proc/mtd)"
    SRC=
    for want in sys1 p3; do
        if [ $want = sys1 ]; then dev=/dev/sda1; mp=/mnt/rs_sys1; else dev=/dev/sda3; mp=/mnt/rs_p3; fi
        n=0; while [ ! -b $dev ] && [ $n -lt 60 ]; do sleep 1; n=$((n + 1)); done
        if rs_mount $dev $mp; then
            rs_log "$want をマウント: $dev -> $mp"
            if [ -z "$SRC" ] && [ -f $mp/spi_backup/spi_mtd0.bin ]; then
                if [ "$(wc -c < $mp/spi_backup/spi_mtd0.bin)" != "$SZ0" ]; then rs_log "NG: $mp/spi_backup/spi_mtd0.bin のサイズが mtd0($SZ0) と違う"
                elif [ -f $mp/spi_backup/SHA1SUMS ] && ! ( cd $mp/spi_backup && sha1sum -c SHA1SUMS >/dev/null 2>&1 ); then rs_log "NG: $mp/spi_backup の SHA1SUMS 検証に失敗(壊れている)"
                else SRC=$mp/spi_backup/spi_mtd0.bin; rs_log "復元元: $SRC (サイズ・SHA1SUMS OK)"; fi
            fi
        else rs_log "NG: $want ($dev) をマウントできない"; fi
    done
    if [ -n "$SRC" ] && [ -x $RS_TOOL ]; then
        rs_slot KNL  1048576 2621440 $SRC; r1=$?
        if [ $r1 != 1 ]; then rs_slot BKNL 3670016 2621440 $SRC; r2=$?; else r2=1; fi
        rs_log "結果: KNL rc=$r1 BKNL rc=$r2 (0=一致 1=失敗/スキップ 2=書き換え)"
        if [ $r1 = 2 ] || [ $r2 = 2 ]; then
            if [ $r1 != 1 ] && [ $r2 != 1 ]; then touch /tmp/rs_reboot; fi
        fi
    else
        rs_log "復元元が無い、または $RS_TOOL が無い。何もしない"
    fi
    for mp in /mnt/rs_sys1 /mnt/rs_p3; do
        [ -d $mp/spi_backup ] && cat $RS_LOG >> $mp/spi_backup/restore.log 2>/dev/null     # 再起動をまたいで履歴を残す(追記)
    done
    sync
    umount /mnt/rs_sys1 2>/dev/null; umount /mnt/rs_p3 2>/dev/null
    rs_log "SPI復元終了"
) > /dev/null 2>&1
if [ -f /tmp/rs_reboot ]; then sync; reboot; sleep 60; fi
# ----------------------------------------------------------------------------------------------------
    /opt/dtvtuner/etc/startdtvtuner
# ======== end ========
