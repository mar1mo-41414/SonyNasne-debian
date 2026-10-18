# 6.12 カーネルの機能一覧と、省いたもの・追加できそうなもの

nasne の KNL スロットは、gzip 後 2,621,376 バイトまで(`newkernel/build.sh` が表示)。**この上限に効くのは、カーネル本体に組み込む(`=y`)機能と、モジュール用に残すエクスポートシンボルの表だけ**で、`=m`(モジュール)は rootfs の `/lib/modules` に置くので、上限に影響しない。

## 1. いまカーネルに組み込んでいる機能(整理後)

サイズは、リンクされたオブジェクトの text+data(目安。gzip 後はおよそ 4 割)。

| 領域 | 内容 | 目安 | 必要な理由 |
|---|---|---|---|
| ファイルシステム | XFS(約 690KB。公式の p3 の読み取りと、ext4 の loop の裏ファイル)、ext4+jbd2(約 470KB。ext2/3 も)、proc・sysfs・devtmpfs・tmpfs・inotify | 約 1.4MB | root は XFS 上の ext4/ext3 イメージ。HDD の p3 は XFS |
| ネットワーク | IPv4/TCP/UDP(約 490KB)、net/core(約 440KB。classic BPF のフィルタを含む)、netlink・ethtool・packet・unix、IP_PNP、netconsole | 約 1.1MB | LAN の配信、DHCP クライアント(AF_PACKET)、デバッグ用の netconsole |
| ストレージ | SCSI(sd)+libata+AHCI(約 560KB)、ブロック層・mq-deadline・loop、MTD+SPI-NOR(SPI フラッシュ。約 110KB)、MSDOS/GPT パーティション | 約 0.9MB | SATA の HDD、SPI の KNL スロットの書き換え |
| コア | mm(約 380KB)、sched・time・irq・printk・futex・workqueue 等、モジュールローダ | 約 1.3MB | 必須 |
| アーキテクチャ | MIPS 74Kc(math-emu の FPU エミュレータ約 60KB、tlb、cache)、DT(of) | 約 0.4MB | FPU は実機にあるが、エミュレータは非実装の演算に必要なので残す |
| nasne 固有 | SoC 初期化・割り込み・タイマ、GMAC、AHCI ラッパ、I2C+MCU、SPI、LED、早期コンソール(UDP)、USB の間接アクセス | 約 0.15MB | 本機固有 |
| その他 | PHYLIB+Generic PHY(PHY は Realtek RTL8211E で、Generic PHY ドライバで 1000Mb/s)、crypto の最小(crc32c)、lib | 約 0.5MB | |
| モジュール(上限に効かない) | USB コア+EHCI+OHCI+usb-storage、FAT/vfat+NLS、乱数、xcode4drv(アウトオブツリー) | — | |

## 2. 今回省いたもの(2026-10-18、gzip 後 **約 59KB** 減。2613276 → 2554259)

| 省いた機能 | 理由 |
|---|---|
| `MARVELL_PHY` | 実際の PHY は Realtek RTL8211E(ID 0x001cc915)で、Generic PHY ドライバが使われている。Marvell 用は未使用 |
| `COMMON_CLK`(clk フレームワーク) | 使っているドライバが無い(AHCI ラッパの "clk" は自前の変数)。`MIPS_XCODE` の `select` を外した(パッチ 0001) |
| `SUSPEND` / `PM_SLEEP` / `FREEZER` | サスペンドしない。ランタイム PM(`PM`)は USB のオートサスペンドで使うので残した |
| `NET_SELFTESTS`、`DNOTIFY`、`LEGACY_PTYS`、`DEVPORT`、`HWMON`、`PPS`、`FW_LOADER`、`SGETMASK_SYSCALL` | 使っていない(inotify・UNIX98 PTY は残している)。ファームウェアが要るモジュールを後で入れるなら、`FW_LOADER` はそのモジュールが選択する |
| `dbg_ifetch`(SIGILL 調査用のダンプ) | 調査用。`XCODE_DBG_IFETCH`(既定 n)にした |
| PTP(前回) | 使っていない |

確認: 実機で起動、1000Mb/s リンク、USB(EHCI/OHCI/vfat/ext4)、xcode4drv、地デジ受信(16MB の TS が全パケット同期)、常駐サービスが通常どおり。

## 3. まだ省けそうなもの(判断が要る。効果は**未測定の目安**。実際に外すときに測る)

