# 09. ツールの使い方

`scripts/` 以下のツールの一覧と使い方。Pythonのツールは Python 3.8+ のみで動く(外部ライブラリ不要)。
`公式ファーム` は Sony の公開サーバから各自取得したもの(例: `KRST3101_0260_SECURE.dlm`)。このリポジトリには含まれない。

## PC側のツール(Python)

### `dlm_crypto.py` — `.dlm` の暗号(Blowfish変種)

```bash
python3 scripts/dlm_crypto.py decrypt-header <file.dlm>            # 先頭64バイトのヘッダを復号して表示
python3 scripts/dlm_crypto.py decrypt-body   <file.dlm> out.tar.gz  # ボディを復号して書き出す(rootfsの中身を取り出せる)
python3 scripts/dlm_crypto.py parse-segments <spi.bin>              # SPIダンプ中の名前付きセグメントを列挙
```

他のツールがライブラリとして使う(`NasneBlowfish`)。仕組み: [docs/03](03_firmware_format.md)。

### `ofw_tool.py` — 公式ファームの分解

```bash
python3 scripts/ofw_tool.py split <公式.dlm> <出力dir>                     # KNL.bin / RFS.bin / DLM.bin に分解
python3 scripts/ofw_tool.py patch <rootfs.dlm> <out.dlm> --major 0         # ヘッダ+8(メジャーバージョン)を書き換えてCRCを再計算
```

`--major` を省略すると、CRCを再計算するだけ(元ファイルとバイト一致すれば、CRCの規則が正しいことの自己検証になる)。

### `dlm_mng.py` — `00110022.dlm`(マネージャ、HDD紐付け)

```bash
python3 scripts/dlm_mng.py decode  <00110022.dlm>                                  # 中身の表示とCRC検証
python3 scripts/dlm_mng.py build   --chipid <16桁hex> --rootfs-date "..." --knl-date "..." --rfs-date "..." -o out.dlm [--pad <16桁hex>]
python3 scripts/dlm_mng.py from-spi <spi.bin> <00550066.dlm> -o out.dlm            # SPIダンプとrootfsから自動で組み立てる
python3 scripts/dlm_mng.py selftest <00110022.dlm>                                  # decode → build で元とバイト一致するか
```

### `nasne_hdd_rebuild.py` — HDD復旧

[docs/04](04_hdd_recovery_guide.md) の手順用。`phase1` / `read-id` / `final` / `verify` の4つ。

### `build_knl.py` — SPIのKNLセグメント

```bash
python3 scripts/build_knl.py info    <spi.bin> [--slot KNL|BKNL]
python3 scripts/build_knl.py extract <spi.bin> <out_vmlinux.bin> [--slot ...]       # 復号+gunzipした生カーネル
python3 scripts/build_knl.py build   <spi.bin> <out_segment.bin> --slot KNL [--kernel vmlinux.bin] [--marker-from A --marker-to B]
python3 scripts/build_knl.py verify  <segment.bin>                                  # 4段目ブートと同じ手順で検証・展開
```

`--marker-from/--marker-to` は同じ長さのバイト列でカーネル中の文字列を置換する(動作確認用)。

### `spi_segments.py` / `spi_boot_decrypt.py` — SPIダンプの解析

```bash
python3 scripts/spi_segments.py <spi.bin> [出力dir]                     # BFWF/INFO/INF2/FMAP/KNL/BKNL を復号・抽出
python3 scripts/spi_boot_decrypt.py <spi.bin> <init(MIPS ELF)> <出力dir>   # U-Boot・4段目ブートの復号(XORパッド復元)
```

`spi_boot_decrypt.py` のパッド復元には、MIPS命令のバイト分布の参照として、公式ファームのminiroot `init`(MIPS ELF)が必要。

### `build_dlm.py` — 自作 `.dlm`

```bash
python3 scripts/build_dlm.py selftest <実物の00550066.dlm>                       # 検証ロジックの自己テスト
python3 scripts/build_dlm.py build --template <実物の00550066.dlm> --body rootfs.tar.gz --out custom.dlm
python3 scripts/build_dlm.py verify <file.dlm>
```

hwtype・日付などは**テンプレートの実ヘッダをそのまま使う**ので、そのファームが動いている本体向けにだけ使える。

## 環境構築

### `fetch_gpl_sources.sh` / `kernel_build/`

[docs/05](05_kernel_and_direct_boot.md) 参照。`NASNE_CUSTOM=1 sh scripts/kernel_build/build.sh` でカーネルをビルド(Docker必須)。

### `build_debian_rootfs.sh`

```bash
sudo SSH_PUBKEY=~/.ssh/id_rsa.pub ./scripts/build_debian_rootfs.sh <出力dir>      # 要 debootstrap, qemu-user-static
```

