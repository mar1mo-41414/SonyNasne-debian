#!/bin/busybox sh
# nasne 新カーネルの initramfs の /init: p3(XFS)の中の Debian 12 のイメージ(ext4)をループでマウントして switch_root する。
#   p3 の /.nasne-debian12/root.img が無い、マウントに失敗したときは、ログを kmsg に出して待つ(ネットワークは ip= で上がっている)。
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
log() { echo "<6>[initramfs] $*" > /dev/kmsg; }
log "start"
# 診断: ディスクを読む前の、候補の物理ページ(過去に壊れたページ)の先頭16バイトを記録する(壊れた後の内容と比べる)
if grep -q "nasne.snap" /proc/cmdline; then
for a in 10938000 10939000 1093a000 1093b000 1093c000 1093f000 11008000 11010000 11011000 11012000 11013000 1003f000; do
  log "snap $a $(dd if=/dev/mem bs=4096 skip=$((0x$a / 4096)) count=1 2>/dev/null | od -An -tx4 -N16 | tr -s ' ' | sed 's/^ //')"
done
fi

# ディスク待ち(最大 120 秒)
i=0
while [ ! -b /dev/sda3 ] && [ "$i" -lt 120 ]; do sleep 1; i=$((i + 1)); done
[ -b /dev/sda3 ] || { log "sda3 が見つかりません"; while :; do sleep 60; done; }
log "sda3 ready after ${i}s"

mkdir -p /p3 /newroot
mount -t xfs -o ro /dev/sda3 /p3 2>/dev/null || mount -o ro /dev/sda3 /p3 || { log "p3 のマウントに失敗"; while :; do sleep 60; done; }
IMG=/p3/.nasne-debian12/root.img
# 実行時の設定: p3 の /.nasne-debian12/opts(1行に1つ。カーネルの起動引数と同じ書き方: nasne.diag3 など。maxkb=<KB>、rakb=<KB> も)。カーネルを作り直さずに診断を切り替えるため
OPTS=/p3/.nasne-debian12/opts
has() { grep -q "$1" /proc/cmdline 2>/dev/null || { [ -f "$OPTS" ] && grep -q "$1" "$OPTS"; }; }
optval() { sed -n "s/^$1=//p" "$OPTS" 2>/dev/null | head -1; }
[ -f "$OPTS" ] && log "opts: $(tr '\n' ' ' < $OPTS)"
MAXKB=$(optval maxkb); RAKB=$(optval rakb)
[ -n "$MAXKB" ] && echo "$MAXKB" > /sys/block/sda/queue/max_sectors_kb && log "max_sectors_kb=$MAXKB"
[ -n "$RAKB" ] && echo "$RAKB" > /sys/block/sda/queue/read_ahead_kb && log "sda read_ahead_kb=$RAKB"
[ -f "$IMG" ] || { log "$IMG がありません"; ls -la /p3 | head -20 > /dev/kmsg; while :; do sleep 60; done; }
if has "nasne.diag"; then
  # 診断: root.img の先頭 64MB を、1MBごとに冷たい状態で読んで md5 を出す(ホスト側の期待値と比べる)
  for mode in pass1 pass2 pass3; do
    echo 3 > /proc/sys/vm/drop_caches
    i=0
    while [ $i -lt 64 ]; do
      log "diag $mode $i $(dd if=$IMG bs=1M skip=$i count=1 2>/dev/null | md5sum | cut -c1-32)"
      i=$((i + 1))
    done
  done
  # 化けた位置の特定: 同じ 8MB(MB 20〜27)を冷たい状態で6回読んで tmpfs に置き、1回目との差をバイト単位で出す
  mount -t tmpfs -o size=90m tmpfs /tmp 2>/dev/null
  for k in 1 2 3 4 5 6 7 8 9; do
    echo 3 > /proc/sys/vm/drop_caches
    dd if=$IMG bs=1M skip=20 count=8 of=/tmp/p$k 2>/dev/null
  done
  for k in 2 3 4 5 6 7 8 9; do
    n=$(cmp -l /tmp/p1 /tmp/p$k | wc -l)
    log "diag p1 vs p$k 違うバイト数=$n"
    [ "$n" != "0" ] && { cmp -l /tmp/p1 /tmp/p$k | head -1 | while read off va vb; do log "diag p1/p$k 最初の違い off=$((off - 1))"; done; cmp -l /tmp/p1 /tmp/p$k | tail -1 | while read off va vb; do log "diag p1/p$k 最後の違い off=$((off - 1))"; done; }
  done
  log "diag done"
fi
if has "nasne.small"; then
  # 診断6: 小さい冷たい読み(32KB)を、いろいろな位置で2回ずつ読んで比べる(最初の読みで化けるか)
  bad=0; tot=0
  for off in $(seq 3 7 900); do
    echo 3 > /proc/sys/vm/drop_caches
    a=$(dd if=$IMG bs=16k skip=$((off * 8)) count=2 2>/dev/null | md5sum | cut -c1-32)
    echo 3 > /proc/sys/vm/drop_caches
    b=$(dd if=$IMG bs=16k skip=$((off * 8)) count=2 2>/dev/null | md5sum | cut -c1-32)
    tot=$((tot + 1))
    if [ "$a" != "$b" ]; then bad=$((bad + 1)); log "small 不一致 位置=$((off * 8 * 16))KB"; fi
  done
  log "small 小さな冷たい読み $tot 回中、不一致 $bad 回"
