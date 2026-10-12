# ======== nasne-debian stage1 (backup: SPIフラッシュの読み出しだけ。scripts/stage1/make_stage1_dlm.py --mode backup が startdtvtuner の直前に挿入) ========
# /dev/mtd0(ブート〜KNL〜RFS、13MB) と /dev/mtd1(NV領域、jffs2) を dd で「読む」だけ。書き込み・消去・マウント以外の変更は一切しない。
# 結果は @DEST@ の /spi_backup/ に置く。すでに /spi_backup/spi_mtd0.bin があればやり直さない(消すと次の起動で取り直す)。
# 終わったら(失敗しても)ふつうに公式アプリを起動する。読み出しは1分半ほどかかり、その間は起動が遅れる(MCUのウォッチドッグ333秒には収まる)。
BK_DEST="@DEST@"
BKLOG=/tmp/spi_backup.log
bk_log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> $BKLOG; }
# bk_mount <デバイス> <マウント先>: ext3 → xfs の順に試す
bk_mount() {
    mkdir -p "$2"
    mount -t ext3 -o rw "$1" "$2" 2>/dev/null && return 0
    mount -t xfs  -o rw "$1" "$2" 2>/dev/null && return 0
    return 1
}
# bk_dump <mtdデバイス> <出力ファイル> <期待サイズ> <2回目の読み直し比較(1=する)>
bk_dump() {
    dd if="$1" of="$2.tmp" bs=65536 2>>$BKLOG
    sz=$(wc -c < "$2.tmp")
    if [ "$sz" != "$3" ]; then bk_log "NG: $1 のサイズ $sz (期待 $3)"; rm -f "$2.tmp"; return 1; fi
    if [ "$4" = 1 ]; then
        if ! dd if="$1" bs=65536 2>/dev/null | cmp - "$2.tmp" >/dev/null 2>&1; then
            bk_log "WARN: $1 を2回読んだら内容が違う(読み出し不安定)。1回目を unstable として残す"
            mv "$2.tmp" "$2.unstable"; return 2
        fi
    fi
    mv "$2.tmp" "$2"; return 0
}
(
    bk_log "SPIバックアップ開始 (保存先: $BK_DEST)"
    [ -c /dev/mtd0 ] || mknod /dev/mtd0 c 90 0
    [ -c /dev/mtd1 ] || mknod /dev/mtd1 c 90 2
    SZ0=; SZ1=
    while read dev size erase name; do
        case "$dev" in mtd0:) SZ0=$((0x$size));; mtd1:) SZ1=$((0x$size));; esac
    done < /proc/mtd
    bk_log "mtd0=$SZ0 mtd1=$SZ1 (/proc/mtd)"
    DIRS=""
    for want in sys1 p3; do
        case "$BK_DEST" in both|$want) ;; *) continue;; esac
        if [ $want = sys1 ]; then dev=/dev/sda1; mp=/mnt/bk_sys1; else dev=/dev/sda3; mp=/mnt/bk_p3; fi
        n=0; while [ ! -b $dev ] && [ $n -lt 60 ]; do sleep 1; n=$((n + 1)); done
        if bk_mount $dev $mp; then DIRS="$DIRS $mp"; bk_log "$want をマウント: $dev -> $mp"; else bk_log "NG: $want ($dev) をマウントできない"; fi
    done
    TODO=""
    for mp in $DIRS; do
        if [ -e $mp/spi_backup/spi_mtd0.bin ]; then bk_log "$mp/spi_backup は作成済み。スキップ"; else TODO="$TODO $mp"; fi
    done
    if [ -n "$TODO" ] && [ -n "$SZ0" ]; then
        set -- $TODO; P=$1; shift
        free=$(df -k $P | awk 'NR==2 {print $4}')
        if [ "${free:-0}" -lt 40000 ]; then
            bk_log "NG: $P の空きが足りない(${free}KB。40000KB以上必要)"
        else
            mkdir -p $P/spi_backup
            bk_log "mtd0 を読み出す(2回読んで比較)"
            bk_dump /dev/mtd0 $P/spi_backup/spi_mtd0.bin $SZ0 1; R0=$?
            bk_log "mtd0 結果 rc=$R0"
            if [ -n "$SZ1" ]; then
                bk_log "mtd1(NV領域)を読み出す(稼働中のjffs2なので2回目の比較は参考)"
                bk_dump /dev/mtd1 $P/spi_backup/spi_mtd1_nv.bin $SZ1 1; bk_log "mtd1 結果 rc=$?"
            fi
            if [ -e $P/spi_backup/spi_mtd0.bin ]; then
                ( cd $P/spi_backup && md5sum spi_mtd0.bin spi_mtd1_nv.bin 2>/dev/null > MD5SUMS; sha1sum spi_mtd0.bin spi_mtd1_nv.bin 2>/dev/null > SHA1SUMS )
                {
                    echo "nasne の SPI フラッシュを読み出しただけのバックアップ(書き込みはしていません)。"
                    echo "spi_mtd0.bin   : /dev/mtd0(オフセット 0 から ${SZ0} バイト。ブートローダ・KNL・RFS など)"
                    echo "spi_mtd1_nv.bin: /dev/mtd1(NV領域 jffs2。設定が入る。稼働中に読んだのでファイルの整合は保証しない)"
                    echo "SPIの最後の 64KB(0xff0000〜)はMTDとして見えないため含まれない。"
                } > $P/spi_backup/README.txt
                for q in $@; do
                    mkdir -p $q/spi_backup && cp $P/spi_backup/* $q/spi_backup/ && ( cd $q/spi_backup && cmp spi_mtd0.bin $P/spi_backup/spi_mtd0.bin ) && bk_log "$q へ複製して比較OK"
                done
            fi
        fi
    fi
    for mp in $DIRS; do cp $BKLOG $mp/spi_backup/backup.log 2>/dev/null; done
    sync
    for mp in $DIRS; do umount $mp 2>/dev/null; done
    bk_log "SPIバックアップ終了。公式アプリを起動する"
) > /dev/null 2>&1
# ----------------------------------------------------------------------------------------------------
    /opt/dtvtuner/etc/startdtvtuner
# ======== end ========
