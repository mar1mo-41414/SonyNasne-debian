# 05. 自前カーネルとDebian直起動の作り方

Sonyのユーザランド(sys2の再構築、`procmng` など)を一切通さず、**Debian wheezy の `sysvinit` を PID 1 として起動**する方法。
流れは次のとおり。

```
① GPLソースからカーネルをビルド(内蔵initramfs入り)
② SPIフラッシュの KNL 領域を自前カーネルに差し替える      ← SPI書き込みが必要(失敗しても書き戻せば元に戻る)
③ HDDの p3 に Debian wheezy (mipsel) を置く
④ 起動: 内蔵initramfs(/nasne_init)が p3 を root にして switch_root
```

⚠️ **SPIフラッシュを書き換える作業を含みます。** 事前に必ずSPIの完全ダンプを複数回取り、一致を確認してください。
HDDは、Debian用と、純正のまま残すものを別にすることを推奨します。

## ① GPLソースからカーネルをビルド

nasne には Linux / U-Boot / ViXS ドライバの GPL ソフトが入っており、SIE が公開している。

```bash
sh scripts/fetch_gpl_sources.sh     # gpl_src/ に取得(sha256検証つき)
```

| ファイル | 内容 |
|---|---|
| `mips-linux-2.6.29.tar.bz2` | Linux 2.6.29 の完全なソースツリー(`arch/mips/xcode/viper/`、実際の`.config`、`mkuImage*.sh` を含む) |
| `u-boot-1.1.3.tar.bz2` | U-Boot 1.1.3(ViXS改造) |
| `AVDriver7GSP47.zip` | `xcode4drv.ko` の全ソース + SDKヘッダ + ビルド済みの`xcode4drv.ko`/`rc.xcode4` |

(公開ページは旧URL `doc.dl.playstation.net/doc/nasne-oss/` にあったが、現在は個別ページが辿れない。ファイル本体は取得できる。)

ビルド環境は Docker 内に古いクロスコンパイラ(**GCC 4.3.2 + binutils 2.19.1**、ターゲット `mipsel-linux`)を作って使う。
Sonyのビルドは Timesys 製の GCC 4.3.2 だが、素の GCC 4.3.2 で問題なく通る。

```bash
# ツールチェーンのソースを scripts/kernel_build/dl/ に置く(GNUのFTPアーカイブから取得)
mkdir -p scripts/kernel_build/dl
wget -O scripts/kernel_build/dl/binutils-2.19.1.tar.bz2 https://ftp.gnu.org/gnu/binutils/binutils-2.19.1.tar.bz2
wget -O scripts/kernel_build/dl/gcc-core-4.3.2.tar.bz2  https://ftp.gnu.org/gnu/gcc/gcc-4.3.2/gcc-core-4.3.2.tar.bz2

docker build -t nasne-kcc:gcc432 scripts/kernel_build

mkdir -p gpl_src/build && tar xjf gpl_src/mips-linux-2.6.29.tar.bz2 -C gpl_src/build   # gpl_src/build/mips-linux-2.6.29 ができる
NASNE_CUSTOM=1 sh scripts/kernel_build/build.sh      # → gpl_src/build/out/vmlinux_new.bin
```

`NASNE_CUSTOM=1` を付けると、`build.sh` がソースに次の変更を入れる:

1. `init/main.c`: ramdisk の実行ファイルの既定を `/init` → `/nasne_init` に変更。
2. `.config`: `CONFIG_INITRAMFS_SOURCE` に `scripts/kernel_build/initramfs/initramfs.list`(`/nasne_init` を1つ入れる)を指定。
3. **`init/initramfs.c`: Sony のソースは内蔵initramfsの展開を `#if 0` で無効化している。これを `#if 1` に戻す**
   (これをしないと内蔵の `/nasne_init` が展開されず、通常の `root=/dev/sda2` 経路に落ちて固まる)。

付けない場合は純正と同じ構成(内蔵initramfsなし)になる。公開ソースは実機のカーネルとバイト同一ではない(サイズ差約0.2%)が、
**公式ファーム v1.00 / v2.60 のユーザランドを完全に動かせる**ことを確認している。

## ② SPIフラッシュの KNL を差し替える

### 準備: SPIの完全ダンプ(必須)

CH341A + SOICクリップ等で、`flashrom` で読み出す。**必ず2〜3回読んで一致を確認**してから進める。

```bash
flashrom -p ch341a_spi -c "S25FL128P......0" -r spi_dump1.bin
flashrom -p ch341a_spi -c "S25FL128P......0" -r spi_dump2.bin && cmp spi_dump1.bin spi_dump2.bin
```

(クリップは基板に付けたまま読めるが、読み出し中は本体の電源を入れないこと。)