fi
umount /p3
mount -t xfs /dev/sda3 /p3 || { log "p3 の rw マウントに失敗"; while :; do sleep 60; done; }
if has "nasne.looptest"; then
  # 診断7: ext4 をマウントする前に、loop デバイス単体の冷たい読みを2回比べる(64KB の読みを多数の位置で)
  losetup /dev/loop5 $IMG
  bad=0; tot=0
  for off in $(seq 11 13 800); do
    echo 3 > /proc/sys/vm/drop_caches
    a=$(dd if=/dev/loop5 bs=64k skip=$((off * 4)) count=2 2>/dev/null | md5sum | cut -c1-32)
    echo 3 > /proc/sys/vm/drop_caches
    b=$(dd if=/dev/loop5 bs=64k skip=$((off * 4)) count=2 2>/dev/null | md5sum | cut -c1-32)
    tot=$((tot + 1))
    if [ "$a" != "$b" ]; then bad=$((bad + 1)); log "looptest 不一致 位置=$((off * 256))KB"; fi
  done
  log "looptest loop 単体の冷たい読み $tot 回中、不一致 $bad 回"
  losetup -d /dev/loop5
fi
mount -t ext4 -o loop "$IMG" /newroot || { log "root.img のマウントに失敗"; ls -la /dev/loop* > /dev/kmsg 2>&1; while :; do sleep 60; done; }
[ -n "$RAKB" ] && for d in /sys/block/loop*/queue/read_ahead_kb; do echo "$RAKB" > $d; done 2>/dev/null
if has "nasne.md5"; then
  # デバッグ: 起動直後(キャッシュが冷たい状態)で、実行されるファイルの内容が正しく読めているかを確認する
  chk() { m=$(md5sum "$2" | cut -d" " -f1); if [ "$m" = "$1" ]; then log "md5 OK  $2"; else log "md5 BAD $2 $m"; fi; }
  chk 969f2555b67be16b8f6864d234a19657 /newroot/lib/mipsel-linux-gnu/libc.so.6
  chk 4722d951d673d015b618750492666a9e /newroot/lib/mipsel-linux-gnu/ld.so.1
  chk ae1aabf34bd88b821135b78e9235db57 /newroot/sbin/init
fi
if has "nasne.diag2"; then
  mount -t tmpfs -o size=90m tmpfs /tmp 2>/dev/null
  L=/newroot/lib/mipsel-linux-gnu/libc.so.6
  LD=$(grep loop /proc/mounts | head -1 | cut -d" " -f1)
  for k in 1 2 3 4 5 6; do
    echo 3 > /proc/sys/vm/drop_caches
    cat $L > /tmp/c$k
    echo 3 > /proc/sys/vm/drop_caches
    dd if=$LD bs=1M skip=20 count=8 of=/tmp/l$k 2>/dev/null
  done
  for k in 2 3 4 5 6; do
    n=$(cmp -l /tmp/c1 /tmp/c$k | wc -l); log "diag2 ext4-libc c1 vs c$k 違うバイト数=$n"
    [ "$n" != "0" ] && { cmp -l /tmp/c1 /tmp/c$k | head -1 | while read off va vb; do log "diag2 c$k 最初の違い off=$((off - 1))"; done; cmp -l /tmp/c1 /tmp/c$k | tail -1 | while read off va vb; do log "diag2 c$k 最後の違い off=$((off - 1))"; done; }
    n=$(cmp -l /tmp/l1 /tmp/l$k | wc -l); log "diag2 loopdev l1 vs l$k 違うバイト数=$n"
    [ "$n" != "0" ] && { cmp -l /tmp/l1 /tmp/l$k | head -1 | while read off va vb; do log "diag2 l$k 最初の違い off=$((off - 1))"; done; cmp -l /tmp/l1 /tmp/l$k | tail -1 | while read off va vb; do log "diag2 l$k 最後の違い off=$((off - 1))"; done; }
  done
  log "diag2 libc md5: $(md5sum /tmp/c1)  (期待 969f2555b67be16b8f6864d234a19657)"
  umount /tmp
fi
if has "nasne.diag3"; then
  # 診断3: 冷たい状態から Debian のプログラムを繰り返し実行して、落ちる回数を数える(実際の起動のページフォルト経路)
  ok=0; ng=0; i=0
  while [ $i -lt 40 ]; do
    echo 3 > /proc/sys/vm/drop_caches
    if chroot /newroot /bin/sed --version >/dev/null 2>&1; then ok=$((ok + 1)); else ng=$((ng + 1)); fi
    i=$((i + 1))
  done
  log "diag3 sed 冷たい実行 40回: 成功=$ok 失敗=$ng"
  ok=0; ng=0; i=0
  while [ $i -lt 40 ]; do
    if chroot /newroot /bin/sed --version >/dev/null 2>&1; then ok=$((ok + 1)); else ng=$((ng + 1)); fi
    i=$((i + 1))
  done
  log "diag3 sed 温かい実行 40回: 成功=$ok 失敗=$ng"
