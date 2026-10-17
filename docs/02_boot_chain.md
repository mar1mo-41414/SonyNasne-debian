# 02. SPIフラッシュとブートチェーン

結論: **署名検証は存在しない**。ブートローダ(U-Boot・4段目ブート)は CRC32 しか見ないので、
SPIフラッシュのKNL領域を書き換えるだけで、ブートローダを触らずにカーネルを差し替えられる。

## 全体の流れ

```
[電源ON]
 → SoC内蔵ROM        DRAMを初期化し、U-Bootを物理0(0x80000000)へ、4段目ブートを0x81000000へ配置(推定)
 → U-Boot 1.1.3      "ViXS System Inc"。既定環境 bootcmd=bootm 81000000、bootdelay=0 で 4段目を起動
 → 4段目ブート        FMAPを読み、KNL(カーネル)を物理0x10000000へ展開、RFS(ramdisk)を0x81400000へ読み込み、カーネルへジャンプ
 → Linux 2.6.29      root=/dev/sda2 rw、ramdisk(Sony miniroot)がinitramfsとして展開される
 → miniroot の /init  HDDのsys1から.dlmを読み、sys2を再構築して switch_root   ← 純正の経路
 → /sbin/init (sys2)  rcS → startdtvtuner → procmng / dtvtuner / webapi ...
```

Debian直起動では、最後の2段(miniroot と sys2)を使わず、カーネル内蔵の initramfs から p3 のDebianへ直接 `switch_root` する([docs/05](05_kernel_and_direct_boot.md))。

## SPIフラッシュの地図(16MB)

FMAP(フラッシュマップ、`0x80000`)の16バイトエントリ(名前4B・開始4B・サイズ4B・フラグ4B、いずれもビッグエンディアン)より。

| 名前 | 開始 | サイズ | 中身 |
|---|---|---|---|
| BOOT | `0x000000` | 256KB | 先頭ブロック + **U-Bootイメージ**(`0x10000`〜`0x2d977`) |
| CER | `0x040000` | 64KB | ブート版数の平文記録(証明書ではない) |
| BFWF | `0x050000` | 64KB | **4段目ブート("Fourth Boot")**(実体は約45KB) |
| INFO | `0x060000` | 64KB | 機種情報(Blowfish暗号) |
| INF2 | `0x070000` | 64KB | **個体情報**(Blowfish暗号。MAC、個体ID。HDD紐付けに使う) |
| FMAP | `0x080000` | 64KB | 本表 |
| (無名) | `0x090000`, `0x0A0000` | 各約3KB | 乱数様のblob(鍵素材の可能性。未解読) |
| ECC | `0x0D0000` | 64KB | **`0x010000`〜`0x08ffff`(U-Boot・CER・4段目ブート・INFO・INF2・FMAP)の Hamming(72,64) パリティ**。3コピー(`0xd0000`/`0xe0000`/`0xf0000`)が同一。下の「ECC領域」参照 |
| KNL / BKNL | `0x100000` / `0x380000` | 各2.5MB | カーネル(A/B冗長)。Blowfish暗号 + gzip |
| RFS / BRFS | `0x600000` / `0x980000` | 各3.5MB | miniroot(A/B冗長) |
| FSYS | `0xD00000` | 2.9MB | JFFS2(設定) |

`BOOT`・`BFWF` は暗号ではなく、**1024バイト周期の固定XORパッド**で難読化されたMIPSリトルエンディアンの平文コード。
KNL/RFS/INFO/INF2/FMAP は `.dlm` と同じ64バイトヘッダ付きセグメント(Blowfish、[docs/03](03_firmware_format.md))。
`scripts/spi_segments.py` でセグメントを列挙・抽出、`scripts/spi_boot_decrypt.py` でブートチェーンを復号できる([docs/09](09_tools.md))。

## U-Boot(`0x80000000` リンク)

- リセット後 BSS をクリアして C の main へ。環境変数は**常に既定値**(`saveenv`なし、"Using default environment"):

  ```
  bootcmd=bootm 81000000     bootdelay=0     baudrate=115200
  loadaddr=0x81000000        eth0_reset_pin=5     Board_ID=0x1202
  ```
