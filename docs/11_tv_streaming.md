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
| ⚠️ | チューナー(地デジ)は1系統だけ扱っている。2つ目のチューナー・BS/CSは未検証(BSはアンテナのない環境で試験したため。コードは純正の手順を再現済みだが動作未確認) |
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
nasne-recpt1 --scan                      # UHF 13〜62 をスキャンして、ロックしたチャンネルのサービス一覧を表示
```

`--b25` `--strip` `--device` `--lnb` など recpt1 のオプションは受け付けて無視する。

終了コード: 0 正常 / 1 エラー(ロックしない・サービスが無い・ストリームが開けない) / 2 使い方 / 3 チューナー使用中。

### HTTP

```
GET /tuner/<UHF ch>[?sid=0xNNNN][&sec=N]   復号済みTS(video/MP2T)。切断するとストリームを閉じる。sec を付けるとN秒で終了
GET /<UHF ch>                              同上(短縮形)
GET /scan                                  サービス一覧(テキスト。約80秒かかる)
GET /status                                idle または busy
```

使用中は `503 Service Unavailable`、信号なしは `404`。

```bash
curl http://<nasne>:8301/tuner/27 | ffplay -
curl -s http://<nasne>:8301/scan
# ch=27 freq=557142kHz sid=0x0400(tv) sid=0x0401(tv) sid=0x0408(tv)
```

### 録画ソフトとの連携

#### Mirakurun(動作確認済み)

Mirakurun 4.1(Node.js 22以上推奨)で、nasne のHTTP配信を「コマンド型チューナー」として使い、**チャンネルスキャン・サービス名の取得・ストリーム取得・番組表(EIT)の取得**まで動いた。
Mirakurun は別のPCで動かす(nasne上ではなく)。EPGStation など Mirakurun 経由の録画ソフトはそのまま使えるはず(EPGStation自体は未検証)。

`tuners.yml`:

```yaml
- name: nasne
  types:
    - GR
  command: curl -s http://<nasneのIP>:8301/tuner/<channel>
  isDisabled: false
```

`channels.yml`(**サービスごとに1エントリ**。`channel` に `?sid=` を付けるのがポイント。nasne は1回に1サービスしか出力しないため):

```yaml
- name: 局名
  type: GR
  channel: '27?sid=0x0400'      # 27 = UHFチャンネル、0x0400 = サービスID(/scan で調べる)
  serviceId: 1024              # 0x0400 の10進数
  isDisabled: false
- name: 局名2
  type: GR
  channel: '27?sid=0x0401'
  serviceId: 1025
  isDisabled: false
```

サービスIDの調べ方: `curl http://<nasne>:8301/scan`(`sid=0x...(tv)` の一覧)。

確認した結果: Mirakurun が起動時にサービススキャンを行い、サービス名(ARIB文字を含む)・ネットワークID・サービスIDを取得した。
番組表は約1分の受信で数百件の番組が取れた。`/api/services/<id>/stream` からは、元画質(MPEG-2 1440×1080 + AAC + 字幕)のTSが取得できた。
Mirakurunのチャンネルスキャン(自動)は、チャンネル全体のTSを前提にするため、**サービスごとのチャンネル設定を手書きするのが確実**。

Mirakurunのデコーダー(`decoder`)設定は不要(nasne が復号済みのTSを出す)。複数サービスの同時視聴は、チューナーが1系統なので不可(同じチャンネルの別サービスも不可)。

#### VLC / ffmpeg / Kodi / Jellyfin など

`http://<nasne>:8301/tuner/<ch>?sid=0x....` を再生/IPTVチャンネルとして登録する(mpv で URL を直接再生できることを確認した。VLC・Kodi・Jellyfin は未確認。M3Uを作って登録するのが簡単)。

#### tvheadend

IPTV ネットワーク(自動ネットワーク)にM3UのURLを登録する形で使えるはずだが、未検証。

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
| `チューナー使用中` / HTTP 503 | 別のセッションが動いている。`/status` で確認 |
| 画が出ない・スクランブルが残る | B-CASカードが挿さっているか、活性化できているか。`-v` でPIDを確認 |
| nasne全体が応答しない(pingは通るがssh不可) | ストリームを開いたまま強制終了した場合などに起こり得る。電源ケーブルを抜き差しする(`kill -9` を避ける。通常の終了(SIGTERM・接続切断)では起こらない) |
