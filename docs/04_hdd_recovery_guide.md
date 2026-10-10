# 04. HDDが全損したnasneを、HDDのファイル操作だけで復旧する手順

> 対象: nasne(CECH-ZNR2J、ファーム v2.60)の内蔵HDDが壊れて、新しいHDDに交換したい人。
> **必要な作業はHDDの区画作成とファイルのコピーだけ**です。基板のフラッシュ(SPI)の吸い出し・書き込みなど、はんだや専用機器が要る作業は一切ありません。
> 仕組みの詳細は [03_firmware_format.md](03_firmware_format.md)。自己責任でお願いします(新しいHDDの中身は全部消えます。録画データも復元できません)。

## 結論(実機で確認した内容)

- **`00110022.dlm`(HDDと本体の紐付け)は、v1.00 のファームがなくても、このリポジトリのツールで v2.60 向けに作れる。**
  v2.60 の `final` が作る `00110022.dlm` は、公式のものとバイト単位で一致する。
- **ただし v2.60 は、p3(録画領域)の「初期構造」を自分では作れない。** 空のXFSのp3だと、v2.60 の公式アプリは起動の途中で止まる
  (PWR/REC赤点灯・LAN点灯、pingだけ生きている)。**v1.00 は空のXFSから p3 を初期化して起動できる。**
  → **v1.00 が必要なのはマネージャのためではなく、p3 を初期化させるため。**
- 推奨手順: **v1.00 で1回起動して p3 を初期化させ、そのあと v2.60 に置き換える**(下の手順)。v1.00 の `00550066.dlm`(21,287,161バイト、
  md5 `1c921378f2a7846a6492982c98c82fcd`)は、v1.00 で動いている中古のnasneなどから取り出す必要がある(Sonyは配布していない)。
  すでに公式に初期化されたp3(の中身)がある場合は、v1.00 なしでも v2.60 だけで起動できる(下の「v1.00が無い場合」)。

## 背景(読み飛ばしてよい)

nasneは起動時に、HDDの `00110022.dlm`(マネージャ)に書かれた「本体の個体ID」が自分のIDと一致するか確認し、一致しないとLEDが点滅したまま起動しません
(これが「HDDと本体の紐付け」の正体です)。IDは本体ごとに違い、外からは見えません。
そこで、**本体自身に一度IDを書かせてから取り出し、それを使って完全なHDDを作る**、という2段階で復旧します。

## 検証状況(正直に)

| 内容 | 状態 |
|---|---|
| **v1.00 の `00550066.dlm` を使う記事の方式**(区画+`mkfs`+sys1に3ファイル、2回起動)で、v1.00 の公式FW(WebUI)が起動し、p3が初期化される | ✅ 実機で確認(HDD1台、SPIの内容は元のダンプとKNLだけ違う状態) |
| 上で初期化されたHDDの `sys1` を `final`(v2.60)に差し替えて起動 | ✅ 実機で確認。`softwareVersion` が `0260`、HDDは登録済み・マウント済み |
| 空のXFS(記事のオプション)のp3 + `final`(v2.60)だけ | ❌ 起動しない(公式アプリが止まる)。再現: HDD2台 |
| 上の「空のXFS」に、公式が初期化したp3の中身を書き戻して `final`(v2.60) | ✅ 起動する(中身は `.hai` と `00000015/` など) |
| ステップ1(ヘッダ`+8=0`の v2.60 で、マネージャを本体に自動生成させる) | ✅ 確認(個体IDが書き出される)。ただし**このあと公式アプリは起動しない**(p3が未初期化のため)。v1.00 方式のほうが確実 |
| 別の個体、v2.60より古いファームがSPIに入っている本体、v2.60以外のファーム | ⚠️ 未検証 |
| 空のp3に対して、v2.60の公式アプリが初期化できる条件(WebUIや外部アプリ(nasne ACCESS等)からの初期化要求) | ⚠️ 未確認。p3が未フォーマット(ゼロ)の状態では、WebUIのポートが開かなかった |

## 準備するもの