- USB(CDC)Ethernetガジェットでのtftp起動に対応したコードがあるが、通常は使われない。
- カーネル引数は `root=/dev/sda2 rw`(`multiboot`/`current_active` 環境変数でMTD rootに切替可能)を組み立てる。**`console=` は存在しない**
  (そのため `unable to open an initial console`。UARTが無いこととも整合)。
- 画像検証は標準のCRC32のみ(`Bad Magic Number` / `Bad Header Checksum` / `Bad Data CRC`)。署名検証の文字列は無い。
  ソースには `CFG_SECURITY_RSA2048` による署名検証コード(`vixs_secu.c`)があるが、**この設定は無効**でバイナリにも含まれない。

## 4段目ブート("Fourth Boot")

U-Boot legacy uImage(load = entry = `0x80200000`)。動作:

1. FMAP を読んでヘッダを復号し、パーティション表を得る。
2. A/B(KNL↔BKNL、RFS↔BRFS)のうち有効な側を選ぶ。選択状態はシステム情報ページ(`0x1ffef000`)の `+6` に置かれ、
   読み込み/検証に失敗すると他方に切り替えて再試行する。
3. カーネルを物理 `0x10000000`(KSEG0 `0x90000000`)へ、ramdisk を `0x81400000` へ読み込む。
   各セグメントは **ヘッダCRC32 の検証 → `[14]==1` なら本体をBlowfish-CBC復号 → `[13]==0x10` ならgunzip** という手順(署名検証なし)。
4. カーネル引数に `ethaddr=`(INF2のMAC)、`rd_start=0x81400000`、`rd_size=<RFSサイズ>` を付加。
5. SoCレジスタ `0xBFFF2820` に `0x55aabeef` を書き、`jalr 0xB0040000`(カーネルエントリ。a0=argc, a1=argv, a2=envp, a3=prom)。

カーネルはこの `0x55aabeef` を見て「ブートローダから起動された」と判断し、引数(`root=` `rd_start=` など)を受け取る
(`arch/mips/xcode/viper/board.c` の `prom_init`)。

典型的なカーネルコマンドライン:
`root=/dev/sda2 rw ethaddr=00:25:DC:xx:xx:xx rd_start=0x81400000 rd_size=0x0023d8b5`

## システム情報ページ(物理 `0x1ffef000`、4KB)

純正ユーザランド(`libcommlib.so` の `seiSysInfoOpen`)が `/dev/mem` でマップして読み書きする。ファーム版数(`+0xcc` に実行中ファームの`DLM`ヘッダ)、
INFO の hwtype(`+0x10c`)、MAC などが入る。個体ID(チップID)は**このページではなく**、SPIのINF2本体 `+0x60` の8バイト。

## 純正の起動(miniroot `init` と sys2 再構築)

SPIのRFS(ramdisk)の `/init` が行うこと:

1. sda1 を `/disk0`、sda2 を `/rfs` にマウントし、**`rm -rf /rfs/*`**(sys2を毎起動空にする)。
2. SPIのINF2から**チップID**を読む。`/disk0/00110022.dlm`(マネージャ)を検査([docs/03](03_firmware_format.md))。
3. `/disk0/00550066.dlm`(rootfsパッケージ)のヘッダを復号・検証(マジック `DLM\0`、CRC32、hwtype、日付)。
4. ボディを復号して `tar zxf` で `/rfs` に展開、`/rfs/sbin/init` の存在を確認し、`switch_root /rfs sbin/init`。
5. **どこかが失敗すると**、`init failure - cancel switch_root` → LEDを交互に点滅し続ける無限ループに入る(ネットワークは出ない)。

つまり sys2 へ直接ファイルを書いても、次の起動で消える。永続させるには `.dlm` を作り直す(または直起動で `/init` を迂回する)。

その後 sys2 の `/sbin/init`(busybox)→ `/etc/init.d/rcS`(proc/sysfs/udev など)→ `/opt/dtvtuner/etc/startdtvtuner`:
`/dev/sda3` を `/disk0` にマウント、`rc.xcode4` でドライバ(`xcode4drv.ko`)をロードし、`procmng`、`dtvtuner`、`webapi` などを起動する。