| 機能 | 効果 | リスク・判断 |
|---|---|---|
| `STACKPROTECTOR_STRONG` | 数十 KB | スタック保護(セキュリティ強化)が無くなる。LAN 内の機器でも、外に出すなら残したい |
| `COREDUMP` / `ELF_CORE` | 約 5KB | クラッシュ調査ができなくなる(`D` 状態の固まりの調査で使う可能性) |
| `NETCONSOLE` / `NETPOLL` | 約 5KB | 開発中のログ収集に使用。配布版では不要だが、いまは開発中 |
| `AIO` | 約 5KB | Samba・一部のアプリが使う(libaio) |
| `SECCOMP` | 約 4KB | OpenSSH のサンドボックスが使う |
| `PROC_PAGE_MONITOR` | 約 3KB | `/proc/*/smaps`・`pagemap`。メモリ調査ツールが使う |
| `ATA_VERBOSE_ERROR` / `ATA_FORCE` | 約 3KB | HDD エラーの読みやすいログ、起動引数での強制設定 |
| `MTD_CONCAT`、各種 NOR ベンダーの定義 | 約 5KB | SPI-NOR ドライバの依存(確認が要る) |
| math-emu(FPU エミュレータ) | 約 25KB | **残す**: 74Kc の FPU が扱わない演算で必要になる。外すと一部のプログラムが SIGILL になる恐れ |

## 4. 追加できそうな機能(余白は 2,621,376 − 2,554,259 ≈ **67KB**)

**まず大事な点**: 多くは**モジュール(`=m`)にできるので、上限に影響しない**(エクスポートシンボルの表が少し増える)。組み込みが必須なのは、ルートをマウントする前に要るものだけ(ext4・XFS・loop・sd・SATA・Ethernet は、すでに組み込み済み)。

測定したもの(組み込みにした場合の増加。gzip 後):

| 機能 | 増加 | 何に効くか |
|---|---|---|
| `IP_MULTICAST` | 約 6KB | **mDNS(avahi)・UPnP/DLNA・IGMP**。現状は無効で、DLNA サーバ/mDNS が使えない |
| `SYSVIPC` + `POSIX_MQUEUE` | 約 22KB | System V の共有メモリ・セマフォ(PostgreSQL、一部の Samba 機能など)。`POSIX_MQUEUE` が大きいので、`SYSVIPC` だけなら小さい見込み(要測定) |
| `SWAP` | 約 23KB | **RAM が約 240MB**なので、スワップ(`zram` をモジュールで足せば圧縮スワップ)で安定する |
| `MAGIC_SYSRQ` | 約 2KB | 固まったときの遠隔復旧・状態ダンプ(`D` 状態の調査に有用) |
| ACL / xattr(ext4・XFS・tmpfs) | 約 7.5KB | Samba の ACL、Debian のパッケージ(xattr を使うもの) |
| `SYN_COOKIES` + `INET_DIAG` 系(`ss` 用) | 約 10KB | `ss`・`netstat` の詳細、SYN フラッド対策 |
| (合計: 上の6件) | 約 71KB | 余白 67KB を少し超える。優先度で選ぶ |

モジュールにできる(上限に影響しない)追加の候補:

| 機能 | 用途 |
|---|---|
| `IPV6`(`ipv6.ko`) | IPv6 対応(現状は無効。`ntpdig -4` などの回避が要る) |
| `NETFILTER`/nftables/conntrack | ファイアウォール(コアの `NETFILTER` は組み込みの bool。数 KB) |
| exFAT / NTFS3 / ISO9660 / SquashFS / overlayfs / FUSE | USB メモリ・ディスクの読み書き、sshfs、ライブイメージ |
| NFS クライアント/サーバ、CIFS クライアント | 他機とのファイル共有 |
| `INPUT`+`HID`+`usbhid`(USB キーボード・マウス) | 現状 `INPUT` が無効。本機では用途が薄い |
| USB シリアル(FTDI・CP210x など) | 外付けのシリアル機器 |
| USB Wi-Fi / Bluetooth(cfg80211・mac80211) | 無線の LAN(ファームウェア読み込みが要る=`FW_LOADER`) |
| USB ネットワーク(`USB_NET_DRIVERS=m` は有効。個別のドライバ) | USB 有線 LAN |
| DVB の USB チューナー | 外付けチューナーの併用 |
| dm-crypt / LVM / mdraid | 暗号化・RAID(HDD は1台だが、USB 外付けに) |
| `bridge`、`tun`/`tap`、`vlan`、WireGuard(要 crypto) | ネットワーク機能 |
| `cgroups` / `namespaces`(組み込みの bool。数十 KB) | Docker・LXC・systemd の機能 |
| `KALLSYMS`(組み込み。約 80KB 超) | oops にシンボル名が出る(余白に収まらない) |
| `RTC`/`WATCHDOG` クラス、`LEDS_CLASS`/`GPIOLIB`/`THERMAL` | 標準のインターフェース(LED・ウォッチドッグの MCU を標準 API で)。本機の LED は `nasne-ledd` が独自に操作している |
| `HIGH_RES_TIMERS`/`NO_HZ`(現状 `HZ=100`) | タイマの精度・省電力。TV 配信の遅延には影響が小さい |

**おすすめの順番**(必要性と費用): ① `IP_MULTICAST`(DLNA/mDNS)、② `MAGIC_SYSRQ`、③ `SWAP`(+ `zram` モジュール)、④ ACL/xattr、⑤ `SYSVIPC`(`POSIX_MQUEUE` なし)、⑥ `ss` 用 DIAG。これで余白をほぼ使う。IPv6・netfilter・FS 類・USB 機器類は、すべてモジュールで足せる。
