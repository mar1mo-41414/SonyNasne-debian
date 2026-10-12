# 17. 公式の Web API(ポート 64210)を、WebUI と同じ手順で呼ぶ

公式ファームが動いている nasne は、ポート 64210 で Web API を提供しています(公式のWebUI、nasne ACCESS などが使う)。
公式WebUIの JavaScript(`/var/opt/dtvtuner/nv/00000022/lib/app.js`。公式 rootfs に入っていて、どの nasne でも配信されている)を読んで、呼び方が分かりました。
標準ライブラリだけの小さなクライアントを `scripts/nasne_api.py` に置いてあります。

## 認証

| 種類 | 認証 |
|---|---|
| **GET**(`/status/...`、`/config/...Get`、`/recorded/...`、LED制御 `/config/LEDControl` など) | なし |
| **POST**(`/config/...Set`、`/config/initialize`、`/config/internalHDDInitialize` など) | ヘッダ **`X-DTVToken`** が必要 |

- トークン = `base64( SHA1( PIN + パス + 本文の先頭256文字 ) )`。`Content-Type: application/x-www-form-urlencoded`。
- **PIN は、本体の MAC アドレスの下位3バイト**です。`/status/networkIfInfoGet` の `macAddr`(例 `00:25:dc:58:da:f0`)からコロンを除いて7文字目以降を取ります
  (APIが返す表記のまま、小文字。例では `58daf0`)。WebUIは、リモート(別ポート)アクセスのときにこの値を PIN にします。
- トークンが違う、または無いと **HTTP 400**。存在しない API は 404。

```bash
python3 scripts/nasne_api.py <nasneのIP> get  /status/HDDListGet
python3 scripts/nasne_api.py <nasneのIP> post /config/boxNameSet name=nasne      # PINは自動で取得
```

## LED の制御(`/config/LEDControl`)

`GET /config/LEDControl?powerGreen=a&powerRed=b&record=c&network=d`。各値は **0 = 変更なし、1 = 点灯、2 = 消灯**。
WebUIの「nasneを探す」(複数台の中から本体を見つける機能)は `powerGreen=2, powerRed=1, record=1, network=1`(緑を消し、赤・REC・LAN を点灯)を呼びます。
`nasne_api.py <IP> led` が同じ呼び出しです。(LEDの見た目の確認は、カメラの位置合わせが安定せず、まだ確定できていません。)

## 主な API(WebUI から抽出)

`config/` に、`boxNameSet`、`networkIfInfoSet`、`areaInfoSet`、`channelScanStart/ProgressGet/Cancel`、`currDateSet`、`HDDPowerSavingModeSet`、`NASEnableSet`、`NASNameSet`、
`parentalRatingInfoSet`、`PINVerify`、`DMPRegister/Release`、`remoteRegister/Release`、`updateFileDownload`、`finishSetup`、
**`HDDRegister` / `HDDRelease`**(外付けHDDの登録。本文 `id=1&forceFlag=0`)、**`initialize`**(設定の初期化。本文 `initialize=0`)、
**`internalHDDInitialize`**(内蔵HDDの初期化。本文 `internalHDDInitialize=0`)、`initializeProgressGet`(両方の進捗)など。
`status/` に `HDDListGet`、`HDDInfoGet`、`boxNameGet`、`softwareVersionGet`、`networkIfInfoGet`、`channelListGet`、`BCASInfoGet`、`isFinishSetup`、
`recorded/` に `recordedContentCountGet`、`recordedContentMaxCountGet`(録画の上限 3000)など。

## 内蔵HDDの初期化 API の動作(実機で確認)

空に近い p3(`.hai` と録画DBの雛形だけ。[docs/15](15_p3_structure_and_hai.md))で `POST /config/internalHDDInitialize` を呼ぶと、
公式ファームが **`/disk0/00000015/` の下に 96 個のディレクトリ(`00`〜`95`、権限 `d-wxr----t`)と、各ディレクトリに DB ファイル 2つ(`000000110.dat` 8バイト、`000000111.dat` 51200バイト)を作ります**
(合計194ファイル)。`10000000.dat` / `10000001.dat` は作り直されず、そのまま使われます。
`000000111.dat` の中身は、ディレクトリごとにすべて異なり(先頭がマジック `a17e4d13`、長さ、4バイト、固定の8バイトのあとに暗号化された本体)、ヘッダの4バイトは単純な CRC32 ではありませんでした。暗号の解読は未了です。
(この呼び出しは、`.hai`・ルートの `10000000.dat` が無い p3 では、そもそも公式アプリが起動しないので使えません。)
