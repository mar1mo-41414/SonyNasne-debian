# 22. 新しいカーネル(Linux 6.12 LTS)+ Debian 12 で動かす(試験中)

nasne の自作カーネルを、Linux 2.6.29 から **6.12 LTS** に置き換え、Debian 12(bookworm、sysvinit)を動かす手順と、分かったことをまとめます。
調査の経緯と、新カーネルが必要になる理由は [docs/19](19_new_kernel_survey.md)、旧カーネル(2.6.29)の直起動は [docs/05](05_kernel_and_direct_boot.md) です。

> ⚠️ **試験中です。SPI(ブートローダの置き場)を書き換えるので、失敗すると、CH341A などで書き戻すまで起動しなくなります。**
> 4段目ブートの A/B フォールバックは、カーネルが「壊れている(CRC 不一致)」ときだけ働き、**起動はするが固まる**カーネルには効きません。
> 試す場合は、**SPI のバックアップ(全域)と、CH341A+書き戻しの手段**を、先に用意してください([docs/04](04_hdd_recovery_guide.md)、[docs/16](16_spi_backup_dlm.md))。
> 6.12 の Debian 12 は、旧カーネル+Debian 7 wheezy(`nasne_setup.py` の既定)より、まだ実績が少ないです。**普段使いは、旧カーネル版を勧めます。**

## 1. 動くもの・動かないもの(作者の実機での確認)

