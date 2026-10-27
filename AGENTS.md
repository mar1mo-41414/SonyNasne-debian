# AGENTS.md — このリポジトリで作業する AI への指示書

このリポジトリは、Sony(SIE)製 **nasne CECH-ZNR2J** を解析し、**Debian を動かす**ための公開リポジトリ。
作業を始める前に、この文書を最後まで読むこと。人間向けの概要は [README.md](README.md)、読む手順書の選び方は [docs/00](docs/00_where_to_start.md)、できる/できない/未解明は [docs/10](docs/10_status_and_limits.md)。

> この文書は「方針・手順・落とし穴・検証すべき内容」をまとめたもの。細かい事実(アドレス、プロトコル、実測値)は `docs/` にある。**この文書と `docs/` が食い違ったら `docs/` の新しい節を信じ、この文書を直す。**
> 秘密の値(パスワード、トークン、認証情報)は、コード・文書・コミット・ログのどこにも書かない。

---

## 1. 対象とゴール

| 項目 | 内容 |
|---|---|
| 機種 | nasne CECH-ZNR2J(2012 年発売の初期型)。SoC は ViXS XCode4210(MIPS 74Kc、リトルエンディアン、32bit)、DDR3 512MB、SPI フラッシュ 16MB |
| ゴール | 公式ファームを通さずに、SPI からカーネルを起動して Debian を PID1 から動かす。地デジ/BS/CS の受信・B-CAS 復号・HTTP 配信(Mirakurun/EPGStation/tvheadend)まで |
| 現在の到達点 | **安定版**: 自前ビルドの Linux 2.6.29 + Debian 7 (wheezy)。**開発版**: Linux 6.12 LTS + Debian 12(sysvinit)。SPI の KNL スロット A から単独起動し、MCU・LED・USB・ネットワーク・チューナー・B-CAS が動く |
| 公式 FW | 解析対象としては扱うが、**配布しない**(`.dlm`、SPI ダンプ、`SampleData/` は公開しない) |
| 録画データ | `.hai` は DTCP-IP で暗号化されており**対象外** |

起動の連鎖(重要):

```
boot ROM → U-Boot 1.1.3 (SPI 0x10000, XOR難読化) → 4段目 BFWF (0x50000)
        → KNL スロット A (0x100000〜0x37ffff。gzip後 2,621,376B 以内。A/B のフォールバックは CRC 失敗時だけ)
        → 内蔵 initramfs → HDD の p3 (XFS) の root.img へ switch_root → sysvinit
```

セキュアブートは**無い**(署名検証なし。「あるかも」と書いた過去の記述は誤りと確定済み。docs/02、docs/10 参照)。

---

## 2. 絶対に守ること(安全規則)

実機の SPI フラッシュを壊すと、**外付けの書き込み器(CH341A など)で書き直さないと復旧できない**。

1. **SPI への書き込みは、バックアップと復旧経路の確認が先**。KNL スロット A (`0x100000`) 以外の領域(U-Boot、4段目、FMAP、miniroot 等)は**触らない**。
2. **`mtdtool` の「拒否されるかのテスト」を許可範囲内(KNL=0x100000)でやらない**(過去に動作中カーネルの先頭 64KB を消した。範囲外が確実な `0x10000` で試す)。
3. **`mtdtool` を実機に置いてから**書く。nasne の `/tmp` は再起動で消える。`scp mtdtool` を忘れて `flash` が失敗しているのに出力の絞り込みで隠れ、古いカーネルで検証してしまった事故あり。書いた後は**読み戻して md5 を照合**する。
4. **ウォッチドッグ(MCU、約 333 秒)**: 公式経路を通らない起動では、MCU に `[0x20,0x01]` を I2C で書き続けないと約 333 秒ごとにリセットされる。`nasne-mcu-wd` サービスが担当。止めたら再起動ループになる。
5. **ワンセグ(PMT PID 0x1FC8)に `OPEN_SECUREDTS` を開かない**。ドライバが D 状態で戻らなくなり `reboot -f` が必要。`nasne-recpt1` は除外済み。
6. **tar を `/` に展開しない**(Debian 12 は usrmerge で `/lib → usr/lib`。展開で `/lib` が実ディレクトリに置き換わり ld.so が消えて全滅した)。ファイルは個別に scp/cp する。
7. **NV(不揮発領域)と HDD の食い違い**: 原本 NV に別 HDD を付けると公式 FW が固まる。Debian 機に `nvreset` dlm を使わない(docs/16)。HDD の扱いは docs/03、docs/04、docs/13 を確認し、取り違えない。**SMART が悪い HDD で実験を続けない**。
8. **`git reset --hard`、強制 push、履歴の書き換え、ブランチ削除**は、失われるものを説明して、確認を取ってから。
9. **ホスト側のシステム全体を変える操作**(`/etc`、systemd、ネットワーク、ファイアウォール、Docker daemon 設定など)は、必要性を説明して確認を取る。Python のパッケージはグローバルに入れない(`python3 -m venv .venv` と `python -m pip` を使う)。

