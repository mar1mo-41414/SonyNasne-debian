# 24. 情報の層と、文書の対応表(自分の環境の情報を、混ぜずに探す)

nasne の情報は、**どの前提に依存するか**で、次の 5 つの層に分かれます。同じ文書の中に複数の層が混ざっていることがあるので、各文書の冒頭と、混在している節の見出しの下に **層の印**(【固定】【OFW】【CFW共通】【旧】【新】)を付けています。**自分の環境に当てはまる層だけ読めば足ります。**

| 記号 | 層 | 意味 | 依存するもの |
|---|---|---|---|
| 【固定】 | ハード・プロトコル・ブートチェーン | 基板・SoC・SPI の地図・U-Boot と 4 段目・MCU・チューナー IC・メディアエンジンのドライバ(`xcode4drv`)の仕様 | **何にも依存しない**(カーネルが何でも、公式でも自作でも同じ) |
| 【OFW】 | 公式ファーム(OFW) | Sony の公式ファーム(カーネル + miniroot + アプリ)の挙動と仕組み。HDD 復旧、SPI のバックアップと復元(`.dlm`)、`.hai`、NV、公式 Web API | 公式の起動経路(miniroot の `init`、`00550066.dlm`) |
| 【CFW共通】 | 自作ファームの共通部分 | `nasne-recpt1`、状態ページ、LED・電源・起床のサービス、Mirakurun などとの連携 | 自作ファーム(旧・新どちらのカーネルでも同じ) |
| 【旧】 | 旧カーネル(安定版) | 自前ビルドの **Linux 2.6.29 + Debian 7 (wheezy)**。`nasne_setup.py` の既定。導入は公式の更新の仕組み(`00550066.dlm`)の差し替え | GPL ソース + Docker(GCC 4.3.2)、Debian 7、SPI の KNL スロット |
| 【新】 | 新カーネル(試験中・非推奨) | **Linux 6.12 LTS + Debian 12**。SPI から単独で起動(`.dlm` を使わない) | 6.12 のソース、`newkernel/`、SPI の KNL スロットへの書き込み |

層の関係: 【固定】は、すべての土台です。【OFW】と【CFW共通】【旧】【新】は、その上に載っています。【CFW共通】は【旧】【新】のどちらでも使えます。【旧】と【新】は、互いに独立です。

## 1. 文書ごとの層

| 文書 | 層 | 混在している節(下の「節ごとの層」を参照) |
|---|---|---|
| [00 振り分けリスト](00_where_to_start.md) | (全層をまたぐ案内) | — |
| [01 ハードウェア](01_hardware.md) | 【固定】 | — |
| [02 SPI とブートチェーン](02_boot_chain.md) | 【固定】 | 公式の起動・更新に関する節は【OFW】 |
| [03 ファームウェア形式と HDD 紐付け](03_firmware_format.md) | 【OFW】 | 自作 `.dlm` の注入は【旧】の基礎 |
| [04 HDD 復旧](04_hdd_recovery_guide.md) | 【OFW】 | — |
| [05 自前カーネルと Debian 直起動](05_kernel_and_direct_boot.md) | 【旧】 | — |
| [06 MCU・ウォッチドッグ・LED・電源](06_mcu_watchdog.md) | 【固定】【CFW共通】 | 公式の挙動の節は【OFW】 |
| [07 ドライバの ioctl と B-CAS](07_driver_ioctl_bcas.md) | 【固定】 | — |
| [08 チューナーの I2C](08_tuner_i2c.md) | 【固定】 | 純正ソフトの解析の節は【OFW】 |
| [09 ツールの使い方](09_tools.md) | 混在(ツールごとに表示) | ツールごと |
| [10 できること・できないこと・未解明](10_status_and_limits.md) | 層ごとに節を分けた | 節 A〜G |
| [11 地デジの受信と配信](11_tv_streaming.md) | 【CFW共通】 | ビルド環境の記述は【旧】 |
| [12 TS の取り出しと B-CAS 復号](12_ts_and_bcas_decrypt.md) | 【固定】 | — |
| [13 公式 HDD から Debian 直起動](13_full_setup_from_official_hdd.md) | 【旧】 | 導入は【OFW】の更新の仕組みを使う |
| [14 Debian・カーネルを新しくできるか](14_newer_debian_kernel_and_distribution.md) | 【旧】と【新】の比較 | — |
| [15 p3 と `.hai`](15_p3_structure_and_hai.md) | 【OFW】 | — |
| [16 SPI のバックアップ・復元・NV リセット](16_spi_backup_dlm.md) | 【OFW】 | 旧・新のどちらとも独立 |
| [17 公式 Web API](17_official_web_api.md) | 【OFW】 | — |
| [18 状態ページ](18_status_page.md) | 【CFW共通】 | — |
| [19 新しいカーネルの調査](19_new_kernel_survey.md) | 【新】 | SoC の構造は【固定】 |
| [20 対応状況のマトリクス](20_compatibility_and_reports.md) | 混在(行ごとに表示) | 節 1〜4 |
| [21 トラブルシューティング](21_troubleshooting.md) | 混在(節・行ごとに表示) | 節 1〜6 |
| [22 新カーネル(6.12)+ Debian 12](22_new_kernel_612.md) | 【新】 | 節 6 の SoC の癖は【固定】に近い |
| [23 6.12 カーネルの機能一覧](23_kernel_feature_inventory.md) | 【新】 | — |
| [Boxster ioctl のコマンド番号表](boxster_cmd_table.md) | 【固定】 | — |

