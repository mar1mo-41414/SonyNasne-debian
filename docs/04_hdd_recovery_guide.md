# 04. HDDが全損したnasneを、HDDのファイル操作だけで復旧する手順

> 対象: nasne(CECH-ZNR2J、ファーム v2.60)の内蔵HDDが壊れて、新しいHDDに交換したい人。
> **必要な作業はHDDの区画作成とファイルのコピーだけ**です。基板のフラッシュ(SPI)の吸い出し・書き込みなど、はんだや専用機器が要る作業は一切ありません。
> **v1.00 のファームは不要です**(入手が難しいため、v2.60 だけで復旧できるようにしました)。
> 仕組みの詳細は [03_firmware_format.md](03_firmware_format.md) と [15_p3_structure_and_hai.md](15_p3_structure_and_hai.md)。
> 自己責任でお願いします(新しいHDDの中身は全部消えます。録画データも復元できません)。

## 結論(実機で確認した内容)

- HDDと本体の紐付け(`00110022.dlm`)は、本体に一度書かせて読み取り、このリポジトリのツールで完全版を作れる。
- v2.60 は、p3(録画領域)に「初期構造」(HDD登録情報 `.hai`、録画DBの雛形、空のディレクトリ群)が無いと、起動の途中で固まる。
  従来は v1.00 に作らせていたが、**この構造は `p3init` で作れる**(`.hai` は形式を解読して生成、録画DBの雛形は固定のデータ)。詳細は [docs/15](15_p3_structure_and_hai.md)。
- 空のHDDから、v1.00 を一度も使わずに v2.60 が起動することを、実機で確認した(下の表)。

## 背景(読み飛ばしてよい)

nasneは起動時に、HDDの `00110022.dlm`(マネージャ)に書かれた「本体の個体ID」が自分のIDと一致するか確認し、一致しないとLEDが点滅したまま起動しません
(これが「HDDと本体の紐付け」の正体です)。IDは本体ごとに違い、外からは見えません。
そこで、**本体自身に一度IDを書かせてから取り出し、それを使って完全なHDDを作る**、という2段階で復旧します。

## 検証状況(正直に)

| 内容 | 状態 |
|---|---|
| 空のHDDで、ヘッダ`+8=0`の v2.60 を置いて起動 → 本体のinitがマネージャ(`00110022.dlm`)・バンクdir・sys2を自動生成(個体IDが読み出せる) | ✅ 実機で確認(HDD2台) |
| 上のIDで `final`(v2.60)+ `p3init` で作ったHDDが、v1.00 なしで v2.60 として起動 | ✅ 実機で確認(HDD2台中1台で通しを確認。本体は1台) |
| `p3init` なし(p3が空のXFS)で `final` だけ | ❌ 起動の約35秒後に固まる |
| (参考)v1.00 の `00550066.dlm` を使う記事の方式でも復旧できる | ✅ 実機で確認(v1.00 のファイルが手元にある場合) |
| 別の本体での `p3init`(録画DBの雛形が本体のIDに依存しないか) | ⚠️ 未検証(本体が1台のため。雛形は、別のHDD2台で v1.00 が作ったものとバイト単位で一致) |
| v2.60より古いファームがSPIに入っている本体、v2.60以外のファーム | ⚠️ 未検証 |

## 準備するもの

- **Linux** が動くPC(Ubuntu等。USBのライブ起動でよい)。Windows/Macではext3・XFSの読み書きが難しいため、Linuxを使ってください。
- 交換用の2.5インチSATA HDD(500GB / 1TB。**中身は消えます**)と、USB-SATA変換アダプタ
- Python 3.8以上と `sfdisk` `partprobe` `wipefs` `mkfs.ext3` `mkfs.xfs`(`sudo apt install python3 fdisk parted e2fsprogs xfsprogs`)
- このリポジトリの `scripts/` フォルダ(`git clone`で取得)
- **公式ファーム v2.60**(Sonyの公開サーバから誰でも入手可能。再配布はしないでください):
  ```bash
  wget http://ps-peripheral.dl.playstation.net/ps-peripheral/nasne/0260/KRST3101_0260_SECURE.dlm
  ```

## 手順

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

### 2. 本体に個体IDを書かせる

```bash
sudo mkdir -p /mnt/nasne_sys1 && sudo mount /dev/sdX1 /mnt/nasne_sys1
python3 scripts/nasne_hdd_rebuild.py phase1 KRST3101_0260_SECURE.dlm /mnt/nasne_sys1     # ヘッダ+8=0の版を置く
sync && sudo umount /mnt/nasne_sys1
```

