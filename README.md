# SonyNasne-debian

SIE(旧SCE)製ネットワークレコーダー **nasne CECH-ZNR2J**(初期型・2012年発売)の
ファームウェア構造を解析し、**Debian GNU/Linux 7 "wheezy"(mipsel)を直接起動する**ためのツールと解析結果です。

- SoC: ViXS XCode 4210("Viper"、MIPS 74Kc)/ 復調IC: 東芝 TC90532系 / SPIフラッシュ: S25FL128P(16MB)
- Debian が 7 "wheezy" なのは、カーネルが 2.6.29 のため(上限は 12 "bookworm"。13 は mipsel が無い。カーネルを新しくすれば 12 まで上げる余地あり)。[docs/14](docs/14_newer_debian_kernel_and_distribution.md)
- 純正のユーザランド(Sony製のプロセス群)を**一切通さず**、Debianの `sysvinit` を PID 1 として起動できます。
- 公式ファーム(v1.00 / v2.60)も、SIEが公開しているGPLソースから自分でビルドしたカーネルで動きます。

> ⚠️ **自己責任のプロジェクトです。** 基板上のSPIフラッシュの書き換えを伴う作業があり、失敗すると起動しなくなります
> (その場合はCH341A等のSPIライタで書き戻します)。対象は自分で所有する実機のみ。
> このリポジトリには Sony の**ファームウェアやSPIフラッシュの内容は含まれません**。公式ファームはSonyの公開サーバから各自で入手してください。

## HDDが壊れて、復旧したいだけの方へ

**[docs/04_hdd_recovery_guide.md](docs/04_hdd_recovery_guide.md)** を読んでください。HDDの区画作成とファイルのコピーだけで、
交換用HDDにnasneを復旧できる手順です(SPIの吸い出しも、v1.00の`00550066.dlm`も不要です)。

## できること / できないこと

| | 内容 |
|---|---|
| ✅ できる | Sony経路を通らない **Debian直起動**(ssh、cron、apt など通常のDebianとして常時稼働) |
| ✅ できる | 自前ビルドのカーネルへの差し替え(ブートローダは無改変。SPIのKNL領域だけ書き換え) |
| ✅ できる | **公式ファームを丸ごと動かす**(自前カーネル上で v1.00 / v2.60 が起動し、WebUIまで表示) |
| ✅ できる | HDDと本体の「紐付け」(`00110022.dlm`)をオフラインで生成。HDD全損機の復旧 |
| ✅ できる | **公式のHDDの `00550066.dlm` を1ファイル差し替えるだけで、nasne自身がp3にDebianを作り、SPIのカーネルを書き換えて、Debian直起動になる**(実機1台で確認。[docs/13](docs/13_full_setup_from_official_hdd.md)) |
| ✅ できる | 約5.5分ごとの自動リセット(基板上MCUのウォッチドッグ)の停止 |
| ✅ できる | 復調IC・RFチューナーICのI2C通信、地デジの選局(ロック・C/N取得) |
| ✅ できる | **地デジの受信**: B-CASで復号した**元画質(MPEG-2 HD)のTS**を `recpt1` 互換コマンド / HTTP で取り出す(番組表用のSIも含む)。[docs/11](docs/11_tv_streaming.md) |
| ✅ できる | **VLCでプレイリスト再生**(`http://<nasne>:8301/playlist.m3u8`。局名つき)。ワンセグは扱わない |
| ✅ できる | **Mirakurunのチューナーとして使える**(チャンネルスキャン、番組表、ストリーム)。mpv/ffmpegでも再生確認済み |
| △ 未検証 | tvheadend・EPGStation 本体との連携、BS/CS、2つ目のチューナー、複数番組の同時視聴(チューナーは1系統) |
| ✅ できる | **PWR LEDを点灯にする**(MCUの表示コードレジスタへ書く。[docs/06](docs/06_mcu_watchdog.md)) |
| ❌ できない | `halt`/`poweroff`による電源断(純正も電源断の手段は電源ケーブルのみ) |
| ❌ できない | UART/JTAGによるデバッグ(基板に未実装) |
| ❌ やっていない | ブートローダ(U-Boot / 4段目)の差し替え、Sony miniroot(SPIのRFS)を使わない完全Debian化 |

録画データ(`.hai`)はDTCP-IPで暗号化されており、このプロジェクトの対象外です。

## 主な解析結果

| 項目 | 要点 | 詳細 |
|---|---|---|
| ハードウェア | XCode 4210 + DDR3 512MB(Linuxに渡るのは248MB)+ TC90532系復調IC | [docs/01](docs/01_hardware.md) |
| ブートチェーン | SoC内蔵ROM → U-Boot 1.1.3 → 4段目ブート → Linux 2.6.29。**署名検証なし(CRC32のみ)** | [docs/02](docs/02_boot_chain.md) |
| ファーム形式 | `.dlm` = 独自Blowfish変種(鍵はファーム内に平文)。HDD紐付けは本体個体IDの照合だけ | [docs/03](docs/03_firmware_format.md) |
| Debian直起動 | 自前カーネル+内蔵initramfs → p3のDebianへ `switch_root` | [docs/05](docs/05_kernel_and_direct_boot.md) |
| ウォッチドッグ | MCUへ I2C で `[0x20, 0x01]` を書くと停止 | [docs/06](docs/06_mcu_watchdog.md) |
| ドライバのioctl | `/dev/vixs/xcodedrv` のプロトコル、B-CASの経路 | [docs/07](docs/07_driver_ioctl_bcas.md) |
| チューナー | I2Cバス1の復調IC×2と、その奥(パススルー)のRFチューナーIC×3。地デジの選局手順 | [docs/08](docs/08_tuner_i2c.md) |
| TV受信・配信 | B-CAS復号済みの元画質TSを取り出す `nasne-recpt1`(CLI/HTTP) | [docs/11](docs/11_tv_streaming.md) |
| 復号の仕組み | TSパススルー、B-CAS、`OPEN_SECUREDTS` のioctl手順 | [docs/12](docs/12_ts_and_bcas_decrypt.md) |

