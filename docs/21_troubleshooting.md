# 21. トラブルシューティング(LEDの状態とネットワークの応答から、原因を探す)

起動しない・止まるときに、**LEDの状態**と**ネットワークに出ているか**から、原因の候補と対処を引く表です。
HDDの復旧([docs/04](04_hdd_recovery_guide.md))、Debian直起動([docs/13](13_full_setup_from_official_hdd.md))、NVの扱い([docs/16](16_spi_backup_dlm.md))で、実機で起きたことをまとめています。
**実機1台での観測**です(検証状況は [docs/20](20_compatibility_and_reports.md))。LEDの色の細かい見え方は、個体や環境で違うことがあります。

## 0. まず確認すること

1. **ネットワークに出ているか**: ルーターのDHCP一覧、または `ping`、公式アプリなら `http://<nasneのIP>:64210/status/softwareVersionGet`(`0260` が返れば公式アプリが起動している)。
2. **LEDの状態**(下の表)。**PWR LED** は基板のマイコン(MCU)が駆動していて、状態が分かりやすい。
3. **どの段階か**: 公式のまま / HDD復旧中 / Debian直起動 / 公式アプリを1回だけ起動(`official-once`)のどれか。

## 1. PWR LED(緑)の意味

PWR LED は MCU の「表示コード」で決まります([docs/06](06_mcu_watchdog.md))。

| PWR の様子 | 意味 |
|---|---|
| 点灯(固定) | 通常運転(Debian では、起動の約1分後に `nasne-mcu-wd` が点ける) |
| 1Hz で点滅 | 電源投入後〜アプリが点灯させるまでの状態 |
| **速い点滅(約4Hz)** | 公式アプリが「HDDが異常または未接続」と判断した状態 |
| 遅い不規則な点滅 | エラー表示。**起動の失敗ループ**(init が展開・検査に失敗)でもこの形になる |
| 消灯 | 電源断、または一部の状態 |
| **赤の点灯(Debian 稼働中)** | **状態ページが異常を報告している**(HDD の温度・SMART・保留/代替/訂正不能セクタの**急増**(24時間で16個以上。あるだけでは赤にならない)、ディスク使用率95%以上、チューナーの固まり、メモリ不足)。`http://<nasne>:8302/` の警告欄で内容を見る。異常が消えると緑に戻る(`nasne-ledd`。判定は30秒ごと・2回続けて同じ結果で切り替え)。赤が続くのが嫌なら `/etc/default/nasne-led` に `NASNE_LED_PWR_WARN=0` |

## 2. 症状から探す(HDDの復旧・公式ファームの起動)

| 症状 | 考えられる原因 | 対処 |
|---|---|---|
| **PWR 緑点滅のまま、LAN/HDD が橙・赤で点滅。ネットワークに出ない** | 起動の失敗ループ(init が `.dlm` を受け付けていない)。**HDDの sys1 に `00550066.dlm` が無い**ことが多い(**sys1 をマウントせずに `phase1`/`final` を実行して、PC側のメモリに書いてしまった**)。ほかに `ChipID ERROR` / `DLM date ERROR` / CRC・hwtype の不一致、区画ラベル(`sys1`/`sys2`)・ext3 でない | HDDをPCに戻し、`ls /mnt/nasne_sys1` で `00550066.dlm` `00110022.dlm` `11002200` `33004400` があるか確認。`nasne_hdd_rebuild.py verify`。最新版のツールは、マウントされていない場所への書き込みを止める。LEDだけでは原因を切り分けられない([docs/03](03_firmware_format.md)) |
| **PWR/REC が赤点灯、LAN が点灯したまま止まる(起動の約35〜40秒後。ネットワークにも出ない)** | 公式アプリが起動してから止まった。(a) **p3 の初期構造が無い**(`p3init` をしていない)、(b) **本体のNV(SPIの設定領域)が、前に使っていたHDDの状態を引きずっていて、新しいHDDと食い違っている**(別のHDDを使っていた本体で起きる。実機で確認) | (a) `p3init`。(b) **NVを空にする `.dlm`(`--mode nvreset`)に差し替えて起動する**。今のNVは先にHDDへ保存されるので、あとで戻せる([docs/04](04_hdd_recovery_guide.md)、[docs/16](16_spi_backup_dlm.md)) |
| REC/LAN が高速点滅(約2分後。手順2) | 公式アプリが p3 の初期化を待っている(v1.00 の方式)。`phase1` の起動では、個体IDが書かれた時点で十分 | 電源を切って、HDDをPCに戻し `read-id` |
| PWR 点滅のままネットワークに出ない(手順4) | p3の区画が無い、または `final` の `--chipid` が違う | `mkdisk` の既定で p3 を作る。`read-id` の値を `--chipid` に |
| **速い点滅(PWR 約4Hz)**。公式アプリは起動している | HDDが異常/未接続と判断された。**`.hai`(HDD登録情報)のモデル名・シリアルが、本体から見たHDDと合っていない**(USB-SATA アダプタが、HDD ではなくアダプタ自身の名前・シリアルを見せていると起きうる) | `smartctl -i /dev/sdX` の値と `lsblk` の値が一致するか確認。違うなら `nasne_hai.py gen` で `--model` `--serial` を指定して作り直す |
| 起動するが、HDDが「未登録」と出る | `.hai` のシリアルとHDDが一致していない(`p3init` の `--device` を取り違えた) | `mkdisk` からやり直す |
| `p3init` が「p3 が空ではない」と言う | **手順2でnasneを1回起動すると、p3に `share` `setup` が作られる**(実機で再現) | sys1 を消さずに `nasne_hdd_rebuild.py mkdisk /dev/sdX --serial <シリアル> --p3-only` で p3 だけ作り直し、`p3init` をやり直す |
| `00110022.dlm` が作られない(`read-id` が失敗する) | initがそこまで進んでいない | 区画のラベルと ext3、`00550066.dlm` の配置を確認 |
| 手順の途中で `not mounted` と出る / マウント先が `/media/...` | デスクトップの自動マウントが先に区画をつかんでいる | `lsblk` でマウント先を確認。ツールが止めてくれる。自動マウントのパスをそのまま使うか、一度 `umount` する |

