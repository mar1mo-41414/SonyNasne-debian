# 19. 新しいカーネルを作るために必要なもの(調査メモ)

> 「いまの Linux 2.6.29 を、新しいカーネルに置き換えたい」と実際にビルドしようと考えたときに必要になる情報の調査結果です(2026-10 時点)。
> **まだ着手していません**(静的な調査だけ)。数字は、SIE が GPL で公開しているソース([docs/09](09_tools.md) の `fetch_gpl_sources.sh`)を実際に調べたものです。
> 法的助言ではありません。成果物の配布の考え方は [docs/14](14_newer_debian_kernel_and_distribution.md) を見てください。


> ### 現状(2026-10-18)
> **新しいカーネル(Linux 6.12 LTS)+ Debian 12 は、SPI からの単独起動まで動きました。** 地デジの受信と B-CAS 復号(xcode4drv の移植)、SATA・Ethernet・SPI/MTD・MCU、前面 LED・電源ボタン・電源断と起床タイマ・状態ページも動きます。
> ビルド手順・パッチ・分かったことは **[docs/22](22_new_kernel_612.md)** にまとめました(**試験中**。SPI を書き換えるので、失敗に備えて CH341A を用意してください)。USB と、BS/CS の確認は、まだです。
> 以下は、着手前の調査メモ(2026-10 の前半)で、数字や方針は、その時点のものです。

## 0. 要約

- **やることは「SoC固有部分の移植」と「メディアエンジンのドライバ(`xcode4drv`)の移植」の2つ。** ARM/x86 のような、メインラインにほぼ揃った状態からの出発ではない。
- SoC固有のカーネル側コードは小さい: **プラットフォーム 約2,500行 + SATA 2,400行 + Ethernet(Synopsys GMAC。メインラインに近いドライバあり)+ USB 約1,500行 + SPI 500行**。メインラインのドライバを使えるものは、薄い「接着コード」で済む見込み。
- **本当に重いのは `xcode4drv`**(チューナー・B-CAS・I2C・MCU を触るメディアエンジンのドライバ)。ソースは公開されている(約13万行のC+ファームウェア5本のCの配列)が、OS依存部分が古いカーネルAPI向けで、5.x/6.x では直す箇所が多い。**これが動かないと、受信機としては使えない**(SSH・ファイルサーバーだけなら不要)。
- 制約: **SPIの空きが小さい**(KNL スロット 2.5MB、gzip 圧縮)、**UART が無い**(起動の失敗が見えない)、**ブートローダは無改造のまま使う**(引数は固定、DTB は渡せない)。
- 現実的な目標は **6.12 LTS(または 6.18 LTS)+ Debian 12(sysvinit)か、musl 系の小さなユーザランド**。ただし Debian 12 の mipsel は、すでにセキュリティ更新が終わっている(§6)。

## 1. 守る前提(ブートの制約)

