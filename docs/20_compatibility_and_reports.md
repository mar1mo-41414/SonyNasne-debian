# 20. 対応状況(マトリクス)と、検証結果の報告のしかた

作者が持っている nasne は **CECH-ZNR2J の1台だけ**です。「自分の環境でも動いた/動かなかった」の報告が、いちばん役に立ちます。
このページは、(1) いま何が確認できているか、(2) 何を報告してもらえると助かるか、(3) 個体情報を出さずに報告する方法、をまとめたものです。

凡例: ✅ 作者が実機で確認 / 🟡 他の人の報告あり(報告が増えたらここに反映) / ⚠️ 一部だけ・条件つき / ❌ 動かないことを確認 / ❓ 未検証(報告を歓迎)

## 1. 本体・ファーム

| 項目 | 状況 |
|---|---|
| CECH-ZNR2J(初期型・2012年発売) | ✅(作者の1台) |
| CECH-ZNR1J ほか、他のモデル | ❓ 基板・チューナー構成・SPIの配置が同じか不明 |
| **CUHJ-15004**(1TBモデル)・公式ファーム v2.57 | 🟡 **SPIバックアップ専用 `.dlm`(`--mode backup`)を、その HDD 自身の `00550066.dlm`(v2.57)から作って sys1 に置いたら、公式ファームが正常に起動し、既存の録画も再生できた**(Issue #2、報告者1名。WSL2 でビルド、HDD は USB-SATA)。**SPI バックアップを回収し、`spi_ab_info.py` で解析した結果、KNL/BKNL はビルド 72(本体 md5 `dc6d8085…`)、RFS はビルド 50、BRFS はビルド 8 で、作者の機体と同じ世代**。公式 v2.57 から取った KNL セグメントは、公式 v2.60 パッケージ内のものとセグメント全体でバイナリ一致(報告者による確認)。基板が同じかは未確認。**Debian の直起動に成功**(`--mode embed --p3-mode image`。初回起動で SPI の KNL を書き換えて検証 OK、ウォッチドッグ停止も動作。Debian 7 + 自作カーネル 2.6.29、HDD 内の XFS に置いた 8GB の ext3 イメージ)。**`nasne-boot-switch official-once` で純正に一時復帰 → 純正の設定画面・既存録画の再生・地デジ(複数局)・BS・CS の受信を確認 → 電源の入れ直しで Debian に復帰**まで一巡(Issue #2、WSL2 でビルド)。純正で BS・CS が受信できたので、この個体の基板は BS/CS に対応。Debian 上でのチューナー利用と長時間稼働は未報告 |
| 公式ファーム v2.60 | ✅ |
| 公式ファーム v1.00 | ✅(v1.00 でのHDD作成と、起動) |
| 公式ファーム v1.51〜v2.5x | ⚠️ 全10パッケージの構造を解析(KNL は3世代、RFS は全バージョン同一)。**v1.51 は実機で通した**(KNL・rootfs とも v1.51 で起動し、公式の更新で v2.60 へ)。v1.52〜v2.5x の途中の状態は、実機では止められない(公式の更新は一度で v2.60 まで上がる)ため未検証 |
| 本体のSPIが v2.60 より古いファームのまま | ⚠️ **KNL(カーネル)だけ v1.51 世代(2012-08)に戻した本体で、v2.60 の rootfs の公式アプリが起動し、HDD も登録された**(実機1台)。RFS(miniroot)は全バージョンで同一、KNL は v1.51 / v1.52〜v1.71 / v2.00〜v2.60 の3世代だけ(パッケージの比較)。KNL も rootfs も v1.51 の完全な旧世代でも、公式アプリが `0151` で起動し(実機)、**そこから公式の WebUI の更新で v2.60 まで上がった**([docs/02](02_boot_chain.md)) |

## 2. HDD

| 項目 | 状況 |
|---|---|
| 320GB(2.5インチSATA、Hitachi) | ✅ 復旧(`mkdisk`→`final`→`p3init`)、公式での録画、`embed` の `.dlm` でDebian、SPIバックアップ・復元 |
| 500GB | ✅ 復旧・Debian直起動(2台) |
| 1TB | ✅ Debian直起動(p3をext3にして直接)/ 🟡 HGST HTS541010B9E380 でも Debian 直起動の報告あり |
| SSD 256GB(2.5インチSATA) | 🟡 公式で稼働していたHDDを SSD に換装し、`00550066.dlm` を上書きして Debian 直起動に成功したとの報告(USB-SATAでPCにつないで作成) |
| 2TB 以上 | ❓(XFSの `agcount` は容量で変える。[docs/04](04_hdd_recovery_guide.md)) |
| 別のメーカー(WD・Seagate・Toshiba など) | ❓ **`.hai`(HDD登録情報)が、型番とシリアルの先頭を使う**。型番の長さや書式が違うと合わない可能性 |
| 500GB(東芝 MQ01ABF050)で、`mkdisk`→`phase1`→(`p3init` が拒否→`mkdisk --p3-only`)→`final`→`p3init`→(NV食い違い→`nvreset`)→公式 v2.60 起動(HDD登録済み) | ✅ 実機の通しで確認(別の人の報告と同じ症状を再現して、手順を修正) |
| SMR のHDD、USB外付けHDD | ❓ |

## 3. 機能(作者の環境での状況)

| 機能 | 状況 | 詳細 |
|---|---|---|
| HDDの復旧(v1.00 なし) | ✅ | [docs/04](04_hdd_recovery_guide.md) |
| SPIのバックアップ(`.dlm`) | ✅ `--dest sys1` `both` `p3`(`p3` 単独も実機で確認。約82秒、MD5・SHA1が一致、別のバックアップと比べて違いは書き換えたカーネルスロットだけ) | [docs/16](16_spi_backup_dlm.md) |
| SPIの復元(`--mode restore`) | ✅ | [docs/16](16_spi_backup_dlm.md) |
| Debian直起動(`embed`・`image`) | ✅ / 🟡 別の人の報告あり(公式で稼働していたHDDの `00550066.dlm` を上書きして成功) | [docs/13](13_full_setup_from_official_hdd.md) |
| Debian直起動(`embed`・`format`、`p3` モード) | ✅ | 同上 |
| 公式アプリを1回だけ起動(`official-once`) | ✅ 1TB の `embed`・`image` 構成でも確認(予約 → 公式が `0260` → 電源を入れ直すと Debian に戻り、印は消費)。NVとHDDが揃っていれば動く(食い違うと固まる→NVを空にして復旧) | [docs/13](13_full_setup_from_official_hdd.md)、[docs/04](04_hdd_recovery_guide.md) |
| 本体のNVを空にする(`--mode nvreset`、`nasne-nvreset`) | ✅ | [docs/16](16_spi_backup_dlm.md) |
| 起動失敗時の自動フォールバック | ✅ `nasne_init` が Sony の miniroot へ切り替える(起動カウンタ `xx`。実機で確認)。**公式アプリは起動せず、ssh で入れる救出用の状態になる**。公式を動かすのは `official-once` |
| SPIの A/B フォールバック(ブートローダ側) | ⚠️ **KNL の本体の1バイトを壊して CRC を不一致にしたら、BKNL で起動して公式ファームが動いた**(実機。A/B はカーネルと miniroot で別々に選ばれる)。復旧は HDD 経由で成功、SPI は実験前と完全一致。**ヘッダまで消えた場合・BRFS で起動する場合は未検証**(KNL がほぼ消去された事故では助からなかった報告がある。[docs/02](02_boot_chain.md)) | [docs/05](05_kernel_and_direct_boot.md) |
| 地デジの受信・配信(HTTP)、Mirakurun、EPGStation | ✅ / 🟡 別の人の報告あり(PCのDockerでMirakurun+EPGStation、iPhoneのEPGPlayerで予約・視聴に成功) | [docs/11](11_tv_streaming.md) |
| 状態ページ・電源オフ・起床タイマ・電源ボタン | ✅ | [docs/18](18_status_page.md)、[docs/06](06_mcu_watchdog.md) |
| 前面LED(REC・LAN・PWR) | ✅ REC=チューナー使用中、LAN=ネットワーク確立で点灯のまま、PWR=緑(異常を報告している間だけ赤。保留セクタなどは「急増」だけ異常扱い) | [docs/06](06_mcu_watchdog.md) |
| EPGStation の予約に合わせた起床・自動電源オフ | ✅ 予約の5分前に自動で起床 → 予約どおり録画 → 使われなくなったら自動で電源オフ(任意) | [docs/06](06_mcu_watchdog.md)、[contrib/epgstation](../contrib/epgstation/README.md) |
| BS/CS | 🟡 **BS は受信できたとの報告あり**(`nasne_recpt1.c` に、復調ICのレジスタ `0x8F`/`0x90` へ TSID を書くスロット選択と、BS全スロットの TSID 表を足して、BS全局を分離・受信。Mirakurun / EPGStation で EPG・同時利用まで確認、とのこと)。**`nasne-recpt1` に取り込み済み**(`BS1`〜`BS23`・`CS2`〜`CS24` を指定できる。[docs/11](11_tv_streaming.md))。作者側では検証できません。**CS110 も受信できたとの報告**。**投稿者の環境で、`/scan?all`(PR #5。BS/CS の自律スキャン)により地デジ33・BS45・CS54の計132サービスを収集し、Mirakurun + EPGStation で視聴と番組表の取得を確認**したとの報告。Mirakurun では、チャンネル名はサービスごとに一意(`27_56360`・`BS15_0_101`・`CS16_333`)にし、サービス指定は `commandVars` に分ける(`?` を含めると API が 404。[docs/11](11_tv_streaming.md)。`nasne_mirakurun_conf.py --sat --scan` で自動生成、Mirakurun 自身のスキャン結果は `--fix-sat` で直せる)。スキャン一覧に BS7・BS17 が無いのは意図的(nasne は 2K の ISDB-S 右旋専用チューナーで、電波の無い枠のタイムアウト待ちを避けるため、放送のある BS1/3/5/9/11/13/15/19/21/23 の 10 トランスポンダに絞っている、PR #5 の投稿者による説明)。アンテナへの給電は未検証(報告者は壁面端子に信号が来ている環境)。手順は [docs/08](08_tuner_i2c.md)。作者の環境は、建物にケーブル局の BS/CS 配信が無いことを確認(信号が来ていない) | [docs/08](08_tuner_i2c.md) |
| 複数のチューナー、同時録画 | ❓ チューナーは1系統 | |
| 数日の連続運用 | ✅ 約4日(EPGStation の予約録画12本、計約45GB)。録画は全部、予約どおりの長さ(30分・35分・約2時間)で、デマックスの最後まで読めた。HDD の温度は28〜47℃。Home Assistant と Prometheus での監視も実機で確認 | [docs/11](11_tv_streaming.md)、[docs/18](18_status_page.md) |

## 4. ビルドするPC

| 環境 | 状況 |
|---|---|
| Debian 13(クリーンな仮想マシン) | ✅ `nasne_setup.py build` が最後まで通る([docs/13](13_full_setup_from_official_hdd.md)) |
| Ubuntu 24.04 | ✅ `nasne_setup.py check` が不足を正しく報告 / ❓ ビルドの通しは未確認 |
| Raspberry Pi 5(ARM64)上の Debian 13 | 🟡 `nasne_setup.py` でセットアップでき、`nasne-recpt1` の MIPS 用ビルドも通ったとの報告(依存関係は一部調整が必要だった。ARM64 ホストでの MIPS クロスコンパイル) |
| WSL2(Windows 11)上の Ubuntu 24.04 | 🟡 `nasne_setup.py build --download-official` で、`knl_new.bin`(約2.1MB、`build_knl.py verify` OK)・`debian_tree.tar`(約150MB)・`vmlinux_new.bin` の生成まで成功の報告(実機適用の前。Wheezy の GPG 期限切れの警告は出るが無害) |
| 他のLinux、macOS | ❓ Docker・`debootstrap`・`qemu-user-static` が要る |

## 5. 報告してほしいこと

次のどれでも歓迎します(うまくいった場合も、いかなかった場合も)。

1. **別のHDD(容量・メーカー)**で、復旧・Debian直起動ができたか。
2. **別のモデル・ファーム版**で、SPIバックアップ(`--mode backup`)が取れたか、`final` まで通ったか。
3. **ビルドPC**の環境(OS・版)で、`nasne_setup.py` が通ったか。
4. 動かなかったときの**症状(LEDの状態、どこで止まるか)と、ログ**。

**まず、書き込みのない `--mode backup`([docs/16](16_spi_backup_dlm.md))から**始めることを勧めます。SPIをいじる前に、自分の機体の原本が手元に残ります。

## 6. 個体情報を出さずに報告する

次の情報は、**貼らないでください**(報告に不要で、個体が特定されます)。

- MACアドレス、IPアドレス(LANの内側のものでも)
- HDD のシリアル番号、本体の個体ID(`read-id` の16桁)、`ChipID`
- 局名(スキャンの結果を貼るときは、チャンネル数・サービス数だけにする)
- B-CAS の番号

**`nasne-report`**(nasne上のDebianで実行。`prepare_tree.sh` が入れる)は、上の情報を含まない形で、必要な情報を Markdown にまとめます:

```bash
nasne-report            # バージョン・カーネル・SPIのスロットのmd5・HDDの型番と容量・SMARTの要約・サービス・状態ページ
nasne-report --scan     # 上に加えて、地デジのスキャン(チャンネル数・サービス数だけ。約80秒、チューナーを使う)
```

出力を、GitHub の Issue の「検証レポート」のテンプレートに貼ってください。

PC側の確認結果(HDD復旧のとき)は、`python3 scripts/nasne_hdd_rebuild.py verify <sys1>` の出力(「結果: OK」)を貼ってください。シリアルや個体IDが出る場合は、伏せてください。

## 7. 反映のしかた

報告が重なった項目は、このページの表を 🟡 や ✅ に更新します。「❌ 動かない」が出た場合は、その条件を [docs/10](10_status_and_limits.md) の「未解明・未検証」に書き、原因を調べます。
