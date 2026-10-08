# 12. TSの取り出しとB-CAS復号(ドライバのioctlレベルの手順)

[docs/11](11_tv_streaming.md) のツール(`nasne-recpt1`)が内部でやっていること。ドライバ `xcode4drv`(`/dev/vixs/xcodedrv`、ioctl `0x17241724`、共通ヘッダ0xa4バイト。[docs/07](07_driver_ioctl_bcas.md))の
ストリームを開いて、B-CASで復号された放送波のTSを受け取るまでを説明する。構造体のオフセットはSDKヘッダ(`drv_if.h`)をコンパイルして確定し、
既定値は純正 `dtvtuner` が組み立てる値を再現して得た(下記)。

前提: チューナーを選局してロックさせてあること([docs/08](08_tuner_i2c.md))、ファームが稼働中([docs/07](07_driver_ioctl_bcas.md))。

## ストリームの種類

| コマンド | 名前 | 使い方 |
|---|---|---|
| `0x2d` | `OPEN_TS_PASSTHROUGH`(サイズ 0x484) | チューナー入力のTSをPID指定でそのまま受け取る(PSI/SI用)。**スクランブルされたまま** |
| `0x2e`/`0x2f` | `OPEN_GENERIC` / `CLOSE_GENERIC` | B-CAS(`generic_mode`=`0x400`)のセッション |
| `0x30` | `OPEN_SECUREDTS`(サイズ 0x9ec) | **B-CASで復号**し、(設定により)映像を変換して多重化したTSを出す。純正が放送を扱うときに使う |
| `0x14` | `SDEV_RECV`(サイズ 0x230) | 出力の受信 |
| `0x11` | `SDEV_CLOSE`(サイズ 0xb4) | ストリームを閉じる |

## 1. TSパススルー(全PID、PSI/SI)

`OPEN_TS_PASSTHROUGH` のバッファ(サイズ `0x484`):

| offset | 値 |
|---|---|
| `+0xac`(64bit `mode`) | `0x800000`(`SDEV_TSPASSTHROUGH_OUTPUT`) |
| `+0xbc`(`src`) | 入力ID。**0**(地デジのTS入力) |
| `+0xc8`(`ts_passthrough_pid`) | `0xffff`(全PID) |
| `+0xd0`(`req_size`) | `0x10000` |

成功すると `+0xa4`(64bit)にハンドルが返り、応答の `+0x1b0` に出力バッファの情報(個数8、1個0x10000バイト、**ユーザ空間にマップされたアドレスが `+0x1b8` から8個**)が入る。
`SDEV_RECV`(`flags` `+0xac` = `SDEV_IO_PSI` `0x20000`、`+0xbc` = `0x10000`)を発行すると、`+0xbc` に受信バイト数(188の倍数)、`+0x13c` に**出力バッファの番号**が返る。
データはマップされたバッファの該当番号にある(188バイトのTSパケット)。

**次の `SDEV_RECV` の `+0x144` に、前回受け取ったバッファ番号を入れて返す**(最初は `0xff`)。これで消費済みバッファが解放される。
返し忘れるとバッファが8個で詰まり、以降はタイムアウト(`return_value`=6)になる。

## 2. B-CASストリーム

1. `OPEN_GENERIC`(サイズ 0x208): `+0xac`=`0x400`(`GENERIC_MODE_BCAS`)、`+0xb0`=`0x1000`。ハンドルが `+0xa4` に返る。
2. `SDEV_SEND`(0x13、サイズ 0x1d0)で `ACTIVATE` を送る: `+0xa4`=ハンドル、`+0xac`=0、`+0xb0`=`0x10`(上位ワード。`SDEV_IO_GENERIC_DATA`)、`+0xc4`=ダミーのバッファ、`+0xcc`=0、
   `+0xd4`=`0x400`、`+0xd8`=`1`(`BCAS_CMD_ACTIVATE`)、`+0xdc`=1。
3. ジェネリックストリームへの `SDEV_RECV`(`+0xac`=0、`+0xb0`=`0x10`)で、イベントを待つ。`+0x130`(`generic_result`)に `0x400`(ACTIVATED)が立てば活性化済み。

**カードIDの取得(`CARD_ID_ACQUIRE`)は不要。** 復号はファームがカードと直接やり取りして行う。

## 3. 復号ストリーム(`OPEN_SECUREDTS`)

純正は、放送を `FUN_00645754` というストリーム開始処理でカテゴリ4(`OPEN_SECURED`)として開く。構造体の既定値は、
`dtvtuner` 内の設定関数(`0x47edfc` → `0x63e84c` → `0x6420fc`)を MIPS エミュレータ(unicorn。[scripts/ghidra/emu_secured_open.py](../scripts/ghidra/emu_secured_open.py))で実行して得た。
ストリームは3本(ビット1/2/4)あり、設定が違う:

| | ストリーム1(ビット1) | ストリーム2(ビット2) |
|---|---|---|
| `mode`(64bit、`+0xac`/`+0xb0`) | `0x08204141` / `0x80006`(**`SDEV_VIDEO_PASSTHROUGH`**) | `0x08204141` / `0x80002` |
| `video_type`(`+0xe0`) | 0(変換なし) | 9(H.264へ変換) |
| 結果 | **元のMPEG-2映像のまま(元画質)** | 8Mbps 720×480 のH.264に変換 |