| 項目 | 内容 | 出どころ |
|---|---|---|
| ブートローダ | U-Boot 1.1.3 と4段目ブートは**変えない**(署名検証は無い。CRC32のみ)。変えるのは SPI の KNL スロットだけ | [docs/02](02_boot_chain.md) |
| KNL スロット | **2.5MB(`0x280000`)**。gzip + Blowfish。4段目ブートが復号・gunzip して、物理 `0x10000000` へ展開(展開後 8MB まで)。**gzip 前提**(xz・lzma は、4段目ブートが対応していないと考えられる) | [docs/02](02_boot_chain.md)、[docs/05](05_kernel_and_direct_boot.md) |
| いまのカーネルの大きさ | 自作の 2.6.29(内蔵 initramfs 入り)で約 **2.1MB**(gzip後)。新しいカーネルは、**機能を絞って(多くをモジュールにして)2.5MB に収める**必要がある | 実測 |
| 引数 | 4段目ブートが `a0` に `0x55aabeef`、`a1`〜 に引数を渡す。中身は `root=/dev/sda2 rw ethaddr=<MAC> rd_start=… rd_size=…`。**`console=` は無い**。新しいカーネルでは **内蔵コマンドライン(`CONFIG_CMDLINE`)で上書き**するのが現実的。MAC は `ethaddr=` から取る必要がある | [docs/02](02_boot_chain.md)、`arch/mips/xcode/common/prom.c` |
| DTB | U-Boot 1.1.3 は DTB を渡せない。MIPS は **カーネルに DTB を内蔵する**(`arch/mips/boot/dts/` に置く方式)か、カーネルの末尾に付ける(`CONFIG_MIPS_RAW_APPENDED_DTB`)ことができる | カーネルの機能 |
| メモリ | 約256MB。ベンダーのカーネルは `memsize=0xf800000`(248MB)を使い、**末尾 64KB(`0x1FFF0000`〜)はレジスタ空間として除く**。メディアエンジンが使う領域の切り出し方は、`xcode4drv` 側とあわせて要調査 | `arch/mips/xcode/viper/board.c` |
| CPU | MIPS 74Kc(MIPS32r2、リトルエンディアン)。**FPU は有る**(74Kf。`/proc/cpuinfo` の `FPU V0.0` は表示だけの癖で、ベンダーのカーネルが無いと誤読していた)。Debian の mipsel はハードフロート前提だが、実機の FPU で動く | `/proc/cpuinfo`、wheezy は動作中 |
| 割り込み | CPU の IRQ 2〜7 を使い、**ViXS 独自の割り込みコントローラ(50本以上、ベクタ割り込み方式)**で配る | `arch/mips/xcode/common/interrupts.c`、`irq-mapping.h` |
| 時計 | MIPS 標準の count/compare(`VIPER_TIMER_IRQ`)。**RTC は基板に無い**(ドライバは不要) | ベンダーソース、docs/10 |

## 2. ベンダーカーネルの SoC 固有部分(行数は実測)

ベースは SIE 公開の 2.6.29(Timesys 版 `-ts-`)。メインラインにあるのは MIPS 全般(74K・MIPS32r2)で、**ViXS の SoC は無い**。

| 場所 | 行数 | 役割 | 新カーネルでの扱い | 難しさ |
|---|---|---|---|---|
| `arch/mips/xcode/`(common・viper) | 約2,500(うち約1,000行が起動用ファームの配列) | プラットフォーム初期化、引数の解釈、メモリ登録、割り込みコントローラ、リセット、2次キャッシュの代わりの PFB 無効化 | **新規に書く(`arch/mips/xcode` を新カーネルへ移植)**。irqchip ドライバ、`plat_mem_setup`、`prom_init`、restart/halt | 中 |
| `arch/mips/include/asm/mach-viper/` | 約21,500(`viper.h` が SoC の全レジスタ定義) | レジスタ定義・IRQ番号・GPIO | そのまま流用できる(資産) | 低 |
| `arch/mips/lib/xc_dma.c` | 270 | DMA補助 | 要確認(使う箇所を洗う) | 低〜中 |
| `drivers/ata/ahci_vixs.c` | 2,406 | **XCode独自の AHCI**。古い `ahci.c` のフォーク。SCR読み書き・`qc_issue`・`rtf` の上書き、クロック設定、ホスト選択(ホスト2本) | メインラインの `libahci` + `ahci_platform` に、SoC 固有の処理(PHY・クロック・SCR)だけを足す形に作り直す | 中〜高 |
| `drivers/net/synopsys_new/` | 約9,600 | Synopsys GMAC の**参考ドライバ**(HWチェックサム、TSO) | メインラインの **`stmmac`(dwmac1000)** を使う。接着コード(プラットフォームデータ・PHY・リセット)が要る。TSO は無くてよい | 中 |
| `drivers/usb/host/ehci-sduh.c`(723)、`ohci-sduh.c`(615)、`core/sduh-hcd.c`(130) | 約1,500 | EHCI/OHCI。**レジスタが「間接アクセス」**(`sduh_usb_indirect_reg_lock`)なので、標準の `ehci-platform` ではそのままは動かないとみられる | EHCI/OHCI の「プラットフォーム」ドライバに、**間接アクセス用の読み書き関数**を差し込む | 中 |
| `drivers/spi/xcode_spi.c` | 492 | SPIコントローラ(SPIフラッシュが繋がる) | 新しい SPI コントローラドライバとして移植。フラッシュは `spi-nor`(`m25p80` の後継)。パーティションは `user1` `0x0`〜`0xD00000`、`user2`(NV)`0xD00000`〜 | 低〜中 |
| `drivers/rtc/rtc-vixs.c`、`serial/vixs_serial.c` | 352 / 1,579 | RTC(基板に無い)/ UART(基板に出ていない) | **不要**。ただしデバッグ用に UART を生かせるなら有用(§5) | — |
| `drivers/mtd/nand`、`mmc/host/xcode4_sdh.c`、`usb/gadget/vixs_*`、`staging/xcode`(IPC)、`yaffs2`、`c2port_xcode.c` | — | NAND・SDカード・USBデバイス・IPC・yaffs2・C2ポート | 使っていない(`.config` で無効、または基板に無い)。**C2ポートは有効**(MCU の書き込みIFの可能性。要調査) | — |
| `firmware/xcode`、起動用ファームの配列(`boot_fw.c`) | 6,892B + 8,980B | **リセット時にコプロセッサへ載せる起動ファーム**(通常用とUSB-CDC用の2本) | `restart` のために必要。**バイナリはそのまま流用** | 低 |

