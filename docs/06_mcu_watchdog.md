# 06. 約333秒ごとのリセット(MCUウォッチドッグ)と、止め方

## 結論

Sony のユーザランド抜きで起動すると、**約333秒(約5.5分)ごとに本体がリセットされる**。原因は SoC の外にある**MCU(基板上のマイコン)のウォッチドッグ**で、
純正の `procmng` は起動直後にこれを止めている。止め方:

> 公式ドライバ(`xcode4drv`)のI2C ioctl で、**MCU(I2Cバス0、8bitアドレス `0x20`)へ `[0x20, 0x01]` を1回書く**
> (`procmng` の文字列 "MCU Watchdog Disable" と同じ操作)

SoC内のWDTレジスタ(`WDT_TIMER0` など)を最大値にしても止まらない(MCU側のため)。Linux標準のwatchdogデバイスも無い。

検証: 手動で書いた場合は1000秒超、起動時サービス化したあとも連続稼働して、リセットは起きなかった(書かないと333秒で落ちる)。

## 使い方(Debian側)

前提: 公式ドライバ `xcode4drv.ko` と `rc.xcode4` が `/usr/local/sbin/` にあること(公式ファームのrootfsの `/opt/dtvtuner/lib/modules/` にある。
`ofw_tool.py` と `dlm_crypto.py` でrootfsを取り出す。SIEのGPLソース `AVDriver7GSP47.zip` にも同世代のものが入っているが、こちらは未検証)。

```bash
# MIPS用にビルド(Dockerの nasne-kcc:gcc432 環境。docs/05)
mipsel-linux-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -e __start -o mcui2c scripts/watchdog/mcui2c.c
scp mcui2c root@<nasne>:/usr/local/sbin/
scp scripts/watchdog/nasne-mcu-wd root@<nasne>:/etc/init.d/
ssh root@<nasne> 'chmod +x /etc/init.d/nasne-mcu-wd; update-rc.d nasne-mcu-wd defaults 05'
```

`nasne-mcu-wd` は起動のたびにドライバを読み込み(`rc.xcode4`)、`mcui2c w 20 20 01` を成功するまで繰り返す。ログ: `/var/log/nasne-mcu-wd.log`。

単発で試すだけなら:

```bash
cd /usr/local/sbin && ./rc.xcode4 && ./mcui2c w 20 20 01       # "mcui2c write addr8=20 n=02 ... (COMPLETE)"
./mcui2c r 20 6                                                 # MCUのステータス6バイトを読む
```

## MCUのコマンド(`procmng` の逆コンパイルから)

MCU(I2Cバス0、8bitアドレス `0x20`)への書き込みは `[レジスタ, 値]` の2バイト。

| 書き込み | 使われる場面 |
|---|---|
| `[0x20, 0x00]` | メインループが**約5秒ごと**に送る(心拍) |
| `[0x20, 0x01]` | **MCU Watchdog Disable**。シャットダウン処理(`/sbin/halt` の前)と、init失敗時 |
| `[0x01, 0x00 / 0x2f]` | 起動完了時 |
| `[0x08, 0x00]` `[0x40, 0 / 3 / 5]` | 起動シーケンス中 |
| `[0x04, 0x02]` | リブート関連(引数ありのとき)。意味は未検証 |
| `[0x10, 0x2a / 0x32 / 0x51]` | 0x51 = シャットダウン時、0x32 = initの電源断前、0x2a = エラー点滅ループ。レジスタ0x10は動作モード/表示コード類(未解析) |
| `[0x02, …]` `[0x04, 0x01]` | 電源・タイマ関連(未解析) |

MCUの読み出しは6バイト(各バイトの意味は未解明。最終バイトはカウンタのように増える)。

## I2Cドライバ側の手順(ソースより)

`drv/i2c.c` の `i2c_submit_command` は、SoC内蔵のハードウェアI2Cブロック `i`(0/1)を次の手順で使う。
①HWミューテックス `MIPS_INTERLOCK1` のID4にドライバID(9)を書いて取得、②`GPIO_I_CTRL` のビット `(29-i)` を落としてI2Cマスタモードにする、
③データを `DATA_LOAD`、`CONFIG`(atomic_period / timeout)、`COMMAND`(slave<<16 | rop<<12 | start<<8 | stop<<9 | bytes)に書いて起動、
④`PPC_INT_STATUS` の XFERDONE / ARB_FAIL / BUSY_ERROR で完了判定。スレーブアドレスは**8bit表記を `>>1`** してコマンドに入れる。
公式のバス速度は既定 **10kHz**(`atomic_period = 6750000 / 周波数`、`timeout = 500×27/4`)。

> ⚠️ **`/dev/mem` でSoCのI2Cレジスタを直接叩いてはいけない。** 公式ドライバと同じ手順を再現しても、ブロック0はNACK/ARB_FAILになり、
> **MCU側のI2Cが固まって、ドライバ経由の通信も失敗し続け、SoCのリセットでも直らず電源サイクルが必要になった**。
> I2Cは必ず公式ドライバのioctl経由(`mcui2c` / `i2cx`)で行うこと。HWミューテックスの絡みがあると考えられる。

## シャットダウン

`halt` / `poweroff` は、ウォッチドッグを止めた状態では電源が切れない(カーネルの `viper_machine_power_off` は `wait` ループで、電源断はMCU任せ)。
純正でも電源断の手段は電源ケーブルを抜くことのみ。`reboot` は動く(GPIOでリセット)。

## LED

LEDはMCUではなく、**SoCのGPIO「ポート10」= レジスタ `0xBFFFFC64` のビット `0x100` / `0x200`**(制御レジスタは `0xFC6C`)を直接叩いている
(`init` / `procmng` のコードより)。GPIOポートNのレジスタは `0xFC04 + 0x10×(N-4)`。
Debian起動中はPWR LEDが点滅のままで、点灯させる方法は未実装(ビットの極性が未確認)。

## 参考: 純正が時間制限を起こす原因の切り分け

カーネルの再起動処理 `viper_machine_restart` は、クロック/電源ゲート系レジスタを順にゼロにしてGPIOを落とすだけの構造で、ウォッチドッグ自体は触らない。
`procmng` は他にも `LD_PRELOAD` / `IFS` 環境変数を検出すると意図的に固まる(anti-tamper)。観測用に `LD_PRELOAD` で割り込ませる手法は使えない。
