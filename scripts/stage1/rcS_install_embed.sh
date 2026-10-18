        # p3 にまだ nasne 用の Debian が無ければ、この .dlm に同梱された Debian (/debian) を入れる。
        # モードは /debian/etc/nasne-install-mode: "image <サイズ>"=XFSのp3(録画データ)を残し、中にext3のイメージを作る / "format"=p3をext3で初期化(録画データは消える)
        need_install=1
        if [ "$P3R" = 0 ] && { [ -f /mnt/p3/etc/nasne-install-ok ] || [ ! -e /mnt/p3/etc/nasne-install-mode ]; } && [ -e /mnt/p3/etc/nasne-direct-boot ] && [ -x /mnt/p3/usr/local/sbin/nasne-stage1.sh ]; then need_install=0; fi
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
            if ! mount -t proc none /debian/proc || ! mount -o bind /dev /debian/dev; then
                echo "Debian chroot のマウントに失敗。インストール中止"
                need_install=0
            else
                mkdir -p /debian/var/log; : > /debian/var/log/nasne-mcu-wd.log
                chroot /debian /etc/init.d/nasne-mcu-wd start
            fi
            wdn=0
            if [ "$need_install" = 1 ]; then
                while [ $wdn -lt 45 ] && ! grep -q "MCU watchdog disabled" /debian/var/log/nasne-mcu-wd.log 2>/dev/null; do sleep 1; wdn=$((wdn + 1)); done
            fi
            echo "watchdog stop wait: ${wdn}s"
            if [ "$need_install" = 1 ] && ! grep -q "MCU watchdog disabled" /debian/var/log/nasne-mcu-wd.log 2>/dev/null; then
                echo "ウォッチドッグを止められなかった。インストールを中止する(途中でリセットされると中途半端になるため)"
                umount /debian/dev; umount /debian/proc
                need_install=0
            fi
            if [ "$need_install" != 1 ]; then
                umount /debian/dev 2>/dev/null
                umount /debian/proc 2>/dev/null
            fi
        fi
        if [ "$need_install" = 1 ]; then
            case "$IMODE" in
                format*)
                    umount $P3X 2>/dev/null
                    echo "mkfs.ext3 /dev/sda3"
                    if ! chroot /debian /sbin/mkfs.ext3 -q -F -L debian -i 262144 -m 1 /dev/sda3; then
                        echo "mkfs failed: abort"
                        need_install=0
                    fi
                    umount /debian/dev; umount /debian/proc
                    if [ "$need_install" = 1 ] && ! mount -t ext3 -o rw /dev/sda3 /mnt/p3; then
                        echo "mount new p3 FAILED: abort"
                        need_install=0
                    fi;;
                image*)
                    ISIZE=${IMODE#image }
                    AVAIL=0
                    case "$ISIZE" in ''|*[!0-9KMGTP]*) echo "invalid image size: abort"; need_install=0;; esac
                    if ! grep -qE '^/dev/sda3[[:space:]]+/mnt/p3x[[:space:]]+xfs[[:space:]]' /proc/mounts; then
                        echo "XFS p3 が /dev/sda3 から /mnt/p3x にマウントされていない: abort"
                        need_install=0
                    else
                        set -- $(df -k "$P3X" | tail -1); AVAIL=${4:-0}
                        case "$AVAIL" in ''|*[!0-9]*) AVAIL=0;; esac
                    fi
                    echo "XFS p3 free: ${AVAIL}KB, image size: $ISIZE"
                    if [ "$need_install" != 1 ] || [ "${AVAIL:-0}" -lt 2000000 ]; then
                        echo "XFSの空きが2GB未満。インストールしない"
                        umount /debian/dev; umount /debian/proc
                        need_install=0
                    else
                        if ! mkdir -p /debian/mnt/p3x || ! mount -o bind "$P3X" /debian/mnt/p3x; then
                            echo "XFS bind mount FAILED: abort"
                            need_install=0
                        fi
                        if [ "$need_install" = 1 ]; then
                            if ! mkdir -p "$P3X/.nasne-debian" ||
                               ! rm -f "$P3X/.nasne-debian/root.img" ||
                               ! chroot /debian /bin/sh -c "truncate -s $ISIZE /mnt/p3x/.nasne-debian/root.img && mkfs.ext3 -q -F -L debian -i 65536 -m 1 /mnt/p3x/.nasne-debian/root.img"; then
                                echo "image mkfs FAILED: abort"
                                need_install=0
                            fi
                        fi
                        umount /debian/mnt/p3x 2>/dev/null
                        umount /debian/dev; umount /debian/proc
                        if [ "$need_install" = 1 ]; then
                            for i in 0 1 2 3 4 5 6 7; do [ -b /dev/loop$i ] || mknod /dev/loop$i b 7 $i; done
                            if ! mount -t ext3 -o loop,rw "$P3X/.nasne-debian/root.img" /mnt/p3; then
                                echo "mount new image FAILED: abort"
                                need_install=0
                            fi
                        fi
                    fi;;
            esac
        fi
        if [ "$need_install" = 1 ]; then
            # 既存ディレクトリ(lost+found)だけで成功判定しない。実際のext3マウントを確認する。
            if grep -qE '^[^ ]+[[:space:]]+/mnt/p3[[:space:]]+ext3[[:space:]]' /proc/mounts; then
                echo "copy /debian -> p3"
                # 不完全なコピーから次回起動時にSPI書き換えへ進まないよう、成功印は先に除去する。
                if rm -f /mnt/p3/etc/nasne-install-ok &&
                   cp -a /debian/. /mnt/p3/ &&
                   [ -e /mnt/p3/etc/nasne-direct-boot ] &&
                   [ -x /mnt/p3/usr/local/sbin/nasne-stage1.sh ] &&
                   sync &&
                   rm -f /mnt/p3/etc/nasne-install-mode &&
                   touch /mnt/p3/etc/nasne-install-ok && sync; then
                    echo "copy + validation SUCCESS"
                    INSTALLED=1
                else
                    echo "copy/validation FAILED: SPI stage1 is blocked"
                    rm -f /mnt/p3/etc/nasne-install-ok
                    need_install=0
                fi
            else
                echo "ext3 /mnt/p3 is not mounted: abort"
                need_install=0
            fi
            umount_p3
            mount_p3; P3R=$?
            echo "remount after install: mount_p3 rc=$P3R mode=$P3MODE"
        fi
