# 13. 公式ファームで動いているHDDから、`00550066.dlm` 1ファイルの差し替えだけでDebian直起動にする

> 対象: **公式ファーム(v2.60)で正常に動いているnasne(CECH-ZNR2J)のHDD**。HDDの紐付けは正常な状態からスタートする。
> ([docs/04](04_hdd_recovery_guide.md) のHDD全損復旧とは別の話。)
>
> やること: HDDのsys1にある `00550066.dlm` を自作版に**1ファイル差し替えるだけ**。nasneに挿して電源を入れると、nasne自身が
> (1) HDDのp3にDebianを作り、(2) SPIフラッシュのカーネル(KNL)を自作カーネルに書き換え、(3) 再起動して**Debian(PID 1)**になる。
> ウォッチドッグの停止、地デジの受信と配信([docs/11](11_tv_streaming.md))まで、そのまま使える。
>
> **公式のp3(XFS。録画データが入っている)は消さずに残す**のが既定(`--p3-mode image`)。p3の中に `/.nasne-debian/root.img`(ext3のイメージ)を作ってそこにDebianを入れ、
> 録画データはDebianの `/data` から見える(`.hai` はDTCP-IPで暗号化されていて、公式アプリでしか再生できない)。p3を初期化して直接Debianにしたい場合は `--p3-mode format`(消える)。
>
> ⚠️ 自己責任です。SPIフラッシュへの書き込みを伴い、失敗すると起動しなくなる可能性があります
> (自動のバックアップと書き戻しを入れていますが、**CH341A等のSPIライタで事前に全体をダンプしておくことを強く勧めます**)。

## 検証状況

**機能ごと・HDDごと・環境ごとの確認状況は [docs/20](20_compatibility_and_reports.md) にまとめています**(こちらの表は、そこに一本化しました)。この手順に直接関わる要点だけ:

- `embed`(`image`・`format`)と `p3` モード、SPIのバックアップ・復元、VMで作った `.dlm`(状態ページ入り)の実機への新規インストール: ✅ 実機1台で通し確認。
- 公式アプリを1回だけ起動して戻る(`nasne-boot-switch official-once`): ✅ **NVとHDDが揃っていれば動く**(下の「公式アプリを1回だけ起動する」)。
- `nasne-stage1.sh` の失敗系(書き込み失敗→自動で元に戻す、試行上限、ハッシュ不一致、ウォッチドッグ停止失敗など)の論理テスト: ✅ PC上の模擬環境で23項目(`scripts/stage1/test_stage1_sim.sh`)。書き込み失敗時の自動書き戻しの実機での動作は、実機では失敗が起きなかったので未確認。
- 他のファーム版数・他の個体: ❌ 未確認(報告を歓迎: [docs/20](20_compatibility_and_reports.md))。

## 仕組み

```
[PCで1回だけ]  00550066.dlm を自作版に差し替える(+必要ならDebianのツリーを同梱する)

[1回目の起動: 公式カーネル]
  公式のminiroot → 自作 .dlm を展開(sys2) → rcS
    └ (公式アプリ procmng / startdtvtuner は起動しない)
       ├ DHCPでネットワークを上げる(`--telnet early|fail` を付けたときだけ、デバッグ用の認証なしtelnetも開く。既定は開かない)
       ├ [embedのみ] p3 にnasne用Debianが無ければ、MCUウォッチドッグを止めてから p3 を ext3 で初期化し、同梱のDebianを展開
       └ p3 のDebianをchrootして nasne-stage1.sh を実行:
            事前検査 → ウォッチドッグ停止 → SPI(mtd0)全域をバックアップ → KNL消去/書込/読み戻し検証
            → 失敗なら元のKNLを自動で書き戻す → 成功なら reboot

[2回目の起動: 自作カーネル]
  内蔵initramfsの /nasne_init が p3 のDebianへ switch_root → Debianの sysvinit が PID 1
    └ ウォッチドッグ停止(nasne-mcu-wd)、チューナードライバ、nasne-recpt1 のHTTPサーバが自動起動
```

`rcS` の差し替えは、公式のtarを**展開せずに** Python の tarfile でメンバー単位にコピーして、最後の `rcS` だけ書き換える(`make_stage1_dlm.py`)。
展開用のディレクトリを使い回して古い設定が混ざる事故が起きない。**公式のtarには `etc/init.d/rcS` が2つあり、後のエントリが有効**なので、最後のものだけを差し替えている。