物理操作が必要な作業(HDD の付け替え、USB の抜き差し、SPI ROM の付け替え、電源ケーブル)は、**人間に依頼する**。

---

## 3. リポジトリの構成

```
.
├── README.md / AGENTS.md                概要 / この文書
├── docs/                                手順書・技術文書(00 が振り分けリスト)
│   ├── 00_where_to_start.md             どの手順書を読むか
│   ├── 10_status_and_limits.md          できる/できない/未解明
│   ├── 20_compatibility_and_reports.md  対応状況のマトリクス(確認状況)
│   ├── 21_troubleshooting.md            トラブルシューティング
│   ├── 22_new_kernel_612.md             6.12 + Debian 12 の記録と手順
│   └── 23_kernel_feature_inventory.md   6.12 の機能一覧、省いたもの、追加できそうなもの
├── newkernel/                           Linux 6.12 のビルド一式
│   ├── build.sh                         ビルド(overlay と patches を当てる)
│   ├── overlay/ patches/                SoC 固有のドライバ・defconfig・パッチ(GMAC, USB, MCU, LED 等)
│   ├── xc4drv/ xc4patches/              xcode4drv(チューナー/B-CAS/メディアエンジンのドライバ)の移植
│   └── initramfs/                       内蔵 initramfs(init.c / init.sh)
├── scripts/
│   ├── build_tools.sh                   nasne 用の小さな静的ツール(libc なし、MIPS32r2)をビルド
│   ├── tools/                           nasne_recpt1.c(受信サーバ)、mtdtool.c、i2cx.c、ts_analyze.py、各種実験
│   ├── watchdog/                        常駐サービス(nasne-mcu-wd、nasne-ledd、nasne-waked、nasne-recpt1-server、nasne-statusd …)
│   ├── newkernel/                       6.12 の rootfs 構築・サービス導入・早期コンソール受信(netlog.py)
│   ├── stage1/                          公式 HDD の `.dlm` 差し替えだけで Debian 直起動へ移行する仕組み
│   ├── nasne_setup.py                   一括ビルド/セットアップ
│   ├── nasne_mirakurun_conf.py          Mirakurun 設定の生成(`--dual` で 2 チューナー)
│   ├── fetch_gpl_sources.sh             SIE 公開 GPL ソースの取得(`gpl_src/` へ。sha256 検証つき)
│   └── nasne_db.py / dlm_crypto.py / build_dlm.py / build_knl.py / spi_*.py   暗号・SPI・DLM の解析/生成ツール
├── contrib/                             EPGStation / tvheadend / Samba / Home Assistant 連携
└── local.env                            環境依存の設定(git 管理外。自分で作る。§4)
```

公式 FW・SPI ダンプなどは、リポジトリに入っていない・入れてはいけない。必要な場合は、自分の実機から取得する(手順は docs/04、docs/13、docs/16)。

---

## 4. 環境と実機の扱い

### ビルド環境

- クロスコンパイラ: `mipsel-linux-gnu-gcc`(Debian/Ubuntu: `apt install gcc-mipsel-linux-gnu`)。
- ユーザー空間ツールは libc なしでビルドする: `-nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -ffreestanding -fno-tree-loop-distribute-patterns -fno-stack-protector -e __start`(`memset` などの暗黙の呼び出しに注意)。`scripts/build_tools.sh` が全部やる。
- 巨大ファイル(カーネルツリー、`build_stage1/`)は Git に入れない。

### `local.env`

`newkernel/build.sh`、`scripts/newkernel/*.sh`、`newkernel/xc4drv/deploy.sh` が読む、環境依存の設定ファイル(git 管理外)。自分の環境に合わせて作る:

```sh
NASNE_IP=<nasne の IP アドレス>
NASNE_EXTRA_BOOTARGS=<カーネルに足す起動引数(任意)>
```

### nasne 実機への接続