## 3. 症状から探す(Debian直起動)

| 症状 | 考えられる原因 | 対処 |
|---|---|---|
| **ssh は通るが、サービス(`nasne-recpt1` 8301、`nasne-statusd` 8302、cron など)が1つも動かない。`runlevel` が `unknown`。約5.5分でリセットされる** | **PID 1 が Debian の `init` ではなく、Sony の `busybox` になっている**(`ls -l /proc/1/exe` が `/bin/busybox_dynamic`)。`nasne_init` がフォールバックしている | `cat /proc/1/fd/1` で `nasne_init` のログを読む(`FALLBACK: ...` の理由が出る)。理由に応じて下の表へ |
| `nasne_init` のログに `FALLBACK: mount sda3 failed` | SPIに書かれたカーネルが古く、**XFS の p3 の中の `root.img`(imageモード)に対応していない** | カーネルを再ビルドして KNL を書き直す。`nasne_setup.py` は、入力が新しければカーネル・KNL・ツリーを自動で作り直す(古いと使い回さない)。書き込みは `NASNE_KNL=<新しい knl_new.bin> nasne-stage1.sh`(下の「SPIの KNL を書き直す」) |
| `FALLBACK: boot count 'xx' >= 2` | Debian が2回続けて、起動成功の印(`/etc/rc.local` がカウンタを空にする)を付けずに終わった | ssh で入れる。原因を直したら `: > /var/lib/nasne-boot-count` で空にして `reboot -f`。Debian に戻る |
| `FALLBACK: no /etc/nasne-direct-boot` | p3 に直起動の印が無い(オプトイン) | 印を置く(`touch /etc/nasne-direct-boot`) |
| `stage1 ABORT: すでに 2 回試行した(上限 2)` | SPIの KNL を書く試行の上限(安全装置)。または KNL が SPI と違う | `/var/lib/nasne-stage1/tries` を消せば再試行。**消す前に、chroot 内の `knl_new.bin` と `.sha256` が、書きたいカーネルになっているか確認**(違うと、新しいカーネルを旧版へ書き戻す) |
| dlm を差し替えたのに、新しいカーネルが入らない | stage1 が書く KNL は、dlm ではなく **Debian のイメージ内**(`/usr/local/share/nasne-stage1/knl_new.bin`)にある。イメージが入っている(`need_install=0`)と、再展開されない | `scp` で新しい `knl_new.bin` を送り、`NASNE_KNL=/tmp/knl_new.bin nasne-stage1.sh`。続けて、イメージ内の `knl_new.bin` と `.sha256` も新しい版にそろえる |
| **約333秒(5.5分)ごとに再起動する** | MCU のウォッチドッグが止まっていない(`nasne-mcu-wd` が動いていない)。公式アプリが動いていない環境では、MCU が本体をリセットする | `/var/log/nasne-mcu-wd.log`。手動で止めるなら `mcui2c w 20 01`([docs/06](06_mcu_watchdog.md)) |
| 状態ページに「**チューナーのプロセスが固まっている**」と出る | **D 状態のプロセスは、正常な受信中にもほぼ常に現れる**(ドライバの中で待つため)。以前の判定は誤検知していた。今の判定は「D のまま CPU 時間が30秒以上進まない」 | `/proc/<pid>/stat` の `utime+stime` が増えているか確認。増えていれば正常。増えずに30秒以上なら固まっている。**電源の入れ直しで戻る**([docs/11](11_tv_streaming.md)) |
| **6.12 カーネルで、起動したのか分からない/固まった** | UART が無いので、画面に何も出ない | 早期コンソール(UDP ブロードキャスト、ポート 6666)を `scripts/newkernel/netlog.py` で受ける。固まったときは、`/proc/sysrq-trigger`(`echo m` でメモリの状態、`echo s` で同期)。詳しくは [docs/22](22_new_kernel_612.md) |
| ssh が `no matching host key type` などで失敗する | nasne の sshd が古い | `-o PubkeyAcceptedAlgorithms=+ssh-rsa -o HostKeyAlgorithms=+ssh-rsa`、RSA の鍵 |
| 時計が1970年から始まる、`apt update` が「Release file is not valid yet」 | 基板にRTCが無い | NTP(`ntpdate`)や `fake-hwclock`。Debian イメージには組み込み済み |