`/sbin/init` を差し替える方法は使えない(`/sbin/init` は `busybox` へのシンボリックリンクで、tar内でこれを通常ファイルに変えると
実機のminirootのtarが途中で止まる。[docs/03](03_firmware_format.md))。`rcS` の差し替えが実績のある方法。

## かんたん手順: `nasne_setup.py` で1コマンド

下の「手順」1〜3(カーネル → Debianのツリー → `.dlm`)を、1つのコマンドにまとめたものです。HDDにはまだ触りません(HDDへ入れる4・5は手動)。

```bash
python3 scripts/nasne_setup.py check            # 足りないパッケージを表示(Debian/Ubuntu 向けに apt のコマンドも出る)
python3 scripts/nasne_setup.py build --download-official --hdd-dlm <HDDのsys1の00550066.dlm> --ssh-pubkey ~/.ssh/id_rsa.pub --work build_nasne
```

- 作業ディレクトリ(`--work`)に結果を残し、2回目以降は済んだステップを飛ばします。止まったら同じコマンドをもう一度実行すれば、続きから始まります。
- `--hdd-dlm` を付けないと、最後の `.dlm` 作成の前で止まります(ビルド環境の確認だけしたいとき)。
- 公式ファームパッケージ(Sony配布。再配布しないこと)は、`--download-official` で Sony のサーバから取得するか、`--official-pkg` で手元のものを指定します。
- 所要時間の目安(4コア): カーネル用ツールチェーンのDockerビルド 約3分、カーネル 約2分、Debianのdebootstrap 約8分、`.dlm` 1分。計15〜20分。ディスクは約6GB(Dockerイメージ約1GB、GPLソース約1GB、ツリー・`.dlm`など)。

### 必要なもの(ビルドするPC)

クリーンな Debian 13 の最小構成で、次の追加だけで最後まで通ることを確認しました(`python3` `curl` `bzip2` `tar` `file` `git` `sudo` は最初から入っていました)。

| パッケージ(apt) | 用途 |
|---|---|
| `docker.io` | カーネルのビルド(古い gcc 4.3.2 のコンテナ) |
| `debootstrap` | Debian wheezy (mipsel) の作成 |
| `qemu-user-static` と `binfmt-support` | mipsel の Debian を PC 上で実行(debootstrap の第2段階) |
| `gcc-mipsel-linux-gnu`(任意) | MIPS 用の小さなツール(`mcui2c` など)のビルド。無ければ Docker の gcc で代用 |

root または `sudo` が必要です(debootstrap・ツリーの作成)。Docker を一般ユーザーで使うなら `docker` グループに入れてください。
Python は標準ライブラリだけで動きます(pip のパッケージは不要)。ネットワークは、GNU(gcc・binutils)、Sony(GPLソース・公式ファーム)、`archive.debian.org`(wheezy)に出られること。
`nasne_setup.py check` は、これらの有無と、qemu の binfmt 登録、Docker の権限を調べます。

## 個別の必要条件と手順(手で進める場合)

- 対象nasneのHDD(USB-SATA変換で接続)。**そのHDD自身の `00550066.dlm` を入力に使う**こと(ヘッダの日付がマネージャ `00110022.dlm` と一致している必要がある。`install_dlm.sh` が検査する)。
- 公式ファーム(Sonyの公開サーバから取得。再配布しないこと): `wget http://ps-peripheral.dl.playstation.net/ps-peripheral/nasne/0260/KRST3101_0260_SECURE.dlm`
- (強く推奨)SPIのバックアップ。CH341A等のSPIライタでの全体ダンプ、または書き込まない[バックアップ用 `.dlm`](16_spi_backup_dlm.md)。復元版 `.dlm`(`--mode restore`)で戻せます。

## 手順

### 1. 自作カーネルを作る(1回だけ。[docs/05](05_kernel_and_direct_boot.md) ①)

```bash
sh scripts/fetch_gpl_sources.sh
sh scripts/kernel_build/fetch_toolchain_sources.sh        # gcc 4.3.2 / binutils 2.19.1 を取得(sha256確認)
docker build -t nasne-kcc:gcc432 scripts/kernel_build
mkdir -p gpl_src/build && tar xjf gpl_src/mips-linux-2.6.29.tar.bz2 -C gpl_src/build
NASNE_CUSTOM=1 sh scripts/kernel_build/build.sh                  # → gpl_src/build/out/vmlinux_new.bin

python3 scripts/ofw_tool.py split KRST3101_0260_SECURE.dlm ofw_out/       # KNL.bin / RFS.bin / DLM.bin に分解
python3 scripts/build_knl.py build ofw_out/KNL.bin knl_new.bin --slot KNL --raw --kernel gpl_src/build/out/vmlinux_new.bin
python3 scripts/build_knl.py verify knl_new.bin
```

