# 04. HDDが全損したnasneを、HDDのファイル操作だけで復旧する手順

> 対象: nasne(CECH-ZNR2J、ファーム v2.60)の内蔵HDDが壊れて、新しいHDDに交換したい人。
> **必要な作業はHDDの区画作成とファイルのコピーだけ**です。基板のフラッシュ(SPI)の吸い出し・書き込みなど、はんだや専用機器が要る作業は一切ありません。
> **v1.00の`00550066.dlm`も不要です**(ネットでは「再生成にはv1.00の`00550066.dlm`が要る」とよく言われますが、この方法では要りません。
> 必要なのは、Sonyの公開サーバから誰でも落とせる最新の公式ファーム(v2.60)だけです)。
> 仕組みの詳細は [03_firmware_format.md](03_firmware_format.md)。自己責任でお願いします。

## 背景(読み飛ばしてよい)

nasneは起動時に、HDDの `00110022.dlm`(マネージャ)に書かれた「本体の個体ID」が自分のIDと一致するか確認し、一致しないとLEDが点滅したまま起動しません
(これが「HDDと本体の紐付け」の正体です)。IDは本体ごとに違い、外からは見えません。
そこで、**本体自身に一度IDを書かせてから取り出し、それを使って完全なHDDを作る**、という2段階で復旧します。

## 検証状況(正直に)

| 内容 | 状態 |
|---|---|
| 公式ファーム(v1.00 / v2.60)+正しいマネージャでの起動(PWR点灯・WebUI表示) | ✅ 実機で確認(区画はnasneが作ったもの) |
| 本手順の`final`が作る4ファイルが、実機が実際に使っていた正常なファイルとバイト完全一致 | ✅ |
| ステップ1(ヘッダ`+8=0`でマネージャを本体に自動生成させる)でinitが個体IDを書き出す | ✅ 実機で確認(ただし区画・バンクdirが既にある状態で) |
| **完全に空のHDD(区画だけ作った状態)からの通し** | ⚠️ **未検証**(ステップ1が空のsys1・sys2で同様に動く見込みだが未確認) |
| 別の個体、v2.60より古いファームがSPIに入っている本体、v2.60以外のファーム | ⚠️ 未検証 |
| p3(録画領域)の作り方(下記A/B) | ⚠️ 未検証 |

うまくいかなかったら、[03_firmware_format.md](03_firmware_format.md)の仕組みを見ながら調べてください。結果をissue等で共有してもらえると助かります。

## 準備するもの

- **Linux** が動くPC(Ubuntu等。インストールしなくてもUSBのライブ起動でよい)。Windows/Macではext3・XFSの読み書きが難しいため、Linuxを使ってください。
- 交換用の2.5インチSATA HDD(500GB / 1TB。**中身は消えます**)と、USB-SATA変換アダプタ
- Python 3.8以上と `sfdisk` `mkfs.ext3` `mkfs.xfs`(`sudo apt install python3 fdisk e2fsprogs xfsprogs`)
- このリポジトリの `scripts/` フォルダ(`git clone`で取得)
- **公式ファーム**(Sonyの公開サーバから誰でも入手可能。再配布はしないでください):
  ```bash
  wget http://ps-peripheral.dl.playstation.net/ps-peripheral/nasne/0260/KRST3101_0260_SECURE.dlm
  ```

## 手順

以下、新しいHDDが `/dev/sdX` として見えているとします。**`sdX`は必ず `lsblk` で型番・容量を見て確認してください。間違えるとPCのディスクが消えます。**

### 0. HDDに区画を作る

```bash
lsblk -o NAME,SIZE,MODEL,SERIAL        # 新しいHDDの名前を確認(以後 /dev/sdX と書く)

# 区画表を書き込む(sdXの中身は全消去されます)
printf 'label: dos\nunit: sectors\nstart=2048, size=524288, type=83, bootable\nstart=526336, size=2097152, type=83\nstart=2623488, type=83\n' | sudo sfdisk /dev/sdX

# sys1(256MB) と sys2(1GB): ext3。nasneの古いカーネル(2.6.29)が読めるよう、古い形式で作る
sudo mkfs.ext3 -L sys1 -b 1024 -I 128 /dev/sdX1
sudo mkfs.ext3 -L sys2 -b 4096 -I 128 /dev/sdX2
```

(「128-byte inodes ... deprecated」「V4 filesystems are deprecated」という警告が出ますが、古いnasneのカーネルに合わせた意図的な形式なので無視してください。
xfsprogsが将来V4形式の作成を廃止した場合は、p3は下のA(空のまま)にしてください。手順の構文は、スパースファイルで確認済みです。)