## 実機側のツール(MIPS用の静的バイナリ)

`scripts/tools/*.c` と `scripts/watchdog/mcui2c.c` は libc 不使用(素のsyscall)の小さなC。Docker の `nasne-kcc:gcc432`(docs/05)でビルドする:

```bash
docker run --rm -v "$PWD":/w -w /w nasne-kcc:gcc432 sh -c \
  'mipsel-linux-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -e __start -o i2cx scripts/tools/i2cx.c'
```

`mtdtool.c` と `physrd.c` だけは glibc の `syscall()` を使う動的リンクの小物なので、ホストのクロスコンパイラ(Debian/Ubuntu の `gcc-mipsel-linux-gnu`)で、標準スタートアップを使わずにビルドする(`/lib/ld.so.1` を要求する通常のMIPSバイナリになる):

```bash
mipsel-linux-gnu-gcc -O2 -nostartfiles -Wl,-e,_start -o mtdtool scripts/tools/mtdtool.c
mipsel-linux-gnu-gcc -O2 -nostartfiles -Wl,-e,_start -o physrd  scripts/tools/physrd.c
```

ドライバを使うツール(`mcui2c`、`i2cx`、`i2cscan`、`boxioctl`、`bcas`)は、公式ドライバ(`rc.xcode4`)がロード済みで `/dev/vixs/xcodedrv` があることが前提。

| ツール | 内容 |
|---|---|
| `watchdog/mcui2c` | MCU(バス0・`0x20`)の読み書き。`mcui2c w 20 20 01` がウォッチドッグ停止([docs/06](06_mcu_watchdog.md)) |
| `watchdog/nasne-mcu-wd` | 上記を起動時に自動実行するinitスクリプト |
| `tools/i2cscan` | `i2cscan <block 0\|1> [hz]`: I2Cブロックの全アドレスを走査して、応答するデバイスを一覧([docs/08](08_tuner_i2c.md)) |
| `tools/i2cx` | `i2cx <block> <addr8> r <n> \| w <b>... \| g <reg>... <n> \| d <reg0> <count> \| p <tuner8> <n> [pre...]`: I2Cの読み書き・ダンプ・パススルー読み出し |
| `tools/boxioctl` | `boxioctl <cmd_hex> <size_hex> [off=val]... [-d words]`: ドライバのioctlを汎用に試す。例 `boxioctl 103 b0 -d 0x2c`(ファーム状態)。[docs/07](07_driver_ioctl_bcas.md) |
| `tools/bcas` | `bcas status\|activate\|id\|deactivate ...` / `bcas m<hex>`: B-CASカードの実験([docs/07](07_driver_ioctl_bcas.md)) |
| `tools/mtdtool` | `mtdtool erase <dev> <off> <len>` / `write <dev> <off> <file>`: `/dev/mtd0` の許可範囲(KNL / BKNL / 未使用領域)だけを消去・書き込み([docs/05](05_kernel_and_direct_boot.md)) |
| `tools/physrd` | `physrd w <phys_hex> <nwords>` / `physrd r <phys_hex> <nbytes>`: `/dev/mem` 経由の読み出し専用(SoCレジスタ・RAM) |
| `tools/physio` | `physio r <phys> <nwords>` / `physio w <phys> <value>`: `/dev/mem` 経由の読み書き。**書き込みは危険** |

> ⚠️ I2C を `/dev/mem` のレジスタ直叩きで行ってはいけない(MCUのI2Cが固まり、電源サイクルが必要になる)。必ずドライバ経由のツールを使う。[docs/06](06_mcu_watchdog.md)

## Ghidra 補助スクリプト(`scripts/ghidra/`)

Ghidra の headless モード(`analyzeHeadless ... -process <プログラム> -noanalysis -scriptPath scripts/ghidra -postScript <スクリプト> <出力ファイル> <引数>...`)で使う:

| スクリプト | 引数 | 内容 |
|---|---|---|
| `DecompByString.java` | `<out> <文字列>...` | 指定の文字列を参照している関数をデコンパイル |
| `DecompAddr.java` | `<out> <アドレス>...` | 指定アドレスの関数をデコンパイル |
| `DecompRange.java` | `<out> <開始> <終了>` | アドレス範囲内の全関数をデコンパイル |
| `CallersDecomp.java` | `<out> <アドレス>...` | 指定関数の呼び出し元をデコンパイル |
| `StringRefs.java` | | 文字列の参照元の列挙 |

純正バイナリ(`init`、`procmng`、`dtvtuner`、`xcode4drv.ko` など)は、公式ファームのrootfsを `dlm_crypto.py decrypt-body` で取り出して `tar zxf` したものを対象にする。
