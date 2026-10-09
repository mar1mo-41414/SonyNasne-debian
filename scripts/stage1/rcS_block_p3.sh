
# ======== nasne-debian stage1 (scripts/stage1/make_stage1_dlm.py が /opt/dtvtuner/etc/startdtvtuner の代わりに挿入) ========
# 公式アプリ(procmng / startdtvtuner)は起動しない。p3 の Debian を chroot して SPI の KNL を自作カーネルへ書き換え、再起動する。
S1LOG=/tmp/nasne_stage1_rcs.log
{
    echo "stage1 rcS start: $(date)"
    ifconfig eth0 up
    udhcpc -i eth0 -t 5 -T 3 -A 3 -b -p /var/run/udhcpc.eth0.pid
@TELNET_EARLY@
    n=0; while [ ! -b /dev/sda3 ] && [ $n -lt 60 ]; do sleep 1; n=$((n + 1)); done
    echo "sda3 wait: ${n}s"
    mkdir -p /mnt/p3
@INSTALL_BLOCK@
    if mount -t ext3 -o rw /dev/sda3 /mnt/p3; then
        if [ -x /mnt/p3/usr/local/sbin/nasne-stage1.sh ]; then
            mount -t proc  none /mnt/p3/proc
            mount -t sysfs none /mnt/p3/sys
            mount -o bind /dev     /mnt/p3/dev
            mount -o bind /dev/pts /mnt/p3/dev/pts
            cp /etc/resolv.conf /mnt/p3/etc/resolv.conf 2>/dev/null
            chroot /mnt/p3 /usr/sbin/sshd
            echo "sshd started in chroot (stage1 が終わるまでの間だけ入れる)"
            chroot /mnt/p3 /usr/local/sbin/nasne-stage1.sh
            echo "stage1 returned rc=$? (成功ならrebootして戻らないはず)"
        else
            echo "p3 に /usr/local/sbin/nasne-stage1.sh が無い"
        fi
    else
        echo "mount /dev/sda3 (ext3) FAILED"
    fi
    echo "stage1 rcS end: $(date)"
} >> "$S1LOG" 2>&1
cp "$S1LOG" /mnt/p3/var/log/nasne_stage1_rcs.log 2>/dev/null
@TELNET_LATE@
# ======== /nasne-debian stage1 ========