補足:

- **リセット(`viper_machine_restart`)は、普通の再起動ではない。** イーサの回路をリセットし、コプロセッサ(PROC2)を止めて起動ファームを `0xA0000800` へ載せ、**I2C0 で MCU へ書き込み**(MCU の再起動コマンド `[0x04,0x02]` に相当する値)、PROC2 を起こす。新しいカーネルでも同じ手順が要る。`halt`/`power_off` は `wait` ループだけ(本当の電源断は MCU への `[0x02,0x01]`。[docs/06](06_mcu_watchdog.md))。
- **2次キャッシュの代わりに「PFB」を持つ。** `bcache_ops`(`bc_wback_inv` / `bc_inv`)で、`PROC5_PFB_INVALID_ALL` レジスタへ書いて無効化している。**DMAの一貫性に関わる**ので、移植で外してはいけない(MIPS の `bcops` の仕組みは新しいカーネルにもある)。
- **PCI は、このボードでは使っていないとみられる**(`mips_pcibios_init` はダミーのバス)。新しいカーネルでは省ける見込み(要確認)。

## 3. `xcode4drv`(メディアエンジンのドライバ)の移植

ソース: `AVDriver7GSP47.zip` の中の `XC4_AVDriver_Src_20140709.tar.bz2`(**公式ファームの `xcode4drv.ko` と同世代**。`Boxster, XCode (R) Driver Version 6.4.0.15, built at Jul 29 2014`)。

| 内訳 | 行数 |
|---|---|
| ドライバ本体(`kdrv/drv`・`meminit`)のC(ファームの配列を除く) | 約12.9万 |
| ヘッダ(`kdrv/inc`、`commsdk`) | 約4万 |
| ファームウェア5本(`firmware.c`・`fmencryptedoutput*.c`・`kernel_image.c`・`fmsecond.c`。Cの配列) | 約71万(ほとんどデータ) |

OS 依存の部分は小さい(主に `Os_Linux_Init.c`、`Os_Wrp_Funct.c` など)。新しいカーネルで**直すべき点**(grep の結果):

| 古いAPI | 場所 | 新カーネルでの対応 |
|---|---|---|
| `get_fs()` / `set_fs()` | `Os_Linux_Init.c` | `set_fs` は **5.10 で削除**。ファイル入出力に使っているなら `kernel_read` 等へ、不要なら除く |
| `sys_call_table`(`FILE_IO` 時) | `Os_Linux_Init.c` | MIPS 版(`CONFIG_MIPS`)では使われていない。**エクスポートされていないので使えない**。`#if` で除かれていることを確認 |
| `pci_alloc_consistent` / `pci_map_*` | 8ファイル・124行 | **PCI DMA ラッパは 5.18 で削除**。`dma_alloc_coherent` / `dma_map_*` に置き換え(機械的) |
| `<linux/config.h>`・`<linux/autoconf.h>`・`<linux/wrapper.h>`・`<linux/iobuf.h>`・`devfs` | 数ファイル | 古いヘッダ。`#if LINUX_VERSION_CODE` で囲われていれば除ける。`generated/autoconf.h` に替える |
| `register_chrdev(120, "boxster", …)` | `Os_Linux_Init.c` | 今も使える(メジャー番号 120 の `/dev/vixs/xcodedrv*` は変えない) |
| `get_user_pages`、`remap_pfn_range`、`tasklet`(42行)、`init_MUTEX` 等 | 数ファイル | 引数・名前が変わった箇所を直す(`tasklet_setup` など)。件数は少ない |
| `KSEG1` 直書き(`0xA0000000`、`0xBFFF0000`) | 4ファイル・5行 | 32bit MIPS なら今も使える。**HIGHMEM を使わず、`XCODE_MEMBASE` が直接マップされること**が前提 |
| ioctl のユーザー空間ABI | — | 変えない(自作ツール `nasne-recpt1` などがそのまま動く) |

