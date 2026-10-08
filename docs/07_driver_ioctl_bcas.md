# 07. ドライバ(`xcode4drv`)のioctlと、B-CAS

ViXS製ドライバ `xcode4drv.ko`(ソースはSIEがGPLで公開、[docs/05](05_kernel_and_direct_boot.md))は、SoC内のメディアエンジン・I2C・ストリーム入出力を、
`/dev/vixs/xcodedrv`(`rc.xcode4` が作る)への**単一の ioctl** で公開している。純正の `procmng` / `dtvtuner` はすべてこれを使う。
コマンド番号表: [boxster_cmd_table.md](boxster_cmd_table.md)(SDKヘッダ `drv_if.h` の `CMD_*` / `CMD2_*` 列挙から作成)。

## リクエストの形式

`ioctl(fd, 0x17241724, buf)`。`buf` は先頭が共通ヘッダ(`IOCTLBUFFERHEADER`、**0xa4 バイト**)で、コマンド固有の本体はその後ろ(`+0xa4`〜)。

| offset | 内容 |
|---|---|
| +0x00 | `size_of_buf`(バッファ全体のバイト数。ヘッダ以上であること) |
| +0x04 | `command`(コマンド番号) |
| +0x08 | `asynchronous` |
| +0x0c | `return_value`(**呼び出し前に 2 を入れる**。完了すると 1 になる) |
| +0x94 | `time_out_value`(ミリ秒。純正は 2000 など) |
| +0x98 | `sub_command`(`CMD_INIT` などで使用) |

`return_value` の主な値: `1` 完了、`4` パラメータ不正、`6` タイムアウト、`0x0d` I2Cの NACK、`0x0e` ハードウェアエラー、`0x2f` ファームウェア停止(`MIPS_DEAD`)。

## ファームウェア(メディアエンジン)の状態

チップ内のARCコアで動くメディアエンジンのファームは、ドライバ(`.ko`)に埋め込まれていて、ロード時に起動される。
多くのコマンドは `Check_Mips_Status() == FW_ALIVE` が前提で、停止していると `MIPS_DEAD` を返す
(ストリームを開く系 = `OPEN_DECODE`(261)、`ALLOCATE_FB`(270)、`OPEN_GENERIC`(46)などはこのため失敗する)。

状態の確認: `CMD_GET_FW_STATE`(0x103、サイズ 0xb0)。`+0xa4` が `0` なら稼働、`0xffffffff` なら停止(`FW_HANGUP`)。

```bash
boxioctl 103 b0 -d 0x2c      # +0xa4 の値を見る
```

**ファームは、ドライバをロードしてからしばらく後に `FW_HANGUP` になることがある(原因は未解明)。** `rc.xcode4` で再読込すると復活するので、
ストリーム系の実験は再読込の直後に行う。I2C(cmd 0x1b)はファームの状態に関係なく使える。

## 主なコマンドと構造体

| cmd | 名前 | 備考 |
|---|---|---|
| 0x0e | `SDEV_GET_CAPS` | コーデック能力(video/audio の入出力ビットマップ)を返す |
| 0x1b | `I2C_COMMAND_STREAM` | I2Cの読み書き。サイズ 0x110。本体: `data@+0xa4`、`num_bytes@+0xe4`、`rop@+0xe8`(1=write)、`slave(8bit)@+0xec`、`atomic_period@+0xf0`、`address_mode@+0xf4`、`timeout@+0xf8`、`i2c_block@+0xfc`、`startbit@+0x100`、`stopbit@+0x104` |
| 0x2e / 0x2f | `SDEV_OPEN_GENERIC` / `CLOSE_GENERIC` | ジェネリックストリームを開く/閉じる(B-CAS など)。OPEN: サイズ 0x208、`generic_mode@+0xac`、`req_size@+0xb0`、返るハンドル(8バイト)が `+0xa4`。CLOSE: サイズ 0xb4、`handle@+0xa4`、`generic_mode@+0xb0` |
| 0x13 / 0x14 | `SDEV_SEND` / `SDEV_RECV` | ストリームへの送受信。SEND: サイズ 0x1d0、RECV: サイズ 0x240 |
| 0x100 | `CMD_INIT` | `sub_command@+0x98`。ボードIDなどを返す |
| 0x105 | `SDEV_OPEN_DECODE` | デコーダを開く(ファーム稼働が前提) |

`SEND` の主なフィールド: `stream_handle@+0xa4`(8バイト)、`flags@+0xac`(64bit)、`buffer@+0xc4`、`size@+0xcc`、`generic_mode@+0xd4`、`bcascommand@+0xd8`、`bcasmsg_id@+0xdc`、`cardstatus@+0xe0`(出力)。
`RECV`: `stream_handle@+0xa4`、`flags@+0xac`、`buffer@+0xb4`、`size@+0xbc`(入力=バッファ長/出力=受信長)、`type@+0xc0`、`more@+0xc4`、`result@+0xc8`、`generic_result@+0x130`(イベントビット)。
ジェネリックデータの `flags` は**64bitで、上位ワード(`+0xb0`)に `0x10`**、下位(`+0xac`)は 0(下位に入れると `PARAM_INVALID`)。

## B-CAS

B-CASカードは、チップ内ファームが扱う**ジェネリックストリームモードの一種**として公開されている(`CableCard_*` / PCA9554 は SDK ボード向けで無関係)。

| 項目 | 値 |
|---|---|
| モード(`generic_mode`) | `GENERIC_MODE_BCAS = 0x400`(他に `BCAS_DRM 0x800`、`BCAS_SESSION 0x1000`、`DTCP 0x200`) |
| コマンド(`bcascommand`) | `ACTIVATE 1`、`PPV_PURCHASE 2`、`CARD_ID_ACQUIRE 4`、`BCASMSG 8`、`DEACTIVATE 0x10`、`CARDSTATUS 0x20`、`UPDATE_CARDSTATUS 0x40` |
| `cardstatus` | `0` REMOVED / `1` INSERTED / `2` ACTIVED |
| RECV `generic_result` のビット | `0x100` 挿入、`0x200` 抜去、`0x400` 活性化、`0x200000` 非活性化、`0x1000000` ATR受信、`0x80000` BCASメッセージ応答、`0x40000` ECM |

### 実機で確認できたこと(`scripts/tools/bcas.c`)

| 操作 | 結果 |
|---|---|
| `OPEN_GENERIC`(mode 0x400) → `CARDSTATUS` | 成功。`generic_result` = `0x00200100`(挿入 + 非活性)。**カードの検出はできる** |
| `ACTIVATE` | 成功。続けて活性化イベント(`0x400`)が返る |
| `CARD_ID_ACQUIRE`、`BCASMSG` + 生APDU | SEND は成功するが、RECV はタイムアウトのみ。**カードIDは取得できていない** |

ATR・メッセージ応答・ECM のイベントは観測できていない。**ただし、放送の復号にはカードIDの取得は不要だった**:
B-CASストリームを活性化して復号ストリーム(`OPEN_SECUREDTS`)にハンドルを渡せば、ファームがカードと直接やり取りして地デジを復号する([docs/12](12_ts_and_bcas_decrypt.md)、[docs/11](11_tv_streaming.md))。
カードIDの取得用コマンドの正しい使い方は未解明のまま。

使い方: `bcas status|activate|id|deactivate ...`(1回の実行内で OPEN → 指定コマンド → CLOSE)、`bcas m<hex>` で `BCASMSG` に生バイト列を送る。
ツールはファーム稼働中(`FW_ALIVE`)でないと `OPEN_GENERIC` が失敗する。