### KNLセグメントを作る

`build_knl.py` が、4段目ブートと同じ検証(ヘッダの復号、CRC32、gzip、Blowfish)を通るセグメントを作る。自己検証つき。

```bash
python3 scripts/build_knl.py info   spi_dump1.bin --slot KNL            # 元のKNLの検証
python3 scripts/build_knl.py build  spi_dump1.bin KNL_new.bin --slot KNL --kernel gpl_src/build/out/vmlinux_new.bin
python3 scripts/build_knl.py verify KNL_new.bin
```

**A/Bの安全網**: KNL(スロットA)とBKNL(スロットB)があり、4段目ブートはAの読み込み/検証に失敗するとBに切り替える……はずだが、
**⚠️ 実機でフォールバックが機能しなかった事例を確認済み**([docs/13](13_full_setup_from_official_hdd.md)の事故)。
sys1上の古い段階1スクリプトの残骸が、KNL領域を`erase`した後、存在しないファイルの`write`に失敗して中途半端な状態(ほぼ消去されたまま)
になったケースで、別の(正常起動することが確認済みの)HDDに挿し替えても起動しなかった(=HDDの内容に関係なく、SPI側の問題として
起動不能になっていた)。Bへの自動切り替えで必ず助かる、とは**期待しないこと**。「CRCは正しいが起動途中で固まる」場合にフォールバックが
働かないことは元々分かっていたが、「CRC不一致(消去されたような内容)でも必ずしも助からない」ことも実機で確認された。
**SPIに書き込む作業をする前は、CH341A等で元のSPI全体を必ずダンプしておき、何かあれば直接書き戻せる状態を確保しておくこと**
(今回もこの方法で復旧できた)。書き換えるのはA(KNL)だけにしてB(BKNL)は純正のまま残す、という方針自体は変わらない
(フォールバックが効くこともあるかもしれないが、それに賭けない)。

### 書き込み

書き込みは、元のダンプのうち KNL 領域だけを置き換える:

```bash
cp spi_dump1.bin new_spi.bin
dd if=KNL_new.bin of=new_spi.bin bs=65536 seek=16 conv=notrunc
printf '0x00100000:0x0037ffff KNL\n' > layout.txt
flashrom -p ch341a_spi -c "S25FL128P......0" -l layout.txt -i KNL -N -w new_spi.bin
flashrom -p ch341a_spi -c "S25FL128P......0" -r verify.bin && cmp <(tail -c +$((0x100001)) verify.bin | head -c $(stat -c%s KNL_new.bin)) KNL_new.bin
```

> **CH341A の書き込みが止まることがある**: 一部のPCのUSBポートでは、CH341A(3.3Vを基板上の小型レギュレータで作る安価なタイプ)が
> 消去/書き込みを始めると `LIBUSB_TRANSFER_TIMED_OUT` で止まる(読み出しは安定)。別のPC(USB給電が違うLinuxサーバ)ではそのまま成功した。
> 対策は、別のPCで書く、またはアダプタのVCC-GND間にコンデンサを足す。

### すでにLinuxが動いているnasneからの書き込み

Linuxが動く状態(純正カーネル/自前カーネルで起動したnasne)では、`/dev/mtd0`(ブート領域13MB)からも書き換えられる。
`scripts/tools/mtdtool.c` は**許可範囲を固定**した消去/書き込みツール(KNL `0x100000-0x37ffff`、BKNL `0x380000-0x5fffff`、
未使用の `0xb0000-0xcffff` のみ。BOOT/BFWF/FMAP/INFO/INF2/ECC/RFSは拒否)。

```bash
mtdtool flash /dev/mtd0 0x100000 /tmp/KNL_new.bin    # 消去(64KB単位に切り上げ)→書き込み→読み戻し比較を一括
mtdtool verify /dev/mtd0 0x100000 /tmp/KNL_new.bin   # 比較だけ(一致なら終了コード0)
mtdtool dump /dev/mtd0 0 0xd00000 spi_backup.bin     # 読み出して保存(許可範囲の制限なし)
```

> `mtdtool` の拒否動作(許可範囲外は `REFUSED`)を試すときは、範囲外が確実なアドレス(例 `0x10000`)を使うこと。`0x100000` はKNLの許可範囲内なので**本当に消える**。

ウォッチドッグ([docs/06](06_mcu_watchdog.md))で約5.5分ごとにリセットされるので、再起動直後に作業を始めること。

**元に戻す**: ダンプのKNL(`0x100000`から)を同じ手順で書き戻す。

## ③ HDDのp3にDebianを置く

p3(user、XFS)を **ext3** で作り直し、Debian wheezy (mipsel) を置く。**p3の録画データは消える**。