```bash
source ./local.env
ssh -o PubkeyAcceptedAlgorithms=+ssh-rsa -o HostKeyAlgorithms=+ssh-rsa root@$NASNE_IP
```

- Debian 7 側の sshd は古い(OpenSSH 6.0)。**`ssh-rsa` の 2 オプションが毎回必須**。ed25519 鍵は使えない(RSA のみ)。`scp` も同様。
- 6.12 + Debian 12 側でも同じオプションで通る。ホスト鍵は再起動や入れ替えで変わるので、実機だけは `-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null` を付けてよい。
- nasne の `/tmp` は再起動で消える。root ファイルシステムは HDD の p3(XFS)の `root.img`。
- 実機が無応答になることがある。電源ケーブルを抜き差しするか、電源を遠隔で切り替えられる仕組み(スマートコンセントなど)で入れ直す。**off と on の間は 8 秒以上**あける。

### 早期コンソール

UART/JTAG は基板に**無い**(信号未結線、docs/01)。代わりに:
- **UDP ブロードキャスト(ポート 6666)の早期コンソール**を `scripts/newkernel/netlog.py` で受ける。
- 前面 LED(PWR=MCU、REC 赤=GPIO 0x100、LAN 橙=GPIO 0x200)の点灯パターンで起動段階を見る(`XCODE_LED_DEBUG`)。
- 状態ページ(ポート 8302)、ハートビートのログ(`/var/log/nasne-hb.log`)。

---

## 5. 典型的な作業の流れ

### 5.1 ユーザー空間ツール(`nasne-recpt1` など)の変更

```bash
sh scripts/build_tools.sh /tmp/out        # mtdtool, mcui2c, i2cx, nasne-recpt1 をビルド
```

実機へは、元のバイナリを退避してから、別名で転送して `mv` で原子的に置き換え、サービスを再起動する:

```bash
scp … root@$NASNE_IP:/usr/local/sbin/nasne-recpt1.new
ssh … 'cp -p /usr/local/sbin/nasne-recpt1 /usr/local/sbin/nasne-recpt1.bak-<理由> \
       && mv /usr/local/sbin/nasne-recpt1.new /usr/local/sbin/nasne-recpt1 \
       && /etc/init.d/nasne-recpt1-server restart'
```

- 試験は、別ポート(例 `--listen 8352`)で試作を動かして行うと、**本番のサービスを止めずに**確認できる。
- `pkill -f <名前>` は自分のシェルも殺しうる(exit 144)。**PID か `[b]racket` パターン**で止める。シェルスクリプトの `wait` は、引数なしだとバックグラウンドのサーバも待ってしまうので PID を指定する。

### 5.2 カーネル(6.12)の変更

```bash
newkernel/build.sh            # newkernel/out/new/vmlinux(.bin) と、KNL 用の gzip 後サイズが出る
```

- **上限**: gzip 後 2,621,376 バイト以内(KNL スロットの大きさ)。機能を足すときは**サイズの増分を測る**(docs/23)。
- 試験の順序(**安全な順**):
  1. 可能なら **kexec** で先に起動する(SPI に書かない。失敗しても電源を入れ直せば元に戻る。docs/22 参照)。
  2. 動いたら `mtdtool flash /dev/mtd0 0x100000 <knl.bin>` で KNL スロットへ。**書く前に現行を読み戻して保存**。書いた後、読み戻して md5 照合。
  3. 電源を入れ直して cold boot(kexec では旧カーネルが SoC を初期化済みなので、cold で動くかは別の確認)。
- ビルドは「パッチが適用済みか」を逆適用の dry-run で判定する。**既存パッチの内容を編集したら、ヘッダの行数も直す**(重複適用や `.rej` の原因になる)。
- 大きな変更の後は、**クリーンな環境(新しい VM や Docker など)**で一連のビルドを通す(毎回は不要)。

### 5.3 ドキュメントの書き分け

- `README.md`: 簡単な使い方と「現在確定している最新情報」だけ。
- 詳しい手順・技術的な背景・未解明の問題・既知の制限は `docs/` に書く(6.12 まわりは docs/22、確認状況は docs/20、できる/できない/未解明は docs/10)。README には長い経緯を書かない。
- このリポジトリは公開されている。文書・コード・コミットに、**環境固有の情報**(自分のホスト名、IP アドレス、シリアル番号、チップ ID、個人のアカウント名など)を書かない。一般化する(例: 「自宅サーバ」、`192.168.x.x`)。

### 5.4 コミット

