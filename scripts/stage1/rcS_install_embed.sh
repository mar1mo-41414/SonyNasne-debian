    # p3 にまだ nasne 用の Debian が無ければ、この .dlm に同梱された Debian (/debian) で p3 を作る(p3は初期化される)
    need_install=1
    if mount -t ext3 -o rw /dev/sda3 /mnt/p3 2>/dev/null; then
        if [ -e /mnt/p3/etc/nasne-direct-boot ] && [ -x /mnt/p3/usr/local/sbin/nasne-stage1.sh ]; then need_install=0; fi
        umount /mnt/p3
    fi
    echo "need_install=$need_install"
    if [ "$need_install" = 1 ] && [ ! -f /debian/etc/nasne-install-format-p3 ]; then
        echo "p3 を初期化する許可(同梱の目印)が無い。インストールしない"
        need_install=0
    fi
    if [ "$need_install" = 1 ]; then
        mount -t proc none /debian/proc
        mount -o bind /dev /debian/dev
        # 長い作業(mkfs・コピー)の間にMCUのウォッチドッグ(約333秒)でリセットされないよう、先に止める
        mkdir -p /debian/var/log; : > /debian/var/log/nasne-mcu-wd.log
        chroot /debian /etc/init.d/nasne-mcu-wd start
        wdn=0; while [ $wdn -lt 45 ] && ! grep -q "MCU watchdog disabled" /debian/var/log/nasne-mcu-wd.log 2>/dev/null; do sleep 1; wdn=$((wdn + 1)); done
        echo "watchdog stop wait: ${wdn}s"
        if ! grep -q "MCU watchdog disabled" /debian/var/log/nasne-mcu-wd.log 2>/dev/null; then
            echo "ウォッチドッグを止められなかった。p3の初期化を中止する(途中でリセットされると中途半端になるため)"
            umount /debian/dev; umount /debian/proc
            need_install=0
        fi
    fi
    if [ "$need_install" = 1 ]; then
        echo "mkfs.ext3 /dev/sda3"
        chroot /debian /sbin/mkfs.ext3 -q -F -L debian -i 262144 -m 1 /dev/sda3
        echo "mkfs rc=$?"
        umount /debian/dev; umount /debian/proc
        if mount -t ext3 -o rw /dev/sda3 /mnt/p3; then
            echo "copy /debian -> p3"
            cp -a /debian/. /mnt/p3/
            echo "copy rc=$?"
            rm -f /mnt/p3/etc/nasne-install-format-p3
            sync
            umount /mnt/p3
        else
            echo "mount new p3 FAILED"
        fi
    fi
