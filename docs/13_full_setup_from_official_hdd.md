# 13. 公式ファームで動いているHDDから、`00550066.dlm` 1ファイルの差し替えだけでDebian直起動にする

> 対象: **公式ファーム(v2.60)で正常に動いているnasne(CECH-ZNR2J)のHDD**。HDDの紐付けは正常な状態からスタートする。
> ([docs/04](04_hdd_recovery_guide.md) のHDD全損復旧とは別の話。)
>
> やること: HDDのsys1にある `00550066.dlm` を自作版に**1ファイル差し替えるだけ**。nasneに挿して電源を入れると、nasne自身が
> (1) HDDのp3にDebianを作り、(2) SPIフラッシュのカーネル(KNL)を自作カーネルに書き換え、(3) 再起動して**Debian(PID 1)**になる。
> ウォッチドッグの停止、地デジの受信と配信([docs/11](11_tv_streaming.md))まで、そのまま使える。
>
> ⚠️ 自己責任です。**p3(録画データ領域)は初期化されて消えます**。SPIフラッシュへの書き込みを伴い、失敗すると起動しなくなる可能性があります
> (自動のバックアップと書き戻しを入れていますが、**CH341A等のSPIライタで事前に全体をダンプしておくことを強く勧めます**)。

## 検証状況

| 内容 | 状態 |
|---|---|
| `embed` モード(`.dlm` 1ファイルの差し替えだけ。p3の初期化→Debian展開→SPI書き換え→Debian PID1) | ✅ 実機1台(公式v2.60のHDD)で通し確認。約5分で完了 |
| `p3` モード(p3にPC側でDebianを置いてある場合に、SPI書き換えだけをnasne自身で行う) | ✅ 実機1台で通し確認 |
| 書き換え後のウォッチドッグ停止・地デジの復号済みTS取得(MPEG-2 1440×1080 + AAC) | ✅ 確認 |
| `nasne-stage1.sh` の失敗系(書き込み失敗→自動で元に戻す、試行上限、ハッシュ不一致、ウォッチドッグ停止失敗など)の論理テスト | ✅ PC上の模擬環境で23項目(`scripts/stage1/test_stage1_sim.sh`)。実機では未発生 |
| 書き込み失敗時の自動書き戻しの実機での動作 | ❌ 実機では失敗が起きなかったので未確認 |
| 他のファーム版数、他の個体、1TBなど別容量のHDD | ❌ 未確認 |

## 仕組み

```
[PCで1回だけ]  00550066.dlm を自作版に差し替える(+必要ならDebianのツリーを同梱する)

[1回目の起動: 公式カーネル]
  公式のminiroot → 自作 .dlm を展開(sys2) → rcS
    └ (公式アプリ procmng / startdtvtuner は起動しない)
       ├ DHCPでネットワークを上げる(+ 認証なしtelnet。作業中の確認用)
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

## 必要なもの

- Linux PC(Python 3.8+、Docker、`debootstrap`、`qemu-user-static`、`sudo`)
- このリポジトリ、**公式ファーム**(Sonyの公開サーバから取得。再配布しないこと):
  ```bash
  wget http://ps-peripheral.dl.playstation.net/ps-peripheral/nasne/0260/KRST3101_0260_SECURE.dlm
  ```
- 対象nasneのHDD(USB-SATA変換で接続)。**そのHDD自身の `00550066.dlm` を入力に使う**こと(ヘッダの日付がマネージャ `00110022.dlm` と一致している必要があるため)
- (強く推奨)CH341A等のSPIライタとSOICクリップ。事前に元のSPI全体(16MB)をダンプしておく

## 手順

### 1. 自作カーネルを作る(1回だけ。[docs/05](05_kernel_and_direct_boot.md) ①)

```bash
sh scripts/fetch_gpl_sources.sh
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

# embed: Debianも .dlm に同梱する(p3は初期化される)
python3 scripts/stage1/make_stage1_dlm.py build --official backup/00550066.dlm.orig --out custom_00550066.dlm \
        --mode embed --debian-tree-tar build_stage1/debian_tree.tar --format-p3