`knl_new.bin` は KNL領域(2.5MB = `0x280000`)に収まる必要がある。`--raw` は `ofw_tool.py split` の単体セグメントを雛形にするときに必須。

### 2. Debianのツリーを作る

```bash
sudo scripts/stage1/prepare_tree.sh --out build_stage1/tree --knl knl_new.bin \
     --official KRST3101_0260_SECURE.dlm --ssh-pubkey ~/.ssh/id_rsa.pub
sudo tar --numeric-owner -cpf build_stage1/debian_tree.tar -C build_stage1/tree .
```

`prepare_tree.sh` は debootstrap でDebian wheezy (mipsel) を作り(RSA鍵のsshdまで)、公式ファームから `xcode4drv.ko` / `rc.xcode4` を取り出して置き(Sonyのファイルなので再配布しないこと)、
MIPS用ツール(`mcui2c` `nasne-recpt1` `i2cx` `mtdtool`)をビルドして置き、起動時サービス(ウォッチドッグ停止・HTTPサーバ)を `update-rc.d` し、
直起動の印(`/etc/nasne-direct-boot`)・起動成功の印(`rc.local`)・段階1スクリプトと `knl_new.bin`(sha256つき)を置く。

> SSHは**RSA鍵**が必須(wheezyのsshdはed25519に非対応)。接続側は `-o PubkeyAcceptedAlgorithms=+ssh-rsa -o HostKeyAlgorithms=+ssh-rsa` が必要。

### 3. `00550066.dlm` を作る

HDDのsys1(p1)をマウントして、元の `00550066.dlm` を手元にコピーしておく(これが元に戻す手段になる)。

```bash
sudo mkdir -p /mnt/nasne_sys1 && sudo mount /dev/sdX1 /mnt/nasne_sys1      # sdXは必ずlsblkで確認
cp /mnt/nasne_sys1/00550066.dlm backup/00550066.dlm.orig

# embed(既定の image): Debianも .dlm に同梱する。公式のp3(XFS、録画データ)は残し、中にext3のイメージを作る(サイズは --image-size、既定 8G の疎ファイル)
python3 scripts/stage1/make_stage1_dlm.py build --official backup/00550066.dlm.orig --out custom_00550066.dlm \
        --mode embed --debian-tree-tar build_stage1/debian_tree.tar

# p3 を初期化してext3にし、直接Debianを置きたい場合(p3の録画データは消える):  --p3-mode format --format-p3 を付ける
```

p3 に PC 側でDebianを置いてある場合(p3を ext3 にして `build_stage1/tree` の中身をコピー済み)は `--mode p3`(`--debian-tree-tar` は不要)。
`image` モードにはXFSの空きが2GB以上必要(イメージの実使用量は約0.4GB。8Gは見かけの大きさ)。
**認証なしtelnetは、既定では開きません(`--telnet off`)。** 進行が見えなくて困ったとき・失敗の原因を調べたいときだけ、デバッグ用に `--telnet early`(最初から)か `--telnet fail`(インストールに失敗したときだけ)を付けて `.dlm` を作り直してください(`nasne_setup.py build` にも同じオプションがある)。開いている間は、**LAN内の誰でもrootシェルを使えます**。終わったら外した `.dlm` に戻してください。

`make_stage1_dlm.py` は作った `.dlm` を開き直して、(1) ヘッダのCRC、(2) 元の全エントリが残っていること、(3) `rcS` が差し替わっていること、を確認してから終わる。

### 4. HDDに入れる

```bash
scripts/stage1/install_dlm.sh /mnt/nasne_sys1 custom_00550066.dlm --backup-dir backup
sudo umount /mnt/nasne_sys1
```

`install_dlm.sh` は新旧のヘッダ(hwtype・日付)が一致することを確認し、元のファイルを退避して、`/00550066.dlm` の**1ファイルだけ**を置き換える
(マネージャ `00110022.dlm` やバンクdirは変更しない)。sys1(256MB)に新旧2つ分の空きが無いときは、元のファイルを退避済みであることを確かめてから消して、直接書きます(`spi_backup` など他のファイルが増えていると、空きが足りなくなります)。**embed の `.dlm` は約75MB**で、sys1(256MB)に収まる。

### 5. nasneに挿して電源を入れる

放っておく(約5分)。確認したければ:

