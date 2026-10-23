# 00. どの手順書を読むか(振り分けリスト)

このリポジトリの手順書を、**「どのバージョンで・何をしたくて・何を入れたくて・どこまで触るか」**から選ぶための一覧です。手順そのものは書いていません。各行のリンク先を読んでください。
個々の確認状況(モデル・ファーム・HDD・機能)は [docs/20](20_compatibility_and_reports.md)、困ったときは [docs/21](21_troubleshooting.md) です。

> ⚠️ SPI フラッシュ(基板)を書き換える手順は、失敗すると起動しなくなります。**事前に SPI のバックアップ(全域)を取り、書き戻せる手段(CH341A 等)を用意**してから進めてください。

## 0. 30秒の振り分け

| いまの状況・やりたいこと | まず読む |
|---|---|
| HDD が壊れた/交換したい。**Debian は要らない**(純正のまま戻したい) | [§1-A](#a-hdd-の復旧純正のまま) |
| 純正のまま、SPI の中身を**保存だけ**しておきたい(保険) | [§1-B](#b-spi-のバックアップ読むだけ) |
| nasne で **Debian を動かしたい**(まず試す、実績が多いほう) | [§1-C](#c-debian-を動かす安定版-linux-2629--debian-7) |
| nasne で **新しいカーネル(6.12)+ Debian 12** を試したい | [§1-D](#d-新しいカーネルlinux-612-debian-12試験中) |
| **テレビを見たい/録画したい**(Mirakurun・EPGStation・tvheadend・VLC など) | [§1-E](#e-テレビを見る録る) |
| 状態を監視したい/Samba のファイルサーバーにしたい/自動起床したい | [§1-F](#f-運用の周辺状態ファイル共有起床) |
| 元に戻したい(純正カーネル・SPI の復元・HDD) | [§1-G](#g-元に戻す困ったとき) |
| 自分の環境で動いたか報告したい | [docs/20](20_compatibility_and_reports.md)(`nasne-report`。個体情報は出ない) |
| 仕組み・解析の中身を知りたい | [§1-H](#h-仕組み解析の中身を知りたい) |

## 1. 目的別の手順書

### A. HDD の復旧(純正のまま)
| したいこと | 手順書 | 前提・注意 |
|---|---|---|
| HDD 全損機を、新しい HDD で純正のまま復旧する | [docs/04](04_hdd_recovery_guide.md) | HDD の区画作成とファイルのコピーだけ。SPI の吸い出しも v1.00 も不要(公式 v2.60 で足りる)。他のファーム版数は同ドキュメントの末尾 |
| HDD を入れ替えたら純正が固まる/起動しない | [docs/16](16_spi_backup_dlm.md) の「本体の NV を空にする」(`--mode nvreset`) | 本体の NV と HDD の食い違いの復旧 |
| p3(録画領域)の初期構造・`.hai` を知りたい | [docs/15](15_p3_structure_and_hai.md) | 復旧の理屈 |

### B. SPI のバックアップ(読むだけ)
| したいこと | 手順書 | 前提・注意 |
|---|---|---|
| nasne 自身に SPI を読ませて HDD に保存する(書き込みなし) | [docs/16](16_spi_backup_dlm.md) | 公式 HDD の `00550066.dlm` を 1 ファイル差し替えるだけ。CH341A は不要。**以降のどの SPI 書き換えの前にも推奨** |
| CH341A 等で外から全域をダンプする | [docs/05](05_kernel_and_direct_boot.md)(`flashrom` での読み出し・書き込み) | 基板にクリップを付ける必要あり。最も確実 |

### C. Debian を動かす(安定版: Linux 2.6.29 + Debian 7)
| したいこと | 手順書 | 前提・注意 |
|---|---|---|
| 公式で動いている HDD から、`00550066.dlm` 1 ファイルの差し替えで Debian 直起動にする(**おすすめ**) | [docs/13](13_full_setup_from_official_hdd.md)(`nasne_setup.py` で 1 コマンド) | ビルドする PC が必要(Linux/WSL2/VM)。SPI の KNL を書き換える。**事前に SPI のバックアップ**([§1-B](#b-spi-のバックアップ読むだけ)) |
| 手でカーネルをビルドして書き込む | [docs/05](05_kernel_and_direct_boot.md) | 仕組みを知りたい人向け。docs/13 の個別手順と重なる |
| 公式の録画データ(p3)を残したまま入れる / 初期化して入れる | [docs/13](13_full_setup_from_official_hdd.md)(`--p3-mode image`/`format`) | `image`(既定)は録画データを消さない。`format` は消える |
| 公式アプリを 1 回だけ起動して戻る | [docs/13](13_full_setup_from_official_hdd.md)「公式アプリを 1 回だけ起動する」 | NV と HDD が揃っていることが条件 |
| Debian を新しくしたい(12 まで) | [docs/14](14_newer_debian_kernel_and_distribution.md)→[§1-D](#d-新しいカーネルlinux-612-debian-12試験中) | 2.6.29 のままでは Debian 7 が上限 |

### D. 新しいカーネル(Linux 6.12)+ Debian 12(試験中)
| したいこと | 手順書 | 前提・注意 |
|---|---|---|
| 6.12 + Debian 12 をビルドして動かす | [docs/22](22_new_kernel_612.md) | **試験中。SPI を書き換えるので CH341A 必須級**。まず §1-C の安定版で Debian が動いている機体から進める(旧カーネル側の `mtdtool` で KNL を書く) |
| カーネルに何が入っているか/足せるか | [docs/23](23_kernel_feature_inventory.md) | KNL スロットの容量(gzip 後 2,621,376 バイト)との兼ね合い |
| USB メモリ(ext4/vfat)・ホットプラグ・スワップ | [docs/22](22_new_kernel_612.md) §7、`scripts/newkernel/setup_swap12.sh` | 6.12 のみ |
| 6.12 のチューナーと常駐サービス(LED・電源・起床・状態ページ) | [docs/22](22_new_kernel_612.md) の手順 5 | `install_tv12.sh`・`install_services12.sh` |
| 新カーネルを作るための調査・設計の経緯 | [docs/19](19_new_kernel_survey.md) | 読み物 |

### E. テレビを見る・録る
| したいこと | 手順書 | 前提・注意 |
|---|---|---|
| 地デジの受信と配信(`nasne-recpt1`)の使い方 | [docs/11](11_tv_streaming.md) | Debian 直起動が前提([§1-C](#c-debian-を動かす安定版-linux-2629--debian-7)かD)。HTTP/コマンド/プレイリスト |
| VLC などで見る | [docs/11](11_tv_streaming.md)(`/playlist.m3u8`) | ワンセグは対象外 |
| Mirakurun + EPGStation で予約録画 | [contrib/epgstation](../contrib/epgstation/README.md) | Docker。Mirakurun の `/app-config` と `/app-data` は**両方**永続化([docs/11](11_tv_streaming.md)) |
| tvheadend で予約録画 | [contrib/tvheadend](../contrib/tvheadend/README.md) | IPTV(M3U)で登録 |
| BS/CS を使う | [docs/11](11_tv_streaming.md)(`--scan-all`/`/scan?all`、`nasne_mirakurun_conf.py --sat`) | 受信できる環境の報告あり([docs/20](20_compatibility_and_reports.md))。既定は**チューナー 1 系統**(同時視聴は不可)。地デジ+BS/CS の同時受信は試験的に可能([docs/11](11_tv_streaming.md) の `--dual-tuner`) |
| KonomiTV など別のアプリで見る | [docs/11](11_tv_streaming.md)・[docs/20](20_compatibility_and_reports.md) | 実視聴の報告あり |
| B-CAS・TS のしくみ | [docs/12](12_ts_and_bcas_decrypt.md)・[docs/07](07_driver_ioctl_bcas.md) | 読み物 |
| 受信がおかしい(busy・503・映像が乱れる) | [docs/21](21_troubleshooting.md)・[docs/11](11_tv_streaming.md)「トラブルシュート」 | TS の中身の検査は `scripts/tools/ts_analyze.py`([docs/09](09_tools.md))で、PAT/PMT・PID ごとの CC エラー・スクランブル・TEI を見る |

> 🚫 DTCP-IP で保護された**既存の録画の取り出し・復号は扱いません**([README](../README.md))。

### F. 運用の周辺(状態・ファイル共有・起床)
| したいこと | 手順書 | 前提・注意 |
|---|---|---|
| HDD の温度・SMART・MCU・チューナーの状態を見る(Prometheus・JSON も) | [docs/18](18_status_page.md) | Debian 直起動なら既定で入る |
| Home Assistant に出す | [contrib/homeassistant](../contrib/homeassistant/README.md) | オプション |
| `/data` を SMB で共有する | [contrib/samba](../contrib/samba/README.md) | オプション |
| 予約に合わせて自動起床・録画後に自動で電源オフ | [contrib/epgstation](../contrib/epgstation/README.md)(`nasne-waked`) | EPGStation が前提。LAN からの電源オンは不可(起床タイマで代替) |
| LED・電源ボタン・ウォッチドッグ | [docs/06](06_mcu_watchdog.md) | Debian 直起動ではウォッチドッグの停止が必須(約 333 秒でリセットされる) |
| ツール(`mtdtool`・`i2cx` など)の使い方 | [docs/09](09_tools.md) | |
| 公式 Web API(LED 制御など)を使う | [docs/17](17_official_web_api.md) | 純正稼働時 |

### G. 元に戻す・困ったとき
| 状況 | 手順書 |
|---|---|
| SPI の KNL を以前のバックアップへ戻す(公式カーネルへ戻す/試したカーネルを戻す) | [docs/16](16_spi_backup_dlm.md)「バックアップへ戻す」(`--mode restore`) |
| Debian 直起動の機体で SPI の KNL を書き直す | [docs/21](21_troubleshooting.md) §5 |
| 純正アプリに戻す/一時的に純正を起動する | [docs/13](13_full_setup_from_official_hdd.md)(`nasne-boot-switch`・「公式ファームに戻す」) |
| 起動しない・LED が点かない・約 5.5 分ごとに再起動する | [docs/21](21_troubleshooting.md)(LED とネットワークの応答から引く表) |
| 全く起動しなくなった(SPI が壊れた) | CH341A で書き戻す([docs/05](05_kernel_and_direct_boot.md) の `flashrom`。バックアップがあること) |

### H. 仕組み・解析の中身を知りたい
基板・ブートチェーン・ファーム形式: [docs/01](01_hardware.md)・[02](02_boot_chain.md)・[03](03_firmware_format.md)。チューナー: [08](08_tuner_i2c.md)・[07](07_driver_ioctl_bcas.md)・[12](12_ts_and_bcas_decrypt.md)・[boxster_cmd_table](boxster_cmd_table.md)。できる/できない: [10](10_status_and_limits.md)。

## 2. バージョンの選び方

### 2-1. 載せるもの(カーネルと Debian)

| | **安定版(おすすめ)** | **試験版** |
|---|---|---|
| カーネル | Linux 2.6.29(SIE の GPL ソースから) | Linux 6.12 LTS |
| Debian | 7 "wheezy"(mipsel) | 12 "bookworm"(mipsel、sysvinit) |
| 実績 | 複数の機体・HDD で確認([docs/20](20_compatibility_and_reports.md)) | 作者の 1 台のみ |
| 入れ方 | `nasne_setup.py`([docs/13](13_full_setup_from_official_hdd.md))。HDD のファイル差し替えが主 | 手でビルド+SPI を書く([docs/22](22_new_kernel_612.md)) |
| できること | 地デジ/BS/CS の配信、状態ページ、LED、起床、Samba など | 同じ+USB(ext4/vfat・ホットプラグ)、スワップ、SysRq、新しい Debian のパッケージ |
| セキュリティ更新 | Debian 7 は EOL(`archive.debian.org`) | Debian 12 の mipsel は更新対象外([docs/19](19_new_kernel_survey.md)) |
| 向いている人 | まず Debian を動かしたい/テレビに使いたい | 新しいユーザランドや USB が欲しい/実験が好き |

Debian 13 以降は mipsel が公式から外れたので使えません([docs/14](14_newer_debian_kernel_and_distribution.md))。

### 2-2. 本体モデルと純正ファーム

| 持っているもの | 状況 | 見るところ |
|---|---|---|
| CECH-ZNR2J(初期型)・公式 v2.60 | 作者の機体。すべての手順で確認 | — |
| CUHJ-15004(1TB)・v2.57/v2.60 | 手順どおりに動いた報告あり(Debian 直起動・地デジ・BS/CS) | [docs/20](20_compatibility_and_reports.md) |
| CECH-ZNR1J(250GB の SSD に交換)・v2.60 | 地デジの配信・BS の受信の報告あり | [docs/20](20_compatibility_and_reports.md) |
| その他のモデル | 基板が同じか不明 | 先に [§1-B](#b-spi-のバックアップ読むだけ) でバックアップを取って様子を見る |
| 公式ファームの版数 | v1.00/v2.57/v2.60 の範囲で確認。HDD の `00550066.dlm`(そのHDD自身のもの)から作るのが基本 | [docs/20](20_compatibility_and_reports.md)・[docs/04](04_hdd_recovery_guide.md)「他のファームバージョン」 |
| HDD | 東芝・HGST・Seagate での報告あり。SSD(250GB)の報告も | [docs/20](20_compatibility_and_reports.md) の「HDD」の表 |

## 3. 何を入れたいか → 必要な手順

| 入れたいもの | 必要な前提 | 手順書 |
|---|---|---|
| 純正のまま(HDD の復旧だけ) | なし | [docs/04](04_hdd_recovery_guide.md) |
| Debian(ssh・cron・apt) | SPI のバックアップ、ビルドする PC | [docs/13](13_full_setup_from_official_hdd.md) |
| + 地デジ/BS/CS の配信(`nasne-recpt1`) | Debian 直起動。ウォッチドッグの停止(自動) | [docs/11](11_tv_streaming.md) |
| + Mirakurun/EPGStation/tvheadend/KonomiTV | 別のサーバー(Docker 等) | [contrib/](../contrib/epgstation/README.md)・[docs/11](11_tv_streaming.md) |
| + 状態ページ | 既定で入る | [docs/18](18_status_page.md) |
| + 自動起床・自動オフ | EPGStation | [contrib/epgstation](../contrib/epgstation/README.md) |
| + Samba | — | [contrib/samba](../contrib/samba/README.md) |
| + Home Assistant | — | [contrib/homeassistant](../contrib/homeassistant/README.md) |
| 新しい Debian 12・USB・スワップ | 6.12 のビルドと SPI 書き込み | [docs/22](22_new_kernel_612.md) |

## 4. どこまで触るか(リスクと戻し方)

| レベル | 何を変える | 戻し方 | 手順書 |
|---|---|---|---|
| 0. 読むだけ | 何も変えない(SPI を読んで HDD に保存) | 不要 | [docs/16](16_spi_backup_dlm.md) |
| 1. HDD だけ | HDD の中身(純正のまま復旧) | HDD を替えるだけ | [docs/04](04_hdd_recovery_guide.md) |
| 2. HDD の `00550066.dlm` 差し替え | HDD の sys1 の 1 ファイル(SPI は触らない版もある) | 元のファイルに戻す(退避される) | [docs/13](13_full_setup_from_official_hdd.md) |
| 3. SPI の KNL を書き換え(安定版の Debian) | SPI の KNL スロット A だけ | `--mode restore`/`mtdtool`、最悪は CH341A。**KNL が壊れた(CRC 不一致)ときは出荷時の B 側で起動** | [docs/13](13_full_setup_from_official_hdd.md)・[docs/16](16_spi_backup_dlm.md) |
| 4. 6.12 のカーネル | SPI の KNL スロット A | 同上。ただし**「起動はするが固まる」カーネルには B 側へのフォールバックが効かない** | [docs/22](22_new_kernel_612.md) |
| 5. ブートローダの差し替え | U-Boot/4 段目 | CH341A のみ | **未実施**(やっていない) |

## 5. 迷ったら

1. [docs/20](20_compatibility_and_reports.md) で、自分のモデル・ファーム・HDD の行を見る。
2. 純正の HDD を**触る前に**、[docs/16](16_spi_backup_dlm.md) で SPI をバックアップする。
3. 安定版([docs/13](13_full_setup_from_official_hdd.md))で Debian を動かす。慣れて、新しいものが欲しくなったら [docs/22](22_new_kernel_612.md)。
4. 動かなかった/動いた結果は、`nasne-report` を添えて Issue の「検証レポート」へ。
