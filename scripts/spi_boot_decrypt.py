#!/usr/bin/env python3
"""SPIフラッシュ先頭のブートチェーン(BOOT内の1段目/U-Boot、BFWF=4段目)を復号する。

これらは暗号ではなく、**1024バイト周期の固定XORパッド**による
難読化(3領域で共有)であり、中身は平文のMIPSリトルエンディアンコードだった。

  0x010000-0x02d977  U-Boot 1.1.3 (Mar 5 2012) 1イメージ(0x1d978バイト、0x80000000リンク)。
                     "XCode_Viper#"。既定環境 bootcmd=bootm 81000000 / bootdelay=0
  0x050040-0x05b0af  BFWF = "Fourth Boot" の U-Boot legacy uImage(ヘッダ0x40+本体0xb030、
                     load=entry=0x80200000)。FMAP/KNL/BKNLを読みBlowfish+gunzipしてカーネルを起動


パッドは「3領域を同一位相(offset mod 1024)で重ね、バイトレーン(offset mod 4)ごとの
MIPS命令バイト分布(miniroot の init バイナリの.textから採取)に対する最尤」で復元する。
復元の正しさは、1段目ブートの16,344命令が不正ゼロで逆アセンブルできること、および
U-Boot/BFWF中の文字列が全レーンで読めることで検証済み。

使い方: spi_boot_decrypt.py <spi.bin> <init(MIPS ELF, 参照分布用)> <出力ディレクトリ>
"""
import os
import sys

import numpy as np

PERIOD = 1024
# (名前, 開始, 長さ, パッド位相オフセット)  BFWFは本体先頭(+0x40)からをパッド0位相とする
REGIONS = [
    ("uboot_image", 0x10000, 0x1d978, 0),
    ("fourth_boot_uimage", 0x50040, 0xb070, 0),
]


def recover_pad(data, ref_text):
    ref = np.frombuffer(ref_text, dtype=np.uint8)
    lane_logp = np.zeros((4, 256))
    for lane in range(4):
        h = np.bincount(ref[lane::4], minlength=256) + 1.0
        lane_logp[lane] = np.log(h / h.sum())
    hist = np.zeros((PERIOD, 256))
    for _, off, n, sh in REGIONS:
        # 統計はコード密度の高い先頭部分で十分(データ領域・BSS分は除外)
        a = np.frombuffer(data[off:off + min(n, 0x10000 + 0xd000 if off == 0x10000 else n)], dtype=np.uint8)
        for i, c in enumerate(a):
            hist[(i + sh) % PERIOD][c] += 1
    idx = np.arange(256)
    pad = np.zeros(PERIOD, dtype=np.uint8)
    for j in range(PERIOD):
        scores = [float((hist[j] * lane_logp[j % 4][idx ^ k]).sum()) for k in range(256)]
        pad[j] = int(np.argmax(scores))
    return bytes(pad)


def main():
    spi, init_path, outdir = sys.argv[1:4]
    data = open(spi, "rb").read()
    init = open(init_path, "rb").read()
    pad = recover_pad(data, init[0x170:0x170 + 0x105df0])
    os.makedirs(outdir, exist_ok=True)
    open(os.path.join(outdir, "xor_pad_1024.bin"), "wb").write(pad)
    for name, off, n, sh in REGIONS:
        enc = data[off:off + n]
        dec = bytes(c ^ pad[(i + sh) % PERIOD] for i, c in enumerate(enc))
        open(os.path.join(outdir, name + ".bin"), "wb").write(dec)
        print(f"{name}: {len(dec):#x} bytes written")
        if name == "fourth_boot_uimage":
            import struct
            import zlib
            magic, hcrc, _, size, load, ep, dcrc = struct.unpack(">IIIIIII", dec[:28])
            hh = bytearray(dec[:64])
            hh[4:8] = b"\0" * 4
            print(f"  uImage magic={magic:#x} load={load:#x} entry={ep:#x} size={size:#x}")
            print("  header crc32:", "OK" if zlib.crc32(bytes(hh)) & 0xFFFFFFFF == hcrc else "NG")
            print("  data   crc32:", "OK" if zlib.crc32(dec[64:64 + size]) & 0xFFFFFFFF == dcrc else "NG")


if __name__ == "__main__":
    main()