| 時間の目安 | 見えるもの |
|---|---|
| 〜1分 | 公式カーネルが起動し、DHCPのアドレスでpingが通る(`--telnet early` のときだけ、telnet(23番、認証なし)も開く) |
| 〜3分 | p3の初期化・コピー(`--telnet early` なら、`telnet <nasne>` でログインして `cat /tmp/nasne_stage1_rcs.log` で進行を見られる) |
| 〜4分 | SPIのバックアップ(13MBで約40秒)→ KNL書き込み(約30秒)→ 自動で再起動 |
| 〜5分 | 自作カーネルでDebianが起動。sshが開く(telnetは閉じる) |

```bash
ssh -o PubkeyAcceptedAlgorithms=+ssh-rsa -o HostKeyAlgorithms=+ssh-rsa root@<nasneのIP>
uname -a                              # #N ... 自作カーネル
cat /var/log/nasne-stage1.log         # "flash + verify OK" "rebooting into the new kernel"
cat /var/log/nasne-mcu-wd.log         # "MCU watchdog disabled"。5.5分以上落ちずに動いていればウォッチドッグ停止成功
```

視聴は [docs/11](11_tv_streaming.md): `curl http://<nasne>:8301/tuner/27 | ffplay -`(ch27は例。`/scan` で調べる)。

## 安全装置と、失敗したとき

`nasne-stage1.sh` がSPIに触る前後でやること(PC上の模擬環境で全場面をテスト済み):

| 場面 | 動作 |
|---|---|
| カーネルファイルが無い/壊れている/サイズがKNL領域を超える | sha256とサイズを検査して中止。SPIには触らない(終了コード1) |
| すでに同じカーネルが書かれている | 何もしない(冪等) |
| MCUウォッチドッグを止められない | 約333秒でリセットされSPIの消去中に落ちる恐れがあるので、SPIに触らず中止(終了コード5) |
| 試行回数が2回に達している | SPIに触らない(終了コード2)。`/var/lib/nasne-stage1/tries` を消せば再試行できる |
| 現在のKNLスロットが空(0xFF) | すでに壊れているので自動では書かない(終了コード1) |
| 書く前 | SPI(`/dev/mtd0`、13MB)全域を `/var/lib/nasne-stage1/spi_mtd0_backup.bin`(p3の中)に保存。元のKNLスロットを `knl_A_before.bin` に切り出す |
| 書き込み(消去→書き込み→読み戻し比較)に失敗 | 元のKNLを自動で書き戻す(終了コード3)。書き戻しにも失敗したら終了コード4(要手動復旧) |

### 動かないとき

| 症状 | 対処 |
|---|---|
| pingが出ない(10分待っても) | 公式のminirootが `.dlm` を受け付けていない可能性(展開失敗は点滅ループになり、HDD側から原因を切り分けられない: [docs/03](03_firmware_format.md))。HDDをPCに戻して元の `00550066.dlm` に戻すと公式ファームで起動する |
| pingは通るが sshが出ない/ネットワークがおかしい | `--telnet early` で `.dlm` を作り直し、`cat /tmp/nasne_stage1_rcs.log`(外側のrcSのログ)と、`/mnt/p3/var/log/nasne-stage1.log`(chroot内の段階1のログ)を見る |
| `ABORT` が出て止まる | ログの理由に従う。`/var/lib/nasne-stage1/` に状態とバックアップがある |
| 再起動後に pingが通るがsshが出ない | sshdが上がっていない。公式ファームのまま(`uname` の `#1 ... 2013`)なら、自作カーネルが起動していない(SPIが書き換わっていない、またはBKNLに落ちた) |
| SPIが壊れて起動しない | 実機で「古い設定の混入でKNLが中途半端に消えた」事故の報告がある。4段目ブートのA/Bフォールバックで助かる**とは限らない**(KNL の1バイトが壊れて CRC が不一致の場合は、BKNL で起動することを実機で確認した。ほぼ消去された場合は助からなかった事例がある)。CH341Aで事前のダンプを書き戻す |
| 約333秒ごとに再起動する | ウォッチドッグが止まっていない。`/var/log/nasne-mcu-wd.log` を確認([docs/06](06_mcu_watchdog.md)) |

### 公式アプリを1回だけ起動する / 救出用のtelnet(`nasne-boot-switch`)

Debianが動いているnasneで:

```bash
nasne-boot-switch official-once   # 次の再起動で公式アプリ(procmng)を1回だけ起動。その次の起動からはDebianに戻る(印は消費される)
nasne-boot-switch rescue-once     # 次の再起動を、Sonyのminiroot環境 + ネットワーク + 認証なしtelnetだけにする(調査・救出用)
nasne-boot-switch cancel / status
```