- コミットメッセージは日本語、要点を 1 行目に。
- 公式 FW(`.dlm`)、SPI ダンプ、`SampleData/`、パスワード・トークンは**絶対にコミットしない**。コミット前に `git status` で確認する。
- 他人のリポジトリへの push、PR のマージ、外部への投稿は、確認を取ってから。

---

## 6. 検証の方針(何をどこまで確認するか)

**状態を 3 つに分けて書く**(README・docs・Issue の返信で徹底する):

| 状態 | 意味 |
|---|---|
| 検証済み | 実機で再現して確認した(何台で、どの構成で、を書く。確認状況は docs/20 に集約) |
| 検証不可 | 別の本体、別のファーム版、BS/CS の信号、別の HDD など、手元に無いものが要る。**報告者に頼る**と明記する |
| 後日検証 | HDD の付け替えなどの物理作業で足りる。まとめて人間に依頼する |

作業の優先順位: **物理操作が要らない静的解析** → **HDD 付け替えが不要な実機解析** → 物理操作が要るもの(まとめて依頼)。

**手元で再現できていないことを「直った」と書かない**。電波に依存しない部分(待機・ロック・プロセスの動作など)は手元で確認できるが、その場合も、**実運用と同じ経路**(クライアントの切断=SIGPIPE、SIGTERM、複数の同時接続)で試す。過去に、時間切れの正常終了だけを試して、切断時の不具合を見逃した。

### 6.1 変更ごとの検証チェックリスト

| 変更したもの | 最低限やること |
|---|---|
| `nasne_recpt1.c` | ①ビルドが通る ②別ポートで試作を起動して、単独の選局・配信が正常(`ts_analyze.py` で PAT/PMT、CC、TEI、スクランブル確認) ③異常終了・切断(SIGPIPE)・SIGTERM の各経路 ④複数セッション(ロックの取り合い)の動作 ⑤docs(`docs/11`、`docs/22`)に反映 |
| カーネル/ドライバ | kexec で起動 → ログ(netlog)で例外がない → 該当機能 → cold boot → 数十分〜数時間の安定。**GMAC の MMC 割り込み**(カウンタが 2^31 に達すると割り込み嵐)のように、**長時間で出る問題**があるので、長時間の確認を省かない |
| USB / ストレージ | ホットプラグを複数回、読み書き中の抜き取り、`fsck`。過電流検出は未実装(基板の信号が不明) |
| 起床タイマ・電源 | `0x02 0x02` は書くと**即電源断**。`nasne-waked` 経由でのみ |
| SPI 書き込み | §2 の 1〜3。読み戻しの md5 を必ず記録 |
| 文書・成果物を公開する前 | 環境固有情報(ホスト名、IP、シリアル、チップ ID、認証情報)が無いか、公式 FW・SPI ダンプが混ざっていないか |

---

## 7. nasne-recpt1(受信サーバ)の設計メモ

最も更新が多いファイル。`scripts/tools/nasne_recpt1.c`(libc なし、1 ファイル、HTTP で `GET /tuner/<ch>?sid=0x…&sec=N`、`/status`、`/playlist.m3u8`)。

- 接続ごとに fork。ドライバの ioctl を直接叩く: `OPEN_PSI_STREAM`(入力選択 0=地デジ、1=衛星)、B-CAS の GENERIC ストリーム(0x2e open / 0x13 ACTIVATE / 0x2f close)、`OPEN_SECUREDTS`(0x30)、ストリームを閉じる(0x11)。`ACTIVATE` の `rv=2` は「カードがビジー」(即座に返る)。
- **`--dual-tuner`**: 地デジ(T、I2C 0x10)と衛星(S、0x11)を別のロック(`/tmp/nasne-recpt1-T.lock`、`-S.lock`)で扱う。B-CAS は 1 枚・1 エンジンなので共有ロック(`-bcas.lock`)。先着が最初の映像/音声を出す(=鍵を得る)まで最大 4 秒ロックを保持し、後着は ACTIVATE をバックオフ付きで再試行する。
- **GENERIC を閉じると、動いている他方が止まる**(カーネルログ `Boxster, [txc_write_input_done] … INPUT_DONE: Mips Failed`)。対策として、終了するセッションは**クライアントとロックを手放したあと、ほかのセッションがなくなるまで GENERIC を閉じずに待つ**(linger。最大 4 時間、待機プロセスは同時に 4 つまで)。待機の脱出条件は `term_flag`(SIGINT/SIGTERM)だけ。**SIGPIPE で `stop_flag` が立っても待つ**こと。
- `?si=1`: SI(EIT 等、暗号化されていない)だけを出す。B-CAS を使わないので、番組表の巡回に付けると、視聴中の ACTIVATE 競合を避けられる。
- Mirakurun の運用: `/app-config`、`/app-data` を永続化、`jobMaxRunning: 1`、番組表のジョブには `?si=1`。BS の既定のチャンネル名 101〜256 は使えない(`useSubCh=1`、`?sid=` が必要。`--sat`/`--fix-sat` あり)。
- 既知の未解決: ①他方の系統を視聴中に新しい系統を開くと、まれに復号の開始に失敗する(`ReceiveStream rv=6`、約 20 秒)。②多数のクライアントの頻繁な開閉でドライバが D 状態になる(検知のみ)。③長期稼働中の原因不明の無応答(観測中)。
- BS/CS と 2 系統同時受信は、**実際に電波が来る環境でしか確認できない**。電波なしの環境では、待機・ロック・プロセスの動作までしか検証できない(§6)。