## 4. フォールバックと `official-once`

| 状況 | 動き |
|---|---|
| **`official-once`**(`nasne-boot-switch official-once` → `reboot`) | 公式アプリが **1回だけ**起動する(`0260` が返る)。印は消費され、**電源を入れ直すと Debian に戻る**。公式アプリが動いている間は ssh できない。NV と HDD が揃っている必要がある |
| **フォールバック**(`nasne_init` が Sony の `/init` へ戻る) | **公式アプリは起動しない**。Sony の miniroot とネットワークが上がり、**ssh が開くので調査ができる**。起動の失敗(カーネルの不整合・イメージの破損など)の原因を、そこで調べて直す |
| `rescue-once`(`nasne-boot-switch rescue-once`) | Sony の miniroot とネットワークだけで待つ(調査・救出用) |

フォールバックが公式を自動で起動しないのは、**設計どおり**です。フォールバックは「Debian が2回、成功印なしで終わった」状態なので、原因を確かめる前に公式アプリを走らせると、公式が p3 の NV・録画DBを書き換えて、Debian 側と食い違うおそれがあるためです([docs/05](05_kernel_and_direct_boot.md))。

## 5. SPIの KNL を書き直す(Debian 直起動の機体で)

```bash
# 新しい knl_new.bin を送って、そのファイルで段階1を実行する(SPI全体のバックアップ→消去→書き込み→読み戻し検証→reboot -f)
scp knl_new.bin root@<nasne>:/tmp/
ssh root@<nasne> 'cd /tmp && sha256sum knl_new.bin > knl_new.bin.sha256 && NASNE_KNL=/tmp/knl_new.bin /usr/local/sbin/nasne-stage1.sh'
```

- 書き込みの範囲は KNL スロットA(`0x100000` から約2MB)だけです(`mtdtool` が許可範囲を固定)。BKNL(スロットB)には触りません。
- **書き込み中は電源に触れないでください。**
- 書き込み前に、SPI 全体(13MB)のバックアップが `/var/lib/nasne-stage1/spi_mtd0_backup.bin` に取られます。失敗したときは、バックアップから自動で書き戻します。
- 試行は2回までです(`tries`)。

## 6. 診断に使うコマンド(Debian・フォールバック中の ssh)

```bash
ls -l /proc/1/exe; runlevel                    # PID 1 が Debian の /sbin/init か、Sony の busybox か
cat /proc/1/fd/1                               # nasne_init のログ(FALLBACK の理由)。削除済みファイルでも読める
tail /var/log/nasne_stage1_rcs.log             # Sony の rcS の段階1のログ
tail /var/log/nasne-stage1.log                 # SPIの書き換えのログ
cat /var/lib/nasne-boot-count /var/lib/nasne-stage1/tries
mcui2c r 20 6                                  # MCU の状態。2バイト目が 0x80 ならウォッチドッグ停止中
dmesg | tail
```

## 7. 報告するとき

- `nasne-report`(個体情報を出さない)の出力を貼ってください([docs/20](20_compatibility_and_reports.md))。
- LEDの状態(PWR/REC/LAN/HDD の色と点灯・点滅)、どの手順のどこで止まったか、ネットワークに出ているか、を添えてください。
