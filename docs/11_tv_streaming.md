# 11. 地デジを受信して配信する(`nasne-recpt1`)

Debian直起動のnasneで、**地デジをB-CASカードで復号し、元の画質のまま(MPEG-2 1440×1080 など)TSとして取り出す**ためのツールと使い方。
録画ソフト・プレーヤーから使える2つの形で提供している。

| 形 | 用途 |
|---|---|
| `nasne-recpt1 <ch> <秒数> <出力>` | `recpt1` 互換のコマンド。標準出力にTSを流せる。nasne上で実行する |
| `nasne-recpt1 --listen 8301`(HTTPサーバ、起動時サービス) | `http://<nasne>:8301/tuner/<ch>` でTSをHTTP配信。VLC、tvheadend、Kodi、ffmpeg、Jellyfin などのIPTV入力に使える |

⚠️ B-CASカードを使います。自分の機器・自分のカードで、私的な視聴・録画の範囲で使ってください。
(復号はnasne純正と同じく、nasne内のファームウェアがB-CASカードと直接やり取りして行います。カードのIDや鍵は取り出していません。)

## できること / 制限

| | |
|---|---|
| ✅ | 地デジ(ISDB-T)UHF 13〜62 の選局、B-CAS復号済みの**元画質TS**(MPEG-2映像、AAC音声、字幕・データ放送のPIDも含む)の取得。ビットレートは約15Mbps |
| ✅ | PAT(元のもの)・NIT / SDT / EIT / TOT などの**SI(番組表の元データ)**も混ぜて出力 |
| ✅ | HTTP配信、`/scan` によるサービス一覧、連続2分(約230MB)の受信で同期エラー・スクランブル残り無し(連続カウンタの乱れは番組表・データ放送のPIDで数回)。**Mirakurunから実際に使えることを確認**(スキャン・番組表・ストリーム) |
| ✅ | 終了時(秒数経過・シグナル・接続切断)にストリームを必ず閉じる。2つ目の要求にはチューナー使用中(exit 3 / HTTP 503)を返す |
| ⚠️ | **同時に1チャンネル・1サービスのみ**。1回の接続で出力されるのは選んだ1サービス(+全サービス分のSI)で、チャンネル全体(全サービス)のTSではない |
| ⚠️ | 既定では、チューナーは1系統として扱っている(地デジも衛星も同じロック)。**地デジと BS/CS の同時受信は、`--dual-tuner`(試験的。[下記](#地デジと-bscs-の同時受信試験的--dual-tuner))で有効にできる**が、電波のある環境での確認は未実施。**BS/CS110 は、他の人の実機で受信できたとの報告をもとに取り込んだもので、作者の環境(BS/CS の信号が来ない)では未検証**([docs/08](08_tuner_i2c.md)) |
| ⚠️ | **ワンセグは扱わない**。ワンセグ(PMTのPIDが0x1FC8、SDTのservice_typeが0xC0)に対して復号ストリームを開くと、ファームが応答しなくなり、受信側の処理がドライバ内で戻らなくなる(`SIGKILL` も効かず、`reboot -f` が必要)。一覧(`/scan`・M3U)には `(1seg)` として載せず、明示的に指定されても404を返す |
| ⚠️ | 開始まで約2〜4秒かかる(選局、ロック待ち、PAT/PMT取得、ストリーム確立) |
| ⚠️ | サービス名・番組表の文字(ARIB文字コード)はそのまま出力しているだけ。デコードは録画ソフト側 |

## 使い方

### 準備

```bash
# MIPS用にビルド(docs/05のDocker環境 nasne-kcc:gcc432)
mipsel-linux-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -e __start -o nasne-recpt1 scripts/tools/nasne_recpt1.c
scp nasne-recpt1 root@<nasne>:/usr/local/sbin/
scp scripts/watchdog/nasne-recpt1-server root@<nasne>:/etc/init.d/
ssh root@<nasne> 'chmod +x /etc/init.d/nasne-recpt1-server; update-rc.d nasne-recpt1-server defaults 20; /etc/init.d/nasne-recpt1-server start'
```

前提: 公式ドライバ(`xcode4drv.ko`、`rc.xcode4`)が `/usr/local/sbin/` にあり、起動時にロードされている([docs/06](06_mcu_watchdog.md)のサービスが行う)こと。
B-CASカードがnasneに挿さっていること。ファームが止まっていれば、ツールが自動で `rc.xcode4` を実行してドライバを再読込する。

### コマンドライン(recpt1互換)

```bash
nasne-recpt1 [--sid <サービスID>] [-v] <UHFチャンネル 13〜62 | --freq MHz> <秒数 | -> <出力ファイル | ->
nasne-recpt1 27 60 /tmp/a.ts            # UHF 27 を60秒、ファイルへ
nasne-recpt1 27 - - | ffplay -           # 無期限、標準出力へ(止めるときはSIGTERM/Ctrl-C/パイプを閉じる)
nasne-recpt1 --sid 0x0401 27 10 -        # サービスを指定
nasne-recpt1 --scan                      # UHF 13〜62 をスキャンして、ロックしたチャンネルのサービス一覧を表示(--scan-sat: BS/CS のみ、--scan-all: 全部。PR #5)
```

`--b25` `--strip` `--device` `--lnb` など recpt1 のオプションは受け付けて無視する。

#### BS/CS110(報告をもとに取り込み。作者の環境では未検証)

チャンネルに `BS1`〜`BS23`(奇数)、`BS15_0` のような `<トランスポンダ>_<スロット>`、`CS2`〜`CS24`(偶数)が指定できる。
スロットや CS は、TSID(復調ICのレジスタ `0x8F`/`0x90` に書く。[docs/08](08_tuner_i2c.md))が決まるとスロット多重の中から目的の局が選ばれる。TSID は `--tsid 0xNNNN`(HTTP は `?tsid=0xNNNN`)でも直接指定できる。

```bash
nasne-recpt1 BS15_0 60 /tmp/a.ts           # BS15 のスロット0を60秒
nasne-recpt1 --sid 0x0067 BS1_0 10 -       # サービスも指定
curl 'http://<nasne>:8301/tuner/CS6?sid=0x....' | ffplay -
```

- BS の TSID 表は、放送事業者の追加や帯域再編で変わる(`nasne_recpt1.c` の `get_bs_tsid()`)。
- 1つのトランスポンダに多くのサービスが載るため、PAT/PMT の収集上限を 31 サービスにしてある。
- **`--scan` と `/scan` は、既定では地デジ(UHF 13〜62)だけ**。BS/CS は `--scan-sat`・`/scan?sat`(衛星のみ)、`--scan-all`・`/scan?all`(全部。BS は全トランスポンダ・全スロット、CS は ND2〜ND24。数分かかる)でスキャンする(PR #5。**作者の環境では信号が無く未検証**。投稿者の環境で、地デジ33・BS45・CS54 の計132サービスを収集して動作確認)。
- 衛星のときだけ、PSI ストリームから PMT を出力 TS へ混ぜる(地デジは復号済みのストリームに PMT が含まれていて、混ぜると二重になる)。

#### Mirakurun で BS/CS を使う

nasne は **1回に1サービスしか出力しない**ので、Mirakurun のチャンネルは「サービスごと」に1つ要り、サービスの指定(`?sid=0x…`)をコマンドに渡す必要がある。
**チャンネル名に `?sid=` を含める方式は、Mirakurun の API や TVTest で 404・不正な形式になる**ので、`commandVars` に分ける(上の `channels.yml` の説明)。`nasne_mirakurun_conf.py` がこの形式を自動で作る。

| 種類 | `nasne-recpt1` が受け付けるチャンネル | 例(Mirakurun のチャンネル名) |
|---|---|---|
| 地デジ | `13`〜`62`(あとに `_<任意>` が付いてもよい) | `27_56360` |
| BS | `BS<奇数1〜23>` または `BS<奇数>_<スロット>`(先頭の 0 は付けても付けなくてもよい。あとに `_<任意>` が付いてもよい)。**偶数は不可(400)** | `BS15_0_101` |
| CS110 | `CS<偶数2〜24>`(`CS02` も可。スロット・接尾辞は無視) | `CS2_257` |

**いちばん簡単な手順**(nasne 自身がスキャンする。PR #5 で追加。BS/CS の信号が来ている環境):

```bash
# nasne に BS/CS を含めてスキャンさせ(数分。チューナーを使うので録画中は避ける)、Mirakurun の設定を作る
python3 scripts/nasne_mirakurun_conf.py <nasne> --sat --scan -o mirakurun-conf
# → tuners.yml(types に GR・BS・CS)と channels.yml(サービスごと)ができる。Mirakurun を再起動
```

`/scan`(地デジのみ)、`/scan?sat`(衛星のみ)、`/scan?all`(全部)で、結果は `/playlist.m3u8` に出る(`group-title` は `UHF`・`BS`・`CS`)。**スキャンのたびに一覧(キャッシュ)は、スキャンした種類だけで置き換わる**(地デジだけをスキャンすると、BS/CS の一覧は消える)。
`tuners.yml` のコマンドは、既定で `node -e "require('http').get(…)"`(Mirakurun の公式イメージには **curl が入っていない**。node は入っている。実測)。curl を足したイメージを使うなら `--command curl`。

**Mirakurun 自身のチャンネルスキャンを使う場合**(代替):
- BS のスキャンの**既定のチャンネル名は `101`〜`256`** で、`nasne-recpt1` は受け付けない(400)。**`useSubCh=1`(`PUT /api/config/channels/scan?type=BS&useSubCh=1`)で `BS01_0`〜`BS23_3` の形式**になる。CS は既定で `CS2`〜`CS24`。Mirakurun は `useSubCh=false` のような文字列も真と読むので、値は `1` にするか、付けない。
- スキャンが書く項目は、`commandVars` が無く、チャンネル名が一意でなく、**衛星の項目は既定で `isDisabled: true`**。`python3 scripts/nasne_mirakurun_conf.py --fix-sat mirakurun-conf/channels.yml --enable` で、一意な名前(`BS01_0_101`・`CS2_257`)と `commandVars` に直し、有効にする(元は `.bak`。何度実行しても同じ結果)。同じディレクトリの `tuners.yml` のコマンドにも `<sid_query>` を足す。
- `tuners.yml` の `types` に BS・CS が入っていること(`--sat`)。

`nasne_mirakurun_conf.py` は **`channels.yml` を書き直す**。局が変わって作り直したら、Mirakurun のスキャンを使っていた場合は、そのあとで `--fix-sat` をやり直すこと。

終了コード: 0 正常 / 1 エラー(ロックしない・サービスが無い・ストリームが開けない) / 2 使い方 / 3 チューナー使用中。

### HTTP

```
GET /tuner/<UHF ch>[?sid=0xNNNN][&sec=N]   復号済みTS(video/MP2T)。切断するとストリームを閉じる。sec を付けるとN秒で終了
GET /<UHF ch>                              同上(短縮形)
GET /scan[?sat|?all]                        サービス一覧(テキスト。地デジのみ約80秒。?sat=衛星のみ、?all=全部で数分)。サービス名も出し、結果を保存する
GET /playlist.m3u8                         /scan の保存結果から作ったM3U(VLCなどでそのまま開ける)
GET /status                                idle または busy
```

使用中は `503 Service Unavailable`、信号なしは `404`。

```bash
curl http://<nasne>:8301/tuner/27 | ffplay -
curl -s http://<nasne>:8301/scan
# ch=27 freq=557142kHz sid=0x0400(tv) name="局名1" sid=0x0401(tv) name="局名2" sid=0x0408(1seg) name="局名携帯"
```

サービス名は SDT(ARIBの8単位符号)をUTF-8に直して出す。`(tv)` が地デジのテレビ、`(1seg)` がワンセグ、`(data)` がデータ放送。
`/scan` は結果を nasne 上の `/var/lib/nasne-recpt1/services.tsv` に保存する(最後まで走ったときだけ置き換える。**スキャンした種類だけで置き換わる**)。

**チューナーの連続選局**: `nasne-recpt1` は、前のセッションの終了から 300ms 以内に次のセッションを開くときだけ、差分を待つ(`/tmp/nasne-recpt1.lock` に終了時刻を書く。PR #5)。複数クライアントの頻繁な開閉で SoC(xcodedrv)が D 状態で固まる件の緩和策の、暫定値(`300ms` は仮。効果は作者の試験では評価できていない)。通常の選局は待たない。

### 録画ソフトとの連携

#### Mirakurun(動作確認済み)

Mirakurun 4.1(Node.js 22以上推奨)で、nasne のHTTP配信を「コマンド型チューナー」として使い、**チャンネルスキャン・サービス名の取得・ストリーム取得・番組表(EIT)の取得**まで動いた。
Mirakurun は別のPCで動かす(nasne上ではなく)。**EPGStation も動作確認済み**(下の「EPGStation」)。

`tuners.yml`:

```yaml
- name: nasne
  types:
    - GR
  command: node -e "require('http').get('http://<nasneのIP>:8301/tuner/<channel><sid_query>', res => res.pipe(process.stdout))"
  isDisabled: false
```

`channels.yml`(**サービスごとに1エントリ**。nasne は1回に1サービスしか出力しないため。`nasne_mirakurun_conf.py` が、これを自動で作る):

```yaml
- name: 局名
  type: GR
  channel: '27_1024'           # 一意な名前(物理チャンネル _ サービスID(10進))。`?` は使わない
  serviceId: 1024              # 0x0400 の10進数
  commandVars:                 # コマンドの <sid_query> に入る(Mirakurun の commandVars)
    sid_query: "?sid=0x0400"
    sid: "0x0400"
  isDisabled: false
- name: 局名2
  type: GR
  channel: '27_1025'
  serviceId: 1025
  commandVars:
    sid_query: "?sid=0x0401"
    sid: "0x0401"
  isDisabled: false
```

- コマンドは `.../tuner/<channel><sid_query>`。`nasne-recpt1` は、`27_1024` の `_` 以降(接尾辞)を無視して、物理チャンネル 27 を選局し、`?sid=` でサービスを選ぶ。
- **チャンネル名を `27?sid=0x0400` のように `?` を含めると、Mirakurun の API(`/api/channels/GR/27?sid=…`)が 404 になり、TVTest(BonDriver_Mirakurun)などでも不正な形式になる**(実測。PR #5 で報告)。そのため、サービス情報は `commandVars` に分ける。
- **名前をサービスごとに一意にする**理由: 同じ物理チャンネルのサービスが同じ名前だと、Mirakurun は「同じチャンネル」とみなして、2つ目のサービスの要求を、1つ目のストリームに相乗りさせる。nasne は1サービスしか出さないので、2つ目は**データが来ないまま切れる**(実測: 約15秒後に空で切断)。名前が違えば、チューナーが空くのを待って選局し直す(実測: 1つ目が終わったあと、続けて取得できた)。

サービスIDの調べ方: `curl http://<nasne>:8301/scan`(`sid=0x...(tv)` の一覧)。

確認した結果: Mirakurun が起動時にサービススキャンを行い、サービス名(ARIB文字を含む)・ネットワークID・サービスIDを取得した。
番組表は約1分の受信で数百件の番組が取れた。`/api/services/<id>/stream` からは、元画質(MPEG-2 1440×1080 + AAC + 字幕)のTSが取得できた。
Mirakurunのチャンネルスキャン(自動)は、チャンネル全体のTSを前提にするため、**サービスごとのチャンネル設定を手書きするのが確実**。

**出力する PAT・PMT と音声(PR #9 で整理、2026-10-22)**: `--sid`(HTTP では `?sid=`)を指定したときの TS は、SoC が作る**単一サービスの PAT**(NIT とそのサービスだけ)と、**CA 記述子を除去した PMT** だけを出します(衛星で元の PMT を混ぜていた処理は廃止。PMT の二重混入による CC エラーと、字幕が暗号化と誤認されて出ない症状が直った)。また、PMT に**音声 ES が複数**(二ヶ国語・解説音声など。最大4本)ある番組は、すべての音声を復号して出します(以前は主音声の1本だけで、副音声は PMT に載っているのにパケットが出なかった=KonomiTV で第2音声が `0 channels` になる原因)。

**Docker で Mirakurun を動かすときは、`/app-config` と `/app-data` の両方を永続化してください**(`contrib/epgstation/docker-compose.yml` の `volumes` のとおり)。`/app-data` を永続化しないと、Mirakurun を再起動するたびに、全チャンネルのサービス検出(`Service.Add.Check.*` のジョブ)と番組表の取得をやり直します。nasne のチューナーは1系統なので、そのジョブが連続してチューナーを占有し、視聴や録画が `503 tuner busy` / `/status` が `busy` のまま、という状態が長く続きます(BS/CS を足してチャンネル数が増えるほど長くなる。Issue #6)。初回のスキャンは、BS/CS を含めて30分〜1時間かかることがあります。`nasne-recpt1` のストリーム子プロセスが `ps` で `D` 状態に見えるのは、受信中の通常の状態です。

Mirakurunのデコーダー(`decoder`)設定は不要(nasne が復号済みのTSを出す)。複数サービスの同時視聴は、チューナーが1系統なので不可(同じチャンネルの別サービスも不可)。

`tuners.yml` / `channels.yml` は `python3 scripts/nasne_mirakurun_conf.py <nasneのIP> --scan -o <設定ディレクトリ>` で nasne のサービス一覧から自動で作れます(手で書く必要はありません)。
**公式の Mirakurun の Docker イメージには curl が入っていません**。`curl` を足したイメージが要ります([contrib/epgstation](../contrib/epgstation/README.md))。

#### EPGStation(動作確認済み)

Mirakurun 4.1 + EPGStation 3.0(beta)を Docker で動かし、nasne をチューナーにして次を確認しました([contrib/epgstation](../contrib/epgstation/README.md) に compose と手順)。

- サービスの検出(20サービス、約3分)→ 全局の番組表の取得(約10分、数千件)→ EPGStation のチャンネル一覧・番組表に反映。
- 放送中の番組の予約 → 録画開始。約15Mbpsの元画質(MPEG-2 1440×1080 + AAC + 字幕)で約160秒を録画し、4797フレーム(29.97fps×160秒と一致)、先頭の数行以外にデコードエラーなし。
- これから始まる番組の予約 → 予約時刻の約15秒前にチューナーを開き、時刻どおりにファイルが作られて録画された。
- 録画中は番組表の取得(EPG収集)が止まり、録画が優先される。チューナーは1系統なので、裏番組の同時録画はできない。

確認済み(2026-10): 約4日の連続運用(EPGStation の予約録画12本)、tvheadend。未確認: 録画後のエンコード。

#### VLC(プレイリスト再生)

先に `curl http://<nasne>:8301/scan` を1回実行して(約80秒)、サービス一覧を保存する。あとは

```bash
vlc http://<nasne>:8301/playlist.m3u8
```

で、局名つきのプレイリストが開く(**VLCで再生できることを確認済み**)。M3Uは `#EXTINF:-1 group-title="UHF27",局名` の形で、各エントリの URL は
`http://<アクセスしたアドレス>:8301/tuner/27?sid=0x....`(リクエストの `Host:` ヘッダのアドレスを使う)。
チューナーは1系統なので、**同時に再生できるのは1チャンネルだけ**。プレイリストを次の項目へ送ると、直前の接続が閉じ終わる前に次を開いて `503` になることがあるが、少し待てば再生できる。

#### ffmpeg / Kodi / Jellyfin など

`http://<nasne>:8301/tuner/<ch>?sid=0x....` を再生/IPTVチャンネルとして登録する。mpv・ffmpeg で、URL を直接再生できることを確認した。
- **Jellyfin**: M3U を「ライブTV」のチューナーに追加すると、**20チャンネルを取り込める**(確認済み)。ただしライブ視聴は**確認できていない**(再生前の `ffprobe` が終わらない、プローブと本再生が別の接続でチューナーを開くため2本目が503になる)。詳しくは [contrib/tvheadend](../contrib/tvheadend/README.md)。
- **Kodi**: tvheadend の HTSP(`pvr.hts`)経由を勧める。IPTV Simple Client で M3U を直接読ませるのは未確認。
- ほかの IPTV クライアントも、**チューナーは1系統**なので、同時に複数のチャンネルは開けない。

#### tvheadend(動作確認済み)

IPTV 自動ネットワークに `playlist.m3u8` を登録すると、20サービスを検出でき、手動のDVR予約で録画できました(約14.4Mbps)。手順・注意は [contrib/tvheadend](../contrib/tvheadend/README.md)(`--ulimit nofile`、`max_streams=1`、アイドルスキャンの停止など)。

## 地デジと BS/CS の同時受信(試験的、`--dual-tuner`)

nasne は、地デジ用と衛星用で、復調 IC の系統(I2C 0x10 と 0x11)も RF チューナーも別々です。既定の `nasne-recpt1` は、安全側に、地デジも衛星も**同じ1つのロック**で直列化していますが、`--dual-tuner` を付けると、**地デジ用と衛星用のロックに分け、地デジ1番組と BS/CS 1番組を同時に使える**ようになります(同じ系統の2番組は同時にできません)。

**有効にする**(nasne 上で):
```
echo NASNE_RECPT1_DUAL=1 > /etc/default/nasne-recpt1
/etc/init.d/nasne-recpt1-server restart
curl 'http://<nasne>:8301/status?detail'     # terrestrial=idle satellite=idle のように、系統ごとに出る
```
元に戻すには、`/etc/default/nasne-recpt1` を消して `restart` します。

**Mirakurun**: 地デジ用(`GR`)と衛星用(`BS`・`CS`)の**2つのチューナー**として登録します。`python3 scripts/nasne_mirakurun_conf.py <nasne> --scan --dual` で、`tuners.yml` を2つのチューナー(`nasne-gr`・`nasne-sat`)で作れます。

**互換性**: `--dual-tuner` のときも、どれかの系統が使われている間は、従来のロック(`/tmp/nasne-recpt1.lock`)を共有ロックで持つので、`nasne-ledd`(REC LED)・`nasne-waked`(自動オフ)・`/status` は、従来どおり「どれかが使用中」を検知します。ファームが止まっていてドライバを再読み込みする場面では、もう一方の系統が使用中なら再読み込みしません(配信が切れるため)。

**確認したこと**(電波が無い環境。CECH-ZNR2J):
- 地デジの配信中に、衛星の選局(ロックしない)を繰り返しても、地デジの TS は乱れない(CC エラー 0、デコードエラー 0)。
- 地デジの配信中に、ファームウェアは、衛星入力の2本目の PSI ストリームと、2本目の B-CAS 用ストリームも開ける。
- 通常モード(既定)は、従来どおり直列(地デジの配信中の衛星の要求は、10秒待って 503)。

**他の人の実機での確認**(CUHJ-15004、公式 v2.57、Issue #12): 地デジ(NHK 総合)と BS(BS 朝日)を**同時に約60秒**受信して、それぞれ約115MB を取得。TS の同期エラー・TEI・不正な AFC は 0、映像・音声(BS は ARIB 字幕も)を ffprobe で認識。受信後は `terrestrial=idle satellite=idle` に戻る。
- **既知の問題**: 地デジと BS の受信を**ほぼ同時に開始**すると、まれに片方で `B-CAS ACTIVATE失敗`(`rv=2`)になり、その接続が HTTP 500、もう片方も途中で止まる(2秒ずらせば、毎回成功)。B-CAS カードは1枚で、ACTIVATE が系統をまたいで共有されることが関係していそうなので、**B-CAS ストリームを開いてから `OPEN_SECUREDTS` までを、プロセス間で直列にし、ACTIVATE の失敗は最大3回まで再試行する**ようにした(`/tmp/nasne-recpt1-bcas.lock`)。この修正で解消するかは、手元(電波なし)では確認できていないので、同時開始での結果を教えてください。
- **ほぼ同時に開始した場合の BS の立ち上がり**: ほぼ同時に開始した 4 秒の試験で、BS の映像・音声が出始めるのが遅れる(4 秒以内に出ない)回があった。30 秒の試験(5 回)では毎回、映像・音声が取れている。「欠落」ではなく「開始の遅れ」と見ていて、`nasne-recpt1` のログ(`first A/V packet, ms since stream start`)で測れる(調査中)。開始を2秒ずらすと、遅れは出ない。Mirakurun のチューナーの起動の待ち時間には余裕を持たせるとよい。
- **B-CAS ACTIVATE の競合への対処後**: ほぼ同時の開始を 15 回(4 秒 10 回+30 秒 5 回)行って、ACTIVATE の失敗は 0 回(ロックの待ちは約 300〜550ms)。
- **未確認**: 長時間(数時間)の安定性、Mirakurun/EPGStation 経由の同時の予約録画、2サービスの B-CAS 復号を長時間続けたときの挙動。**試した結果を教えてください**([Issue #12](https://github.com/mar1mo-41414/SonyNasne-debian/issues/12) か、検証レポート)。

## 仕組み

詳細: [docs/12](12_ts_and_bcas_decrypt.md)。要約:

1. 復調IC/チューナーをI2Cで選局([docs/08](08_tuner_i2c.md))。
2. TSパススルーストリーム(PSI)で全PIDを受け、PAT/PMTからサービスの映像・音声・PCR・ECMのPIDを得る。
3. B-CASストリームを開いて活性化し、`OPEN_SECUREDTS`(純正と同じ設定で、映像はパススルー=変換なし)を開く。ファームがB-CASで復号し、TS化して出力する。
4. 出力(192バイトのタイムスタンプ付きTS)の先頭4バイトを外す。ファームが作り直すPAT(選んだ1サービス分だけ)は捨てて、PSIストリームの**元のPAT**(NITのPIDを含む。録画ソフトがNITの場所を知るために必要)と、SI(PID 0x10〜0x14、0x23、0x24、0x26〜0x28)を混ぜて出力する。

## トラブルシュート

| 症状 | 対処 |
|---|---|
| `ロックしなかった` | アンテナ・チャンネル番号を確認。`--scan` でロックするチャンネルを調べる(チャンネル番号は物理チャンネル。共同受信設備では再送信のチャンネルが一般的な配置と異なることがある) |
| `チューナー使用中` / HTTP 503 | 別のセッションが動いている(**最大10秒は待ってから**503を返す)。`/status` で確認 |
| 画が出ない・スクランブルが残る | B-CASカードが挿さっているか、活性化できているか。`-v` でPIDを確認 |
| `/status` が `busy` のまま戻らない。状態ページに「チューナーのプロセスが固まっている」と出る | `nasne-recpt1` のプロセスが D 状態(ドライバの中で戻らない)になっている。**D 状態は正常な受信中にも現れる**ので、目印は「D のまま CPU 時間(`/proc/<pid>/stat` の utime+stime)が進まない」こと。**電源の入れ直しでしか復旧しない**(`reboot` は効く)。複数クライアントの頻繁な開閉で、まれに起きる |
| nasne全体が応答しない(pingは通るがssh不可) | ストリームを開いたまま強制終了した場合などに起こり得る。電源ケーブルを抜き差しする(`kill -9` を避ける。通常の終了(SIGTERM・接続切断)では起こらない) |