```

p3 に PC 側でDebianを置いてある場合(p3を ext3 にして `build_stage1/tree` の中身をコピー済み)は `--mode p3`(`--debian-tree-tar` / `--format-p3` は不要)。
`--telnet early|fail|off` で、作業中の認証なしtelnetの開き方を選べる(既定 `early` = 最初から。確認しやすいが、作業中の数分間はLAN内の誰でもrootシェルを使える)。

`make_stage1_dlm.py` は作った `.dlm` を開き直して、(1) ヘッダのCRC、(2) 元の全エントリが残っていること、(3) `rcS` が差し替わっていること、を確認してから終わる。

### 4. HDDに入れる

```bash
scripts/stage1/install_dlm.sh /mnt/nasne_sys1 custom_00550066.dlm --backup-dir backup
sudo umount /mnt/nasne_sys1
```

`install_dlm.sh` は新旧のヘッダ(hwtype・日付)が一致することを確認し、元のファイルを退避して、`/00550066.dlm` の**1ファイルだけ**を置き換える
(マネージャ `00110022.dlm` やバンクdirは変更しない)。**embed の `.dlm` は約75MB**で、sys1(256MB)に収まる。

### 5. nasneに挿して電源を入れる

放っておく(約5分)。確認したければ:

| 時間の目安 | 見えるもの |
|---|---|
| 〜1分 | 公式カーネルが起動し、DHCPのアドレスでpingが通る。telnet(23番)が開く(認証なし) |
| 〜3分 | `telnet <nasne>` でログインして `cat /tmp/nasne_stage1_rcs.log` で進行を見られる(p3の初期化、コピー) |
| 〜4分 | SPIのバックアップ(13MBで約40秒)→ KNL書き込み(約30秒)→ 自動で再起動 |
| 〜5分 | 自作カーネルでDebianが起動。telnetは閉じ、sshが開く |

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
| pingもtelnetも出ない(10分待っても) | 公式のminirootが `.dlm` を受け付けていない可能性(展開失敗は点滅ループになり、HDD側から原因を切り分けられない: [docs/03](03_firmware_format.md))。HDDをPCに戻して元の `00550066.dlm` に戻すと公式ファームで起動する |
| telnetは出るが sshが出ない/ネットワークがおかしい | `cat /tmp/nasne_stage1_rcs.log`(外側のrcSのログ)と、`/mnt/p3/var/log/nasne-stage1.log`(chroot内の段階1のログ)を見る |
| `ABORT` が出て止まる | ログの理由に従う。`/var/lib/nasne-stage1/` に状態とバックアップがある |
| 再起動後に pingが通るがsshが出ない | sshdが上がっていない。公式ファームのまま(`uname` の `#1 ... 2013`)なら、自作カーネルが起動していない(SPIが書き換わっていない、またはBKNLに落ちた) |
| SPIが壊れて起動しない | 実機で「古い設定の混入でKNLが中途半端に消えた」事故の報告がある。4段目ブートのA/Bフォールバックで助かる**とは限らない**(B側のBRFSは別版のminirootで、HDDの内容次第で起動しない)。CH341Aで事前のダンプを書き戻す |
| 約333秒ごとに再起動する | ウォッチドッグが止まっていない。`/var/log/nasne-mcu-wd.log` を確認([docs/06](06_mcu_watchdog.md)) |

### 公式ファームに戻す(手順は未検証。個々の操作は他の場面で確認済み)

1. HDDをPCに繋ぎ、退避してある元の `00550066.dlm` をsys1に戻す。
2. p3 の `/etc/nasne-direct-boot` を削除する(自作カーネルの `/nasne_init` は、この印が無いと公式のminirootへフォールバックする)。
   p3のDebianごと捨てて公式のp3(XFS)に戻す場合は、公式ファームのセットアップでp3が初期化される(**録画データは embed の時点で消えている**)。
3. SPIのKNLも公式に戻す場合は、`scripts/tools/mtdtool` で `ofw_out/KNL.bin` を書く(Debianが動いている間に):
   `mtdtool flash /dev/mtd0 0x100000 KNL.bin`。事前にCH341Aで取ったダンプがあれば、そちらを書き戻すのが確実。

## 使ったツール(`scripts/stage1/`)

| ファイル | 役割 |
|---|---|
| `make_stage1_dlm.py` | PC側(root不要)。`.dlm` の作成(`build`)、ドライバの取り出し(`extract-drivers`)、中身の確認(`list`) |
| `prepare_tree.sh` | PC側(sudo)。Debianツリーを作る |
| `install_dlm.sh` | `.dlm` をHDDのsys1に入れる(検査・退避つき) |
| `nasne-stage1.sh` | nasne上(Debianのchroot内)。SPIの書き換え本体 |
| `rcS_block_p3.sh` / `rcS_install_embed.sh` | `rcS` に差し込む処理のひな形 |
| `test_stage1_sim.sh` | `nasne-stage1.sh` の論理テスト(実機不要) |
| `test_rcS_sim.sh` | 生成した `.dlm` を公式minirootのbusybox(qemu-user)で展開し、`rcS` の呼び出し順をモックで確認(要sudo・qemu-user-static) |
| `../tools/mtdtool.c` | `/dev/mtd0` の消去/書き込み/検証/ダンプ(許可範囲は固定)。libc不使用の静的バイナリ([docs/09](09_tools.md)) |

## 補足: 事故の記録

- 公式のtarを展開したディレクトリを使い回して古い設定が残り、SPIのKNLが中途半端に消えて起動不能になった事例が報告されている。
  この手順は展開しない(メンバー単位のコピー)ので起こらない。
- `mtdtool` の拒否動作を確かめるつもりで、許可範囲内の `0x100000`(KNL)を指定してしまい、動作中のカーネルの先頭64KBを消した。
  他は無事だったので、既知のセグメントの先頭64KBだけを書き戻して読み戻し検証で復旧できた。**拒否のテストは範囲外が確実な場所(例 `0x10000`)でやる**。