できること・できないこと・未解明の項目の詳細は **[docs/10_status_and_limits.md](docs/10_status_and_limits.md)**。

## ドキュメント

| 文書 | 内容 |
|---|---|
| [01_hardware](docs/01_hardware.md) | 基板・主要IC・メモリ配分 |
| [02_boot_chain](docs/02_boot_chain.md) | SPIフラッシュの地図、ブートチェーン、純正の起動フロー |
| [03_firmware_format](docs/03_firmware_format.md) | `.dlm` の暗号とヘッダ、HDD紐付け(`00110022.dlm`) |
| [04_hdd_recovery_guide](docs/04_hdd_recovery_guide.md) | **HDD全損機の復旧手順** |
| [05_kernel_and_direct_boot](docs/05_kernel_and_direct_boot.md) | カーネルのビルド、SPIへの書き込み、Debian直起動の作り方 |
| [06_mcu_watchdog](docs/06_mcu_watchdog.md) | ウォッチドッグの正体と止め方、MCU・LEDのメモ |
| [07_driver_ioctl_bcas](docs/07_driver_ioctl_bcas.md) | `xcode4drv` のioctl、B-CAS |
| [08_tuner_i2c](docs/08_tuner_i2c.md) | 復調IC・RFチューナーICとのI2C通信 |
| [09_tools](docs/09_tools.md) | **スクリプト・ツールの使い方** |
| [11_tv_streaming](docs/11_tv_streaming.md) | **地デジの受信・配信**(`nasne-recpt1`) |
| [12_ts_and_bcas_decrypt](docs/12_ts_and_bcas_decrypt.md) | TS取得とB-CAS復号のioctlレベルの手順 |
| [10_status_and_limits](docs/10_status_and_limits.md) | できること・できないこと・未解明 |
| [13_full_setup_from_official_hdd](docs/13_full_setup_from_official_hdd.md) | **公式HDDの `00550066.dlm` を1ファイル差し替えるだけで、Debian直起動まで**(SPI書き換えもnasne自身で実行) |
| [14_newer_debian_kernel_and_distribution](docs/14_newer_debian_kernel_and_distribution.md) | Debian・カーネルを新しくできるか / 成果物を配布してよいか |
| [boxster_cmd_table](docs/boxster_cmd_table.md) | ドライバのioctlコマンド番号表 |

## 必要なもの

- Linux のPC(Python 3.8+、Docker、`debootstrap`、`qemu-user-static`、`flashrom`、`sfdisk`、`mkfs.ext3` など。作業ごとに[docs/09](docs/09_tools.md)に記載)
- SPIフラッシュ書き込みには CH341A 等のSPIライタとSOICクリップ(カーネル差し替えに必要。HDD復旧だけなら不要)
- 実験用の2.5インチHDD(**Debian開発用と、純正のまま残すHDDは別にしておくこと**を強く推奨)

## リポジトリ構成

```
.
├─ README.md
├─ docs/                      … 解析結果とやり方(上の表)
└─ scripts/
   ├─ dlm_crypto.py           … .dlm のBlowfish変種(暗号・復号)
   ├─ build_dlm.py            … .dlm の構築(ヘッダ/CRC)
   ├─ dlm_mng.py              … 00110022.dlm(マネージャ)の解析・生成
   ├─ ofw_tool.py             … 公式ファームパッケージの分解
   ├─ nasne_hdd_rebuild.py    … HDD復旧用ツール
   ├─ build_knl.py            … SPIのKNLセグメントの解析・再構築
   ├─ spi_segments.py / spi_boot_decrypt.py … SPIフラッシュのセグメント列挙・ブートチェーン復号
   ├─ build_debian_rootfs.sh  … Debian wheezy (mipsel) のrootfs作成
   ├─ fetch_gpl_sources.sh    … SIE公開のGPLソースの取得
   ├─ kernel_build/           … 自前カーネルのビルド(Docker)と内蔵initramfs
   ├─ stage1/                 … 公式HDDの00550066.dlm差し替えだけでDebian直起動へ移行するツール一式
   ├─ watchdog/               … MCUウォッチドッグ停止(mcui2c、initサービス)
   ├─ tools/                  … 実機用のツール(`nasne-recpt1`(TS取得・HTTP配信)、I2C、ioctl、/dev/mem、MTD書き込み、B-CAS)
   ├─ nasne_fe.py             … チューナーのI2C操作(PCからssh経由)
   └─ ghidra/                 … Ghidra headless用の補助スクリプト
```

## ライセンス・注意

- 解析結果とスクリプトは、相互運用性(自分の機器でLinuxを動かすこと)のための個人研究として、[MIT License](LICENSE) で公開しています。自由に clone・改変・再配布してください。
- Sonyのファームウェア本体、SPIフラッシュの内容、B-CASカードや個体の識別情報は含めていません。
- nasne、PlayStation、B-CAS 等は各社の商標です。本プロジェクトはSIE、バッファロー、各社とは無関係です。