---

## 8. 積みタスクと方針

- **ブートローダの差し替えは「方式 A」**: KNL スロット A にチェーンローダーを入れ、HDD からカーネルを読む。**U-Boot と 4 段目は触らない**。未着手。着手前の低リスクな確認: 4 段目から KNL への受け渡し、ローダーの文脈で SATA が動くか。4 段目の差し替え・U-Boot の差し替えは、理由がないのでやらない。
- **バイナリ(Sony 由来の `.dlm`・ドライバ・SPI)は配布しない**。そのうえで、面倒な手順はツール・スクリプトで吸収する(手作業の手順書にしない)。
- **Debian 13 は mipsel が無い**。Debian 12 が上限。
- そのほかの未解明・未検証は docs/10。

---

## 9. 落とし穴(過去に踏んだもの)

- **キャッシュ/DMA の試験**: バッファの種類(`mmap` の既定は `MAP_SHARED` の shmem)で結果が変わる。MIPS 74Kc は D-cache がエイリアスする(64KB 4-way)ので、`MAP_SHARED` と `O_DIRECT` の組み合わせで古い値が見える。まず private / malloc / shared を切り分けてから原因を探す。
- **実機のハングの原因を HDD に決めつけない**: ある固まりは HDD ではなく GMAC の MMC カウンタ割り込み嵐だった(MMC_RX_INTR_MASK 0x10c、TX 0x110、RX_IPC 0x200 をマスク)。
- **USB**: SDUH の間接レジスタ(EHCI は EOC 0x2004、OHCI は OOC 0x200c)。ctypes での usbfs 試験はオフセットを誤りやすいので C で書く。
- **ホスト全体の `sync`** は他の重い書き込みで何分も待たされる。`sync <file>` を使う。
- **PAT の差し替え**: `--sid` 指定時は単一サービスの PAT にして出力する。SI のレーン(`is_si_pid`)は別扱い。
- **公式 FW に自作カーネルや観測シムを混ぜると不調になる**(クリーンな公式では、スキャン・録画ともできる)。
- **procmng の anti-tamper**: `LD_PRELOAD`/`IFS` を検出する。自前シムが原因の誤認が過去にあった。
- **ssh のホスト鍵**: 実機は再起動や入れ替えで鍵が変わる。なりすましと決めつけない(実機以外では警告を無視しない)。
- **推測を事実として書かない**。例: 「ACTIVATE の待ち時間を延ばせば直る」は誤りだった(`rv=2` は即時のビジー)。仮説は仮説と書き、実測で確定したものだけを README に書く。分からない設定項目名などを推測で書かない。

---

## 10. 作業開始時のチェックリスト

1. この文書、[docs/00](docs/00_where_to_start.md)、[docs/10](docs/10_status_and_limits.md)、[docs/20](docs/20_compatibility_and_reports.md)、[docs/22](docs/22_new_kernel_612.md) を読む。`start-note.md` があれば先に読む(読み取り専用。編集しない。`.gitignore` に入れる)。
2. `git status` と `git log --oneline -10` で現況を確認する。
3. 実機を触る前に、`local.env` の `NASNE_IP` へ ssh が通るか、いまの構成(安定版か 6.12 か)を確認する(`uname -r`、`cat /etc/debian_version`)。
4. 変更したら: 試験 → docs(該当する手順書、確認状況は docs/20)→ コミット。
5. 分からないこと、この文書と食い違う状況は、勝手に直さず、人間に確認する。