```bash
sudo mkfs.ext3 -L debian /dev/sdX3                    # sdXは必ずlsblkで確認すること
sudo ./scripts/build_debian_rootfs.sh /tmp/debian-root     # debootstrap + qemu-user-static(要root)。SSH_PUBKEYでroot用公開鍵を指定
sudo mount /dev/sdX3 /mnt/p3 && sudo cp -a /tmp/debian-root/. /mnt/p3/
```

nasneのカーネルは古い(2.6.29)ため、**Debian 7 "wheezy" より新しいものは起動しない**。リポジトリは `archive.debian.org` にある。

p3に置く設定:

| ファイル | 内容 |
|---|---|
| `/etc/nasne-direct-boot` | 空ファイルでよい。**これがある区画だけ直起動する**(オプトイン。無ければ純正の起動に戻る) |
| `/etc/rc.local` | 起動成功の印として、起動カウンタを空にする: `: > /var/lib/nasne-boot-count` |
| `/etc/network/interfaces` | `allow-hotplug eth0` / `iface eth0 inet dhcp`(カーネルにEthernet(SynopGMAC)が組み込まれている) |
| sshd | OpenSSH 6.0。**RSA鍵のみ**(ed25519やrsa-sha2には非対応。クライアントは `-o PubkeyAcceptedAlgorithms=+ssh-rsa -o HostKeyAlgorithms=+ssh-rsa` が必要) |
| `/etc/inittab` | getty は `tty1`〜 を使う(シリアルコンソールは無い) |
| udev | **使わない**(udev 175 はカーネル2.6.29では起動しない。`/dev`は`nasne_init`が用意する) |
| MCUウォッチドッグ停止 | [docs/06](06_mcu_watchdog.md)。これが無いと約5.5分でリセットされる |

## ④ 起動の仕組み(`/nasne_init`)

カーネル内蔵のinitramfsの `/nasne_init`([scripts/kernel_build/initramfs/nasne_init](../scripts/kernel_build/initramfs/nasne_init))の流れ:

1. proc / sysfs をマウントし、`/dev/sda3` が現れるのを待つ(最大40秒)。標準入出力を `/dev/null` とログに向ける
   (カーネルが `/dev/console` を開けず fd 0-2 が閉じた状態で始まるため)。
2. `/dev/sda3` を ext3 で `/newroot` にマウント。
3. 次のどれかなら、**純正の `/init`(SPIのRFS、Sony miniroot)に `exec`** して従来の起動経路に戻る:
   sda3が無い/マウントできない、`/etc/nasne-direct-boot` が無い、**起動カウンタが2以上**
   (カウンタ `/var/lib/nasne-boot-count` は起動のたびに "x" を1つ足し、Debianの`rc.local`が成功時に空にする。直近2回が成功印なしで終わったらフォールバック)。
4. `/newroot/dev` に tmpfs をマウントし、`console null zero random tty0-6 ptmx loop*` と、`/sys` から `sda*`・`mtd*` のノードを作る
   (devtmpfsはカーネル2.6.32以降のため無い)。
5. `exec switch_root /newroot /sbin/init`。**Debianの `sysvinit` が真の PID 1** になる。

デバッグ用: p3に `/etc/nasne-init-debug`(中身は1行 `IP NETMASK GATEWAY`)があると、静的IPと**認証なしの** telnet を上げる。
起動直後に固まる原因調査用。普段は置かないこと。`/etc/nasne-init-hold` があると、`/go` というファイルができるまで待機する。

外部ramdisk(SPIのRFS = Sony miniroot)は内蔵cpioの後に展開されるので、`/nasne_init` の実行時点でフォールバック先の `/init` が存在する。
SPIのRFSはまだ読み込まれている(`switch_root` で捨てられる)。完全に外すには自前のRFSが必要(未実施)。

## 失敗したとき

| 症状 | 原因と対処 |
|---|---|
| 起動してもネットワークに出ない・LEDが消えたまま | `/nasne_init` まで届いていない可能性(内蔵initramfsが展開されていない = `#if 1` の修正漏れ、など)。電源サイクルし、SPIを元のKNLに書き戻す |
| 約5.5分ごとに勝手に再起動する | MCUウォッチドッグ未停止。[docs/06](06_mcu_watchdog.md) |
| 起動カウンタが2に達してSony経路に戻る | Debian側で `rc.local` が走っていない、またはDebianが起動していない。HDDをPCに繋いでログ `/var/log/nasne_init.log` を確認 |
| 純正の起動に戻したい | `/etc/nasne-direct-boot` を消す。あるいはSPIのKNLを元のダンプのものに戻す |