見積り: `xcode4drv` 自体は**数日〜数週間の作業**(機械的な修正が多い)。ただし、**動作確認は実機が要り、実機でしか確かめられない**(メディアエンジンのファーム読み込み、DMA、I2C、B-CAS)。

## 4. 新しいカーネルの選び方

2026-10 時点で kernel.org が保守しているLTS(想定のEOL): **6.18(2028-12)・6.12(2028-12)・6.6(2027-12)・6.1(2027-12)・5.15(2026-12)・5.10(2026-12)**。([kernel.org](https://www.kernel.org/category/releases.html))

| 候補 | 利点 | 欠点 |
|---|---|---|
| **6.12 LTS(推奨の第一候補)** / 6.18 LTS | 2028 年末まで更新あり。OpenWrt の開発ブランチが MIPS(bcm47xx の74K向け)の既定を 6.12 にしており(2025-11 のコミット)、**MIPS の実績が新しい**。`stmmac`・`ahci_platform`・`spi-nor` が揃っている | `xcode4drv` の移植は最も多い(§3) |
| 6.6 / 6.1 LTS | 2027 年末まで。移植の差は少なめ | 更新が短い |
| 5.10 / 5.15 LTS | `xcode4drv` の移植が少し楽(`pci_*` が残る) | **2026-12 で終わる**。今から始めるのは無意味 |

MIPS アーキテクチャ自体は、メインラインで**保守されている**(低速)。Debian が mipsel を捨てたのは、ビルド機・保守者の不足で、**カーネル側の問題ではない**。

## 5. 開発の進め方(UARTが無いのが最大の壁)

- **UART は基板に出ていない。** 起動に失敗しても何も見えない。使える観測手段:
  1. **`netconsole`**(ネットワークが立ち上がってから。GMAC が動けば `printk` を UDP で送れる)。
  2. **`pstore`/`ramoops`**: 再起動をまたいで DRAM が保たれる場合、パニックのログを次の起動で読める(SoC の ROM が DRAM を消さないかは要確認)。
  3. **LED(MCU の表示コード)**: `[0x10, code]` で PWR LED の状態を変えられるので、**起動の進み具合の目印**に使える([docs/06](06_mcu_watchdog.md))。
  4. SoC には UART ブロックがある(`UART0〜3`)。**基板のパッドにはんだ付けして取り出せるなら**、最も確実(要調査)。
- **毎回 SPI を書き換えない開発ループ**: 自作の 2.6.29 で **kexec**(`CONFIG_KEXEC`)を有効にして、実機の Debian から新しいカーネルを `kexec` で起動して試す(SPI に触れない)。メディアエンジンのファームをまだ読み込んでいない状態(`xcode4drv` を入れる前)で試すのが安全。MIPS は kexec に対応している。**未検証**。
- **安全網**: 書くのは KNL スロット A だけ。元に戻すには、[docs/16](16_spi_backup_dlm.md) の復元版 `.dlm`(`--mode restore`)か、CH341A 等のSPIライタ。**BKNL(スロットB)への自動切り替えは当てにならない**([docs/05](05_kernel_and_direct_boot.md))。
- **ビルド環境**: ホストの Debian/Ubuntu の `gcc-mipsel-linux-gnu`(gcc 12/14)で 6.x がビルドできる想定(古い gcc 4.3.2 は不要)。`CONFIG_CPU_MIPS32_R2`、gzip 圧縮、`CONFIG_MIPS_FP_SUPPORT`(FPUエミュレータ)、内蔵 initramfs、内蔵コマンドライン。

## 6. ユーザランドの選択肢(カーネル 3.2 以上の後)

| 選択肢 | 状況 |
|---|---|
| **Debian 12 bookworm(mipsel)+ sysvinit** | glibc 2.36 が要求するカーネルは 3.2 以上(これは満たす)。systemd は新しいカーネル機能が前提なので、`sysvinit-core` を使う。**ただし、セキュリティ更新は期待できない**: 通常のサポートは 2026-07 に終わり、LTS(〜2028-06)の対象アーキテクチャは amd64・i386・arm64・armhf・ppc64el で、**mipsel は無い**([Debian ニュース](https://www.debian.org/News/2026/20260712.en.html))。 |
| Debian 13 trixie | **mipsel は公式のアーキテクチャから外れた**(リリースノート)。使えない |
| **musl 系**(OpenWrt 風 / Buildroot / Void など) | 新しい OpenSSH・busybox・各種ツールを載せられる。OpenWrt の bcm47xx(mips74k、musl、gcc 14)は、**74K向けのクロスビルドの実例**として参考になる([OpenWrt の Makefile](https://git.openwrt.org/openwrt/staging/jow/plain/target/linux/bcm47xx/Makefile))。`nasne-recpt1` は libc 不使用の静的バイナリなので、どのユーザランドでも動く |

## 7. 段階的な計画(案)

1. **ビルドの足場**: 6.12 の MIPS32r2 最小構成を、Debian の cross gcc でビルドし、gzip 後 2.5MB に収まるか確かめる(まだ実機で動かさない)。
2. **プラットフォーム**: `arch/mips/xcode` を新カーネルへ。irqchip・メモリ・引数・restart。**LED と netconsole で、初期化のどこまで進むかを見る**(kexec で)。
3. **SATA**(`ahci_platform` + 固有処理)→ **initramfs から p3 のルートへ switch_root** ができれば、最小の「起動する新カーネル」。
4. **GMAC(`stmmac`)・USB(間接アクセス)・SPI**。ここまででメディアエンジン抜きの Debian/musl サーバーとして使える。
5. **`xcode4drv` の移植**(§3)。受信機として使えるようにする。
6. SPI の KNL スロットへ書いて(復元版 `.dlm` を安全網に)、実機で通す。

## 8. 未調査・リスク

- メディアエンジン(別プロセッサ)が使う DRAM 領域の切り出し方、PFB 無効化の要否が移植後も同じか。
- 割り込みコントローラの詳細(ベクタ割り込みのマスク・確認レジスタ)と、IRQ 番号の割り当て(`irq-mapping.h`)の確認。
- PHY の初期化(Ethernet が上がるまでのクロック・リセット)、EEE の無効化。
- `ahci_vixs.c` の固有処理(SCR の読み書きの上書き、`rtf` の取り方)が、メインラインの AHCI で再現できるか。
- C2 ポート(MCU)の用途。
- 2.5MB に収まるか(gzip しか使えない)。収まらないときは、モジュール化と、initramfs の最小化。
- 実機が1台のため、**失敗したときの復旧は SPI ライタ頼み**になりうる。

## 付録: 実測の根拠

- ベンダーカーネル: `gpl_src/build/mips-linux-2.6.29`(SIE 公開のソース。`.config` は `CONFIG_MIPS_XCODE=y`、`CONFIG_SATA_AHCI=y`、`CONFIG_SYNOPSYS=y`、`CONFIG_USB_SDUH_HCD=y`、`CONFIG_SPI_XCODE=y`、`CONFIG_RTC_DRV_XCODE=y`、`CONFIG_C2PORT_XCODE=y`、`CONFIG_XFS_FS=y`、`CONFIG_EXT3_FS=y`、`CONFIG_HZ_100=y`、`CONFIG_KEXEC` は無し、`CONFIG_NETCONSOLE` は無し)。
- vanilla 2.6.29 との差分で、ベンダー固有として挙がるのは `arch/mips/xcode`・`mach-viper`・`ahci_vixs`・`synopsys_new`・`sduh` 系・`xcode_spi`・`rtc-vixs`・`vixs_serial`・`nand_xcode`・`xcode4_sdh`・`vixs_usbd`・`staging/xcode`・`firmware/xcode`・`yaffs2` など(ほかに、Timesys が取り込んだ上流の修正が多数)。
- ドライバ: `AVDriver7GSP47.zip`(`xcode4drv.ko`、`rc.xcode4`、ソースのtar、`commsdk` ヘッダ)。
