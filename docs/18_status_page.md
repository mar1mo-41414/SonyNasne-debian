# 18. 状態ページ(温度・SMART・メモリ・ディスク・MCU・チューナー)

nasne(Debian直起動)には、既定で**状態ページ**が入ります。HDDの温度やSMART、メモリ・ディスクの使用量、MCUの状態、チューナーの使用状況を、ブラウザや他のソフトから読めます。
**読み取り専用で、何も変更しません。**

> ⚠️ **状態ページ(8302番)には認証がありません。** LAN内の誰でも、稼働状況・HDDの機種とSMART・MCUの状態・チューナーの使用状況を読めます(読み取り専用で、操作はできません)。
> **ルーターのポート開放などで、インターネットに出さないでください。** 自分のPCからだけ見るなら `NASNE_STATUS_BIND=127.0.0.1`、使わないなら `NASNE_STATUS_ENABLE=0`(下の「設定・無効化」)。
> 同じ理由で、チューナーのHTTP配信(8301番)も認証なしです([docs/11](11_tv_streaming.md))。ネットワークに出るものの一覧は [README のセキュリティ](../README.md#セキュリティネットワークに出るもの) を見てください。

```
http://<nasne>:8302/             状態ページ(HTML。10秒ごとに自動更新、ダーク/ライト対応)
http://<nasne>:8302/status.json  JSON(Home Assistant などから読む用)
http://<nasne>:8302/metrics      Prometheus 形式
http://<nasne>:8302/healthz      ok
```

## 見えるもの

| 項目 | 取り方 | 備考 |
|---|---|---|
| 稼働時間・負荷・メモリ・カーネル | `/proc` | メモリの「空き」は free + buffers + cached |
| ディスク使用量(`/` と `/data`) | `df` | `/` は p3 の中の ext3 イメージ、`/data` は録画領域(XFS) |
| HDD の温度・SMART 健康状態・通電時間・電源投入回数・代替/保留/訂正不能セクタ | `smartctl`(smartmontools) | **待機中(スピンダウン中)は起こさず**、前回の値を返す(`standby: true`)。SMART の再取得は最短60秒おき |
| MCU の状態(ウォッチドッグ停止中か、電源ボタンの長押しラッチ、タイマー起動か) | `mcui2c r 20 6` | [docs/06](06_mcu_watchdog.md) |
| チューナーの使用状況 | `nasne-recpt1` の `/status`(8301番) | 録画・視聴中は `busy` |
| 警告 | 上の値から判定 | HDD温度55℃以上、SMART が PASSED でない、保留/代替/訂正不能セクタがある、ディスク使用率95%以上、メモリの空きが15MB未満、**チューナーのプロセスが固まっている**(`nasne-recpt1` が D 状態のまま。1秒おきに3回確かめる。電源の入れ直しが必要。[docs/11](11_tv_streaming.md)) |

SoC(MIPS)の温度センサーは、カーネルに hwmon/thermal が無く、取れません。HDD の温度(SMART)が、この機体で取れる唯一の温度です。

`status.json` の例(抜粋):

```json
{"hdd":{"device":"/dev/sda","health":"PASSED","power_on_hours":624,"temperature_c":38,"pending_sectors":1,"standby":false, ...},
 "mcu":{"available":true,"watchdog_disabled":true,"power_button_latch":false,"timer_wake_boot":false,"raw":"13 80 00 02 4c 6f"},
 "tuner":{"available":true,"busy":false,"status":"idle"},
 "warnings":["HDD の保留セクタがある (1)"], "ok":false, ...}
```

## 設定・無効化

`/etc/default/nasne-status`(無ければ作る)に書く:

| 変数 | 既定 | 意味 |
|---|---|---|
| `NASNE_STATUS_ENABLE` | 1 | `0` で起動しない |
| `NASNE_STATUS_PORT` | 8302 | ポート |
| `NASNE_STATUS_BIND` | 0.0.0.0 | 待ち受けアドレス。**認証がない**ので、LAN の外に出さないこと。自分だけなら `127.0.0.1`(その場合、他のPCからは見えない) |
| `NASNE_STATUS_DEVICE` | /dev/sda | SMART を読むデバイス |
| `NASNE_STATUS_TEMP_WARN` | 55 | 温度警告のしきい値(℃) |

`/etc/init.d/nasne-status restart` で反映します。

## 実装

`scripts/watchdog/nasne-statusd`(Perl のコアモジュールだけ。wheezy の `perl-base` で動く。単一プロセス、1回の取得は約0.6〜1.4秒でほとんどが `smartctl`)と、init スクリプト `scripts/watchdog/nasne-status`。
`scripts/stage1/prepare_tree.sh` が、`smartmontools`(推奨パッケージは入れない)と一緒にツリーへ入れます(`update-rc.d nasne-status defaults 25`)。

## 確認したこと

- 実機で、`/` `/status.json` `/metrics` `/healthz`、404、HEAD が期待どおり返る。ブラウザ(狭い画面・ダークモード)でも崩れない。
- SMART の取得結果が `smartctl` と一致(温度・通電時間・セクタ)。この機体の古い320GB HDDは保留セクタが1つあり、実際に警告として出た。
- 待機中の分岐は、実機ではHDD自身が頻繁に使われて確認できていない(コードは `hdparm -C` の結果で判定)。

## Home Assistant

オプションで、状態ページを HA に出せます。[contrib/homeassistant](../contrib/homeassistant/README.md)。