- **Linux** が動くPC(Ubuntu等。USBのライブ起動でよい)。Windows/Macではext3・XFSの読み書きが難しいため、Linuxを使ってください。
- 交換用の2.5インチSATA HDD(500GB / 1TB。**中身は消えます**)と、USB-SATA変換アダプタ
- Python 3.8以上と `sfdisk` `partprobe` `wipefs` `mkfs.ext3` `mkfs.xfs`(`sudo apt install python3 fdisk parted e2fsprogs xfsprogs`)
- このリポジトリの `scripts/` フォルダ(`git clone`で取得)
- **公式ファーム v2.60**(Sonyの公開サーバから誰でも入手可能。再配布はしないでください):
  ```bash
  wget http://ps-peripheral.dl.playstation.net/ps-peripheral/nasne/0260/KRST3101_0260_SECURE.dlm
  ```
- **v1.00 の `00550066.dlm`**(上記。Sony由来のファイルなので、このリポジトリには含まれません)

## 手順(v1.00で p3 を初期化する方式、推奨)

以下、新しいHDDが `/dev/sdX` として見えているとします。**`sdX`は必ず `lsblk` で型番・容量・シリアルを見て確認してください。間違えるとPCのディスクが消えます。**

### 1. HDDを作り直す(全消去)

```bash
lsblk -o NAME,SIZE,MODEL,SERIAL                              # 新しいHDDの名前とシリアルを確認
sudo python3 scripts/nasne_hdd_rebuild.py mkdisk /dev/sdX --serial <lsblkで確認したシリアル>
```

`mkdisk` は、シリアルが合わないとき・マウント中の区画があるとき・システムのディスクのときは何もしない。やること:
区画(sys1=256MB・ブート、sys2=1GB、p3=残り)を作り、sys1・sys2を `mkfs.ext3 -F -L sys1/sys2`、p3を **記事と同じオプションのXFS**
(`mkfs.xfs -f -m crc=0 -d agcount=4 -i size=256,attr=2,projid32bit=0 -L user -n ftype=0 -s size=512`。`agcount` は1TBまで4、2TBまで8、それ以上は16)にする。

> 古いnasneのカーネル(2.6.29)に合わせたオプションです。デフォルトのまま作ったXFSは、nasneが正しく認識しないという報告が複数あります。

### 2. sys1にv1.00のファイルを置く

```bash
sudo mkdir -p /mnt/nasne_sys1 && sudo mount /dev/sdX1 /mnt/nasne_sys1
sudo python3 scripts/nasne_hdd_rebuild.py v100 <v1.00の00550066.dlm> /mnt/nasne_sys1
sync && sudo umount /mnt/nasne_sys1
```

置くのは `00550066.dlm`、`55006600/00550066.dlm`(ファーム更新の受け渡しディレクトリ。nasneのinitが処理して消す)、148バイトのゼロの `00110022.dlm`(initが個体IDを書く)の3つだけ。

### 3. nasneで2回起動する

1. HDDをnasneに挿して電源を入れる。**約2分後**、PWRがゆっくり点滅し、**REC・LANが高速点滅**、HDDランプが消灯になる(初期化完了の印。このあと進まないのは正常)。
2. **電源を抜いて、入れ直す。** 約40秒で v1.00 が起動する(PWR点灯、WebUI `http://<nasneのIP>:64210/nasne_home/index.html` に `nasne HOME ver 1.00` が出る)。p3には公式アプリが `00000000.hai` などを作る。

### 4. v2.60 に上げる(どちらか)

**(a) WebUIから**: 「nasne システムソフトウェアアップデート」で v2.60 へ。(この方法自体は本リポジトリでは未検証。記事では行われている)

**(b) PCで置き換える(オフライン。本リポジトリで確認)**: 電源を切ってHDDをPCに戻し、個体IDを読んで `final` で sys1 を v2.60 にする。

```bash
sudo mount /dev/sdX1 /mnt/nasne_sys1
python3 scripts/nasne_hdd_rebuild.py read-id /mnt/nasne_sys1        # 個体ID(16桁)を控える
python3 scripts/nasne_hdd_rebuild.py final KRST3101_0260_SECURE.dlm /mnt/nasne_sys1 --chipid <控えた16桁>
python3 scripts/nasne_hdd_rebuild.py verify /mnt/nasne_sys1         # 「結果: OK」を確認
sync && sudo umount /mnt/nasne_sys1
```

