# 13. 公式HDDの状態から、Debian直起動・ウォッチドッグ停止・TVチューナー化までの一連の手順

> 対象: **すでに公式ファーム(v2.60)で正常に動いているnasne(CECH-ZNR2J)**。HDDは壊れておらず、紐付けも正常な状態からスタートする。
> ([docs/04](04_hdd_recovery_guide.md) のHDD全損復旧とは別の話。復旧が必要な場合は先にそちらを終わらせてから、このドキュメントに進む。)
>
> **SPIフラッシュ(KNL)の書き換えは、CH341A等の外部SPIライタを使わず、nasne自身の上で(1回目の起動時に公式カーネル上で動く「段階1スクリプト」から)実行する。**
> 作業はすべて「HDDをPCに挿してファイルを用意する」→「nasneで2回起動する」だけで完結する。
>
> ⚠️ 自己責任です。SPIフラッシュの内容を壊すと起動しなくなる可能性があります(その場合の回復手段は[トラブルシュート](#トラブルシュート)参照)。
> **作業前に、このHDDの `00550066.dlm` を含むsys1の内容を必ずバックアップしてください**(このファイル自身が、以降の手順でテンプレートとして必要になります)。

## 検証状況(正直に)

| 内容 | 状態 |
|---|---|
| SPIのKNL差し替え → p3のDebianへ直起動([docs/05](05_kernel_and_direct_boot.md)) | ✅ 実機で確認(CH341AでSPIを書いた場合) |
| ウォッチドッグ停止([docs/06](06_mcu_watchdog.md))、地デジ受信・配信([docs/11](11_tv_streaming.md)) | ✅ 実機で確認 |
| 公式rootfsの `.dlm` 注入による任意ファイル展開([docs/03](03_firmware_format.md)) | ✅ 実機で確認(Debian chroot起動の初期実装で使用) |
| **本ドキュメントの「段階1スクリプトからの `/dev/mtd0` 書き込み」**(公式カーネル・公式rootfs上でのSPI書き換え) | ⚠️ **未検証**。`mtdtool` はDebian上(自前カーネル)での動作は確認済みだが、公式miniroot上のuserlandで動くかは別問題(下記参照) |
| `dlm_crypto.py decrypt-body` で公式rootfs(v2.60、30MB超)を取り出し、`tar` で展開する | ✅ 実機で確認(`--tail cfb --inflate` が必要。[docs/03](03_firmware_format.md)参照。末尾8バイト未満の処理に実装上のバグがあり、本ドキュメント執筆時点では未解決だった) |
| 実機の公式rootfsの構成(`/sbin/init` は `busybox` への**シンボリックリンク**、`/etc/init.d/rcS` は実行可能シェルスクリプト) | ✅ 実機で確認。[docs/02](02_boot_chain.md)の推定通りだが、`/sbin/init`がシンボリックリンクだったため、段階1の仕込みは**rcS方式**(下記)に変更した |
| 段階1スクリプトを `rcS` の先頭に差し込む設計 | ⚠️ 未検証(`/sbin/init`がbusyboxへのシンボリックリンクと確認できたため、initラッパー化は不採用に変更。rcSの実際の中身を見ながら調整が必要) |

このドキュメントは「手動でここまでできるはず」という設計と手順のまとめです。実機での通し確認、スクリプトの自動化はこれから。差異が出た箇所は都度このファイルを更新してください。

## 全体の流れ

```
1. PCでHDDを準備する
   ├─ sys1 の 00550066.dlm を自作版に差し替える
   │    ヘッダ: そのHDD自身の 00550066.dlm をテンプレートにする(→ マネージャの日付検査に自動で通る)
   │    ボディ: 公式rootfs + 「1回目だけ動く段階1スクリプト」
   ├─ sys1 に、書き込み先の新カーネル(KNLセグメント)も置く
   └─ p3 を ext3 にして Debian (wheezy) を置く。nasne-recpt1 / ドライバ / nasne-mcu-wd もここに入れる

2. 1回目の起動(まだ公式カーネル)
   公式の miniroot init が自作 .dlm を展開し、段階1スクリプトが動く:
   ├─ /dev/mtd0 の KNL だけを自作カーネル(内蔵initramfs入り)に書き換え、読み戻して確認する
   ├─ sys1に完了印を書く(2回目以降は何もしない)
   └─ reboot

3. 2回目の起動(自作カーネル)
   自作カーネルの /nasne_init が p3 の Debian へ switch_root する。Debian側のサービスが:
   ├─ ウォッチドッグ停止 (nasne-mcu-wd)
   ├─ rc.xcode4 でチューナードライバ読み込み
   └─ nasne-recpt1 --listen (HTTP配信サーバ)

4. curl http://<nasne>:8301/tuner/<ch> | ffplay -  で視聴
```

仕組みの元ネタ: `.dlm` とヘッダ検査は[docs/03](03_firmware_format.md)、ブートチェーンは[docs/02](02_boot_chain.md)、カーネル差し替えと直起動は[docs/05](05_kernel_and_direct_boot.md)、ウォッチドッグは[docs/06](06_mcu_watchdog.md)、TV視聴は[docs/11](11_tv_streaming.md)。

## 必要なもの

- Linux PC(Ubuntu等)、Python 3.8+、`sfdisk` `mkfs.ext3`、Docker(カーネル/ツールのクロスビルド用)
- クロスコンパイラ環境 `nasne-kcc:gcc432`([docs/05](05_kernel_and_direct_boot.md)①で作る) と `gcc-mipsel-linux-gnu`(mtdtool等用)
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

公式ファームから、同じバージョンの「素の」rootfsセグメントも取り出しておく(後で中身を比較・展開するため):

```bash
python3 scripts/ofw_tool.py split KRST3101_0260_SECURE.dlm ofw_out/   # ofw_out/DLM.bin = 00550066.dlmと同一(md5一致)
python3 scripts/dlm_crypto.py decrypt-body backup/sys1/00550066.dlm official_rootfs_raw.tar --tail cfb --inflate   # 公式rootfsのtar本体を取り出す
mkdir official_rootfs && sudo tar -C official_rootfs -xf official_rootfs_raw.tar    # 中身を確認。rootとして展開(権限・デバイスファイルの保持にsudoが必要)
```

> `decrypt-body` は30MB超のrootfsだとボディが8バイト境界に満たず、既定の `--tail cfb`(直前のCBCブロックを再暗号化したキーストリームで末尾をXORする)
> と `--inflate`(gzipトレーラのCRC検証をスキップして生のtarを取り出す)を付けないと `tar` が途中で壊れる。詳細は[docs/03](03_firmware_format.md)。

`official_rootfs/` の中身を見て、起動スクリプトの実際の構成を確認する。実機(v2.60)で確認した結果:
- `sbin/init` は **`busybox` へのシンボリックリンク**(`/sbin/init -> ../bin/busybox`)。[docs/02](02_boot_chain.md)の推定通りだが、
  「`/sbin/init` を直接リネームしてラップする」手は使えない(シンボリックリンクなので、`mv` するとリンクが消えるだけでbusybox本体には影響しないが、
  ラップの意味がなくなる)。
- `etc/init.d/rcS` は通常の実行可能シェルスクリプト。段階1の呼び出しは、**このファイルの先頭に1行追加する**方式にする(下記)。

必ず `ls -la official_rootfs/sbin/init` と `cat official_rootfs/etc/init.d/rcS` で、自分のファームのバージョンでも同じ構成か確認してから次に進む。

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

```bash
python3 scripts/build_knl.py info   ofw_out/KNL.bin --slot KNL
python3 scripts/build_knl.py build  ofw_out/KNL.bin knl_new.bin --slot KNL --kernel gpl_src/build/out/vmlinux_new.bin
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

p3に置く設定は[docs/05](05_kernel_and_direct_boot.md)③の表の通り(`/etc/nasne-direct-boot`、`/etc/rc.local` で起動カウンタを空にする、`/etc/network/interfaces` など)に加えて、
[docs/06](06_mcu_watchdog.md)のウォッチドッグ停止サービス、[docs/11](11_tv_streaming.md)の `nasne-recpt1-server` を `/tmp/debian-root` の中に仕込んでおく(MIPS用バイナリのビルド方法は各ドキュメント参照)。

## 手順2. 「段階1スクリプト」を作り、公式rootfsに足す

### 2-1. nasne上で動くSPI書き込みツールを用意する

[docs/09](09_tools.md)の手順で、Docker (`nasne-kcc:gcc432`) で `mtdtool` をビルドする:

```bash
mipsel-linux-gnu-gcc -O2 -nostartfiles -Wl,-e,_start -o mtdtool scripts/tools/mtdtool.c
```

> ⚠️ **未検証ポイント**: `mtdtool` は `/lib/ld.so.1` を要求する動的リンクバイナリ([docs/09](09_tools.md))。
> Debian上では動作確認済みだが、**公式miniroot/sys2のuserlandにMIPS用glibcの動的リンカ(`/lib/ld.so.1`)と対応する `libc.so` があるか**は未確認。
> 無ければ `mtdtool` はそのまま動かない。切り分け方法は[トラブルシュート](#トラブルシュート)。代替として、`scripts/tools/*.c` と同じ作法(`-nostdlib -static`、素のsyscall)で
> MTD書き込み専用の小さなツールを書き直せば、この依存を無くせる(未実装)。

### 2-2. 段階1スクリプト本体

sys1(段階1の作業領域としても使う。sys2は毎起動 `rm -rf` されるため永続化できない)に完了マーカーを置く設計にする。

`nasne-stage1.sh`(公式rootfsの `/sbin/` に置く):

```sh
#!/bin/sh
# 1回目の起動(公式カーネル)でだけ、/dev/mtd0のKNLを自作カーネルへ書き換える。
# sys1(/dev/sda1)に完了マーカーを置いて、2回目以降は何もしない(フォールバックで純正initが再度動いた場合の保険)。
set -e
MNT=/tmp/disk0
MARKER=$MNT/.stage1_done
KNL=$MNT/knl_new.bin

mkdir -p $MNT
mount -t ext3 -o rw /dev/sda1 $MNT

if [ -e "$MARKER" ]; then
    umount $MNT
    exit 0
fi

echo "stage1: erasing KNL" 
/sbin/mtdtool erase /dev/mtd0 0x100000 0x280000

echo "stage1: writing new kernel"
/sbin/mtdtool write /dev/mtd0 0x100000 "$KNL"

echo "stage1: verifying"
SIZE=$(wc -c < "$KNL")
dd if=/dev/mtd0ro bs=65536 skip=16 count=40 of=/tmp/verify.bin 2>/dev/null
head -c "$SIZE" /tmp/verify.bin > /tmp/verify_trim.bin
if ! cmp -s /tmp/verify_trim.bin "$KNL"; then
    echo "stage1: VERIFY FAILED - not rebooting, SPI left untouched by reboot" 
    umount $MNT
    exit 1
fi

echo "stage1: OK, marking done and rebooting"
touch "$MARKER"
sync
umount $MNT
sync
reboot -f
```

**検証に失敗した場合は `reboot` しない**(そのまま公式ファームとして起動を続ける。KNLの消去だけ終わって書き込みが不完全な場合は、
次の電源再投入で4段目ブートがCRC不一致を検出し、BKNL(Bスロット、純正のまま)へ自動フォールバックするはず。[docs/05](05_kernel_and_direct_boot.md)「A/Bの安全網」参照)。

### 2-3. `rcS` の先頭に段階1呼び出しを差し込む

実機(v2.60)で確認した結果、`/sbin/init` は `busybox` への**シンボリックリンク**だった。この場合、「`/sbin/init` を `mv` して独自スクリプトに置き換える」
手は使えない(シンボリックリンクの実体は `busybox` 1つなので、`init` という名前のリンクをどかしても `busybox` 自体は無事だが、
`/sbin/init` という経路そのものが無くなり、`switch_root` の2番目の引数 `sbin/init` が解決できなくなる)。

そこで、busybox initの起動シーケンス自体には触らず、**`/etc/init.d/rcS`(busybox initが`/etc/inittab`経由で呼ぶ、通常の実行可能シェルスクリプト)に
段階1呼び出しを1行追加する**方式にする。実機(v2.60)の `rcS` の中身:

```sh
#!/bin/sh
trap "" SIGHUP
mount -t proc none /proc        # ← これより前だと /proc が無く mtdtool が動かない
mount -t usbfs none /proc/bus/usb
mount -t sysfs none /sys
/usr/bin/uevent_daemon
start_udev                      # ← udevのcoldplugが終わるのを待ってから /dev/sda1 等を使う
mount -t devpts none /dev/pts
sysctl -p
ifconfig lo 127.0.0.1 netmask 255.0.0.0 up
route add -net 127.0.0.0 netmask 255.0.0.0 dev lo
# ---- for DTVTuner ----
/opt/dtvtuner/etc/startdtvtuner
```

**挿入位置は `start_udev` の直後**にする。`mtdtool` は標準スタートアップを使わず `/proc/self/cmdline` から引数を取る実装([docs/09](09_tools.md))なので、
`mount -t proc` より前では動かない。また `/dev/sda1`・`/dev/mtd0` のデバイスノードも、`start_udev`(coldplugでの初回デバイススキャン)が終わっていないと
存在しない可能性がある。ドライバ関連の `startdtvtuner` より前なので、SPI書き換え中にドライバと競合する心配もない。

```bash
cd official_rootfs
cat etc/init.d/rcS   # 自分のファームでも同じ構成か必ず確認する(行の前後関係が違えば挿入位置も変える)
sudo sed -i '/^start_udev$/a /sbin/nasne-stage1.sh || true' etc/init.d/rcS
sudo install -m 0755 <path>/nasne-stage1.sh sbin/nasne-stage1.sh
sudo install -m 0755 <path>/mtdtool          sbin/mtdtool
cat etc/init.d/rcS   # 挿入結果を確認(構文を壊していないか)
```

`nasne-stage1.sh` 自体は[2-2](#2-2-段階1スクリプト本体)のまま(sys1の完了マーカーで1回だけ実行される設計)で変更不要。

> `mtdtool` は動的リンクバイナリ(`/lib/ld.so.1`要求)で、公式rootfs上で動くかは[2-1](#2-1-nasne上で動くspi書き込みツールを用意する)の時点では未検証だったが、
> 実機の `tar -tvf` の一覧に `bin/busybox_dynamic`(動的リンク版busybox)があったため、この公式rootfsには動的リンクの実行環境(`/lib/ld.so.1`等)が
> 既に含まれている可能性が高い。`ls -la official_rootfs/lib/ld.so.1 official_rootfs/dev/sda* official_rootfs/dev/mtd*` で確認すること。

### 2-4. tar.gzに固めて `.dlm` を作る

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
> **HDD側からは原因を切り分けられない**([docs/03](03_firmware_format.md))。`sys2` は1GBだが、公式rootfsのサイズ次第では余裕が少ないことがあるので、
> 事前に `tar tzf custom_rootfs.tar.gz | wc -l` や展開サイズを確認しておく。

## 手順3. HDDに書き込む

```bash
sudo mount /dev/sdX1 /mnt/nasne_sys1
sudo cp custom_00550066.dlm /mnt/nasne_sys1/00550066.dlm
sudo cp knl_new.bin        /mnt/nasne_sys1/knl_new.bin
sync && sudo umount /mnt/nasne_sys1

# p3をDebianに置き換える(録画データは消える)
sudo mkfs.ext3 -L debian /dev/sdX3
sudo mount /dev/sdX3 /mnt/p3 && sudo cp -a /tmp/debian-root/. /mnt/p3/
sync && sudo umount /mnt/p3
```

`00110022.dlm` や各バンクディレクトリ(`11002200/`、`33004400/`)は**一切変更しない**(マネージャは正規のものがそのまま使われる)。

## 手順4. 1回目の起動(公式カーネルのまま)

HDDをnasneに挿して電源を入れる。

1. 公式のminiroot `/init` が `00110022.dlm`・`00550066.dlm` を検証(ヘッダのhwtype・日付・CRCはテンプレート継承なので通る)。
2. ボディを `/rfs` に展開し、ラップした `/sbin/init` へ `switch_root`。
3. `nasne-stage1.sh` が走り、`/dev/mtd0` のKNLを `knl_new.bin` に書き換え、読み戻して確認する。
4. OKなら `reboot`。NGなら公式ファームのまま起動を続ける(ログや挙動から原因を確認する。[トラブルシュート](#トラブルシュート))。

このときの所要時間・LEDの挙動は未検証。**最初は電源を入れたまま数分待ち、`ping` が通るか、PCに繋いだ状態でSPIが実際に書き変わったかを確認する**のが安全
(可能であれば、この段階でもCH341Aでの読み出し確認を併用して様子を見るとよい)。

## 手順5. 2回目の起動(自作カーネル → Debian直起動)

リブート後、4段目ブートが新しいKNLを読み、自作カーネルが起動する。内蔵initramfsの `/nasne_init` が `/dev/sda3`(p3、Debian)を見つけ、
`/etc/nasne-direct-boot` があるので `switch_root` する([docs/05](05_kernel_and_direct_boot.md)④)。

sshで入れることを確認:

```bash
ssh -o PubkeyAcceptedAlgorithms=+ssh-rsa -o HostKeyAlgorithms=+ssh-rsa root@<nasneのIP>
```

Debian起動後、サービスが正しく動いているか確認する([docs/06](06_mcu_watchdog.md)、[docs/11](11_tv_streaming.md)で事前にp3へ仕込んだ `update-rc.d` 済みのもの):

```bash
tail -f /var/log/nasne-mcu-wd.log        # "MCU watchdog disabled" が出ること。これが無いと約333秒でリセットされる
ps aux | grep nasne-recpt1               # --listen 8301 が起動していること
cat /var/log/nasne-recpt1.log
```

333秒(約5.5分)以上落ちずに動いていれば、ウォッチドッグ停止は成功。

## 手順6. TVチューナーとして使う

[docs/08](08_tuner_i2c.md)・[docs/11](11_tv_streaming.md)の通り。まずはPCから選局確認:

```bash
python3 scripts/nasne_fe.py --host <nasneのIP> t-scan 13 62    # ロックするチャンネルを確認
```

視聴:

```bash
curl http://<nasneのIP>:8301/tuner/27 | ffplay -
curl -s http://<nasneのIP>:8301/scan      # サービスID一覧(sid=0x...)
```

Mirakurunからの利用は[docs/11](11_tv_streaming.md)の `tuners.yml` / `channels.yml` の設定例を参照。

## トラブルシュート

| 症状 | 考えられる原因 / 対処 |
|---|---|
| 1回目の起動後、いつまでもLEDが点滅したまま・SPIが書き変わらない | `.dlm` の展開失敗(ヘッダ不正・CRC不一致・容量不足)、または `/sbin/init` ラッパーまで届いていない。HDDをPCに戻し、`build_dlm.py verify` と `tar tzf` でサイズ確認 |
| 段階1スクリプトが動いたログが無い(sys1に `.stage1_done` が無い) | `mtdtool` が動かなかった可能性(`/lib/ld.so.1` 不在など)。公式rootfs上で `ldd` 相当の確認ができないため、代わりに段階1スクリプトの各行に `echo ... > /tmp/stage1.log` を追加し、sys1へコピーするようにして原因を特定する |
| 書き込み途中で電源が落ちた・`reboot` 後も公式ファームのまま | 4段目ブートがKNLのCRC不一致を検出し、BKNL(Bスロット)へフォールバックした可能性。これは安全に働いた証拠。原因(段階1スクリプトの `allowed()` 範囲・サイズ計算)を見直し、やり直す |
| CRCは正しいが起動途中で固まる(フォールバックが効かない) | [docs/05](05_kernel_and_direct_boot.md)の通り、CRC一致・内容不正の場合はA/Bのフォールバックが働かない。HDDをPCに戻し、`00550066.dlm` を手順0のバックアップに戻して公式ファームの `.dlm` のまま起動させ、SPIを元のダンプ(`backup`時に未取得なら、別途CH341Aで読み出した正常なKNL)に書き戻す必要がある |
| 2回目の起動でDebianに届かない(起動カウンタが2でSony経路に戻る) | [docs/05](05_kernel_and_direct_boot.md)の通り。`rc.local` が走っていない、Debian側のp3の構成を確認 |
| 約333秒ごとに再起動する | ウォッチドッグ未停止。[docs/06](06_mcu_watchdog.md) |
| 元の公式ファームに戻したい | `00550066.dlm` を手順0のバックアップで上書き、SPIのKNLも元のダンプ(または `ofw_out/KNL.bin` ベースで再構築したもの)に書き戻す |

## まだ詰めていないところ

- `mtdtool` を公式rootfs上でそのまま動かせるか(動的リンカの有無)。動かない場合は nostdlib 静的バイナリで書き直す。
- `/sbin/init` ラッパー方式が実際の公式rootfsの構成(busybox multi-call、inittabの参照形式)と噛み合うか。
- 段階1スクリプトの各ステップのログを、2回目の起動後(Debian側)からも読める場所(sys1など)に残す仕組み。
- 1回目の起動から段階1完了・rebootまでの所要時間、途中のLED挙動。