## 2. 節ごとの層(混在している文書)

| 文書 | 節 | 層 |
|---|---|---|
| 02 | 全体の流れ、SPI の地図、U-Boot、4 段目ブート、システム情報ページ、ECC 領域、Blowfish の鍵 | 【固定】 |
| 02 | 純正の起動(miniroot `init` と sys2 再構築)、純正カーネルとの関係、スロット B(BKNL・BRFS)の正体、公式のアップデートが書き換えるもの | 【OFW】 |
| 03 | `.dlm` の暗号、ヘッダ、公式パッケージ、sys1、`00110022.dlm`(HDD 紐付け) | 【OFW】 |
| 03 | 自作 `.dlm` で sys2 に任意のファイルを注入する | 【旧】の基礎 |
| 06 | 結論、MCU のコマンド、I2C ドライバ側の手順、電源を切る MCU コマンド、PWR LED、前面の4つの LED | 【固定】 |
| 06 | 使い方(Debian 側)、Debian での電源オフ、録画予約に合わせて起こす、電源ボタンの長押し | 【CFW共通】 |
| 06 | 公式の終了処理の順序、純正が時間制限を起こす原因の切り分け | 【OFW】 |
| 08 | 地デジ・衛星の選局手順、結論 | 【固定】 |
| 08 | 純正ソフトの I2C 呼び出し(`dtvtuner` のデコンパイル) | 【OFW】(解析対象) |
| 09 | `dlm_crypto.py`・`ofw_tool.py`・`dlm_mng.py`・`nasne_hdd_rebuild.py`・`nasne_hai.py`・`nasne_db.py`・`build_dlm.py` | 【OFW】 |
| 09 | `build_knl.py`・`spi_segments.py` / `spi_boot_decrypt.py`・`nasne_fe.py`・Ghidra 補助 | 【固定】 |
| 09 | `nasne_setup.py`・`fetch_gpl_sources.sh` / `kernel_build/`・`build_debian_rootfs.sh` | 【旧】 |
| 09 | 実機側のツール(MIPS 用の静的バイナリ) | 【CFW共通】 |
| 10 | 節 A | 【固定】 |
| 10 | 節 B | 【OFW】 |
| 10 | 節 C | 【CFW共通】 |
| 10 | 節 D | 【旧】 |
| 10 | 節 E | 【新】 |
| 20 | 節 1 本体・ファーム | 【固定】【OFW】 |
| 20 | 節 2 HDD | 【OFW】【旧】 |
| 20 | 節 3 機能 | 行ごとに表示 |
| 20 | 節 4 ビルドする PC | 【旧】(`nasne_setup.py`) |
| 21 | 節 1 PWR LED の意味 | 【固定】 |
| 21 | 節 2 HDD の復旧・公式ファームの起動 | 【OFW】 |
| 21 | 節 3 Debian 直起動 | 行ごとに表示 |
| 21 | 節 4 フォールバックと `official-once`、節 5 SPI の KNL を書き直す | 【旧】 |
| 21 | 節 6 診断に使うコマンド | 【CFW共通】 |