`final` は、`00550066.dlm`(公式のまま)、`00110022.dlm`(本体のIDを入れた完全版)、バンクdir `11002200`・`33004400`(KNL・RFS・rootfs・マネージャ)を作る。
p3 には触れない(v1.00 が初期化したものがそのまま使われる)。HDDをnasneに挿して電源を入れると、約45秒で v2.60 が起動する。

## v1.00 が無い場合

v2.60 は空のXFSのp3だと止まるので、p3の初期構造が要る。`.hai` などは本体のIDに結びついている可能性があり(暗号化とみられる)、他の本体のものを流用できるかは未確認。
**同じ本体で一度でも公式に初期化されたp3(の中身)が手元にある**なら、新しいXFSに書き戻して、`final`(v2.60)だけで起動できることを確認している:

```bash
# 旧p3(読み取り専用でマウントできるなら)から中身を保存
sudo mount -o ro,norecovery /dev/sdOLD3 /mnt/old && sudo tar -C /mnt/old -czpf p3_skeleton.tgz . && sudo umount /mnt/old
# 新しいHDDを mkdisk で作ったあと、p3に書き戻す
sudo mount /dev/sdX3 /mnt/p3 && sudo tar -C /mnt/p3 -xpf p3_skeleton.tgz && sudo umount /mnt/p3
```

その後は、手順4(b)の `read-id` の代わりに、旧HDDの `00110022.dlm` から個体IDを読む(`read-id <旧sys1>`)か、ステップ1(下)で本体にIDを書かせる。

## (参考)ステップ1 / ステップ2 だけで作る方式(v1.00なし)

```bash
sudo python3 scripts/nasne_hdd_rebuild.py mkdisk /dev/sdX --serial <シリアル> --p3 none     # p3は作らない
sudo mount /dev/sdX1 /mnt/nasne_sys1
python3 scripts/nasne_hdd_rebuild.py phase1 KRST3101_0260_SECURE.dlm /mnt/nasne_sys1       # ヘッダ+8=0の版を置く
sudo umount /mnt/nasne_sys1                                                                 # nasneに挿して2〜3分待ち、電源を抜いてPCに戻す
sudo mount /dev/sdX1 /mnt/nasne_sys1
python3 scripts/nasne_hdd_rebuild.py read-id /mnt/nasne_sys1                                # 個体IDが出れば、initが動いた
```

ここまでは**空のHDDから実機で確認**(initがマネージャとバンクdir、sys2のrootfsを作り、個体IDは本体のものと一致した)。ただし、そのあと `final` で完全版にしても、
p3が無い/空のXFSでは公式アプリが起動しない。p3の初期構造を別の方法(上)で用意すること。

## 困ったとき

| 症状 | 考えられる原因 |
|---|---|
| 1回目の起動で、2〜3分たってもREC/LANが高速点滅にならない | initの検査に失敗している(`ChipID ERROR`/`DLM date ERROR`/`CRC ERROR`)。`verify` で確認。区画ラベル(`sys1`/`sys2`)・ext3か、v1.00のmd5が `1c921378…` か確認 |
| 2回目の起動後、PWR/REC赤点灯・LANオレンジ・pingだけ生きている | 公式アプリが検査に通らず止まった(`/sbin/halt`)。p3が空のXFS/未初期化のとき。上の「v1.00が無い場合」 |
| 2回目の起動後、PWR点滅のままネットワークに出ない | p3の区画が無い、または公式アプリが先へ進めない。p3を作る(mkdisk の既定) |
| PWRが点灯しない/アプリに出ない | p3(録画領域)が未初期化。v1.00方式にする |
| `00110022.dlm` が作られない | initがそこまで進んでいない。区画のラベルとext3、`00550066.dlm` の配置を確認 |

## 他のファームバージョンを使う場合

`KRST3101_xxxx_SECURE.dlm` は、どのバージョンでも「KNL ‖ RFS ‖ rootfs」の連結で、`nasne_hdd_rebuild.py`はヘッダから日付を取って整合させるので原理的には使えますが、
**本体のSPIに入っているファーム(通常は最後に動かしていたバージョン)より古い/新しいものは未検証**です。迷ったら、その本体が最後に使っていた最新の公式ファーム(v2.60)を使ってください。
