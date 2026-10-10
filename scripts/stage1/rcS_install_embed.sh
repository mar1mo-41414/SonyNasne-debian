        # p3 にまだ nasne 用の Debian が無ければ、この .dlm に同梱された Debian (/debian) を入れる。
        # モードは /debian/etc/nasne-install-mode: "image <サイズ>"=XFSのp3(録画データ)を残し、中にext3のイメージを作る / "format"=p3をext3で初期化(録画データは消える)
        need_install=1
        if [ "$P3R" = 0 ] && [ -e /mnt/p3/etc/nasne-direct-boot ] && [ -x /mnt/p3/usr/local/sbin/nasne-stage1.sh ]; then need_install=0; fi
        IMODE=$(cat /debian/etc/nasne-install-mode 2>/dev/null)
        echo "need_install=$need_install install-mode='$IMODE'"
        if [ "$need_install" = 1 ] && [ -z "$IMODE" ]; then
            echo "インストールの許可(同梱の印)が無い。何もしない"
            need_install=0
        fi
        if [ "$need_install" = 1 ]; then
            case "$IMODE" in
                image*)
                    if [ "$P3R" = 0 ] && [ "$P3MODE" = direct ]; then
                        echo "p3 はext3だがnasne用Debianではない。imageモードはXFSのp3専用。何もしない(formatモードで作り直すこと)"
                        need_install=0
                    elif [ "$P3R" = 1 ]; then
                        echo "p3 をマウントできない。何もしない"
                        need_install=0
                    fi;;
            esac
        fi
        if [ "$need_install" = 1 ]; then
            # 長い作業(mkfs・コピー)の間にMCUのウォッチドッグ(約333秒)でリセットされないよう、先に止める
            umount -d /mnt/p3 2>/dev/null
            mount -t proc none /debian/proc
            mount -o bind /dev /debian/dev
            mkdir -p /debian/var/log; : > /debian/var/log/nasne-mcu-wd.log
            chroot /debian /etc/init.d/nasne-mcu-wd start
            wdn=0; while [ $wdn -lt 45 ] && ! grep -q "MCU watchdog disabled" /debian/var/log/nasne-mcu-wd.log 2>/dev/null; do sleep 1; wdn=$((wdn + 1)); done
            echo "watchdog stop wait: ${wdn}s"
            if ! grep -q "MCU watchdog disabled" /debian/var/log/nasne-mcu-wd.log 2>/dev/null; then
                echo "ウォッチドッグを止められなかった。インストールを中止する(途中でリセットされると中途半端になるため)"
                umount /debian/dev; umount /debian/proc
                need_install=0
            fi
        fi
        if [ "$need_install" = 1 ]; then
            case "$IMODE" in
                format*)
                    umount $P3X 2>/dev/null
                    echo "mkfs.ext3 /dev/sda3"
                    chroot /debian /sbin/mkfs.ext3 -q -F -L debian -i 262144 -m 1 /dev/sda3
                    echo "mkfs rc=$?"
                    umount /debian/dev; umount /debian/proc
                    mount -t ext3 -o rw /dev/sda3 /mnt/p3 || echo "mount new p3 FAILED";;
                image*)
                    ISIZE=${IMODE#image }
                    set -- $(df -k $P3X | tail -1); AVAIL=$4
                    echo "XFS p3 free: ${AVAIL}KB, image size: $ISIZE"
                    if [ "${AVAIL:-0}" -lt 2000000 ]; then
                        echo "XFSの空きが2GB未満。インストールしない"
                        umount /debian/dev; umount /debian/proc
                        need_install=0
                    else
                        mkdir -p /debian/mnt/p3x; mount -o bind $P3X /debian/mnt/p3x
                        mkdir -p $P3X/.nasne-debian; rm -f $P3X/.nasne-debian/root.img
                        chroot /debian /bin/sh -c "truncate -s $ISIZE /mnt/p3x/.nasne-debian/root.img && mkfs.ext3 -q -F -L debian -i 65536 -m 1 /mnt/p3x/.nasne-debian/root.img"
                        echo "image mkfs rc=$?"
                        umount /debian/mnt/p3x; umount /debian/dev; umount /debian/proc
                        for i in 0 1 2 3 4 5 6 7; do [ -b /dev/loop$i ] || mknod /dev/loop$i b 7 $i; done
                        mount -t ext3 -o loop,rw $P3X/.nasne-debian/root.img /mnt/p3 || echo "mount new image FAILED"
                    fi;;
            esac
        fi
        if [ "$need_install" = 1 ]; then
            if mountpoint -q /mnt/p3 2>/dev/null || [ -d /mnt/p3/lost+found ]; then
                echo "copy /debian -> p3"
                cp -a /debian/. /mnt/p3/
                echo "copy rc=$?"
                INSTALLED=1
                rm -f /mnt/p3/etc/nasne-install-mode
                sync
            fi
            umount_p3
            mount_p3; P3R=$?
            echo "remount after install: mount_p3 rc=$P3R mode=$P3MODE"
        fi