## 3. 自分の環境でこうしたいとき(逆引き)

自分の環境(モデル・公式ファームの版・HDD・やりたいこと)から、**どの層の、どの文書を読めばよいか**を引く表です。さらに細かい振り分けは [docs/00](00_where_to_start.md)、確認状況は [docs/20](20_compatibility_and_reports.md) です。

| やりたいこと・状況 | 層 | 読む文書 |
|---|---|---|
| 純正のまま、壊れた HDD を交換・復旧したい | 【OFW】 | [04](04_hdd_recovery_guide.md)、(症状別は [21 節 2](21_troubleshooting.md)) |
| 純正のまま、SPI の中身を保存しておきたい | 【OFW】 | [16](16_spi_backup_dlm.md) |
| Debian を動かしたい(安定版) | 【旧】(導入に【OFW】の仕組みを使う) | [13](13_full_setup_from_official_hdd.md)、仕組みは [03](03_firmware_format.md)・[05](05_kernel_and_direct_boot.md) |
| 新しいカーネル(6.12)を試したい | 【新】 | [22](22_new_kernel_612.md)、機能一覧は [23](23_kernel_feature_inventory.md) |
| テレビを見る・録る(カーネルは問わない) | 【CFW共通】 | [11](11_tv_streaming.md)、手順の根拠は [07](07_driver_ioctl_bcas.md)・[08](08_tuner_i2c.md)・[12](12_ts_and_bcas_decrypt.md) |
| 状態・温度・SMART を監視したい | 【CFW共通】 | [18](18_status_page.md) |
| 電源オフ・予約起動・LED を扱いたい | 【固定】【CFW共通】 | [06](06_mcu_watchdog.md) |
| 起動しない・止まる原因を探したい | 層による | [21](21_troubleshooting.md)(LED の意味は【固定】、症状ごとに層を表示) |
| 自分の組み合わせ(モデル・ファーム・HDD)で動いたか知りたい | 混在 | [20](20_compatibility_and_reports.md) |
| ブートチェーン・SPI の構造を知りたい | 【固定】 | [01](01_hardware.md)・[02](02_boot_chain.md) |
| 公式の API・公式の挙動を知りたい | 【OFW】 | [17](17_official_web_api.md)・[15](15_p3_structure_and_hai.md)・[03](03_firmware_format.md) |
| 旧と新、どちらを選ぶか迷う | 【旧】【新】 | [00 の 2-1](00_where_to_start.md)、[14](14_newer_debian_kernel_and_distribution.md) |
| 別のモデルやファームの版で試したい | 【固定】【OFW】 | [20 節 1](20_compatibility_and_reports.md)、[10 の F](10_status_and_limits.md)(作者側では検証不可の項目) |

**調べ方の手順**(人でも AI でも同じです):
1. 自分の環境で、**どの層を使うか**を決める。公式のままなら【OFW】、自作ファームなら【旧】か【新】(迷ったら安定版の【旧】)。どちらにも【固定】と、自作なら【CFW共通】が付きます。
2. 上の逆引きで、文書を選ぶ。
3. その文書の**冒頭の層の印**と、**節の見出しの下の層の印**を見て、自分の層に当てはまる節だけ読む。
4. **確認状況は [docs/20](20_compatibility_and_reports.md)** で、自分の組み合わせ(モデル・ファーム・HDD・層)を探す。載っていなければ「未確認」です。報告を歓迎します。
5. 層をまたぐ記述に出会ったら(例: 旧カーネルの手順の中の「公式の更新の仕組み」)、**その節が属する層**の文書(この表で引ける)で、前提を確かめる。