仕組み: 印(`/etc/nasne-boot-official-once` など)があると自作カーネルの `/nasne_init` が Sony の `/init` へ戻り、`.dlm` の段階1(rcS)が印を消して処理を分ける。
SPIのカーネルは書き換えない。戻るには電源を入れ直す(official-once は公式アプリが動いている間は操作手段が無い)。

**official-once は、本体のNV(SPIのjffs2)と、p3(HDD)の状態が揃っていれば動きます**(実機で確認: 公式で録画したHDDで、Debianから `official-once` → 公式アプリが起動し、録画一覧も見える)。
以前、「有効なXFSのp3があると公式アプリが固まる」と書いていましたが、**原因は本体のNVとHDDの食い違いでした**(原本のNVを入れたまま、別に初期化したHDDを使うと固まる。[docs/04](04_hdd_recovery_guide.md))。
固まったときは、`nasne-nvreset reset --yes`(Debian上でNVを保存して空にする)をしてから、もう一度試してください。NVを空にしても、公式アプリは固まらず、NVを作り直します。
固まっても、電源を入れ直せば(印は消費済みなので)Debianに戻ります。`rescue-once`(ネットワーク+telnetだけ)は問題なく使えます。

### 公式ファームに戻す(手順を通しては未検証。個々の操作は実機で確認済み。sys1 に公式の `.dlm` がある状態での公式アプリの起動は、`official-once` や A/B フォールバックの実験で確認)

1. HDDをPCに繋ぎ、退避してある元の `00550066.dlm` をsys1に戻す。
2. p3 の `/etc/nasne-direct-boot` を削除する(自作カーネルの `/nasne_init` は、この印が無いと公式のminirootへフォールバックする)。
   p3のDebianごと捨てて公式のp3(XFS)に戻す場合は、公式ファームのセットアップでp3が初期化される(**録画データは embed の時点で消えている**)。
3. SPIのKNLも公式に戻す場合は、事前に取ったバックアップ([docs/16](16_spi_backup_dlm.md))から戻す復元版 `.dlm`(`--mode restore`、実機で確認済み)が簡単です。手で書くなら、`scripts/tools/mtdtool` で `ofw_out/KNL.bin` を書く(Debianが動いている間に):
   `mtdtool flash /dev/mtd0 0x100000 KNL.bin`。事前にCH341Aで取ったダンプがあれば、そちらを書き戻すのが確実。

## 使ったツール(`scripts/stage1/`)

| ファイル | 役割 |
|---|---|
| `make_stage1_dlm.py` | PC側(root不要)。`.dlm` の作成(`build`)、ドライバの取り出し(`extract-drivers`)、中身の確認(`list`) |
| `prepare_tree.sh` | PC側(sudo)。Debianツリーを作る |
| `install_dlm.sh` | `.dlm` をHDDのsys1に入れる(検査・退避つき) |
| `nasne-stage1.sh` | nasne上(Debianのchroot内)。SPIの書き換え本体 |
| `rcS_block_p3.sh` / `rcS_install_embed.sh` | `rcS` に差し込む処理のひな形(p3のマウント、official-once / rescue-once、インストール、段階1) |
| `nasne-boot-switch` | nasne上。`official-once` / `rescue-once` の予約 |
| `test_stage1_sim.sh` | `nasne-stage1.sh` の論理テスト(実機不要) |
| `test_rcS_sim.sh` | 生成した `.dlm` を公式minirootのbusybox(qemu-user)で展開し、`rcS` の呼び出し順をモックで確認(要sudo・qemu-user-static) |
| `../tools/mtdtool.c` | `/dev/mtd0` の消去/書き込み/検証/ダンプ(許可範囲は固定)。libc不使用の静的バイナリ([docs/09](09_tools.md)) |

## 補足: 事故の記録

- 公式のtarを展開したディレクトリを使い回して古い設定が残り、SPIのKNLが中途半端に消えて起動不能になった事例が報告されている。
  この手順は展開しない(メンバー単位のコピー)ので起こらない。
- `mtdtool` の拒否動作を確かめるつもりで、許可範囲内の `0x100000`(KNL)を指定してしまい、動作中のカーネルの先頭64KBを消した。
  他は無事だったので、既知のセグメントの先頭64KBだけを書き戻して読み戻し検証で復旧できた。**拒否のテストは範囲外が確実な場所(例 `0x10000`)でやる**。
