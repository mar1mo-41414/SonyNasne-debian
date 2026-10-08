# 01. ハードウェア

対象: nasne **CECH-ZNR2J**(初期型、基板シルク `ST15010-D70 REV8`)。

## 主要部品

| 部品 | 型番 | 役割 |
|---|---|---|
| メインSoC | **ViXS XCode 4210**("Viper") | MIPS 74Kc コア(512MHz)+ メディア処理(MPEG-2/H.264)+ SATA / USB / Ethernet。Linuxはこのチップ上で動く |
| 復調IC | **東芝 TC90532系**(基板刻印 `TC90532XBG`) | 地上波(ISDB-T)/ BS・CS(ISDB-S)の復調 |
| DRAM | Samsung K4B1G1646G-BCH9 ×4 | DDR3 1Gbit ×4 = **512MB**(513MHz) |
| ブートフラッシュ | **Spansion S25FL128P**(SPI NOR、16MB、基板リファレンス U44) | U-Boot・カーネル・miniroot・設定を格納 |
| Ethernet PHY | Realtek RTL8211EG | ギガビットEthernet |
| MCU | 型番未確認(基板上のマイコン) | 電源・ウォッチドッグ(I2C)。[docs/06](06_mcu_watchdog.md) |
| B-CASスロット | miniB-CAS | 放送のデスクランブル用カード |

> 初代nasneの分解記事(日経テクノロジー)にも、復調ICとして TC90532XBG、SoCとしてViXS製とある。

## デバッグ用ポート

基板にはUART / JTAG用に見えるランド(5ピン×2、2×5ピン×1)があるが、**いずれも信号が配線されていない**
(導通トレースの結果、GND・電源以外どこにも繋がっていない)。Silicon Labs C2ポートも実装なし。
このため **シリアルコンソール・JTAGは使えない**。デバッグはネットワーク経由とHDD/SPIのオフライン検証だけで行う。

## CPU とメモリ

- `system type: Viper`、`MIPS 74Kc`(MIPS32r2、mips16 + DSP ASE)、BogoMIPS 約256。
- 物理メモリ(512MB = `0x00000000`〜`0x1fffffff`)の使われ方:

| 物理アドレス | 用途 |
|---|---|
| `0x00000000`〜`0x0fffffff`(256MB) | メディアエンジン(映像処理)のメモリ。Linuxの管理外 |
| `0x10000000`〜`0x1f7fffff`(248MB) | **Linuxが使うRAM**(`CONFIG_XCODE_MEMBASE`、`memory: 0f800000 @ 10000000`) |
| `0x1f800000`〜`0x1fffffff`(約8MB) | ブートローダとの共有領域。**システム情報ページ `0x1ffef000`**(個体情報、MAC、ファーム版数など) |
| `0x1fff0000`〜(64KB) | **SoCのレジスタ窓**(KSEG1 `0xBFFF0000`)。Board_ID(`0xBFFF00FC`)、GPIO、I2C、WDT など |

- SoCレジスタの主なもの: `0xFC04`+`0x10`×N = GPIO ポートN(`0xFC64` = ポート10 = LED)、I2Cブロック0/1、SATA(`xcode-ahci`)、USB(`sduh-ehci/ohci`)。
- U-Bootは既定環境の `Board_ID=0x1202` をSoCレジスタ `0xBFFF00FC` に書き、カーネルがこれを読む。
  `0x1202` はSDK基板(C2ポートあり)ではないため、dmesgに `BID 00001202 is not a valid XC42xx SDK board` と出る(正常)。
- ドライバソース上の基板名: `BID_VIPER_1202 = XC42xx_H5TQ1G63DFR_H9C_2x2x16_513MHz`。

## SPIフラッシュ

Linuxからは MTD として見える: `mtd0`(`0x000000`〜`0xcfffff`、ブート領域〜RFS)、`mtd1`(`0xd00000`〜`0xfeffff`、JFFS2)。
末尾64KBは非公開。中身の地図は [docs/02](02_boot_chain.md)。

## ディスク

HDDはMBRで3区画。ブートローダやカーネルはHDDには無い(すべてSPIフラッシュ側)。

| 区画 | ラベル | FS | 中身 |
|---|---|---|---|
| p1(256MB) | sys1 | ext3 | ファーム更新パッケージ領域(`.dlm`ファイル群)。[docs/03](03_firmware_format.md) |
| p2(1GB) | sys2 | ext3 | 純正ルートファイルシステム。**毎起動、p1の`.dlm`から丸ごと再構築される** |
| p3(残り) | user | XFS | 録画データ(`.hai`、DTCP-IP暗号化)、設定、共有フォルダ。**Debian構成ではここをext3にしてDebianを置く** |

実機内部でのデバイス名は `/dev/sda1`〜`sda3`(別のPCに繋ぐと名前は変わる)。