`mode` の下位 `0x08204141` = `SDEV_TS_INPUT | LOCAL_TIME_STAMPS | AUDIO_TS_OUTPUT | AUDIO_XCODE | VIDEO_TS_OUTPUT | VIDEO_XCODE`、上位 `0x80000`=`MULTIPLE_OUTPUT`、`0x2`=`FRPT_DATA_OUT`。
**`nasne-recpt1` は元画質のストリーム1の設定を使う。**

ストリーム1の既定値(`+オフセット`=値): `+0xd4`=1(`output_port`)、`+0xe0`=0、`+0xe4`=`0x1e8480`、`+0xe8`=`0x1e`、`+0xec`=`0x2d0`、`+0xf0`=`0x1e0`、`+0x120`=`0x10000`(`req_size`)、
`+0x12c`=`0x2dc6c0`、`+0x134`=`0x300d8`、`+0x138`=`0x30`、`+0x140`=`0x900000`、`+0x144`=2、`+0x148`=3、`+0x184`=1(`input_encrypt_mode`=BCAS)、
`+0x19c`=`0x40`(`securedts_mode`=`VIA_BCASCARD`)、`+0x608`=`0x21`、`+0x67c`=`0x10000`、`+0x698`=`0x30000`、`+0x894`=`0xc`、`+0x898`=`0x5b8d80`、`+0x8a8`=`0x2ee00`、`+0x8ac`=`0xbb80`、
`+0x8b0`=2、`+0x8b4`=`0x400`、`+0x924`=2、`+0x940`=`0xf03`、`+0x950`=`0x1e8480`、`+0x954`=2、`+0x958`=3、`+0x964`=`0x1e8480`、`+0x968`=2、`+0x96c`=3、`+0x978`=1、`+0x980`=2。

番組(サービス)ごとに入れる値:

| offset | 内容 |
|---|---|
| `+0xb4`(64bit) | `generic_handle`(B-CASストリームのハンドル) |
| `+0xbc`(64bit) | `passthrough_handle`(PSIストリームのハンドル。**これが無いと `CMD_FAILED`**) |
| `+0xcc` | `video_src` = 0(入力ID) |
| `+0xd8` / `+0xdc` | `video_pid` / `video_pid_remap`(同じ値) |
| `+0xf4` / `+0xf8` | `num_audio_pids` = 1 / `audio_src` = 0 |
| `+0x100` / `+0x110` | `in_audio_pid[0]` / `out_audio_pid[0]`(同じ値) |
| `+0x13c` | `pcr_pid` |
| `+0x160` / `+0x194` | `out_program_number` / `program_number`(サービスID) |
| `+0x178` | `ecm_pid`(PMTのプログラム記述子で CA_system_id=5 の PID) |
| `+0x1a0` | `num_scrambled_pid` = 2 |
| `+0x1a4`〜 | `scrambled_es_pid[]` = [映像PID, 音声PID] |
| `+0x204`〜 | `scrambled_ecm_pid[]` = [ECM, ECM] |
| `+0x264`〜 | `scrambled_es_type[]` = [映像stream_type(2か0x1b), 音声stream_type(0x0f)] |

成功すると、応答の `+0x2cc`(`soc_output_video_buf_info`)に出力バッファ(16個×64KB。アドレス表は `+0x2d4`)が返る。

`SDEV_RECV`(`flags`=`SDEV_IO_TS` `0x2000`、`+0xbc`=`0x10000`、`+0x144`=前回のバッファ番号)で受信する。応答の `+0xbc` が受信バイト数、`+0x13c` がバッファ番号。
**データは192バイトのパケット(先頭4バイトがローカルタイムスタンプ + 188バイトTS)**。先頭4バイトを外せば通常のTS。
出力のPES(映像・音声)は `scrambling_control` が0(復号済み)。出力には選んだ1サービスのPAT/PMTと映像・音声・字幕・データ放送が入り、NIT/SDT/EITなどのSIは入らない
(`nasne-recpt1` はPSIストリームから該当PIDを混ぜている)。

## 4. 終了

`SDEV_CLOSE`(`0x11`、サイズ `0xb4`、`+0xa4`にハンドル)を 復号ストリーム → PSIストリームの順に、`CLOSE_GENERIC`(`0x2f`、`+0xb0`=`0x400`)でB-CASを閉じてからfdを閉じる。
**復号ストリームを開いたままプロセスを終えると、nasne全体が応答しなくなったことがある**(pingは通るがsshが応答せず、電源の抜き差しで復旧)。必ず上の順で閉じること。

## 実験用ツール

- [scripts/tools/drvsh.c](../scripts/tools/drvsh.c): 同一fdで ioctl を連続実行する(`io` / `rx` / `ts` / `dump` / `sleep`)。ストリームのハンドルは fd に紐づくため、複数ストリームを同時に開く実験に必要。
- [scripts/ghidra/emu_secured_open.py](../scripts/ghidra/emu_secured_open.py): 純正の設定関数を unicorn で実行して既定値を取り出す。