| 項目 | 状態 |
|---|---|
| SPI からの単独起動(cold boot。電源投入から) | ✅ 繰り返し確認 |
| Ethernet(1Gbps)・SATA・XFS/ext4・loop・sshd・apt | ✅ |
| SPI フラッシュ(MTD。`/dev/mtd0`・`mtd1`。読み・消去・書き込み) | ✅(`mtdtool` が旧カーネルと同じように動く) |
| MCU(ウォッチドッグ停止・PWR LED・電源断・再起動・起床タイマ) | ✅(カーネルのドライバ。`reboot` は MCU へ再起動を頼む) |
| **地デジの受信・B-CAS 復号**(`nasne-recpt1`。xcode4drv を 6.12 に移植) | ✅(HTTP 配信・状態ページも) |
| 前面 LED(PWR 緑・異常で赤、REC=チューナー使用中、LAN)、電源ボタン長押しでの停止、`poweroff` と起床タイマ | ✅ |
| BS/CS | ❓(旧カーネルと同じ `nasne-recpt1` なので、旧カーネルで受信できる環境なら動くはずだが、6.12 では未確認) |
| EPGStation の予約に合わせた起床(`nasne-waked`)→録画→自動オフ | ✅(予約の約4分前に起床、録画の180秒後に自動オフを、実機で確認) |
| USB ホスト(EHCI+OHCI。USB メモリの ext4/vfat、USB マウス、ホットプラグ) | ✅([§7](#7-usb-ホストehci)) |
| スワップ(`/data/swapfile`)・SysRq | ✅(RAM が約240MBなので、スワップを有効にしている。[§5](#5-knl-スロットの容量)) |

## 2. 試す手順(概要)

前提: Linux のホストに、`gcc-mipsel-linux-gnu`(クロスコンパイラ)、`make`・`gcc`・`flex`・`bison`・`bc`(カーネルのビルド)、`debootstrap`・`qemu-user-static`(rootfs 用)、`python3`、`curl`、`patch`、`unzip`。公式ファームを使った HDD と、**SPI のバックアップ**。

1. **カーネル**(ビルドは数分〜20分):
   ```bash
   newkernel/initramfs/mk_root.sh $PWD/newkernel/initramfs/root_boot boot      # 起動用の小さな init(約4KB。busybox なし)
   INITRAMFS=$PWD/newkernel/initramfs/root_boot newkernel/build.sh              # kernel.org から 6.12 を取得してビルド(成果は newkernel/out/new/)
   ```
   `newkernel/out/new/vmlinux.knl.bin` が、KNL スロットに入れる中身です。ビルドの最後に、gzip 後の大きさと上限(2,621,376)が出ます。余裕は約60KB です(クリーンなビルドで gzip 後 約 2,560,000 バイト)。機能を足すときは、先にこの大きさを確かめてください([§5](#5-knl-スロットの容量)、[docs/23](23_kernel_feature_inventory.md))。
2. **チューナーのドライバ(xcode4drv.ko)**:
   ```bash
   newkernel/xc4drv/build.sh        # GPL ソース(AVDriver7GSP47.zip)を取得・展開し、パッチを当ててビルド → newkernel/out/xcode4drv.ko
   ```
   ファーム(Sony 由来の配列)を含むため、`.ko` は**配布しません**。自分でビルドしてください。
3. **Debian 12 の rootfs**(root が必要):
   ```bash
   sudo scripts/newkernel/build_rootfs12.sh rootfs12                       # debootstrap(mipsel)。SSH の公開鍵を入れる
   sudo scripts/newkernel/make_rootfs12_image.sh rootfs12 root.img 8192    # 8GB の ext4 イメージ(ジャーナルなし)
   ```
   `root.img` を、HDD の p3(XFS)の **`/.nasne-debian12/root.img`** に置きます([docs/15](15_p3_structure_and_hai.md))。起動用の init が、p3 を `/dev/sda3` からマウントして、この `root.img` を loop で `/` にします。
4. **KNL セグメントを作って、SPI のスロット A に書く**(旧カーネルの Debian が動いている nasne で):
   ```bash
   python3 scripts/build_knl.py build <自分のSPIバックアップ.bin> KNL_612.bin --slot KNL --kernel newkernel/out/new/vmlinux.knl.bin
   python3 scripts/build_knl.py verify KNL_612.bin          # 大きさと CRC の自己検証(0x280000 バイトに収まること)
   scp KNL_612.bin root@<nasne>:/tmp/ ; ssh root@<nasne> 'mtdtool flash /dev/mtd0 0x100000 /tmp/KNL_612.bin && mtdtool verify /dev/mtd0 0x100000 /tmp/KNL_612.bin'
   ```
   書き込むのは**スロット A だけ**です(スロット B の出荷時カーネルは、そのまま残る)。電源を入れ直すと、6.12 で起動します。
   旧カーネル(2.6.29)が動いていない場合に、6.12 のまま SPI を書くための最小のツール `scripts/tools/nasne_spitool.c`(`/dev/mem` から SoC の SPI を直接叩く。書き込めるのは A スロットだけ)も付けています(ビルド: `mipsel-linux-gnu-gcc -static -O2 -o nasne_spitool nasne_spitool.c`)。
5. **チューナーと常駐サービス**(6.12 の Debian 12 に ssh で入って):
   ```bash
   NASNE_IP=<nasne の IP> scripts/newkernel/install_tv12.sh --now        # xcode4drv.ko・起動時の読み込み・nasne-recpt1 等
   NASNE_IP=<nasne の IP> scripts/newkernel/install_services12.sh --apt  # LED・状態ページ・電源ボタン・電源断と起床・時刻合わせ
   NASNE_IP=<nasne の IP> scripts/newkernel/install_modules12.sh         # カーネルのモジュール(USB・vfat など)
   NASNE_IP=<nasne の IP> scripts/newkernel/setup_swap12.sh 512          # スワップファイル(/data/swapfile。RAM が約240MBなので推奨)
   ```
6. **元に戻す**: スロット A に、元のカーネル(`knl_new.bin` 等。`nasne_setup.py` が作る)を、同じ `mtdtool flash` で書き戻す(6.12 のままでは、`mtdtool` が動く間なら可能)。起動しなくなったら、CH341A で SPI を書き戻す。

## 3. 作った物

- `newkernel/overlay/`: 6.12 のツリーへ重ねる、nasne(ViXS XCode 4210 "Viper")用のコード。プラットフォーム(`arch/mips/xcode/`)、DT(`nasne.dts`)、`nasne_defconfig`、ドライバ: SATA(`ahci_xcode.c`)、Ethernet(`xcode_gmac.c`)、I2C(`i2c-xcode.c`)、MCU(`nasne_mcu.c`)、SPI(`spi-xcode.c`)。`newkernel/patches/` は、Kconfig・Makefile への小さな差分。
- `newkernel/xc4patches/` と `newkernel/xc4drv/`: xcode4drv の 6.12 向けパッチと、ビルドの道具([§4](#4-xcode4drv-の移植))。
- `scripts/newkernel/`: rootfs の作成(`build_rootfs12.sh`・`make_rootfs12_image.sh`)、実機への導入(`install_tv12.sh`・`install_services12.sh`)、サービス(`debian12/`)。早期コンソール(UDP ブロードキャスト、ポート 6666)の受信側 `netlog.py`。
- 起動ログの見方: UART が無いので、**Ethernet 経由の早期コンソール**(カーネル自身が GMAC を直接叩いて UDP ブロードキャストで出す)を使う。`netlog.py` で受ける。ビルド時に `NASNE_EXTRA_BOOTARGS`(または `local.env`)で、カーネルの引数(固定 IP、netconsole など)を足せる。

## 4. xcode4drv の移植

- GPL ソース(`XC4_AVDriver_Src_20140709`。ドライバのバージョン 6.4.0.15。公式の `.ko` と同じ版で、ファームの配列も同一)に、**5 ファイル 371 行のパッチ**。`newkernel/xc4drv/build.sh` が、zip を展開して、パッチを当てて、公式 `drvbuild/Makefile` と同じ構成(`-DLINUX -DBOXSTER_DRIVER -DBCAS -DXC_SOC_BUILD -DCUST4_BUILD -DXC_FW_BUILD -DXCODE3`)でビルドする(約 20 秒)。
- 直した所: `asm/system.h`・`DECLARE_TASKLET`・`access_ok`・`do_mmap`/`do_munmap`(→`vm_mmap`/`vm_munmap`)・`get_user_pages`・`mmap_sem`・`VM_RESERVED`/`vm_flags`・タイマー(`data` が無くなった)・`pte_offset_map`・`pci_*` の DMA(→ platform_device を作って `dma_*`)など。互換定義は `newkernel/xc4drv/xc_compat.h`。
- **モジュール対応と KNL スロット**: モジュールを使うには `MODULES=y` が要るが、輸出シンボルの表で、gzip 後が約 88KB 増えて KNL スロットに収まらない。**`TRIM_UNUSED_KSYMS`** で、`xcode4drv.ko` が使う約 65 シンボルだけに絞る(`newkernel/xc4drv/ksyms_whitelist.txt`)。
- **I2C ブロック1(チューナー)が応答しない問題**: ファームが基板(チューナー部)の電源を入れる前に、I2C1 の SDA/SCL が Low のため、I2C1 のマスターが「バスビジー」(`I2C1_COMMAND` の bit29)に張り付き、復調 IC が全く ACK しない(戻り値 `0x0e`)。`PPC_SFT_RSTN` の bit0 を一度落として戻すと復帰する。**初期化の最後に、バスビジーならこのソフトリセットを1回行う**処理(`xc_i2c_unwedge`)をパッチに入れた。
- 旧カーネルとの SoC レジスタ全域の差分(`physio` で 4KB ずつ)を取る方法が、切り分けに有効だった。

## 5. KNL スロットの容量

KNL スロットは 0x100000〜0x37ffff(約2.5MB)で、4段目ブートが gzip で展開するため、**gzip 後で 2,621,376 バイト以下**にする必要があります。**この上限に効くのは、カーネルに組み込む機能(`=y`)と、モジュール用に残すエクスポートシンボルの表だけ**で、モジュール(`=m`。USB・vfat など)は rootfs の `/lib/modules` に置くので影響しません。

`nasne_defconfig` で外している物: `NET_SELFTESTS`・`ETHTOOL_NETLINK`・`PAGE_POISONING`・`MQ_IOSCHED_KYBER`・`BLK_DEV_BSG`・`DEBUG_FS`・`INET_DIAG`・`KEXEC`・`MARVELL_PHY`(PHY は Realtek で Generic PHY ドライバ)・`COMMON_CLK`・`SUSPEND`・`PTP_1588_CLOCK`・`HWMON`・`PPS`・`FW_LOADER` ほか。入れている物: `MAGIC_SYSRQ`・`SWAP`。**現在の余裕は約 60KB**(クリーンなビルドで gzip 後 約 2,560,000 バイト)。詳しい機能一覧と、足せる機能の費用は [docs/23](23_kernel_feature_inventory.md)。

## 6. SoC・基板の癖(新カーネルで必要だったこと)

- **物理 `0x10000000`〜`0x1003ffff`(256KB)は使わない**(`memblock_reserve`)。ここへ SATA の DMA で書くと、読んだデータが化ける(ベンダーのカーネルでは、イメージ先頭のゼロ埋めで、Linux が使っていなかった領域)。
- **Ethernet の cold 初期化**(`xcode_eth_cold_init`): 電源投入直後は、4段目ブートが Ethernet を触らないので、クロック・PHY のリセット(専用 GPIO3)・ブロックのリセット・`ETH_CTRL_REG` を、ベンダーの `turnon_eth_clk`/`plat_eth_init` と同じ手順で設定する。これが無いと、カーネルは動くがネットワークが沈黙する(旧カーネルの kexec では、先に動いていたカーネルが済ませていたので、気づけなかった)。
- ほかに、割り込みの `IntCtl.VS`、GMAC・AHCI レジスタの間接窓、SATA の SoC 割り込み、PFB(先読みバッファ)の無効化、DMA 後のキャッシュ・フラッシュなど。詳しくはソースのコメント。
- **SPI/MTD**: SPI コントローラは PPC ブロックにあり(ポーリングで足りる)、SPI ピンは NAND と共有で PPC 側もフラッシュを触るので、ハードウェア・ミューテックス(`MIPS_INTERLOCK2`、ID 11)を取り、パッドを SPI 側に向けてから使う。区画は旧カーネルと同じ(`user1`=0〜0xd00000、`user2`=NV)。
- **Ethernet の MMC(統計カウンタ)割り込み**: Synopsys GMAC の MMC は、カウンタが半分(0x80000000)に達すると割り込みを上げ続けます。送信バイトが累計約 2GB(2^31)に達した時点で、共有の IRQ 6 が止まらなくなって**本体全体が固まる**不具合があり、`xg_hw_start()` で MMC の割り込みマスク(`0x10c`・`0x110`・`0x200`)を全部立てて直しました(3.7GB の送信で確認)。**これより前の版では、大量の送信(録画の配信など)で、累計約 2GB で固まります。更新してください。**
- **再起動・電源断**: MCU へのコマンド(`[0x04,0x02]` 再起動、`[0x02,0x01]` 電源断、起床タイマ `[0x02,0x02,u32]`)。6.12 には再起動の処理が無く、`reboot` が止まるので、MCU ドライバに `SYS_OFF_MODE_RESTART` を足した。
- **2GiB を超える loop の裏ファイル**: 起動用の `init` が `root.img` を開くとき、**`O_LARGEFILE` が必要**(32bit では、これが無いと 2GiB 以降への書き込みが失敗する)。
- **MCU の操作はカーネル(sysfs)に一本化**: `/sys/bus/i2c/devices/0-0010/{status,raw,display_code}`。xcode4drv の I2C と、カーネルの I2C ドライバが、同じハードウェアを取り合わないため。既存のサービスが呼ぶ `mcui2c` は、sysfs を使う互換版(`scripts/newkernel/debian12/mcui2c`)に差し替える。

## 7. USB ホスト(EHCI)

背面の USB ポートを使えます(ベンダーの 2.6.29 の `ehci-xcode` 相当を、6.12 の EHCI ドライバへ載せ替え)。USB メモリ(MBR/FAT32)が高速(480Mbps)で認識され、`dd` の読み出し・書き込みが正しいことを確認しています。

- **レジスタは間接アクセス**: EHCI/OHCI のレジスタは直接マップされず、SoC の窓(オフセット 0x2000〜0x2014)経由。`newkernel/overlay/arch/mips/xcode/usb.c` の `xcode_sduh_readl/writel`、パッチ `0012-usb-ehci-xcode.patch` が `ehci.h` のレジスタ読み書きを差し替えます。
- **ドライバ** `newkernel/overlay/drivers/usb/host/ehci-xcode.c`(モジュール `ehci-xcode`): VBUS の GPIO、クロック、リセット、PHY のクロック(内部 PLL・48MHz)、割り込み(IP6 共有)の順に初期化します。
- カーネルの KNL スロットに余白が無いので、`USB`・`USB_EHCI_HCD`・`USB_EHCI_XCODE`・`USB_STORAGE` は**モジュール**です(`install_modules12.sh` で入れ、`modprobe`/自動読み込み)。
- **OHCI(コンパニオン)**: low/full-speed の機器(キーボード・マウス・USB 1.1 のハブ)は、EHCI がポートを OHCI へ渡して扱います。`newkernel/overlay/drivers/usb/host/ohci-xcode.c`(モジュール `ohci-xcode`)とパッチ `0013-usb-ohci-xcode.patch`。OHCI のレジスタも間接アクセス(別のアドレスレジスタ)で、電源・クロック・PHY の初期化は EHCI と共有します。USB マウス(full-speed)が OHCI 側で認識され、割り込み転送(マウスのレポート)が約 2400 回、エラーなしで通ることを確認しました(HID ドライバは入れていません)。
- **USB メモリのマウントと書き込み**: FAT はモジュール(`vfat`、`nls_*`)です(`modprobe vfat; mount -t vfat -o noatime /dev/sdb1 /mnt/usb`)。FAT のモジュールを足すとカーネルのエクスポートシンボル表が増えて KNL スロットに収まらなくなるので、使っていない PTP を切りました。32GB の USB メモリ(FAT32、high-speed)で、1GiB のファイルのコピーと、500 個の小ファイル・長い日本語のファイル名の書き込みを、再マウントしても一致することを確認しました。速度の目安は、書き込み約 12MB/s、読み出し約 21MB/s です。ext4 で作り直した USB メモリ(組み込み済みなのでモジュール不要)でも、1GiB・小ファイル 1000 個・長い日本語名・`fsync` 付きの追記・`O_DIRECT` の書き込みと、`umount` 後の `e2fsck` まで、問題ありませんでした。
- **ホットプラグ**: 延長ケーブルで USB メモリを26回抜き差ししても、毎回列挙・切断が検出され、列挙の失敗やハングはありませんでした。読み出し中・書き込み中に抜いても、I/O エラーで終わるだけでシステムは動き続け、再挿入すると ext4 のジャーナルが再生されて `e2fsck` がクリーンになりました。
- **未対応**: 過電流の検出(ベンダーのドライバにもなく、基板で過電流の信号がどのピンに来ているか不明)、`fsck.vfat`。

## 8. 既知の問題・制限

- 4段目ブートの A/B 切り替えに使う、SoC の「システム情報ページ」の `+6` は、**起動のたびに作り直され**、soft reset をまたいで残らない。**カーネル自身が「次は B で」と指示して戻る**ことはできない(CRC 不一致のときだけ B)。
- 6.12 から 6.12 への `kexec` は、kexec-tools の mips 版が配置できず、使えない(旧カーネルから 6.12 へは、開発用の `kexec` で起動できた)。
- RTC が無いので、起動直後の時計は、`fake-hwclock` が戻した「電源を切った時点」の時刻。ネットワークが上がった時点で NTP で合わせる(`ntpdig -S -4`。カーネルに IPv6 が無いので `-4`)。**`ntpdig -S` が同時に2つ走ると、どちらも同じずれを測って2回進める**(電源を切っていた分だけ時計が進む)ので、`flock` で1つずつ走らせる(`scripts/newkernel/debian12/if-up-nasne-time`)。時計が狂うと、`nasne-waked` の起こす時刻もずれる。
- Debian 12 の mipsel は、セキュリティ更新の対象外です(docs/19)。
- ウォッチドッグを止めるのはカーネルの MCU ドライバなので、そのドライバが起動しないと、約333秒でリセットが繰り返される。
- 電源ボタンは**背面**にあり、長押しは**約4秒(3秒以上5秒未満)**で反応する。
- D キャッシュにエイリアスがあるため(64KB・4-way)、**共有マップ(`MAP_SHARED` の匿名メモリやファイル)のページを `O_DIRECT` の DMA 先にすると、古い内容が見える**ことがあります(MIPS 一般の制限)。通常の `malloc`・`MAP_PRIVATE` のバッファは正常です。
