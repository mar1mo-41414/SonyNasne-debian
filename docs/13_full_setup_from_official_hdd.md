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
| 段階1スクリプトからの `/dev/mtd0` 書き込み(公式rootfs上での `mtdtool` 実行、SPI書き換え) | ✅ 実機で確認。`mtdtool`(動的リンクバイナリ)は公式rootfs上で実際に動いた。erase→write→verify OK→reboot まで1回成功した |
| `rcS` の先頭(`start_udev` の直後)に段階1呼び出しを差し込む設計 | ✅ 実機で確認。少なくとも1回は正しく実行された |
| `dlm_crypto.py decrypt-body` で公式rootfs(v2.60、30MB超)を取り出し、`tar` で展開する | ✅ 実機で確認(`--tail cfb --inflate` が必要。[docs/03](03_firmware_format.md)参照) |
| 実機の公式rootfsの構成(`/sbin/init` は `busybox` への**シンボリックリンク**、`/etc/init.d/rcS` は実行可能シェルスクリプト) | ✅ 実機で確認。[docs/02](02_boot_chain.md)の推定通り |
| **段階1成功 → 2回目起動(自作カーネル) → Debianへの`switch_root`の通し** | ⚠️ 1回はswitch_rootまで到達した形跡があるが、その後安定して動いていない。原因は下記の「procmngによるHDDロールバック」の可能性が高い |
| **procmngが、HDD(sys1)の`00550066.dlm`をバンクの内容へ自動的にロールバックする現象** | ⚠️ **重大な未解明の問題。実機で複数回確認**。[まだ詰めていないところ](#まだ詰めていないところ)・[トラブルシュート](#トラブルシュート)参照。段階1スクリプトの判定方式を、この現象に影響されないよう「HDDマーカー」から「SPIのKNL読み戻し」に変更済み(根本原因への対策ではない) |

このドキュメントは「手動でここまでできるはず」という設計と手順のまとめです。SPIの書き換え自体は実機で成立することを確認できたが、
HDD側で起きる自動ロールバックという想定外の壁に今ぶつかっている。差異が出た箇所は都度このファイルを更新してください。

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
以下、このディレクトリに**直接**ファイルを置いていく(nasneにsshでログインできるのは2回目起動後なので、`scp`/`ssh`は使わず、PC上のパスとして直接書き込む)。

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

### 1-3. MIPS用バイナリをクロスビルドする(mcui2c, nasne-recpt1, i2cx)

[docs/05](05_kernel_and_direct_boot.md)①で作ったDocker環境 `nasne-kcc:gcc432`(nostdlib・静的リンクのクロスコンパイラ)を使う。
`i2cx` は手順6([docs/08](08_tuner_i2c.md)のチューナー選局、`scripts/nasne_fe.py`がsshで呼ぶ)用に必要なので、ここで一緒にビルドしておく:

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

### 1-4. 起動時サービス(nasne-mcu-wd, nasne-recpt1-server)を仕込む

```bash
sudo cp scripts/watchdog/nasne-mcu-wd         /tmp/debian-root/etc/init.d/
sudo cp scripts/watchdog/nasne-recpt1-server  /tmp/debian-root/etc/init.d/
sudo chmod +x /tmp/debian-root/etc/init.d/nasne-mcu-wd /tmp/debian-root/etc/init.d/nasne-recpt1-server
```

`update-rc.d`(ランレベルごとの起動シンボリックリンク作成)はMIPSバイナリなので、`build_debian_rootfs.sh` と同じ要領で
`chroot` + `qemu-mipsel-static` を使う(同スクリプトが最後にqemuバイナリを消しているので、入れ直す):

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

B-CASカードは、この後の手順5(2回目起動)でDebianが立ち上がった後、実機にそのまま挿しておけばよい(ソフト的な設定は不要)。

## 手順2. 「段階1スクリプト」を作り、公式rootfsに足す

### 2-1. nasne上で動くSPI書き込みツールを用意する

`mtdtool` はglibcの `syscall()` を使う動的リンクの小物なので、Dockerの `nasne-kcc:gcc432`(nostdlibの古いクロスツールチェーン)ではなく、
**ホストのクロスコンパイラ**(Debian/Ubuntuの `gcc-mipsel-linux-gnu` パッケージ)でビルドする([docs/09](09_tools.md)):

```bash
sudo apt install gcc-mipsel-linux-gnu   # 未導入なら
mipsel-linux-gnu-gcc -O2 -nostartfiles -Wl,-e,_start -o mtdtool scripts/tools/mtdtool.c
file mtdtool   # "ELF 32-bit LSB executable, MIPS ... dynamically linked, interpreter /lib/ld.so.1" になっていることを確認(実機で確認済み)
```

`mtdtool` は `/lib/ld.so.1` を要求する動的リンクバイナリ。Debian上では動作確認済みだが、**公式miniroot/sys2のuserlandにMIPS用glibcの動的リンカと
対応する `libc.so` があるか**は、実機の `official_rootfs/lib/ld.so.1 -> ld-2.9.so` の存在と、`tar -tvf` 一覧の `bin/busybox_dynamic`(動的リンク版busybox)
から、**動的リンクの実行環境が公式rootfsに含まれていることを確認済み**(実機検証)。`mtdtool` 自身が実際に動くかは1回目の起動で初めて確認できる
(未検証なら[トラブルシュート](#トラブルシュート)。代替として `scripts/tools/*.c` と同じ作法(`-nostdlib -static`、素のsyscall)でMTD書き込み専用の
小さなツールを書き直せば、この依存自体を無くせる)。

### 2-2. 段階1スクリプト本体

**完了判定はHDD側のマーカーファイルではなく、SPIのKNLヘッダを読み戻して「もう自作版になっているか」を直接確認する方式にする。**
実機で、**procmngがHDD(sys1)の`00550066.dlm`を、何らかの異常検出(詳細未解明。p3=sda3がDebianに置き換わっていることを検出した可能性が高い)
をきっかけに、Aバンク/Bバンクの内容へ自動的にロールバックしてしまう事例を確認した**(現用の`00550066.dlm`がv1.00のバンクコピーと
バイト同一になっていた)。HDD上に書く `.stage1_done` のようなマーカーは、この自動ロールバックの対象になり得るため、「段階1は1回だけ」
という判定の根拠には使えない。SPI(KNL領域)自体の内容で判定すれば、HDD側で何が起きても判定を誤らない。

リポジトリのルート(このドキュメントのコマンドをどこで実行しているかの基準ディレクトリ。`official_rootfs/` や `mtdtool` もこの下にある)に、
`nasne-stage1.sh` というファイルを作る:

**診断のため、各ステップの結果をsys1に残るログファイル(`/disk0/.stage1_debug.log`)に書き出すようにしてある**
(`set -e` は使わず、各コマンドの終了コードを個別に見て、失敗した時点のログも必ずsys1に残してから終了する。
途中でハングする/sshが使えない状況でも、HDDをPCに戻して `.stage1_debug.log` を見れば、どこで失敗したかが分かる。
ただしこのログ自体もHDD(sys1)上にあるので、ロールバックが起きれば消えることがある — あくまで診断用の補助情報):

```bash
cat > nasne-stage1.sh <<'EOF'
#!/bin/sh
# 1回目の起動(公式カーネル)でだけ、/dev/mtd0のKNLを自作カーネルへ書き換える。
# 「すでに自作版か」の判定は、SPIのKNLヘッダ(64バイト)を読み戻して、書き込み予定のヘッダと
# バイト比較するだけ(HDD側のマーカーは使わない。理由は上の説明を参照)。
MNT=/tmp/disk0
KNL=$MNT/knl_new.bin
DBGLOG=/tmp/stage1_debug.log

log() {
    echo "$(date) $*" >> "$DBGLOG"
    cp "$DBGLOG" "$MNT/.stage1_debug.log" 2>/dev/null
}

: > "$DBGLOG"
log "start"

mkdir -p "$MNT"
if ! mount -t ext3 -o rw /dev/sda1 "$MNT" >> "$DBGLOG" 2>&1; then
    log "mount /dev/sda1 FAILED"
    exit 1
fi
log "mount /dev/sda1 OK"

log "checking current KNL header against target (SPI readback)"
dd if=/dev/mtd0ro bs=64 count=1 skip=16384 of=/tmp/cur_knl_hdr.bin >> "$DBGLOG" 2>&1   # 0x100000 / 64 = 16384
head -c 64 "$KNL" > /tmp/new_knl_hdr.bin
if cmp -s /tmp/cur_knl_hdr.bin /tmp/new_knl_hdr.bin; then
    log "KNL header already matches target, skipping (SPI already has the target kernel)"
    umount "$MNT"
    exit 0
fi
log "KNL header differs from target, proceeding to write"

log "checking mtdtool exists/runs"
ls -la /sbin/mtdtool >> "$DBGLOG" 2>&1
/sbin/mtdtool >> "$DBGLOG" 2>&1
log "mtdtool usage-check exit=$?"

log "erasing KNL"
/sbin/mtdtool erase /dev/mtd0 0x100000 0x280000 >> "$DBGLOG" 2>&1
RC=$?
log "erase exit=$RC"
if [ "$RC" -ne 0 ]; then
    log "ERASE FAILED, aborting (not rebooting)"
    umount "$MNT"
    exit 1
fi

log "writing new kernel"
/sbin/mtdtool write /dev/mtd0 0x100000 "$KNL" >> "$DBGLOG" 2>&1
RC=$?
log "write exit=$RC"
if [ "$RC" -ne 0 ]; then
    log "WRITE FAILED, aborting (not rebooting)"
    umount "$MNT"
    exit 1
fi

log "verifying"
SIZE=$(wc -c < "$KNL")
dd if=/dev/mtd0ro bs=65536 skip=16 count=40 of=/tmp/verify.bin >> "$DBGLOG" 2>&1
head -c "$SIZE" /tmp/verify.bin > /tmp/verify_trim.bin
if ! cmp -s /tmp/verify_trim.bin "$KNL"; then
    log "VERIFY FAILED - not rebooting, SPI left untouched"
    umount "$MNT"
    exit 1
fi
log "verify OK"

log "OK, rebooting (next boot's KNL readback check will see the new header and skip)"
sync
umount "$MNT"
sync
reboot -f
EOF
chmod +x nasne-stage1.sh
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
# ../nasne-stage1.sh と ../mtdtool は、2-2・2-1でリポジトリのルートに作ったファイル
# (official_rootfsに cd した直後なので、1つ上の ".." から見える)
sudo install -m 0755 ../nasne-stage1.sh sbin/nasne-stage1.sh
sudo install -m 0755 ../mtdtool          sbin/mtdtool
cat etc/init.d/rcS   # 挿入結果を確認(構文を壊していないか)
```

`nasne-stage1.sh` 自体は[2-2](#2-2-段階1スクリプト本体)のまま(sys1の完了マーカーで1回だけ実行される設計)で変更不要。
`mtdtool` が公式rootfs上で動くかどうかの検証状況は[2-1](#2-1-nasne上で動くspi書き込みツールを用意する)参照。

### 2-4. tar.gzに固めて `.dlm` を作る

**2-3で `cd official_rootfs` したままなら、まずリポジトリのルート(`official_rootfs/` が見える場所)に戻る**:

```bash
cd ..   # 2-3で official_rootfs に cd したままの場合。すでにルートにいるなら不要
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
mount | grep debian-root   # 何も出ないこと(出たら手順1-4の umount が効いていない。上の注記を参照)
sudo mkfs.ext3 -L debian /dev/sdX3
sudo mkdir -p /mnt/p3
sudo mount /dev/sdX3 /mnt/p3 && sudo cp -a /tmp/debian-root/. /mnt/p3/
sync && sudo umount /mnt/p3
```

`00110022.dlm` や各バンクディレクトリ(`11002200/`、`33004400/`)は**一切変更しない**(マネージャは正規のものがそのまま使われる)。

## 手順4. 1回目の起動(公式カーネルのまま)

HDDをnasneに挿して電源を入れる。

1. 公式のminiroot `/init` が `00110022.dlm`・`00550066.dlm` を検証(ヘッダのhwtype・日付・CRCはテンプレート継承なので通る)。
2. ボディを `/rfs` に展開し、`sbin/init`(busyboxへのシンボリックリンク、無改変)へ `switch_root`。
3. busybox initが `/etc/inittab` 経由で `rcS` を実行し、[2-3](#2-3-rcs-の先頭に段階1呼び出しを差し込む)で仕込んだ呼び出しにより `nasne-stage1.sh` が走る。
   `/dev/mtd0` のKNLを `knl_new.bin` に書き換え、読み戻して確認する。
4. OKなら `reboot`。NGなら公式ファームのまま起動を続ける(ログや挙動から原因を確認する。[トラブルシュート](#トラブルシュート))。

このときの所要時間・LEDの挙動は未検証。**最初は電源を入れたまま数分待ち、`ping` が通るか、PCに繋いだ状態でSPIが実際に書き変わったかを確認する**のが安全
(可能であれば、この段階でもCH341Aでの読み出し確認を併用して様子を見るとよい)。

> ⚠️ **[トラブルシュート](#トラブルシュート)の「HDD(sys1)の`00550066.dlm`が自動的にロールバックされる」現象に注意**。段階1が成功して
> `reboot` した後も、**何らかの理由でSony純正の起動に戻ってしまった場合**は、電源を切ってHDDをPCに戻し、`00550066.dlm` の
> ヘッダ(`decrypt-header`)が手順0でバックアップしたものと変わっていないか、毎回必ず確認すること。変わっていたら、段階1スクリプト
> 自体がHDD上から失われている(次にnasneに挿してもKNLは書き換わらない)ので、`backup/sys1/00550066.dlm` を一旦書き戻し、
> カスタム`.dlm`の作成(手順2)からやり直す必要がある。

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
| 1回目の起動後、いつまでもLEDが点滅したまま・SPIが書き変わらない | `.dlm` の展開失敗(ヘッダ不正・CRC不一致・容量不足)、または `rcS` の段階1呼び出しまで届いていない(`sed` の挿入位置がファームのバージョンで変わっている等)。HDDをPCに戻し、`build_dlm.py verify` と `tar tzf` でサイズ確認、`official_rootfs/etc/init.d/rcS` の中身を再確認 |
| 段階1がまた実行される(SPIはすでに自作版なのに書き込みが走る) | [2-2](#2-2-段階1スクリプト本体)のKNL読み戻し判定が機能していない。`nasne-stage1.sh` 冒頭の `cmp` 結果を `.stage1_debug.log` で確認(ヘッダ64バイトが一致していないなら、`knl_new.bin` 自体が前回書き込んだものと違う可能性。`build_knl.py` を再実行してファイルが変わっていないか確認) |
| 段階1のログ(`.stage1_debug.log`)に何も残っていない(sys1に書き込んだ直後の内容と変わらない、または全く無い) | [2-2](#2-2-段階1スクリプト本体)のログ付き版なら、sys1の `.stage1_debug.log` を見れば、どのステップ(mount・KNL読み戻し判定・mtdtool起動・erase・write・verify)で失敗したかが分かる。`mtdtool` が動かなかった場合(`/lib/ld.so.1` 不在など)は、ログの `mtdtool usage-check exit=...` が非ゼロになる。ログ自体が全く残っていない(前回のままに見える)場合は、下記の「HDD(sys1)が自動的にロールバックされる」を参照 |
| **HDD(sys1)の`00550066.dlm`が、いつの間にか別バージョン(バンクの内容と同一)に戻っている**(実機で確認した重大な既知の問題) | procmngが何らかの異常(p3=sda3がDebianに置き換わっていることの検出が濃厚)をきっかけに、現用の`00550066.dlm`をAバンク/Bバンク(`11002200/`・`33004400/`)のコピーへ自動的にロールバックする挙動を実機で確認した。発生すると、段階1スクリプト自体(カスタム`.dlm`のボディに仕込んだもの)がHDD上から失われ、以降は公式の`rcS`がそのまま実行される。確認方法: `python3 scripts/dlm_crypto.py decrypt-header /mnt/nasne_sys1/00550066.dlm` の `major_version` や `date` が、手順0でバックアップした`backup/sys1/00550066.dlm`と違う値になっていないか、`ls -la /mnt/nasne_sys1/11002200/00550066.dlm` 等とサイズが一致していないか確認する。**トリガーとなる条件・回避策は未解明**(調査中。[2-2](#2-2-段階1スクリプト本体)のKNL読み戻し判定は、この現象が起きても段階1自体の冪等性だけは保てるようにする対策だが、根本原因への対策ではない)。発生したら `backup/sys1/00550066.dlm` を書き戻してやり直す |
| 書き込み途中で電源が落ちた・`reboot` 後も公式ファームのまま | 4段目ブートがKNLのCRC不一致を検出し、BKNL(Bスロット)へフォールバックした可能性。これは安全に働いた証拠。原因(段階1スクリプトの `allowed()` 範囲・サイズ計算)を見直し、やり直す |
| CRCは正しいが起動途中で固まる(フォールバックが効かない) | [docs/05](05_kernel_and_direct_boot.md)の通り、CRC一致・内容不正の場合はA/Bのフォールバックが働かない。HDDをPCに戻し、`00550066.dlm` を手順0のバックアップに戻して公式ファームの `.dlm` のまま起動させ、SPIを元のダンプ(`backup`時に未取得なら、別途CH341Aで読み出した正常なKNL)に書き戻す必要がある |
| 2回目の起動でDebianに届かない(起動カウンタが2でSony経路に戻る) | [docs/05](05_kernel_and_direct_boot.md)の通り。`rc.local` が走っていない、Debian側のp3の構成を確認 |
| 約333秒ごとに再起動する | ウォッチドッグ未停止。[docs/06](06_mcu_watchdog.md) |
| 元の公式ファームに戻したい | `00550066.dlm` を手順0のバックアップで上書き、SPIのKNLも元のダンプ(または `ofw_out/KNL.bin` ベースで再構築したもの)に書き戻す |

## まだ詰めていないところ

実機で `mtdtool`(動的リンクでも実際に動いた)・段階1スクリプト(erase→write→verify OK→reboot)・`rcS` への仕込みは、
少なくとも1回は成功することを確認している。残っている大きな課題は次の2つ:

- **procmngによるHDD(sys1)の自動ロールバック(重大、未解明)**: [トラブルシュート](#トラブルシュート)参照。段階1が成功しても、
  その後の起動(procmngが動くタイミング)で、現用の`00550066.dlm`がバンクの内容(確認した事例ではv1.00)に戻ってしまう事例を複数回確認した。
  トリガー条件(p3=sda3がDebianであること自体か、ボディのサイズか、展開後の内容の何らかの不整合か)は未解明。これが解決しないと、
  「2回目の起動」に正しく進めるかどうかが安定しない。
  - 対策の方向性(実績のある別実装からの助言、未検証): ボディを「公式rootfs全体」ではなく、**段階1に必要な最小限のファイル
    (busybox相当、段階1スクリプト、`mtdtool`、`xcode4drv.ko`だけ)に絞る**。「ボディが大きすぎてsys2の想定を超えると起動不能になった」
    という実績があるとのことなので、サイズそのものが今回のロールバックの引き金になっている可能性がある。試す場合は
    `custom_rootfs.tar.gz`(手順2-4)の実際のサイズを確認し、公式rootfsそのままのサイズと比較すること。
  - [2-2](#2-2-段階1スクリプト本体)のKNL読み戻し判定は、この問題が起きても段階1の冪等性(やり直しが安全に効く)だけは保てるように
    した対策。根本原因への対策ではない。
- `sda3` 検出タイムアウトを40秒→180秒に拡大した(`scripts/kernel_build/initramfs/nasne_init`)効果は、上のロールバック問題に
  行き着く前に確認できていない。ロールバック問題が解決したら、改めて2回目の起動(自作カーネル)が安定するか確認すること。

解決済み・検証済みの事項(参考):
- `mtdtool` は実機の公式rootfs上で動作した(動的リンク環境ありで問題なし)。
- `knl_new.bin` の展開後カーネル先頭8バイトが `0000000000000000` だった点は、KNLの展開先物理アドレス(`0x10000000`)とジャンプ先
  (`0xB0040000`、[docs/02](02_boot_chain.md))の間に`0x40000`のオフセットがあることに由来する可能性が高く、特に問題ないと考えている
  (実際にブート自体は、段階1のverifyを経て少なくとも一度はswitch_rootまで到達した実績がある)。
