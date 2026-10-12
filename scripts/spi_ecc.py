#!/usr/bin/env python3
"""nasne の SPI フラッシュの ECC 領域(0xd0000 / 0xe0000 / 0xf0000 の3コピー)の確認と再構築。

解読した仕様(実機のダンプ3種で検証済み):
  - ECC 領域(64KB、FMAPの "ECC")は、フラッシュ 0x010000〜0x08ffff(512KB: U-Boot、CER、4段目ブート(BFWF)、INFO、INF2、FMAP)の
    **Hamming(72,64) のパリティ**。データ8バイトにつきパリティ1バイト。
    ECC領域のオフセット X のバイトは、フラッシュ 0x10000 + 8*X からの8バイトのパリティ。
  - 8バイトは、先頭4バイトを a、次の4バイトを b とするリトルエンディアンの32ビット語。a・b を16ビットずつ4つに分け、
    各ビット i が立っているときに表 T[i] を XOR する(4つの表は同じもので、a下位 / a上位 / b下位 / b上位の順に16個ずつ、計64個)。
  - 表 T の 64 個は「ビット7が立っていて、1のビットが3個以上の値を、小さい順に64個」(0x83,0x85,0x86,0x87,0x89,...,0xc7)。
    (libcommlib.so の eccHammingEncode64 と同じ。逆アセンブルで確認)
  - 計算に使うのは、フラッシュ上の**生のバイト**(難読化された状態のまま)。消去済み(0xff)のデータのパリティは 0x7f、ゼロのデータは 0x00。
  - ECC領域は同じ内容が 0xd0000 / 0xe0000 / 0xf0000 に3つある。

ブートローダ類(0x10000〜0x8ffff)を書き換えるときは、ECC領域も再計算して書き込むこと(`build`)。

使い方:
  spi_ecc.py verify <spi.bin>                  ECCが一致するか調べる(不一致の範囲を、対応するフラッシュのアドレスで表示)
  spi_ecc.py build  <spi.bin> -o <out.bin>     ECC領域(3コピー)を計算し直して書き込んだイメージを作る
"""
import argparse
import sys

ECC_BASE = 0xD0000          # ECC領域(3コピー: +0x00000 / +0x10000 / +0x20000)
ECC_SIZE = 0x10000
ECC_COPIES = 3
DATA_BASE = 0x10000         # 保護されるデータの先頭
DATA_SIZE = ECC_SIZE * 8    # 0x80000(512KB) → 0x10000〜0x8ffff

# 表 T: ビット7が立っていて、1のビットが3個以上の値を、小さい順に64個
TABLE = [v for v in range(0x100) if (v & 0x80) and bin(v).count("1") >= 3][:64]
assert len(TABLE) == 64 and TABLE[0] == 0x83 and TABLE[-1] == 0xC7
T = [TABLE[0:16], TABLE[16:32], TABLE[32:48], TABLE[48:64]]


def parity8(chunk8):
    """8バイト → パリティ1バイト(eccHammingEncode64)"""
    a = int.from_bytes(chunk8[0:4], "little")
    b = int.from_bytes(chunk8[4:8], "little")
    x = 0
    for v, t in ((a & 0xFFFF, T[0]), (a >> 16, T[1]), (b & 0xFFFF, T[2]), (b >> 16, T[3])):
        for i in range(16):
            if v >> i & 1:
                x ^= t[i]
    return x


def compute(spi):
    return bytes(parity8(spi[DATA_BASE + 8 * x: DATA_BASE + 8 * x + 8]) for x in range(ECC_SIZE))


def cmd_verify(a):
    spi = open(a.image, "rb").read()
    if len(spi) < ECC_BASE + ECC_SIZE * ECC_COPIES:
        sys.exit("エラー: イメージが小さすぎる(0x100000 バイト以上が必要)")
    exp = compute(spi)
    rc = 0
    for k in range(ECC_COPIES):
        got = spi[ECC_BASE + k * ECC_SIZE: ECC_BASE + (k + 1) * ECC_SIZE]
        diff = [i for i in range(ECC_SIZE) if got[i] != exp[i]]
        if not diff:
            print(f"ECCコピー{k + 1} (0x{ECC_BASE + k * ECC_SIZE:06x}): 一致")
            continue
        rc = 1
        # 連続する不一致を、対応するフラッシュのアドレス範囲にまとめる
        rngs = []
        s = p = diff[0]
        for i in diff[1:]:
            if i != p + 1:
                rngs.append((s, p)); s = i
            p = i
        rngs.append((s, p))
        print(f"ECCコピー{k + 1} (0x{ECC_BASE + k * ECC_SIZE:06x}): 不一致 {len(diff)} バイト")
        for s, p in rngs[:10]:
            print(f"   ECC+0x{s:04x}〜0x{p:04x} ← フラッシュ 0x{DATA_BASE + 8 * s:06x}〜0x{DATA_BASE + 8 * p + 7:06x}")
    return rc


def cmd_build(a):
    spi = bytearray(open(a.image, "rb").read())
    ecc = compute(spi)
    for k in range(ECC_COPIES):
        spi[ECC_BASE + k * ECC_SIZE: ECC_BASE + (k + 1) * ECC_SIZE] = ecc
    open(a.out, "wb").write(spi)
    print(f"{a.out}: ECC領域(3コピー)を 0x{DATA_BASE:06x}〜0x{DATA_BASE + DATA_SIZE - 1:06x} から計算して書き込みました")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("verify"); p.add_argument("image")
    p = sub.add_parser("build"); p.add_argument("image"); p.add_argument("-o", "--out", required=True)
    a = ap.parse_args()
    sys.exit({"verify": cmd_verify, "build": cmd_build}[a.cmd](a) or 0)


if __name__ == "__main__":
    main()
