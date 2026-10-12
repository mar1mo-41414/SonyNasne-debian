# 16. SPIフラッシュを「読むだけ」でバックアップする `.dlm`

> 基板のSPIフラッシュ(ブートローダ・カーネル・RFS・NV領域)を、**書き込まず・Debianも入れず**、nasne自身に読み出させてHDDに置く方法です。
> はんだ付けやSPIライタ(CH341A等)は要りません。[docs/13](13_full_setup_from_official_hdd.md) のようにSPIを書き換える前の保険や、ブートローダ類の保存に使えます。
> 公式HDDの `00550066.dlm` を1ファイル差し替える仕組みは docs/13 と同じです。自己責任でお願いします。

## 何をするか

`make_stage1_dlm.py --mode backup` は、公式の `rcS` の最後(公式アプリを起動する直前)に、次の処理だけを挿し込んだ `.dlm` を作ります。

1. `/dev/mtd0`(SPIの先頭13MB: ブートローダ・カーネル・RFSなど)と `/dev/mtd1`(NV領域、3MB弱)を `dd` で**読むだけ**。書き込み・消去はしません。
   `mtd0` は2回読んで一致を確認します(`mtd1` は稼働中のjffs2なので2回目の比較は参考)。
2. 結果を、HDDの **sys1(既定)** または p3 の `/spi_backup/` に置きます(`--dest sys1|p3|both`)。
   `spi_mtd0.bin` / `spi_mtd1_nv.bin` / `MD5SUMS` / `SHA1SUMS` / `README.txt` / `backup.log`
3. そのあと、**公式アプリを普通に起動**します。

安全のため: すでに `/spi_backup/spi_mtd0.bin` があれば何もしません(SPIの内容が変わっていても上書きしません。取り直すときはそのディレクトリを消します)。
サイズ不一致・空き不足・マウント失敗などの場合は、バックアップを飛ばして公式の起動へ進みます。

## 手順

```bash
# 1. そのHDDの 00550066.dlm から、バックアップ版を作る(PC側。root不要)
python3 scripts/stage1/make_stage1_dlm.py build --official <HDDのsys1にある00550066.dlm> --out spi_backup.dlm --mode backup
# 2. HDDをPCにつなぎ、sys1をマウントして差し替える(元のファイルは退避される)
sudo mount /dev/sdX1 /mnt/nasne_sys1
sudo bash scripts/stage1/install_dlm.sh /mnt/nasne_sys1 spi_backup.dlm --backup-dir ./backup_sys1
sudo umount /mnt/nasne_sys1
```

3. HDDをnasneに挿して電源を入れる。**起動は約80秒遅れます**(読み出し中。基板のウォッチドッグ333秒には収まります)。公式アプリが起動したら完了です。
4. 電源を切り、HDDをPCに戻して `sys1` の `/spi_backup/` を取り出す。**そのあと `00550066.dlm` を元に戻してください**(`--backup-dir` に退避された `00550066.dlm.orig` を、同じ `install_dlm.sh` で書き戻す)。
   戻さなくても、次回からは「作成済み」でスキップするだけで害はありません。

`install_dlm.sh` は、新旧のヘッダ(ハードウェア種別・日付)が一致することを確認します。**`--official` には、そのHDD自身の `00550066.dlm` を渡してください**(別のバージョンのものだと拒否されます)。

## 注意・限界

- SPIの最後の64KB(`0xff0000` 以降)は、MTDとして見えないため含まれません。
- NV領域(`spi_mtd1_nv.bin`)は稼働中のファイルシステムを生で読むので、ファイルとしての整合は保証しません(設定の控えとして)。
- 公式の `busybox`(1.13.3)に `sha256sum` が無いため、同梱のハッシュは SHA-1 と MD5 です。PCで `sha256sum` を取っておくと確実です。
- 実機1台(公式 v2.60)で確認: `mtd0` を2回読んで一致し、CH341Aで取った原本ダンプと比べて、**カーネル(KNL)スロット以外は完全に一致**しました
  (そのときのKNLスロットは自作カーネルに替えてあったため。無改造の機体なら全域が一致するはずです)。
- テスト: `scripts/stage1/test_backup_sim.sh`(MTD・マウントを模擬ファイルに差し替えた論理テスト。保存・2回目のスキップ・失敗時の起動継続など12件)。