## 参考: 純正カーネルとの関係

実機のカーネルバナーは `Linux version 2.6.29-ts-mipsisa32r2el`。SIEが公開しているGPLソース([docs/05](05_kernel_and_direct_boot.md))から
自分でビルドしたカーネルで、ブートローダ無改変のまま同じように起動する(バイト同一ではないが機能的に同等)。

## スロットB(BKNL・BRFS)の正体(静的解析、実機1台のSPIダンプ)

A/B冗長の4つのセグメントを、ローダと同じ手順(CRC32検証 → Blowfish復号)で開いて比べた(`scripts/spi_ab_info.py`):

| セグメント | ビルド番号 | ビルド日時(復号ヘッダ) | 中身 |
|---|---|---|---|
| KNL(A、公式 v2.00〜v2.60 の状態) | 72 | 2013-06-17 | 展開後カーネル 4,788,358 バイト |
| **BKNL(B)** | **72** | **2013-06-17** | **KNL(v2.00〜v2.60)と、先頭64バイトのヘッダ(名前 `BKNL`・CRC)以外が完全に同一** |
| RFS(A、miniroot) | 50 | 2012-07-09 | cpio(gzip) |
| **BRFS(B)** | **8** | **2012-05-07** | **RFS より古い miniroot。違うファイルは `/init` の1つだけ**(約400ファイルの残りは同一) |

- **BKNL は、工場出荷のカーネル(ビルド72)で、公式の最新(v2.00〜v2.60)の KNL と同じ**。公式のアップデートは A だけを書くので、B は出荷時のまま。以前の「どのバージョンの KNL とも一致しない」は、暗号化されたセグメント同士の比較(ヘッダの違いで全体が変わる)だったための誤りで、復号後は一致する。
- v1.51 の KNL(ビルド51、2012-08-23)を A に書いた状態では、**B(ビルド72)のほうが A より新しい**。
- **BRFS の `/init`(ビルド8)は、RFS の `/init`(ビルド50)の古い版**。`.dlm` の検証(ChipID・日付・hwtype・CRC・エリア検査)と A/B バンクの入れ替え(`mv`/`cp -r`/`rm -rf` を `system()` で呼ぶ)、「backup romfs start」は同じように持つ。新しい版にだけあるのは、`DLM Update found`、`bufw_restart` の更新ファイル(`BUFW_RESTART_PREVIOUS/UPDATE`)、ATA の flush・sleep、I2C の書き込み(MCU)、DIPスイッチの読み取り。
- つまり **B は「出荷時カーネル + 古い miniroot」の組**。A が読み込み/検証に失敗して B に切り替わると、HDD の `.dlm` を読む点は同じだが、新しい版の電源・MCU まわり(ウォッチドッグ停止など)は B の `/init` には無いとみられる(**B で実際に起動する検証は未実施**。A/B フォールバックが機能しなかった事例の報告があるのは、この違いが関係しているのかもしれない)。
- **どちらのスロットで起動したかは、稼働中に分かる**: カーネルのコマンドラインの `rd_size`(`cat /proc/cmdline`)が RFS(A)なら `0x23d8b5`、BRFS(B)なら `0x23d669`(本体の総長 − 64バイト)。システム情報ページ(物理 `0x1ffef000`)の `+6` が A/B の選択状態で、A のときは 0(実機で確認)。

## 公式のアップデートが書き換えるもの(実機の実測)

公式のアップデート(WebUI の「ソフトウェア更新」)が、SPI と HDD の sys1 の**どこを書き換えるか**を、実機1台で、更新の前後の SPI を吸い出して比べた(吸い出しは [docs/16](16_spi_backup_dlm.md) の `--mode backup`)。
v1.51(KNL も rootfs も v1.51)の本体を、公式の更新で v2.60 へ上げた:

| 場所 | 変化 |
|---|---|
| SPI の **KNL スロットA**(`0x100000`〜、約2MB) | **v1.51 の KNL → v2.60 の KNL に書き換わる**。更新の前後の中身は、それぞれ公式パッケージの KNL セグメントとバイト一致 |
| SPI の KNL スロットB(BKNL、`0x380000`〜) | **変化なし** |
| SPI の RFS(miniroot、`0x600000`〜)、ブートローダ類(`0`〜`0xfffff`) | **変化なし** |
| HDD sys1 の `00550066.dlm` | v2.60 の公式 rootfs に置き換わる |
| HDD sys1 の `00110022.dlm`(マネージャ) | v2.60 の日付(rootfs・KNL・RFS)に更新される。**`final` が作るものと、バイト単位で同一** |
| HDD sys1 の A バンク(`11002200/`) | 新しい版(v2.60)の KNL・RFS・rootfs・マネージャが置かれる |
| HDD sys1 の B バンク(`33004400/`) | 更新前の版(v1.51)が残る(旧版の控え) |
| HDD sys1 の `00CC00DD.dlm`(8バイト) | 新しく作られる(更新/復元の状態を示す印) |

つまり、**公式のアップデートが SPI で触るのは KNL スロットA だけ**で、スロットBには書かない。このプロジェクトの「書き換えるのは A(KNL)だけ、B は純正のまま」という方針と同じ動き。
更新はダウンロードと適用で約90秒、再起動を含めて約3分。

**バージョンごとの SPI の違い**(公式パッケージ全10種の比較): RFS(miniroot)は**全バージョンで同一**。KNL は **3世代だけ**(v1.51 / v1.52〜v1.71 / v2.00〜v2.60)で、**v2.00 以降の7バージョンは SPI が同一**(変わるのは HDD の rootfs だけ)。
v1.51 の本体の `updateCheck` は `newVersion: "0251"` を返したが、実際の更新は途中を経由せず、**一度で v2.60 まで上がった**(KNL は最新のものだけが書かれる)。

**SPI の KNL だけが古い本体**でも、新しい rootfs の公式アプリは動く(KNL だけ v1.51 世代にして v2.60 の公式アプリが起動することを、実機で確認した)。

## ECC 領域(`0x0D0000`)

SPIフラッシュの ECC 領域は、ブートローダ類を守る **Hamming(72,64) のパリティ**です(`scripts/spi_ecc.py` で確認・再構築できます)。

- **対象**: フラッシュ `0x010000`〜`0x08ffff`(512KB)。データ8バイトにつきパリティ1バイト。ECC領域のオフセット X のバイトは、フラッシュ `0x10000 + 8*X` からの8バイトのパリティ。
- **計算**: 8バイトの先頭4バイトを a、次の4バイトを b とするリトルエンディアンの32ビット語。a・b を16ビットずつ4つに分け、立っているビット i ごとに表 `T[i]` を XOR。
  表は16個ずつの4組で、**「ビット7が立っていて1のビットが3個以上の値を、小さい順に64個」**(`0x83,0x85,0x86,0x87,0x89,…,0xc7`)。
- **生のバイトで計算する**: 難読化されたままのフラッシュの内容に対して計算します。消去済み(`0xff`)のデータのパリティは `0x7f`、ゼロは `0x00`。
- **コピー**: 同じ内容が `0xd0000` / `0xe0000` / `0xf0000` に3つ。
- 確認: 実機のダンプ3つで、ECC領域の 65,536 バイトのうち 65,507 バイトが一致。不一致は CER(`0x040000`〜`0x0402f7`、バージョン記録)の29バイトだけで、ECC作成後に書き込まれたためとみられます。
- **ブートローダ類(`0x10000`〜`0x8ffff`)を書き換えるときは、ECC領域も計算し直して書き込んでください**(`spi_ecc.py build`)。ROMが読み出し時にこのECCで訂正する場合、書き換えた内容を元に戻される・弾かれるおそれがあります(ROMの動作は未確認)。

## 4段目ブートが持つ Blowfish の鍵

KNL/RFS を復号する Blowfish の鍵(`.dlm` と同じ16バイト)は、4段目ブート(BFWF、`0x50000`)の**データ領域の末尾に平文で置かれています**(`initrd_size` 等の文字列表のすぐ後。難読化を解いたイメージで確認)。
(以前は「平文では見つからない」としていましたが、誤りでした。)