HDDをnasneに挿して電源を入れ、**2〜3分待って電源を切り**、HDDをPCに戻す(このときアプリは起動しない。initが個体IDを書くところまでで十分)。

```bash
sudo mount /dev/sdX1 /mnt/nasne_sys1
python3 scripts/nasne_hdd_rebuild.py read-id /mnt/nasne_sys1        # 個体ID(16桁)が出れば成功。控える
```

### 3. 完全版のHDDにする

```bash
python3 scripts/nasne_hdd_rebuild.py final KRST3101_0260_SECURE.dlm /mnt/nasne_sys1 --chipid <控えた16桁>
python3 scripts/nasne_hdd_rebuild.py verify /mnt/nasne_sys1         # 「結果: OK」を確認
sync && sudo umount /mnt/nasne_sys1

sudo mkdir -p /mnt/nasne_p3 && sudo mount /dev/sdX3 /mnt/nasne_p3
sudo python3 scripts/nasne_hdd_rebuild.py p3init /mnt/nasne_p3 --chipid <控えた16桁> --device /dev/sdX
sync && sudo umount /mnt/nasne_p3
```

- `final` は、`00550066.dlm`(公式のまま)、`00110022.dlm`(本体のIDを入れた完全版)、バンクdir `11002200`・`33004400`(KNL・RFS・rootfs・マネージャ)を作る。
- `p3init` は、空のp3に `.hai`(HDD登録情報。IDとHDDのシリアルから生成)、録画DBの雛形、空のディレクトリ群を作る。**p3が空のときだけ動く**(録画データを守るため)。

### 4. 起動する

HDDをnasneに挿して電源を入れる。約1分で公式アプリが起動し、PWRが緑になる(`http://<nasneのIP>:64210/status/softwareVersionGet` が `0260` を返す)。
ネットワークの設定などは公式の画面(nasne HOME、nasne ACCESS)から行う。

## (参考)v1.00 の `00550066.dlm` が手元にある場合

v1.00(21,287,161バイト、md5 `1c921378f2a7846a6492982c98c82fcd`)で p3 を初期化させる、ネットの記事の方式も使える。Sony由来のファイルで、Sonyは配布していない。

```bash
sudo python3 scripts/nasne_hdd_rebuild.py mkdisk /dev/sdX --serial <シリアル>
sudo mount /dev/sdX1 /mnt/nasne_sys1
sudo python3 scripts/nasne_hdd_rebuild.py v100 <v1.00の00550066.dlm> /mnt/nasne_sys1; sync; sudo umount /mnt/nasne_sys1
```

nasneに挿して電源を入れ、**約2分後にREC・LANが高速点滅したら電源を抜いて入れ直す**(約40秒で v1.00 が起動し、p3が初期化される)。
そのあとHDDをPCに戻し、手順2の `read-id`、手順3の `final`(`p3init` は不要。p3は初期化済み)でv2.60に置き換える。

## 困ったとき

| 症状 | 考えられる原因 |
|---|---|
| 手順2で、2〜3分たってもREC/LANが高速点滅にならない | initの検査に失敗している(`ChipID ERROR`/`DLM date ERROR`/`CRC ERROR`)。区画ラベル(`sys1`/`sys2`)・ext3か確認 |
| 手順4で、PWR/REC赤点灯・LANオレンジで止まる(約35秒後にネットワークも切れる) | p3の初期構造が無い。`p3init` を実行したか確認(空のp3にだけ作れる。すでに何か入っているなら `mkdisk` からやり直す) |
| 手順4で、PWR点滅のままネットワークに出ない | p3の区画が無い(`mkdisk` の既定で作る)、または `final` の `--chipid` が違う |
| `00110022.dlm` が作られない(`read-id` が失敗する) | initがそこまで進んでいない。区画のラベルとext3、`00550066.dlm` の配置を確認 |
| 起動するが、HDDが「未登録」と出る | `.hai` のシリアルとHDDが一致していない(`p3init` の `--device` を取り違えた)。`mkdisk` からやり直す |

## 他のファームバージョンを使う場合

`KRST3101_xxxx_SECURE.dlm` は、どのバージョンでも「KNL ‖ RFS ‖ rootfs」の連結で、`nasne_hdd_rebuild.py`はヘッダから日付を取って整合させるので原理的には使えますが、
**本体のSPIに入っているファーム(通常は最後に動かしていたバージョン)より古い/新しいものは未検証**です。迷ったら、その本体が最後に使っていた最新の公式ファーム(v2.60)を使ってください。