fi
if has "nasne.memscan"; then
  # 診断5: 物理メモリを2回(間を空けて)読み比べて、勝手に書き換わっている領域を探す(誰かが DMA で書き続けている)
  scan() { i=0; while [ $i -lt $2 ]; do echo "$(dd if=/dev/mem bs=65536 skip=$(( $1 + i )) count=1 2>/dev/null | md5sum | cut -c1-8)"; i=$((i + 1)); done; }
  # 0x10800000(MB 128)付近は避け、0x11000000〜0x12000000(256ブロック=16MB)を見る。ブロック番号 = 物理/64KB
  scan $((0x11000000 / 65536)) 256 > /tmp/m1 2>/dev/null || { mount -t tmpfs tmpfs /tmp; scan $((0x11000000 / 65536)) 256 > /tmp/m1; }
  sleep 1
  scan $((0x11000000 / 65536)) 256 > /tmp/m2
  paste /tmp/m1 /tmp/m2 | awk '$1 != $2 {print NR-1}' | while read b; do log "memscan 変わったブロック: 物理 0x$(printf %x $(( 0x11000000 + b * 65536 )))"; done
  log "memscan done"
fi
if has "nasne.md5b"; then
  # 診断4: マウント直後の最初の読みで libc が化けた位置を、2回目(drop 後)の読みと比べて特定する
  mount -t tmpfs -o size=30m tmpfs /tmp 2>/dev/null
  L=/newroot/lib/mipsel-linux-gnu/libc.so.6
  cat $L > /tmp/c1
  echo 3 > /proc/sys/vm/drop_caches
  cat $L > /tmp/c2
  n=$(cmp -l /tmp/c1 /tmp/c2 | wc -l)
  log "md5b 1回目と2回目の違うバイト数=$n"
  if [ "$n" != "0" ]; then
    cmp -l /tmp/c1 /tmp/c2 | head -1 | while read off va vb; do log "md5b 最初の違い off=$((off - 1))"; done
    cmp -l /tmp/c1 /tmp/c2 | tail -1 | while read off va vb; do log "md5b 最後の違い off=$((off - 1))"; done
    cmp -l /tmp/c1 /tmp/c2 | awk '{print int(($1-1)/16384)}' | uniq -c | head -20 | while read cnt pg; do log "md5b ページ(16KB) $pg: $cnt バイト違う"; done
    pg=$(cmp -l /tmp/c1 /tmp/c2 | head -1 | awk '{print int(($1-1)/16384)}')
    log "md5b 化けたページ $pg の1回目の md5: $(dd if=/tmp/c1 bs=16384 skip=$pg count=1 2>/dev/null | md5sum | cut -c1-32)"
    log "md5b 化けたページ $pg の2回目の md5: $(dd if=/tmp/c2 bs=16384 skip=$pg count=1 2>/dev/null | md5sum | cut -c1-32)"
    log "md5b 化けたページ先頭32バイト(1回目): $(dd if=/tmp/c1 bs=16384 skip=$pg count=1 2>/dev/null | head -c 32 | od -An -tx1 | tr -d ' \n')"
    log "md5b 化けたページ先頭32バイト(2回目): $(dd if=/tmp/c2 bs=16384 skip=$pg count=1 2>/dev/null | head -c 32 | od -An -tx1 | tr -d ' \n')"
    for q in $(seq 0 115); do log "md5b c2 page $q $(dd if=/tmp/c2 bs=16384 skip=$q count=1 2>/dev/null | md5sum | cut -c1-32)"; done
  fi
  umount /tmp
fi
if has "nasne.mapsum"; then
  # 診断8: マウント直後に、冷たい libc を mmap(フォルト経路)で読んだ和と、期待値(ホスト側で計算)を比べる
  L=/newroot/lib/mipsel-linux-gnu/libc.so.6
  log "mapsum x(最初・mmap+EXEC): $(mapsum $L x | tr '\n' ' ')"
  echo 3 > /proc/sys/vm/drop_caches
  log "mapsum r(drop後・mmap読み取りのみ): $(mapsum $L r | tr '\n' ' ')"
  echo 3 > /proc/sys/vm/drop_caches
  log "mapsum x(drop後・mmap+EXEC): $(mapsum $L x | tr '\n' ' ')"
fi
mkdir -p /newroot/data
mount --move /p3 /newroot/data
mount --move /dev /newroot/dev 2>/dev/null
mount --move /proc /newroot/proc
mount --move /sys /newroot/sys
log "switch_root to Debian 12"
exec switch_root /newroot /sbin/init
log "switch_root 失敗"
while :; do sleep 60; done
