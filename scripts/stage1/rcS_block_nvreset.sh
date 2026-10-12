# ======== nasne-debian stage1 (nvreset: 本体のNV(SPIのjffs2)を空にする。scripts/stage1/make_stage1_dlm.py --mode nvreset が startdtvtuner の直前に挿入) ========
# HDDを入れ替えたあと、古いHDD向けの状態が残った本体のNV(/dev/mtd1)が新しいHDDと食い違うと、公式アプリが起動の約40秒後に固まることがある(docs/04)。
# NVを空(全部 0xff)にして、公式アプリに作り直させる。設定(本体名など)は初期値に戻る。**1回だけ**実行する(sys1 に nv_reset.done を残し、次の起動からは何もしない)。
# 先に、いまのNVを sys1 の /spi_backup/nv_before_reset_<番号>.bin に保存する(保存できなければ消さない)。終わったら再起動して、空のNVで公式アプリを起動する。
NV_LOG=/tmp/nv_reset.log
NV_TOOL=/usr/bin/mtdtool
nv_log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> $NV_LOG; }
(
    nv_log "NVリセット開始"
    [ -c /dev/mtd1 ] || mknod /dev/mtd1 c 90 2
    n=0; while [ ! -b /dev/sda1 ] && [ $n -lt 60 ]; do sleep 1; n=$((n + 1)); done
    mkdir -p /mnt/nv_sys1
    if mount -t ext3 -o rw /dev/sda1 /mnt/nv_sys1 2>/dev/null; then
        if [ -e /mnt/nv_sys1/spi_backup/nv_reset.done ]; then
            nv_log "実施済み(spi_backup/nv_reset.done)。何もしない。もう一度行うときは、このファイルを消す"
        elif [ ! -x $NV_TOOL ]; then
            nv_log "NG: $NV_TOOL が無い"
        else
            mkdir -p /mnt/nv_sys1/spi_backup
            n=1; while [ -e /mnt/nv_sys1/spi_backup/nv_before_reset_$n.bin ]; do n=$((n + 1)); done
            BK=/mnt/nv_sys1/spi_backup/nv_before_reset_$n.bin
            if $NV_TOOL dump /dev/mtd1 0x0 0x2f0000 $BK >> $NV_LOG 2>&1 && [ "$(wc -c < $BK)" = 3080192 ]; then
                nv_log "いまのNVを保存: $BK"
                umount /var/opt/dtvtuner/nv/ffs 2>/dev/null || { sleep 2; umount /var/opt/dtvtuner/nv/ffs 2>/dev/null; }
                dd if=/dev/zero bs=65536 count=47 2>/dev/null | tr '\000' '\377' > /tmp/nv_ff.bin
                if [ "$(wc -c < /tmp/nv_ff.bin)" = 3080192 ] && $NV_TOOL flash /dev/mtd1 0x0 /tmp/nv_ff.bin >> $NV_LOG 2>&1; then
                    nv_log "NVを空にした(消去・書き込み・読み戻し検証OK)。再起動する"
                    date > /mnt/nv_sys1/spi_backup/nv_reset.done
                    touch /tmp/nv_reboot
                else
                    nv_log "NG: NVの書き込みに失敗。元は $BK にある。電源を入れ直して確認(失敗すると、NVが半端な状態の可能性がある)"
                fi
            else
                nv_log "NG: NVの保存に失敗(空き不足など)。消さない"
                rm -f $BK
            fi
        fi
        [ -d /mnt/nv_sys1/spi_backup ] && cat $NV_LOG >> /mnt/nv_sys1/spi_backup/nv_reset.log 2>/dev/null
        sync; umount /mnt/nv_sys1 2>/dev/null
    else
        nv_log "NG: sys1 をマウントできない"
    fi
) > /dev/null 2>&1
if [ -f /tmp/nv_reboot ]; then sync; reboot; sleep 60; fi
# ----------------------------------------------------------------------------------------------------
    /opt/dtvtuner/etc/startdtvtuner
# ======== end ========