3つ目の区画(p3、残り全部)は録画用の「user」領域です。次のどちらかにしてください(どちらも未検証):
- **A. 作らずに空のままにする**(推奨)。nasneのアプリからHDDの初期化(フォーマット)を行う。
- B. 自分で作る(古いカーネルで読めるよう、新しいXFSの機能を切る):
  `sudo mkfs.xfs -f -L user -m crc=0 -n ftype=0 -i sparse=0 /dev/sdX3`

### 1. ステップ1: 本体に個体IDを書かせる

```bash
sudo mkdir -p /mnt/nasne_sys1
sudo mount /dev/sdX1 /mnt/nasne_sys1
python3 scripts/nasne_hdd_rebuild.py phase1 KRST3101_0260_SECURE.dlm /mnt/nasne_sys1
sync && sudo umount /mnt/nasne_sys1
```

`phase1`は、公式ファームのヘッダの1か所(`+8`、メジャーバージョン)を0にした版を `00550066.dlm` として置き、`00110022.dlm`を作りません
(こうすると、nasneのinitが「マネージャが無いので、この本体のIDで新規作成する」処理をします)。

HDDをnasneに挿して電源を入れます。**その後の挙動は気にしなくて構いません**: このあと固まる(PWR/REC赤点灯など)ことがありますが想定内です。
**2〜3分待ってから、ACアダプタを抜いて電源を切り**、HDDをPCに戻します。

### 2. 個体IDを読み出す

```bash
sudo mount -o ro /dev/sdX1 /mnt/nasne_sys1
python3 scripts/nasne_hdd_rebuild.py read-id /mnt/nasne_sys1
```

`個体ID: xxxxxxxxxxxxxxxx`(16桁の16進数)と表示されれば成功です。この値を控えてください。
- `00110022.dlm が無い` と出たら、nasneのinitがそこまで進んでいません(ステップ1をやり直す。LEDが点滅したまま・ネットワークに出ない場合は、別の原因なので下の「困ったとき」を参照)。

### 3. ステップ2: 完全なHDDを作る

```bash
sudo umount /mnt/nasne_sys1
sudo mount /dev/sdX1 /mnt/nasne_sys1
python3 scripts/nasne_hdd_rebuild.py final KRST3101_0260_SECURE.dlm /mnt/nasne_sys1 --chipid 控えた16桁
python3 scripts/nasne_hdd_rebuild.py verify /mnt/nasne_sys1     # 「結果: OK」を確認
sync && sudo umount /mnt/nasne_sys1
```

`final`は、`00550066.dlm`(公式のまま)、`00110022.dlm`(本体のIDを入れた完全版)、バンクdir `11002200`・`33004400`(KNL・RFS・rootfs・マネージャの4ファイル)を作ります。
**ステップ1の自動生成された`00110022.dlm`は不完全(日付が空)で、そのまま使うと起動後に固まるため、必ず`final`で置き換えてください。**

### 4. 起動

HDDをnasneに挿して電源を入れます。PWRランプが点灯し、nasneのアプリ(WebUI)から見えれば成功です。
初回は、アプリからHDDの初期化(フォーマット)を求められることがあります(p3を作らなかった場合など)。

## 困ったとき

| 症状 | 考えられる原因 |
|---|---|
| 電源ランプが緑点滅のまま止まる(ネットワークにも出ない) | initの検査に失敗している。`verify`で確認: CRC・個体ID・日付の不整合(`ChipID ERROR`/`DLM date ERROR`/`CRC ERROR`)。ファイルを作り直す |
| PWR/REC赤点灯・HDD消灯でネットワーク無応答 | マネージャが不完全(ステップ1のまま)。ステップ2の`final`をやり直す |
| ステップ1の後に`00110022.dlm`が作られない | initがマネージャ作成まで進んでいない。区画のラベル(`sys1`/`sys2`)・ext3か、`00550066.dlm`が置けているか確認 |
| PWRが点灯しない/アプリに出ない | p3(録画領域)が未フォーマットの可能性。アプリからHDDを初期化 |

## 他のファームバージョンを使う場合

`KRST3101_xxxx_SECURE.dlm` は、どのバージョンでも「KNL ‖ RFS ‖ rootfs」の連結で、`nasne_hdd_rebuild.py`はヘッダから日付を取って整合させるので原理的には使えますが、
**本体のSPIに入っているファーム(通常は最後に動かしていたバージョン)より古い/新しいものは未検証**です。迷ったら、その本体が最後に使っていた最新の公式ファーム(v2.60)を使ってください。
