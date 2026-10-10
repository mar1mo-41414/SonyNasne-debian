
# ======== nasne-debian stage1 (scripts/stage1/make_stage1_dlm.py が /opt/dtvtuner/etc/startdtvtuner の代わりに挿入) ========
# 通常は公式アプリ(procmng / startdtvtuner)を起動しない。p3 の Debian を chroot して SPI の KNL を自作カーネルへ書き換え、再起動する。
# p3 の /etc/nasne-boot-official-once があれば、その1回だけ公式アプリを起動する(印は消す。次の起動からはDebianに戻る)。
S1LOG=/tmp/nasne_stage1_rcs.log
P3X=/mnt/p3x
P3MODE=
OFFICIAL=0
RESCUE=0
INSTALLED=0
# p3 の Debian を /mnt/p3 にマウントする。戻り値: 0=成功(P3MODE=direct: p3がext3 / image: XFSのp3の中のイメージ)、
#   1=ext3でもXFSでもマウントできない、2=XFSだがDebianのイメージが無い(公式のp3。$P3X にマウントしたまま)
mount_p3() {
    mkdir -p /mnt/p3 $P3X
    if mount -t ext3 -o rw /dev/sda3 /mnt/p3 2>/dev/null; then P3MODE=direct; return 0; fi
    if mount -t xfs -o rw /dev/sda3 $P3X 2>/dev/null; then
        for i in 0 1 2 3 4 5 6 7; do [ -b /dev/loop$i ] || mknod /dev/loop$i b 7 $i; done
        if [ -f $P3X/.nasne-debian/root.img ] && mount -t ext3 -o loop,rw $P3X/.nasne-debian/root.img /mnt/p3 2>/dev/null; then P3MODE=image; return 0; fi
        return 2
    fi
    return 1
}
umount_p3() { umount -d /mnt/p3 2>/dev/null; umount $P3X 2>/dev/null; }
{
    echo "stage1 rcS start: $(date)"
    n=0; while [ ! -b /dev/sda3 ] && [ $n -lt 60 ]; do sleep 1; n=$((n + 1)); done
    echo "sda3 wait: ${n}s"
    mount_p3; P3R=$?
    echo "mount_p3 rc=$P3R mode=$P3MODE"
    if [ "$P3R" = 0 ] && [ -e /mnt/p3/etc/nasne-boot-official-once ]; then
        rm -f /mnt/p3/etc/nasne-boot-official-once
        cp "$S1LOG" /mnt/p3/var/log/nasne_stage1_rcs.log 2>/dev/null; sync
        umount_p3
        OFFICIAL=1
        echo "official-once: 公式アプリを1回だけ起動する(印は消した。次の起動からDebianに戻る)"
    fi
    if [ "$P3R" = 0 ] && [ "$OFFICIAL" != 1 ] && [ -e /mnt/p3/etc/nasne-boot-rescue-once ]; then
        rm -f /mnt/p3/etc/nasne-boot-rescue-once; sync
        RESCUE=1
        echo "rescue-once: ネットワークとtelnetだけ上げて待つ(段階1のインストール・SPI書き換えはしない)"
    fi
    if [ "$OFFICIAL" != 1 ]; then
        # 公式アプリを起動しない場合だけ、ネットワークと(確認用の)telnetを上げる。公式アプリの起動前には手を加えない
        killall telnetd 2>/dev/null     # nasne_init のデバッグtelnetが残っていると、(switch_root後は動かない)そちらが23番を握ってしまう
        ifconfig eth0 up
        udhcpc -i eth0 -t 5 -T 3 -A 3 -b -p /var/run/udhcpc.eth0.pid
@TELNET_EARLY@
    fi
    if [ "$OFFICIAL" != 1 ] && [ "$RESCUE" != 1 ]; then
@INSTALL_BLOCK@
        if [ "$P3R" = 0 ]; then
            if [ -x /mnt/p3/usr/local/sbin/nasne-stage1.sh ]; then
                mount -t proc  none /mnt/p3/proc
                mount -t sysfs none /mnt/p3/sys
                mount -o bind /dev     /mnt/p3/dev
                mount -o bind /dev/pts /mnt/p3/dev/pts
                cp /etc/resolv.conf /mnt/p3/etc/resolv.conf 2>/dev/null
                chroot /mnt/p3 /usr/sbin/sshd
                echo "sshd started in chroot (stage1 が終わるまでの間だけ入れる)"
                chroot /mnt/p3 /usr/local/sbin/nasne-stage1.sh
                S1RC=$?
                echo "stage1 returned rc=$S1RC (成功ならrebootして戻らないはず)"
                # Debianを入れた直後で、カーネルがすでに目的のもの(stage1が何もせず終了)なら、ここで再起動してDebianに移る
                if [ "$INSTALLED" = 1 ] && [ "$S1RC" = 0 ]; then echo "installed: rebooting into Debian"; sync; reboot -f; fi
            else
                echo "p3 に /usr/local/sbin/nasne-stage1.sh が無い"
            fi
        else
            echo "Debian の p3 をマウントできなかった (mount_p3 rc=$P3R)。何もしない"
        fi
    fi
    echo "stage1 rcS end: $(date)"
} >> "$S1LOG" 2>&1
cp "$S1LOG" /mnt/p3/var/log/nasne_stage1_rcs.log 2>/dev/null
@TELNET_LATE@
# rescue-once: rcSを終わらせずに待つ(telnetdを生かしておく。戻るには電源を入れ直す)
if [ "$RESCUE" = 1 ]; then while true; do sleep 60; done; fi
if [ "$OFFICIAL" = 1 ]; then
@OFFICIAL_PRE@
    /opt/dtvtuner/etc/startdtvtuner
fi
# ======== /nasne-debian stage1 ========
