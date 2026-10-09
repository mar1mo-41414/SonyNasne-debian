# 13. 公式HDDの状態から、Debian(chroot)・ウォッチドッグ停止・TVチューナー化までの一連の手順

> 対象: **すでに公式ファーム(v2.60)で正常に動いているnasne(CECH-ZNR2J)**。HDDは壊れておらず、紐付けも正常な状態からスタートする。
> ([docs/04](04_hdd_recovery_guide.md) のHDD全損復旧とは別の話。復旧が必要な場合は先にそちらを終わらせてから、このドキュメントに進む。)
>
> **この版では、SPIフラッシュ(KNL=カーネル)は一切変更しない。公式カーネル・公式起動チェーンはそのまま使い、
> `/etc/init.d/rcS` の最後の1ステップ(公式TVアプリ機能=`startdtvtuner`の呼び出し)だけを、
> 「`/dev/sda3`(p3)にDebianを置いて`chroot`する」処理に差し替える。**
> SPIを書き換える前のバージョンでは、段階1スクリプトでSPIのKNLを自作カーネルに書き換えて2回目起動でDebianに
> `switch_root`する方式を試したが、**procmngが何らかのきっかけでHDD(sys1)の`00550066.dlm`を自動的にロールバックする**
> という未解明の問題にぶつかり続けた(詳細は[変更履行の経緯](#変更履行の経緯防げなかったspi書き換え方式との違い)参照)。
> この版は、別の実績ある実装(`pivot_root`/`chroot`でp3のDebianに処理を渡す方式)を参考に、**procmngの起動自体を
> そもそも避ける**設計に変えたもの。
>
> ⚠️ **トレードオフの自覚**: 公式のスマホ/PS4用nasneアプリ・録画機能は使えなくなる(procmng・dtvtunerアプリ層を
> 起動しなくなるため)。地デジチューナー自体は使えるが、視聴は本リポジトリの`nasne-recpt1`(HTTP配信)経由のみになる。
>
> ⚠️ 自己責任です。作業前に、このHDDの `00550066.dlm` を含むsys1の内容と、p3(録画データ含む)を必ずバックアップしてください。

## 検証状況(正直に)

| 内容 | 状態 |
|---|---|
| 公式rootfsの `.dlm` 注入による任意ファイル展開([docs/03](03_firmware_format.md)) | ✅ 実機で確認 |
| `dlm_crypto.py decrypt-body` で公式rootfs(v2.60、30MB超)を取り出し、`tar` で展開する | ✅ 実機で確認(`--tail cfb --inflate` が必要。[docs/03](03_firmware_format.md)参照) |
| 実機の公式rootfsの構成(`/sbin/init` は `busybox` への**シンボリックリンク**、`/etc/init.d/rcS` は実行可能シェルスクリプト) | ✅ 実機で確認。[docs/02](02_boot_chain.md)の推定通り |
| ウォッチドッグ停止([docs/06](06_mcu_watchdog.md))、地デジ受信・配信([docs/11](11_tv_streaming.md)) | ✅ 実機で確認(旧SPI書き換え方式での実績。スクリプト自体は同じものをそのまま使う) |
| `rcS` の末尾を `chroot`/`pivot` 処理に差し替え、procmngを起動させずにp3のDebianへ処理を渡す設計 | ⚠️ **本ドキュメントでの新方式。別の実績ある実装を参考にしたが、このリポジトリの実機ではまだ通し確認していない** |
| SPIのKNL差し替えでDebianへ直接`switch_root`する方式([docs/05](05_kernel_and_direct_boot.md)) | ⚠️ 旧方式。SPI書き換え自体・段階1スクリプトの実機動作は確認したが、procmngによるHDDロールバックが未解明のまま残り、この版では採用しない |

このドキュメントは「手動でここまでできるはず」という設計と手順のまとめです。差異が出た箇所は都度このファイルを更新してください。

## 全体の流れ

```
1. PCでHDDを準備する(1回だけ、sys1とp3を同時に)
   ├─ sys1 の 00550066.dlm を自作版に差し替える
   │    ヘッダ: そのHDD自身の 00550066.dlm をテンプレートにする(→ マネージャの日付検査に自動で通る)
   │    ボディ: 公式rootfs + 「rcSの末尾をchroot処理に差し替えたもの」(startdtvtunerは呼ばない=procmngを起動させない)
   └─ p3 を ext3 にして Debian (wheezy) を置く。nasne-recpt1 / ドライバ / nasne-mcu-wd もここに入れる

2. 起動(公式カーネルのまま。以後、毎回このルート)
   公式の起動チェーンがそのまま動き、rcSの最後で:
   ├─ eth0をDHCPで上げる
   ├─ /dev/sda3(p3=Debian)をmountし、proc/sys/devをbind
   └─ chrootで Debian 側の以下を順に起動:
        ウォッチドッグ停止 (nasne-mcu-wd start、ドライバxcode4drvのロードも兼ねる)
        nasne-recpt1 --listen (HTTP配信サーバ)
        sshd

3. curl http://<nasne>:8301/tuner/<ch> | ffplay -  で視聴
```

仕組みの元ネタ: `.dlm` とヘッダ検査は[docs/03](03_firmware_format.md)、ブートチェーンは[docs/02](02_boot_chain.md)、ウォッチドッグは[docs/06](06_mcu_watchdog.md)、TV視聴は[docs/11](11_tv_streaming.md)。
(SPIのKNL差し替えによる直起動方式は[docs/05](05_kernel_and_direct_boot.md)に残しているが、この手順では使わない。)

## 必要なもの

- Linux PC(Ubuntu等)、Python 3.8+、`mkfs.ext3`、Docker(静的バイナリのクロスビルド用)
- クロスコンパイラ環境 `nasne-kcc:gcc432`([docs/05](05_kernel_and_direct_boot.md)①の**Dockerイメージを作る部分だけ**。カーネル自体のビルドは不要)
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

公式ファームから、同じバージョンの「素の」rootfsセグメントも取り出す(これがボディの土台になる):

```bash
python3 scripts/ofw_tool.py split KRST3101_0260_SECURE.dlm ofw_out/   # ofw_out/DLM.bin = 00550066.dlmと同一(md5一致)
python3 scripts/dlm_crypto.py decrypt-body backup/sys1/00550066.dlm official_rootfs_raw.tar --tail cfb --inflate   # 公式rootfsのtar本体を取り出す
mkdir official_rootfs && sudo tar -C official_rootfs -xf official_rootfs_raw.tar    # 中身を確認。rootとして展開(権限・デバイスファイルの保持にsudoが必要)
```

> `decrypt-body` は30MB超のrootfsだとボディが8バイト境界に満たず、既定の `--tail cfb`(直前のCBCブロックを再暗号化したキーストリームで末尾をXORする)
> と `--inflate`(gzipトレーラのCRC検証をスキップして生のtarを取り出す)を付けないと `tar` が途中で壊れる。詳細は[docs/03](03_firmware_format.md)。

`official_rootfs/` の中身を見て、起動スクリプトの実際の構成を確認する。実機(v2.60)で確認した結果:
- `sbin/init` は **`busybox` へのシンボリックリンク**(`/sbin/init -> ../bin/busybox`)。このファイル自体は一切変更しない。
- `etc/init.d/rcS` は通常の実行可能シェルスクリプト。**末尾の `/opt/dtvtuner/etc/startdtvtuner` 呼び出しだけを差し替える**(下記手順2)。

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

## 手順1. Debianのrootfsを用意する(PC側)

[docs/05](05_kernel_and_direct_boot.md)①の**Dockerイメージを作る部分だけ**先に済ませておく(カーネルのビルドは不要):

```bash
docker build -t nasne-kcc:gcc432 scripts/kernel_build
```

Debianのrootfsを作る(未展開のまま、あとでp3へコピーする):

```bash
sudo SSH_PUBKEY=~/.ssh/id_rsa.pub ./scripts/build_debian_rootfs.sh /tmp/debian-root
```

これで `/tmp/debian-root` に、sshd・root用SSH鍵(RSA)までは設定済みのDebian wheezy (mipsel) ができる。
以下、このディレクトリに**直接**ファイルを置いていく(nasneにsshでログインできるのは起動後なので、`scp`/`ssh`は使わず、PC上のパスとして直接書き込む)。

### 1-1. ネットワーク設定

```bash
sudo tee /tmp/debian-root/etc/network/interfaces >/dev/null <<'EOF'
auto lo
iface lo inet loopback
EOF
```

> `eth0`はDHCPを含めてrcS側(outer、公式カーネルの方)で先に上げてしまうので、Debian側の`/etc/network/interfaces`に`eth0`の設定は不要
> ([手順2](#手順2-rcs-の末尾を-chroot-処理に差し替える)参照。chrootした先から見てもインターフェース自体は同じネットワーク名前空間で見えている)。

### 1-2. 公式ドライバ(xcode4drv.ko、rc.xcode4)を取り出す

手順0で取り出した `official_rootfs/` の中に、公式ドライバ一式が入っている([docs/06](06_mcu_watchdog.md)の前提)。

```bash
find official_rootfs/opt/dtvtuner -iname 'xcode4drv.ko' -o -iname 'rc.xcode4'   # 実際のパスを確認(バージョンにより多少違う可能性がある)
sudo mkdir -p /tmp/debian-root/usr/local/sbin
sudo cp official_rootfs/opt/dtvtuner/lib/modules/xcode4drv.ko /tmp/debian-root/usr/local/sbin/
sudo cp official_rootfs/opt/dtvtuner/lib/modules/rc.xcode4    /tmp/debian-root/usr/local/sbin/
sudo chmod +x /tmp/debian-root/usr/local/sbin/rc.xcode4
```

### 1-3. MIPS用バイナリをクロスビルドする(mcui2c, nasne-recpt1, i2cx)

[手順1](#手順1-debianのrootfsを用意するpc側)冒頭で作った `nasne-kcc:gcc432`(nostdlib・静的リンクのクロスコンパイラ)を使う。
`i2cx` は手順5([docs/08](08_tuner_i2c.md)のチューナー選局、`scripts/nasne_fe.py`がsshで呼ぶ)用に必要なので、ここで一緒にビルドしておく:

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

### 1-4. 起動スクリプト(nasne-mcu-wd, nasne-recpt1-server)を置く

```bash
sudo cp scripts/watchdog/nasne-mcu-wd         /tmp/debian-root/etc/init.d/
sudo cp scripts/watchdog/nasne-recpt1-server  /tmp/debian-root/etc/init.d/
sudo chmod +x /tmp/debian-root/etc/init.d/nasne-mcu-wd /tmp/debian-root/etc/init.d/nasne-recpt1-server
```

> **この版では `update-rc.d` は不要**(以前のSPI直起動方式では、Debianの通常のランレベル機構でこれらを自動起動させるために必要だった)。
> 今回は`chroot`した先のinitを使わず、[手順2](#手順2-rcs-の末尾を-chroot-処理に差し替える)で外側(公式カーネル)の`rcS`から
> `chroot ... /etc/init.d/nasne-mcu-wd start` のように**直接パスを指定して呼ぶ**ので、ランレベルの仕組みに頼らない。
> (`chroot`で起動した`/sbin/init`はPID1ではなく、ランレベル経由の自動起動が正しく機能しないことが実績ある別実装でも報告されているため、
> 最初から頼らない設計にしている。)
>
> `nasne-mcu-wd start` は内部で `rc.xcode4`(ドライバロード)も自動で呼ぶ([scripts/watchdog/nasne-mcu-wd](../scripts/watchdog/nasne-mcu-wd)参照)ので、
> 個別に`rc.xcode4`を呼ぶ必要もない。

B-CASカードは、この後の手順4(起動確認)でDebianが立ち上がった後、実機にそのまま挿しておけばよい(ソフト的な設定は不要)。

## 手順2. `rcS` の末尾を `chroot` 処理に差し替える

公式の `rcS`(手順0で確認した内容)の最後の行 `/opt/dtvtuner/etc/startdtvtuner` を削除し、代わりに
「p3(Debian)をmountしてchrootする」処理を追記する。**mountやchrootに失敗した場合だけ、フォールバックとして元の`startdtvtuner`を呼ぶ**
(procmngは正常系では起動しないが、Debian側に問題があった場合でも、nasne自体は公式ファームと同じように使える状態を保つ)。

```bash
cd official_rootfs
sed -i '/^\/opt\/dtvtuner\/etc\/startdtvtuner$/d' etc/init.d/rcS   # 末尾の呼び出し行だけを削除
cat >> etc/init.d/rcS <<'EOF'

# ---- Debian(p3)へchrootして必要なサービスだけ起動する ----
# procmng(公式TVアプリ機能)はここでは意図的に起動しない。
LOG=/tmp/nasne_debug.log
: > "$LOG"

ifconfig eth0 up >> "$LOG" 2>&1
udhcpc -i eth0 -t 5 -T 3 -A 3 -b -p /var/run/udhcpc.eth0.pid >> "$LOG" 2>&1

mkdir -p /mnt/p3
if mount -t ext3 -o rw /dev/sda3 /mnt/p3 >> "$LOG" 2>&1; then
    mount -t proc  none /mnt/p3/proc       >> "$LOG" 2>&1
    mount -t sysfs none /mnt/p3/sys        >> "$LOG" 2>&1
    mount -o bind /dev     /mnt/p3/dev     >> "$LOG" 2>&1
    mount -o bind /dev/pts /mnt/p3/dev/pts >> "$LOG" 2>&1
    cp /etc/resolv.conf /mnt/p3/etc/resolv.conf 2>/dev/null

    chroot /mnt/p3 /etc/init.d/nasne-mcu-wd start        >> "$LOG" 2>&1
    chroot /mnt/p3 /etc/init.d/nasne-recpt1-server start >> "$LOG" 2>&1
    chroot /mnt/p3 /usr/sbin/sshd                        >> "$LOG" 2>&1
    echo "chroot setup done" >> "$LOG"
else
    echo "mount /dev/sda3 FAILED, falling back to official dtvtuner" >> "$LOG"
    /opt/dtvtuner/etc/startdtvtuner
fi

# デバッグ用(常時起動。安定したらコメントアウトしてよい)
telnetd -l /bin/sh &
EOF
cat etc/init.d/rcS   # 挿入結果を確認(構文を壊していないか)
cd ..
```

**挿入位置の根拠**: この位置は元の`startdtvtuner`呼び出しと同じ(`devpts`マウント・`sysctl`・`lo`アップ・公式ドライバ関連の処理が全て終わった後)。
`mount -t devpts`が既に済んでいるので、`/mnt/p3/dev/pts`へのbindがそのまま使える。

> `nasne-recpt1-server start` は内部で `/dev/vixs/xcodedrv` の出現を最大60秒待つ([scripts/watchdog/nasne-recpt1-server](../scripts/watchdog/nasne-recpt1-server)参照)。
> これは直前の`nasne-mcu-wd start`がバックグラウンドで`rc.xcode4`を読み込んでいる最中でも、順番に間に合うようにするため。

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

procmngを起動させない設計なので、以前のようにp3の書き込みタイミングを分ける必要はない。一度に両方書き込む。

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

## 手順4. 起動確認

HDDをnasneに挿して電源を入れる。SPI・カーネルは完全に無改造の公式のままなので、**起動シーケンス自体(LEDの挙動含む)は基本的に公式ファームと同じになるはず**。
rcSの最後でp3へのmount・chrootが走り、procmngの代わりに以下が立ち上がる。

```bash
ssh -o PubkeyAcceptedAlgorithms=+ssh-rsa -o HostKeyAlgorithms=+ssh-rsa root@<nasneのIP>
```

sshが通らない場合は、常時起動にしてある認証なしtelnetdで確認する:

```bash
telnet <nasneのIP>
cat /tmp/nasne_debug.log   # outer(公式カーネル)側のrcSが書いたログ。mount/chrootが成功したか確認
```

Debian側(chroot先)に入れたら、サービスの状態を確認する:

```bash
tail -f /var/log/nasne-mcu-wd.log        # "MCU watchdog disabled" が出ること。これが無いと約333秒でリセットされる
ps aux | grep nasne-recpt1               # --listen 8301 が起動していること
cat /var/log/nasne-recpt1.log
```

333秒(約5.5分)以上落ちずに動いていれば、ウォッチドッグ停止は成功。

## 手順5. TVチューナーとして使う

地デジのアンテナ線とB-CASカードを挿した状態で電源を入れる(ソフト的な設定は不要。カードはnasne純正と同じスロットにそのまま挿す)。
手順2の`rcS`末尾で`nasne-mcu-wd start`が走ると、内部で`rc.xcode4`が自動でドライバ(`xcode4drv.ko`)をロードする。

まずPCから選局確認([docs/08](08_tuner_i2c.md)。`nasne_fe.py` はsshでnasneに接続し、手順1-3で配置した `i2cx` を呼ぶ):

```bash
python3 scripts/nasne_fe.py --host <nasneのIP> t-init             # 地デジ側の初期化(初回/スタンバイ復帰後)
python3 scripts/nasne_fe.py --host <nasneのIP> t-scan 13 62       # ロックするチャンネルを一覧表示
```

視聴([docs/11](11_tv_streaming.md)。手順1-4で設置した `nasne-recpt1-server` を`rcS`から直接起動済みなので、HTTPでTSが取れる):

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
| `mount /dev/sda3 FAILED`がログに出る | p3のファイルシステムが壊れている、または`mkfs.ext3`がうまくいっていない。HDDをPCに戻し`fsck.ext3 /dev/sdX3`で確認。この場合でもフォールバックで公式`startdtvtuner`が動くので、nasne自体は公式ファームと同じように使えるはず |
| chrootまでは成功したが`nasne-mcu-wd`/`nasne-recpt1-server`のログが無い | `chroot /mnt/p3 /etc/init.d/nasne-mcu-wd start`自体が失敗している可能性。`/mnt/p3/etc/init.d/nasne-mcu-wd`の実行権限(`chmod +x`、手順1-4)、`/mnt/p3/usr/local/sbin/`配下のバイナリがMIPS用に正しくクロスビルドされているか(`file`で確認)を見直す |
| sshdに繋がらない(`chroot .../usr/sbin/sshd`はログ上成功している) | ホストキーの生成タイミング(`build_debian_rootfs.sh`実行時に生成されているはず)、または古いOpenSSHクライアント互換オプション不足。`ssh -o PubkeyAcceptedAlgorithms=+ssh-rsa -o HostKeyAlgorithms=+ssh-rsa`を付けているか確認 |
| ウォッチドッグが止まらない(約333秒ごとに再起動する) | [docs/06](06_mcu_watchdog.md)。`nasne-mcu-wd.log`に`MCU watchdog disabled`が出ているか確認。出ていなければ`rc.xcode4`によるドライバロード自体が失敗している(`lsmod`でxcode4drvの有無を確認) |
| 公式のTVアプリ(スマホ/PS4)で見えてしまう/procmngが動いているように見える | `rcS`の`sed`による行削除が効いていない(元の`startdtvtuner`呼び出しがまだ残っている)。`cat etc/init.d/rcS`で最終行付近を再確認 |
| 元の公式ファームに戻したい | `00550066.dlm`を手順0のバックアップで上書き、p3もバックアップ(録画データ)から戻す。SPI・カーネルは一切変更していないので、これだけで完全に元の状態に戻る |

## まだ詰めていないところ

- **この版の`rcS`差し替え〜chroot起動の一連の流れは、このリポジトリの実機ではまだ通し確認していない**。参考にした別実装
  (`pivot_root`方式)では、「`chroot`で起動した`/sbin/init`はPID1ではないため、ランレベル経由でのサービス自動起動に頼れない」
  という制限が報告されている。本ドキュメントは最初からその制限を踏まえ、`update-rc.d`を使わず`rcS`から各サービスを直接パスで
  呼ぶ設計にしているが、この設計自体の実機検証はまだ。
- `chroot`先で動く`sshd`・`nasne-recpt1`が、**PID1(本来のinit)の子孫ではない**ことによる副作用(ゾンビプロセスの回収、
  シグナル伝播、電源断時の挙動など)は未検証。長時間の安定動作(ウォッチドッグ停止が333秒を超えて持続するか)は要確認。
- procmngが本当に`startdtvtuner`経由でのみ起動するのか(他の経路で起動する可能性がないか)は未確認。もし別経路があれば、
  今回の設計でも旧版と同じロールバック問題に当たる可能性が残る。

## 変更履行の経緯(旧SPI書き換え方式との違い)

以前のバージョンでは、1回目の起動(公式カーネル)で段階1スクリプトがSPIのKNLを自作カーネル(内蔵initramfs入り)に書き換え、
2回目の起動でDebianへ`switch_root`する方式([docs/05](05_kernel_and_direct_boot.md)ベース)を実機で検証していた。
SPI書き換え自体(`mtdtool`でのerase/write/verify)は実機で動作したが、**procmngが何らかのきっかけでHDD(sys1)の
`00550066.dlm`を自動的にバンクの内容へロールバックしてしまう**現象に複数回遭遇し、トリガー条件を特定できなかった。

別の実績ある実装(Gitea上の開発リポジトリ)を確認したところ、いずれも**SPIには触れず、公式カーネルのまま`rcS`の最後で
`pivot_root`/`chroot`によりp3のDebianへ処理を渡す**方式を採っていた。この方式では、`rcS`中で公式の`startdtvtuner`
(procmngを含む公式TVアプリ機能一式の起点と推測される)を呼ばないため、**procmngそのものが起動しない**。
これにより、ロールバック現象の引き金(詳細不明だが、procmngが何かを検出して起きていた可能性が高い)を構造的に避けられる
と考え、このドキュメントの設計を全面的にこちらへ切り替えた。
