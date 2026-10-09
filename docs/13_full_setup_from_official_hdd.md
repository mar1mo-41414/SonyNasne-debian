# 13. 公式HDDの状態から、Debian(PID1)直起動・ウォッチドッグ停止・TVチューナー化までの一連の手順

> 対象: **すでに公式ファーム(v2.60)で正常に動いているnasne(CECH-ZNR2J)**。HDDは壊れておらず、紐付けも正常な状態からスタートする。
> ([docs/04](04_hdd_recovery_guide.md) のHDD全損復旧とは別の話。復旧が必要な場合は先にそちらを終わらせてから、このドキュメントに進む。)
>
> **最終形態は、公式機能を全部捨てて完全にDebian(PID1)で起動すること。** そのために、SPIフラッシュ(KNL=カーネル)を
> 自作カーネル(内蔵initramfs入り)に書き換える([docs/05](05_kernel_and_direct_boot.md)と同じ方式)。
> **ただし、SPI書き換え作業そのものは、公式の激ミニマムなrootfs上ではなく、一旦`chroot`で本物のDebian環境に入ってから行う。**
> 以前は公式rootfsの`rcS`から直接`mtdtool`を呼んでSPIを書き換えていたが、procmngが何らかのきっかけでHDD(sys1)の
> `00550066.dlm`を自動的にロールバックする未解明の現象に悩まされた。別の実績ある実装を参考に、**公式TVアプリ機能
> (procmng/`startdtvtuner`)を最初から一切起動しない**設計に変えたことで、この問題を構造的に避けられると考えている。
>
> 全体は2回起動すれば完結する: **1回目(公式カーネルのまま)は`chroot`でDebianに入ってSPIを書き換えるだけの「踏み台」**、
> **2回目(自作カーネル)で初めて`switch_root`により完全なPID1のDebianになる**。
>
> ⚠️ 自己責任です。SPIフラッシュの内容を壊すと起動しなくなる可能性があります(その場合の回復手段は[トラブルシュート](#トラブルシュート)参照)。
> **作業前に、このHDDの `00550066.dlm` を含むsys1の内容と、p3(録画データ含む)を必ずバックアップしてください**。

## 検証状況(正直に)

| 内容 | 状態 |
|---|---|
| SPIのKNL差し替え → p3のDebianへ`switch_root`([docs/05](05_kernel_and_direct_boot.md)) | ✅ 実機で確認(CH341AでSPIを書いた場合) |
| ウォッチドッグ停止([docs/06](06_mcu_watchdog.md))、地デジ受信・配信([docs/11](11_tv_streaming.md)) | ✅ 実機で確認 |
| 公式rootfsの `.dlm` 注入による任意ファイル展開([docs/03](03_firmware_format.md)) | ✅ 実機で確認 |
| `mtdtool`による`/dev/mtd0`書き込み(erase→write→verify→reboot) | ✅ 実機で確認(ただし公式rootfs上での実行。今回chroot環境に場所を変える) |
| `dlm_crypto.py decrypt-body` で公式rootfs(v2.60、30MB超)を取り出し、`tar` で展開する | ✅ 実機で確認(`--tail cfb --inflate` が必要。[docs/03](03_firmware_format.md)参照) |
| 実機の公式rootfsの構成(`/sbin/init` は `busybox` への**シンボリックリンク**、`/etc/init.d/rcS` は実行可能シェルスクリプト) | ✅ 実機で確認。[docs/02](02_boot_chain.md)の推定通り |
| `rcS`末尾の`startdtvtuner`呼び出しを削除し、`chroot`でDebianへ処理を渡す設計 | ⚠️ **本ドキュメントでの新方式。別の実績ある実装(`pivot_root`/`chroot`でp3に処理を渡すもの)を参考にしたが、この形(chroot内からさらにSPIを書き換えて`reboot`する部分まで含めて)はこのリポジトリの実機でまだ通し確認していない** |
| procmngによるHDD(sys1)の自動ロールバック現象 | ⚠️ 以前の方式(公式rootfsの`rcS`から直接SPI書き換え、`startdtvtuner`はそのまま動かす)で複数回確認した重大な問題。今回`startdtvtuner`自体を呼ばない設計にしたことで、理論上は再発しないはずだが未確認 |

このドキュメントは「手動でここまでできるはず」という設計と手順のまとめです。差異が出た箇所は都度このファイルを更新してください。

## 全体の流れ

```
1. PCでHDDを準備する(1回だけ、sys1とp3を同時に)
   ├─ sys1 の 00550066.dlm を自作版に差し替える
   │    ヘッダ: そのHDD自身の 00550066.dlm をテンプレートにする(→ マネージャの日付検査に自動で通る)
   │    ボディ: 公式rootfs + 「rcSの末尾を、p3(Debian)へのchroot+SPI書き換えに差し替えたもの」
   │    (startdtvtunerは呼ばない=procmngを起動させない)
   └─ p3 を ext3 にして Debian (wheezy) を置く。最終形態で必要なもの一式
       (nasne-recpt1 / ドライバ / nasne-mcu-wd / update-rc.d済み) に加え、
       SPI書き換え用ツール(mtdtool、自作カーネルknl_new.bin、nasne-stage1.sh)もここに入れる

2. 1回目の起動(公式カーネルのまま。procmngは起動しない)
   公式の起動チェーンがそのまま動き、rcSの最後で:
   ├─ /dev/sda3(p3=Debian)をmountし、proc/sys/devをbind
   ├─ chrootでsshdを起動(動作確認用)
   └─ chrootでnasne-stage1.shを実行:
        /dev/mtd0 の KNL を自作カーネル(内蔵initramfs入り)に書き換え、読み戻して確認する
        OKなら reboot

3. 2回目の起動(自作カーネル)
   自作カーネルの /nasne_init が p3 の Debian へ本当の switch_root をする(ここで初めてPID1がDebianになる)。
   Debian側の通常のinit(update-rc.d済み)が:
   ├─ ウォッチドッグ停止 (nasne-mcu-wd、rc.xcode4でチューナードライバ読み込みも兼ねる)
   └─ nasne-recpt1 --listen (HTTP配信サーバ)

4. curl http://<nasne>:8301/tuner/<ch> | ffplay -  で視聴
```

仕組みの元ネタ: `.dlm` とヘッダ検査は[docs/03](03_firmware_format.md)、ブートチェーンは[docs/02](02_boot_chain.md)、カーネル差し替えと直起動は[docs/05](05_kernel_and_direct_boot.md)、ウォッチドッグは[docs/06](06_mcu_watchdog.md)、TV視聴は[docs/11](11_tv_streaming.md)。

## 必要なもの

- Linux PC(Ubuntu等)、Python 3.8+、`mkfs.ext3`、Docker(カーネル/ツールのクロスビルド用)
- クロスコンパイラ環境 `nasne-kcc:gcc432`([docs/05](05_kernel_and_direct_boot.md)①で作る) と `gcc-mipsel-linux-gnu`(mtdtool用)
- `debootstrap`、`qemu-user-static`(`build_debian_rootfs.sh`用)
- このリポジトリの `scripts/`
- **公式ファーム**(Sonyの公開サーバから取得。再配布しないこと):
  ```bash
  wget http://ps-peripheral.dl.playstation.net/ps-peripheral/nasne/0260/KRST3101_0260_SECURE.dlm
  ```
- 対象のnasneから抜いたHDD(USB-SATA変換で接続)。**公式ファームで正常起動している状態のもの**
- B-CASカード(TV視聴まで確認する場合)

---

## 手順0. バックアップと下調べ

HDDをPCに挿し、sys1(p1)をマウントして現状を保存する。

```bash
lsblk -o NAME,SIZE,MODEL,SERIAL        # 対象HDDを確認(以後 /dev/sdX)
sudo mkdir -p /mnt/nasne_sys1 && sudo mount -o ro /dev/sdX1 /mnt/nasne_sys1
mkdir -p backup/sys1 && cp -a /mnt/nasne_sys1/. backup/sys1/
sudo umount /mnt/nasne_sys1
```

`backup/sys1/00550066.dlm` が、このあと新しい `.dlm` を作るときの**テンプレート**になる(ヘッダの hwtype・日付文字列をそのままコピーして使うので、
マネージャ `00110022.dlm` の日付検査([docs/03](03_firmware_format.md))に自動で通る。個体ID・マネージャ自体は一切変更しない)。

公式ファームから、同じバージョンの「素の」rootfsセグメントも取り出しておく(ボディの土台・ドライバ取り出し用):

```bash
python3 scripts/ofw_tool.py split KRST3101_0260_SECURE.dlm ofw_out/   # ofw_out/DLM.bin = 00550066.dlmと同一(md5一致)
python3 scripts/dlm_crypto.py decrypt-body backup/sys1/00550066.dlm official_rootfs_raw.tar --tail cfb --inflate   # 公式rootfsのtar本体を取り出す
mkdir official_rootfs && sudo tar -C official_rootfs -xf official_rootfs_raw.tar    # 中身を確認。rootとして展開(権限・デバイスファイルの保持にsudoが必要)
```

> `decrypt-body` は30MB超のrootfsだとボディが8バイト境界に満たず、既定の `--tail cfb`(直前のCBCブロックを再暗号化したキーストリームで末尾をXORする)
> と `--inflate`(gzipトレーラのCRC検証をスキップして生のtarを取り出す)を付けないと `tar` が途中で壊れる。詳細は[docs/03](03_firmware_format.md)。

`official_rootfs/` の中身を見て、起動スクリプトの実際の構成を確認する。実機(v2.60)で確認した結果:
- `sbin/init` は **`busybox` へのシンボリックリンク**。このファイル自体は変更しない。
- `etc/init.d/rcS` は通常の実行可能シェルスクリプト。**末尾の `/opt/dtvtuner/etc/startdtvtuner` 呼び出しだけを差し替える**([手順2](#手順2-rcs-の末尾を-chroot--spi書き換えに差し替える))。

必ず `cat official_rootfs/etc/init.d/rcS` で、自分のファームのバージョンでも同じ構成(特に最後の行が `/opt/dtvtuner/etc/startdtvtuner` であること)を確認してから次に進む。参考(実機・v2.60):

```sh
#!/bin/sh
trap "" SIGHUP
mount -t proc none /proc
mount -t usbfs none /proc/bus/usb
mount -t sysfs none /sys
/usr/bin/uevent_daemon
start_udev
mount -t devpts none /dev/pts
sysctl -p
ifconfig lo 127.0.0.1 netmask 255.0.0.0 up
route add -net 127.0.0.0 netmask 255.0.0.0 dev lo
# ---- for DTVTuner ----
/opt/dtvtuner/etc/startdtvtuner
```

## 手順1. 自作カーネルとDebianのrootfsを用意する(PC側)

既存の[docs/05](05_kernel_and_direct_boot.md) ①③の通りに進める(ここでは差分だけ述べる)。

```bash
# ① 自前カーネル(内蔵initramfs入り)をビルド
sh scripts/fetch_gpl_sources.sh
docker build -t nasne-kcc:gcc432 scripts/kernel_build
mkdir -p gpl_src/build && tar xjf gpl_src/mips-linux-2.6.29.tar.bz2 -C gpl_src/build
NASNE_CUSTOM=1 sh scripts/kernel_build/build.sh      # → gpl_src/build/out/vmlinux_new.bin
```

**この手順ではCH341Aを使わないので、SPIの完全ダンプは手元に無い。** `build_knl.py` はKNLセグメントの構築にSPIダンプ(ヘッダの雛形・マーカー位置の取得用)を使うが、
同じ用途であれば、公式ファームパッケージの `KNL.bin`(`ofw_tool.py split` の出力)でも流用できる(いずれもSonyが書いたオリジナルのKNLセグメントのヘッダを土台にするだけで、
検証ロジック自体はSPI上の実バイトに依存しない)。

> `ofw_tool.py split` の `KNL.bin` は**単体のセグメントファイル**(オフセット0開始)。`build_knl.py` は既定で「SPI全体ダンプ(16MB)の中の `0x100000` オフセット」
> から読む作りなので、**`--raw` を付けないと、ファイル内の無関係な場所(カーネル本体のデータ)をヘッダとして誤読する**(実機で確認済みのバグ、修正済み)。

```bash
python3 scripts/build_knl.py info   ofw_out/KNL.bin --slot KNL --raw
python3 scripts/build_knl.py build  ofw_out/KNL.bin knl_new.bin --slot KNL --raw --kernel gpl_src/build/out/vmlinux_new.bin
python3 scripts/build_knl.py verify knl_new.bin
```

`knl_new.bin` のサイズを確認する(KNL領域は `0x100000`〜`0x37ffff` の2.5MB = `0x280000` バイト。これを超えてはいけない):

```bash
stat -c '%s (0x%x)' knl_new.bin
```

Debianのrootfsを作る(未展開のまま、あとでp3へコピーする):

```bash
sudo SSH_PUBKEY=~/.ssh/id_rsa.pub ./scripts/build_debian_rootfs.sh /tmp/debian-root
```

これで `/tmp/debian-root` に、sshd・root用SSH鍵(RSA)までは設定済みのDebian wheezy (mipsel) ができる。
以下、このディレクトリに**直接**ファイルを置いていく(nasneにsshでログインできるのは起動後なので、`scp`/`ssh`は使わず、PC上のパスとして直接書き込む)。

### 1-1. 直起動フラグと基本設定([docs/05](05_kernel_and_direct_boot.md)③相当)

```bash
sudo touch /tmp/debian-root/etc/nasne-direct-boot

# 起動成功の印(rc.local)。rc.localが無ければ作る。
sudo tee -a /tmp/debian-root/etc/rc.local >/dev/null <<'EOF'
: > /var/lib/nasne-boot-count
exit 0
EOF
sudo chmod +x /tmp/debian-root/etc/rc.local

# Ethernet(カーネルにSynopGMAC組み込み済み、dhcp)
sudo tee /tmp/debian-root/etc/network/interfaces >/dev/null <<'EOF'
auto lo
iface lo inet loopback

allow-hotplug eth0
iface eth0 inet dhcp
EOF
```

### 1-2. 公式ドライバ(xcode4drv.ko、rc.xcode4)を取り出す

手順0で取り出した `official_rootfs/` の中に、公式ドライバ一式が入っている([docs/06](06_mcu_watchdog.md)の前提)。

```bash
find official_rootfs/opt/dtvtuner -iname 'xcode4drv.ko' -o -iname 'rc.xcode4'   # 実際のパスを確認(バージョンにより多少違う可能性がある)
sudo mkdir -p /tmp/debian-root/usr/local/sbin
sudo cp official_rootfs/opt/dtvtuner/lib/modules/xcode4drv.ko /tmp/debian-root/usr/local/sbin/
sudo cp official_rootfs/opt/dtvtuner/lib/modules/rc.xcode4    /tmp/debian-root/usr/local/sbin/
sudo chmod +x /tmp/debian-root/usr/local/sbin/rc.xcode4
```

### 1-3. MIPS用バイナリをクロスビルドする(mcui2c, nasne-recpt1, i2cx, mtdtool)

静的リンクの小物(mcui2c・nasne-recpt1・i2cx)は[docs/05](05_kernel_and_direct_boot.md)①で作ったDocker環境 `nasne-kcc:gcc432`
(nostdlib・静的リンクのクロスコンパイラ)を使う。`i2cx` は手順6([docs/08](08_tuner_i2c.md)のチューナー選局、`scripts/nasne_fe.py`がsshで呼ぶ)用に必要なので、
ここで一緒にビルドしておく:

```bash
docker run --rm -v "$PWD":/w -w /w nasne-kcc:gcc432 \
  mipsel-linux-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -e __start -o mcui2c scripts/watchdog/mcui2c.c
docker run --rm -v "$PWD":/w -w /w nasne-kcc:gcc432 \
  mipsel-linux-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -e __start -o nasne-recpt1 scripts/tools/nasne_recpt1.c
docker run --rm -v "$PWD":/w -w /w nasne-kcc:gcc432 \
  mipsel-linux-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -e __start -o i2cx scripts/tools/i2cx.c

sudo cp mcui2c nasne-recpt1 i2cx /tmp/debian-root/usr/local/sbin/
sudo chmod +x /tmp/debian-root/usr/local/sbin/mcui2c /tmp/debian-root/usr/local/sbin/nasne-recpt1 /tmp/debian-root/usr/local/sbin/i2cx
```

`mtdtool`(SPI書き換え本体)はglibcの `syscall()` を使う動的リンクの小物なので、`nasne-kcc:gcc432`(nostdlibの古いクロスツールチェーン)ではなく、
**ホストのクロスコンパイラ**(Debian/Ubuntuの `gcc-mipsel-linux-gnu` パッケージ)でビルドする([docs/09](09_tools.md)):

```bash
sudo apt install gcc-mipsel-linux-gnu   # 未導入なら
mipsel-linux-gnu-gcc -O2 -nostartfiles -Wl,-e,_start -o mtdtool scripts/tools/mtdtool.c
file mtdtool   # "ELF 32-bit LSB executable, MIPS ... dynamically linked, interpreter /lib/ld.so.1" になっていることを確認

sudo cp mtdtool knl_new.bin /tmp/debian-root/usr/local/sbin/
sudo chmod +x /tmp/debian-root/usr/local/sbin/mtdtool
```

> 以前は公式rootfs(busybox環境)上で`mtdtool`を動かしていたが(動的リンクでも動作することは実機で確認済み)、
> この版では**Debian wheezy自体(`/lib/ld.so.1`・対応する`libc`を含む、通常のDebianインストール)の中で動かす**ので、
> 動的リンクの実行環境についてはむしろ以前より心配が少ない。

### 1-4. 起動時サービス(nasne-mcu-wd, nasne-recpt1-server)を仕込む

```bash
sudo cp scripts/watchdog/nasne-mcu-wd         /tmp/debian-root/etc/init.d/
sudo cp scripts/watchdog/nasne-recpt1-server  /tmp/debian-root/etc/init.d/
sudo chmod +x /tmp/debian-root/etc/init.d/nasne-mcu-wd /tmp/debian-root/etc/init.d/nasne-recpt1-server
```

`update-rc.d`(ランレベルごとの起動シンボリックリンク作成)はMIPSバイナリなので、`build_debian_rootfs.sh` と同じ要領で
`chroot` + `qemu-mipsel-static` を使う(同スクリプトが最後にqemuバイナリを消しているので、入れ直す)。
**これは2回目起動(本当のPID1での`switch_root`)のときに、Debian自身の通常のinitがこれらのサービスを自動起動するために必要**
(1回目起動の`chroot`はPID1ではないので、ここでは効かない。[手順2](#手順2-rcs-の末尾を-chroot--spi書き換えに差し替える)参照):

```bash
sudo cp "$(command -v qemu-mipsel-static)" /tmp/debian-root/usr/bin/
sudo mount -t proc  proc  /tmp/debian-root/proc
sudo mount -t sysfs sysfs /tmp/debian-root/sys
sudo chroot /tmp/debian-root update-rc.d nasne-mcu-wd defaults 05          # docs/06: ウォッチドッグは早めに止める
sudo chroot /tmp/debian-root update-rc.d nasne-recpt1-server defaults 20  # docs/11: ドライバ起動後でよい
sudo umount /tmp/debian-root/proc /tmp/debian-root/sys
sudo rm -f /tmp/debian-root/usr/bin/qemu-mipsel-static
```

> **`umount` が本当に効いたか必ず確認する**: `mount | grep debian-root` を実行して**何も表示されない**こと。chroot内の何かがまだ `/proc`/`/sys` を
> 使用中だと通常の `umount` が `busy` で失敗し、`/tmp/debian-root/proc` にホストの本物の `/proc` がマウントされたまま残ってしまう。
> この状態で後の手順3の `cp -a /tmp/debian-root/. /mnt/p3/` を実行すると、ホストの実行中プロセス情報(`/proc/1/task/1/mem` 等の特殊ファイル)を
> 誤ってコピーしようとして読み込みエラーになる。残っていたら `sudo umount -l /tmp/debian-root/proc /tmp/debian-root/sys`(lazy umount)で外すこと。

`/etc/rc2.d/`〜`/etc/rc5.d/`に2つのサービスへのシンボリックリンクができていることを確認:

```bash
ls /tmp/debian-root/etc/rc2.d/ | grep nasne
```

> **数字は `defaults 05`/`defaults 20` のとおりにはならない(実機で確認)**: wheezyの `update-rc.d` は `insserv` 経由でLSBヘッダ
> (`### BEGIN INIT INFO` の `Required-Start`)の依存関係から優先度を自動計算するため、指定した数字は実質ヒントに過ぎない。
> 実機では `S01nasne-mcu-wd` → `S02nasne-recpt1-server` になった。**大事なのは数字そのものではなく順序**で、
> `nasne-recpt1-server` のヘッダに `Required-Start: $local_fs $network nasne-mcu-wd` とある(`nasne-mcu-wd` の後と明記)ので、
> 必ず `nasne-mcu-wd` が先に実行される。`ls` の結果が「`nasne-mcu-wd` の番号 < `nasne-recpt1-server` の番号」になっていればOK。

### 1-5. 段階1スクリプト(SPI書き換え、chroot内で動く)を仕込む

**1回目起動時、`chroot`したDebian環境の中からSPIを書き換えるスクリプト**。公式rootfsのマーカーではなくSPIのKNLヘッダを読み戻して
「もう自作版か」を判定するので、何度起動しても安全(冪等)。sys1には一切触らない(mountもしない)ので、以前の版で心配していた
procmngによるHDDロールバックの対象にもならない。

```bash
sudo tee /tmp/debian-root/usr/local/sbin/nasne-stage1.sh >/dev/null <<'EOF'
#!/bin/sh
# 1回目の起動(公式カーネル、chroot内)でだけ、/dev/mtd0のKNLを自作カーネルへ書き換える。
# 「すでに自作版か」の判定は、SPIのKNLヘッダ(64バイト)を読み戻して、書き込み予定のヘッダと
# バイト比較するだけ。成功すると自身でrebootする(2回目起動は自作カーネルになり、このスクリプト自体は
# 二度と呼ばれない。switch_root後の完全なDebianからは、公式rootfsの.dlm自体がもう参照されないため)。
KNL=/usr/local/sbin/knl_new.bin
MTDTOOL=/usr/local/sbin/mtdtool
LOG=/var/log/nasne-stage1.log

log() { echo "$(date) $*" >> "$LOG"; }

: > "$LOG"
log "start (inside chroot)"

log "checking current KNL header against target (SPI readback)"
dd if=/dev/mtd0ro bs=64 count=1 skip=16384 of=/tmp/cur_knl_hdr.bin >> "$LOG" 2>&1   # 0x100000 / 64 = 16384
head -c 64 "$KNL" > /tmp/new_knl_hdr.bin
if cmp -s /tmp/cur_knl_hdr.bin /tmp/new_knl_hdr.bin; then
    log "KNL header already matches target, nothing to do"
    exit 0
fi
log "KNL header differs from target, proceeding to write"

log "erasing KNL"
$MTDTOOL erase /dev/mtd0 0x100000 0x280000 >> "$LOG" 2>&1
RC=$?
log "erase exit=$RC"
if [ "$RC" -ne 0 ]; then
    log "ERASE FAILED, aborting (not rebooting)"
    exit 1
fi

log "writing new kernel"
$MTDTOOL write /dev/mtd0 0x100000 "$KNL" >> "$LOG" 2>&1
RC=$?
log "write exit=$RC"
if [ "$RC" -ne 0 ]; then
    log "WRITE FAILED, aborting (not rebooting)"
    exit 1
fi

log "verifying"
SIZE=$(wc -c < "$KNL")
dd if=/dev/mtd0ro bs=65536 skip=16 count=40 of=/tmp/verify.bin >> "$LOG" 2>&1
head -c "$SIZE" /tmp/verify.bin > /tmp/verify_trim.bin
if ! cmp -s /tmp/verify_trim.bin "$KNL"; then
    log "VERIFY FAILED - SPIは書き込み済みの可能性あり(次回起動のA/Bフォールバックに期待)。rebootしない"
    exit 1
fi
log "verify OK, rebooting into new kernel"
sync
reboot -f
EOF
sudo chmod +x /tmp/debian-root/usr/local/sbin/nasne-stage1.sh
```

**検証に失敗した場合は `reboot` しない**(そのまま1回目起動の`chroot`状態で待機する。KNLの消去だけ終わって書き込みが不完全な場合は、
次の電源再投入で4段目ブートがCRC不一致を検出し、BKNL(Bスロット、純正のまま)へ自動フォールバックするはず。[docs/05](05_kernel_and_direct_boot.md)「A/Bの安全網」参照)。

## 手順2. `rcS` の末尾を `chroot` + SPI書き換えに差し替える

公式の `rcS`(手順0で確認した内容)の最後の行 `/opt/dtvtuner/etc/startdtvtuner` を削除し、代わりに
「p3(Debian)をmountしてchrootし、SPIを書き換える」処理を追記する。**procmngはここでは一切起動しない**(成功・失敗どちらの経路でも、
`startdtvtuner`を呼び直すことはしない。p3はこの手順で既にDebianに書き換わっている前提なので、公式機能に戻す意味がないため)。

```bash
cd official_rootfs
sed -i '/^\/opt\/dtvtuner\/etc\/startdtvtuner$/d' etc/init.d/rcS   # 末尾の呼び出し行だけを削除
cat >> etc/init.d/rcS <<'EOF'

# ---- 公式機能(procmng/dtvtunerアプリ)は使わない。p3(Debian)へchrootし、
#      そこでSPI(KNL)を自作カーネルに書き換えて最終的に完全なDebian(PID1)へ移行する ----
LOG=/tmp/nasne_debug.log
: > "$LOG"

mkdir -p /mnt/p3
if mount -t ext3 -o rw /dev/sda3 /mnt/p3 >> "$LOG" 2>&1; then
    mount -t proc  none /mnt/p3/proc       >> "$LOG" 2>&1
    mount -t sysfs none /mnt/p3/sys        >> "$LOG" 2>&1
    mount -o bind /dev     /mnt/p3/dev     >> "$LOG" 2>&1
    mount -o bind /dev/pts /mnt/p3/dev/pts >> "$LOG" 2>&1

    ifconfig eth0 up >> "$LOG" 2>&1
    udhcpc -i eth0 -t 5 -T 3 -A 3 -b -p /var/run/udhcpc.eth0.pid >> "$LOG" 2>&1
    cp /etc/resolv.conf /mnt/p3/etc/resolv.conf 2>/dev/null

    chroot /mnt/p3 /usr/sbin/sshd >> "$LOG" 2>&1
    echo "chroot sshd started (動作確認用。stage1がSPIを書き換えるまでの短い間だけログインできる)" >> "$LOG"

    chroot /mnt/p3 /usr/local/sbin/nasne-stage1.sh >> "$LOG" 2>&1
    echo "stage1 returned without rebooting (失敗。下のtelnetdで調査する)" >> "$LOG"
else
    echo "mount /dev/sda3 FAILED" >> "$LOG"
fi

telnetd -l /bin/sh &
EOF
cat etc/init.d/rcS   # 挿入結果を確認(構文を壊していないか)
cd ..
```

**挿入位置の根拠**: 元の`startdtvtuner`呼び出しと同じ位置(`devpts`マウント・`sysctl`・`lo`アップが全て終わった後)。
`mount -t devpts`が既に済んでいるので、`/mnt/p3/dev/pts`へのbindがそのまま使える。

> 成功時(`nasne-stage1.sh`がSPI書き換え→`reboot -f`まで進む)は、この`chroot`状態のまま再起動される。**procmngは一度も起動しない**。
> 失敗時(mount失敗・SPI書き換え失敗)は、`telnetd`(認証なし)だけが残る。この場合も`startdtvtuner`は呼ばない
> (p3は既にDebianなので、公式機能を動かしても意味がないどころか、以前見られたロールバックの再発リスクがあるため)。

tar.gzに固めて `.dlm` を作る:

```bash
sudo tar -C official_rootfs --numeric-owner -p -czf custom_rootfs.tar.gz .
python3 scripts/build_dlm.py selftest backup/sys1/00550066.dlm            # まずテンプレートの自己検証(OKになるはず)
python3 scripts/build_dlm.py build --template backup/sys1/00550066.dlm \
    --body custom_rootfs.tar.gz --out custom_00550066.dlm
python3 scripts/build_dlm.py verify custom_00550066.dlm                   # OKを確認
```

`--template` に**このHDD自身の** `00550066.dlm` を使うことで、hwtype・日付文字列がそのまま継承され、マネージャ(`00110022.dlm`)との日付整合チェックにも通る
([docs/03](03_firmware_format.md))。個体IDやマネージャ自体はまったく変更しない。

> ⚠️ 展開(`tar zxf`)に失敗すると(`/sbin/init` が無い、tar形式が壊れている、sys2(256MB)の空き容量不足など)、ヘッダ不正と同じ無限点滅ループに入り、
> **HDD側からは原因を切り分けられない**([docs/03](03_firmware_format.md))。事前に `tar tzf custom_rootfs.tar.gz | wc -l` でサイズ感を確認しておく。

## 手順3. HDDに書き込む(sys1とp3を同時に)

procmngを起動させない設計なので、sys1とp3は一度に両方書き込んでよい(以前の版のような2段階に分ける必要はない)。

```bash
sudo mount /dev/sdX1 /mnt/nasne_sys1
sudo cp custom_00550066.dlm /mnt/nasne_sys1/00550066.dlm
sync && sudo umount /mnt/nasne_sys1

sudo mkfs.ext3 -L debian /dev/sdX3
sudo mkdir -p /mnt/p3
sudo mount /dev/sdX3 /mnt/p3
sudo cp -a /tmp/debian-root/. /mnt/p3/
sync && sudo umount /mnt/p3
```

`00110022.dlm` や各バンクディレクトリ(`11002200/`、`33004400/`)は**一切変更しない**(マネージャは正規のものがそのまま使われる)。

## 手順4. 1回目の起動(公式カーネル、chroot+SPI書き換え)

HDDをnasneに挿して電源を入れる。SPI・カーネルはまだ公式のまま。`rcS`の最後でp3へのmount・chrootが走り、procmngの代わりに
[手順2](#手順2-rcs-の末尾を-chroot--spi書き換えに差し替える)の処理が動く。

動作確認用に一瞬だけsshdが起動するので、覗けるなら覗く(stage1が成功すると、これもすぐrebootで終わる):

```bash
ssh -o PubkeyAcceptedAlgorithms=+ssh-rsa -o HostKeyAlgorithms=+ssh-rsa root@<nasneのIP>
# ログインできたら(タイミング次第で間に合わないこともある):
tail -f /var/log/nasne-stage1.log
```

sshが間に合わなくても、`telnetd`(常時起動)経由で後から確認できる:

```bash
telnet <nasneのIP>
cat /tmp/nasne_debug.log      # outer(公式カーネル)側rcSのログ(mount/chrootの成否)
cat /var/log/nasne-stage1.log # chroot内でのSPI書き換えログ("verify OK, rebooting" まで進んでいること)
```

`verify OK, rebooting` まで進んでいれば、SPIは自作カーネルに書き換わっており、まもなく自動で再起動する。

## 手順5. 2回目の起動(自作カーネル → 本当のswitch_root)

リブート後、4段目ブートが新しいKNLを読み、自作カーネルが起動する。内蔵initramfsの `/nasne_init` が `/dev/sda3`(p3、Debian)を見つけ、
`/etc/nasne-direct-boot` があるので**本当に`switch_root`する**([docs/05](05_kernel_and_direct_boot.md)④)。ここでようやくPID1がDebianになり、
公式機能は完全に無くなる。

sshで入れることを確認:

```bash
ssh -o PubkeyAcceptedAlgorithms=+ssh-rsa -o HostKeyAlgorithms=+ssh-rsa root@<nasneのIP>
```

Debian起動後、サービスが正しく動いているか確認する([docs/06](06_mcu_watchdog.md)、[docs/11](11_tv_streaming.md)で事前にp3へ仕込んだ `update-rc.d` 済みのもの。
今回は本物のPID1 initが起動するので、通常のランレベル機構どおりに自動起動する):

```bash
tail -f /var/log/nasne-mcu-wd.log        # "MCU watchdog disabled" が出ること。これが無いと約333秒でリセットされる
ps aux | grep nasne-recpt1               # --listen 8301 が起動していること
cat /var/log/nasne-recpt1.log
```

333秒(約5.5分)以上落ちずに動いていれば、ウォッチドッグ停止は成功。

## 手順6. TVチューナーとして使う

地デジのアンテナ線とB-CASカードを挿した状態で電源を入れる(ソフト的な設定は不要。カードはnasne純正と同じスロットにそのまま挿す)。
手順1-4で `nasne-mcu-wd` サービスを `update-rc.d` 済みなので、Debian起動時に `rc.xcode4` が自動でドライバ(`xcode4drv.ko`)をロードする。

まずPCから選局確認([docs/08](08_tuner_i2c.md)。`nasne_fe.py` はsshでnasneに接続し、手順1-3で配置した `i2cx` を呼ぶ):

```bash
python3 scripts/nasne_fe.py --host <nasneのIP> t-init             # 地デジ側の初期化(初回/スタンバイ復帰後)
python3 scripts/nasne_fe.py --host <nasneのIP> t-scan 13 62       # ロックするチャンネルを一覧表示
```

視聴([docs/11](11_tv_streaming.md)。手順1-4で `nasne-recpt1-server` を自動起動済みなので、HTTPでTSが取れる):

```bash
curl http://<nasneのIP>:8301/scan             # サービスID一覧を取得(sid=0x...(tv))。約80秒かかる
curl http://<nasneのIP>:8301/tuner/27 | ffplay -               # UHF 27をそのまま再生
curl http://<nasneのIP>:8301/tuner/27?sid=0x0400 | ffplay -    # サービスを指定して再生
```

Mirakurunから使う場合は、別PCでMirakurunを動かし、[docs/11](11_tv_streaming.md)の `tuners.yml`(`command: curl -s http://<nasneのIP>:8301/tuner/<ch>`)・
`channels.yml`(サービスごとに `channel: '<ch>?sid=0x...'` を1エントリずつ、`/scan` で調べたサービスIDを使う)をそのまま設定すればよい(nasne側の追加作業は無い)。

## トラブルシュート

| 症状 | 考えられる原因 / 対処 |
|---|---|
| sshもtelnetも通らない、pingも通らない | mount・chrootより前(outer rcS)で止まっている可能性。`.dlm`の展開失敗(ヘッダ不正・CRC不一致)、または`sed`の行削除がファームのバージョンで一致していない(手順0で確認した`rcS`の最後の行が本当に`/opt/dtvtuner/etc/startdtvtuner`か再確認) |
| pingは通るがsshもtelnetも通らない | telnetdの起動自体が失敗している可能性(rcSの構文エラーで途中で止まった等)。HDDをPCに戻し`official_rootfs/etc/init.d/rcS`の構文を`sh -n`等で確認 |
| telnetは通るが`/tmp/nasne_debug.log`が無い/短い | outer rcSの追記ブロックまで到達していない。`sed`で削除したはずの行がまだ残っている、または追記位置がずれている(`cat etc/init.d/rcS`で最終確認したか) |
| `mount /dev/sda3 FAILED`がログに出る | p3のファイルシステムが壊れている、または`mkfs.ext3`がうまくいっていない。HDDをPCに戻し`fsck.ext3 /dev/sdX3`で確認 |
| `nasne-stage1.log`が無い/`chroot sshd started`の後で止まっている | `chroot /mnt/p3 /usr/local/sbin/nasne-stage1.sh`自体が失敗している可能性。`/mnt/p3/usr/local/sbin/nasne-stage1.sh`の実行権限(手順1-5)、シェバン(`#!/bin/sh`)を確認 |
| `nasne-stage1.log`に`mtdtool usage-check`や`erase`/`write`が無い、または非ゼロ終了 | `mtdtool`がDebian wheezy chroot内で動いていない。`file /mnt/p3/usr/local/sbin/mtdtool`で動的リンクMIPSバイナリになっているか、`/mnt/p3/lib/ld.so.1`の存在を確認 |
| `VERIFY FAILED`でrebootしない | SPIへの書き込み自体は(部分的に)発生している可能性がある。そのまま電源を入れ直すと、4段目ブートがCRC不一致を検出してBKNL(Bスロット、純正)へ自動フォールバックするはず([docs/05](05_kernel_and_direct_boot.md))。フォールバックしたら`knl_new.bin`のサイズ・ヘッダを見直し、`build_knl.py`からやり直す |
| CRCは正しいが起動途中で固まる(フォールバックが効かない) | [docs/05](05_kernel_and_direct_boot.md)の通り、CRC一致・内容不正の場合はA/Bのフォールバックが働かない。HDDをPCに戻し、`00550066.dlm`を手順0のバックアップに戻して公式ファームの`.dlm`のまま起動させ、SPIを元のダンプに書き戻す必要がある |
| 2回目の起動でDebianに届かない(起動カウンタが2でSony経路に戻る) | [docs/05](05_kernel_and_direct_boot.md)の通り。`rc.local`が走っていない、Debian側のp3の構成を確認 |
| 約333秒ごとに再起動する(2回目起動後) | ウォッチドッグ未停止。[docs/06](06_mcu_watchdog.md) |
| 元の公式ファームに戻したい | `00550066.dlm`を手順0のバックアップで上書き、p3もバックアップ(録画データ)から戻す。SPIのKNLも元のダンプ(または`ofw_out/KNL.bin`ベースで再構築したもの)に書き戻す |

## まだ詰めていないところ

- **1回目起動の`chroot`内から`nasne-stage1.sh`でSPIを書き換えて`reboot`する、という一連の流れは、このリポジトリの実機ではまだ通し確認していない**。
  SPI書き換え自体(`mtdtool`のerase/write/verify)は以前の版(公式rootfs上で直接実行)で成功実績があるが、場所をDebian chroot内に移したことによる
  差異(動的リンク環境・`/dev/mtd0`へのアクセス権限など)は未検証。
- procmngが本当に`startdtvtuner`経由でのみ起動するのか(他の経路で起動する可能性がないか)は未確認。もし別経路があれば、
  1回目起動の短い`chroot`状態でも、以前と同じロールバック問題に当たる可能性が残る。
- 1回目起動の`chroot`(PID1ではない)環境で`mtdtool`のようなSPI直接操作が安全に行えるか(`/dev/mtd0`のロック・他プロセスとの競合など)は未検証。

## 変更履行の経緯

以前のバージョンでは、公式rootfsの`rcS`から直接`mtdtool`を呼んでSPIを書き換え、`startdtvtuner`(procmngを含む公式TVアプリ機能)は
そのまま動かしたままにしていた。この場合、procmngが何らかのきっかけでHDD(sys1)の`00550066.dlm`を自動的にバンクの内容へ
ロールバックしてしまう現象に複数回遭遇し、トリガー条件を特定できなかった。

別の実績ある実装(Gitea上の開発リポジトリ)を確認したところ、SPIには触れず、公式カーネルのまま`rcS`の最後で`pivot_root`/`chroot`により
p3のDebianへ処理を渡す方式を採っていた。一時はこの「`chroot`したDebianを最終形にする」方式へ丸ごと切り替えたが、それでは
`chroot`された`/sbin/init`がPID1にならず、通常のサービス自動起動(`update-rc.d`/ランレベル機構)に頼れないという制約が残ってしまう。

最終的に、**両方の利点を組み合わせる**設計にした: `startdtvtuner`を呼ばないことで**procmngそのものを起動させない**(ロールバック対策)
一方で、**SPI書き換えという本来やりたかった作業自体は、`chroot`で入った本物のDebian環境(公式の激ミニマムなbusybox環境よりずっと
扱いやすい)の中から行う**。これが成功して`reboot`すれば、2回目の起動では自作カーネルの内蔵initramfsが`switch_root`を行い、
**公式機能を一切介さない、本当のPID1としてのDebian**に到達する。1回目起動の`chroot`はあくまで「SPIを書き換えるための踏み台」であって、
最終状態ではない。
