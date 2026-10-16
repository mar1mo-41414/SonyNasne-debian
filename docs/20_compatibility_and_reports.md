# 20. 対応状況(マトリクス)と、検証結果の報告のしかた

作者が持っている nasne は **CECH-ZNR2J の1台だけ**です。「自分の環境でも動いた/動かなかった」の報告が、いちばん役に立ちます。
このページは、(1) いま何が確認できているか、(2) 何を報告してもらえると助かるか、(3) 個体情報を出さずに報告する方法、をまとめたものです。

凡例: ✅ 作者が実機で確認 / 🟡 他の人の報告あり(報告が増えたらここに反映) / ⚠️ 一部だけ・条件つき / ❌ 動かないことを確認 / ❓ 未検証(報告を歓迎)

## 1. 本体・ファーム

| 項目 | 状況 |
|---|---|
| CECH-ZNR2J(初期型・2012年発売) | ✅(作者の1台) |
| CECH-ZNR1J ほか、他のモデル | ❓ 基板・チューナー構成・SPIの配置が同じか不明 |
| 公式ファーム v2.60 | ✅ |
| 公式ファーム v1.00 | ✅(v1.00 でのHDD作成と、起動) |
| 公式ファーム v1.51〜v2.5x | ❓ ファイルの構造の解析のみ。実機の通しは未確認 |
| 本体のSPIが v2.60 より古いファームのまま | ❓ |

## 2. HDD

| 項目 | 状況 |
|---|---|
| 320GB(2.5インチSATA、Hitachi) | ✅ 復旧(`mkdisk`→`final`→`p3init`)、公式での録画、`embed` の `.dlm` でDebian、SPIバックアップ・復元 |
| 500GB | ✅ 復旧・Debian直起動(2台) |
| 1TB | ✅ Debian直起動(p3をext3にして直接)/ 🟡 HGST HTS541010B9E380 でも Debian 直起動の報告あり |
| SSD 256GB(2.5インチSATA) | 🟡 公式で稼働していたHDDを SSD に換装し、`00550066.dlm` を上書きして Debian 直起動に成功したとの報告(USB-SATAでPCにつないで作成) |
| 2TB 以上 | ❓(XFSの `agcount` は容量で変える。[docs/04](04_hdd_recovery_guide.md)) |
| 別のメーカー(WD・Seagate・Toshiba など) | ❓ **`.hai`(HDD登録情報)が、型番とシリアルの先頭を使う**。型番の長さや書式が違うと合わない可能性 |
| SMR のHDD、USB外付けHDD | ❓ |

## 3. 機能(作者の環境での状況)

| 機能 | 状況 | 詳細 |
|---|---|---|
| HDDの復旧(v1.00 なし) | ✅ | [docs/04](04_hdd_recovery_guide.md) |
| SPIのバックアップ(`.dlm`) | ✅ `--dest sys1` `both` / ⚠️ `p3` 単独は未確認 | [docs/16](16_spi_backup_dlm.md) |
| SPIの復元(`--mode restore`) | ✅ | [docs/16](16_spi_backup_dlm.md) |
| Debian直起動(`embed`・`image`) | ✅ / 🟡 別の人の報告あり(公式で稼働していたHDDの `00550066.dlm` を上書きして成功) | [docs/13](13_full_setup_from_official_hdd.md) |
| Debian直起動(`embed`・`format`、`p3` モード) | ✅ | 同上 |
| 公式アプリを1回だけ起動(`official-once`) | ✅ NVとHDDが揃っていれば動く(食い違うと固まる→NVを空にして復旧) | [docs/13](13_full_setup_from_official_hdd.md)、[docs/04](04_hdd_recovery_guide.md) |
| 本体のNVを空にする(`--mode nvreset`、`nasne-nvreset`) | ✅ | [docs/16](16_spi_backup_dlm.md) |
| 起動失敗時の自動フォールバック | ❓ 実機では失敗が起きなかった | [docs/05](05_kernel_and_direct_boot.md) |
| 地デジの受信・配信(HTTP)、Mirakurun、EPGStation | ✅ / 🟡 別の人の報告あり(PCのDockerでMirakurun+EPGStation、iPhoneのEPGPlayerで予約・視聴に成功) | [docs/11](11_tv_streaming.md) |
| 状態ページ・電源オフ・起床タイマ・電源ボタン | ✅ | [docs/18](18_status_page.md)、[docs/06](06_mcu_watchdog.md) |
| BS/CS | 🟡 **BS は受信できたとの報告あり**(`nasne_recpt1.c` に、復調ICのレジスタ `0x8F`/`0x90` へ TSID を書くスロット選択と、BS全スロットの TSID 表を足して、BS全局を分離・受信。Mirakurun / EPGStation で EPG・同時利用まで確認、とのこと)。**このリポジトリのコードにはまだ取り込んでいない**ので、標準の `nasne-recpt1` では BS の既定スロット以外は受信できません。CS は未確認。作者の環境は BS/CS の信号が来ていない | [docs/08](08_tuner_i2c.md) |
| 複数のチューナー、同時録画 | ❓ チューナーは1系統 | |
| 数日の連続運用 | ❓ | |

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
