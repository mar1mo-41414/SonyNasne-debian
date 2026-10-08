#!/usr/bin/env python3
"""nasne の `/disk0/00110022.dlm`(マネージャファイル、148バイト)の解析・生成・検証ツール。

形式(miniroot `init` の seiCryptEncryptSave/seiCryptDecryptLoad、FUN_0040307c/FUN_00402a24 の逆コンパイルで確定):
    [0:4]      CRC32(平文0x90バイト、zlib.crc32、リトルエンディアンでfwrite)
    [4:148]    平文0x90バイトを .dlm と同じ独自Blowfish-CBC(scripts/dlm_crypto.py、IV=0)で暗号化したもの
平文0x90バイトの中身(init が実際に見るもの):
    [0x00:0x08]  チップ個体ID(SPI INF2 の本体+0x60 の8バイト = ロード済みINF2バッファの+0xa0)。HDDとnasne本体の「紐付け」
    [0x08:0x10]  不明(既存ファイルでは 85a6f2154471f4c4。initは読まない)
    [0x10:0x30]  00550066.dlm(rootfs)ヘッダ +0x14 の32バイト日付文字列
    [0x30:0x50]  SPI KNL セグメントヘッダ +0x14 の日付文字列(32B)
    [0x50:0x70]  SPI RFS セグメントヘッダ +0x14 の日付文字列(32B)
    [0x70:0x90]  不明(既存ファイルでは上と同じ8バイトの4回繰り返し)

使い方:
  dlm_mng.py decode  <00110022.dlm>                     中身を表示しCRCを検証
  dlm_mng.py build   --chipid <16hex> --rootfs-date "..." --knl-date "..." --rfs-date "..." -o out.dlm [--pad <16hex>]
  dlm_mng.py from-spi <spi.bin> <00550066.dlm> -o out.dlm  SPIダンプ(INF2/KNL/RFS)とrootfs.dlmから自動で組み立てる
  dlm_mng.py selftest <00110022.dlm>                    decode→buildで元とバイト一致するか確認
"""
import argparse
import os
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dlm_crypto import NasneBlowfish  # noqa: E402

DEFAULT_PAD = bytes.fromhex("85a6f2154471f4c4")
SIZE = 0x90


def parse(data: bytes) -> dict:
    if len(data) != 4 + SIZE:
        raise ValueError(f"サイズが{4 + SIZE}ではない: {len(data)}")
    bf = NasneBlowfish()
    crc = int.from_bytes(data[:4], "little")
    pt = bf.chain_decrypt(data[4:])
    calc = zlib.crc32(pt) & 0xFFFFFFFF
    s = lambda a, b: pt[a:b].split(b"\0", 1)[0].decode("latin1")  # noqa: E731
    return {
        "crc_ok": crc == calc, "crc": crc, "crc_calc": calc, "plain": pt,
        "chipid": pt[0:8], "pad8": pt[8:16], "rootfs_date": s(0x10, 0x30),
        "knl_date": s(0x30, 0x50), "rfs_date": s(0x50, 0x70), "tail": pt[0x70:0x90],
    }


def build(chipid: bytes, rootfs_date: str, knl_date: str, rfs_date: str, pad: bytes = DEFAULT_PAD) -> bytes:
    assert len(chipid) == 8 and len(pad) == 8
    pt = bytearray(SIZE)
    pt[0:8] = chipid
    pt[8:16] = pad
    for off, txt in ((0x10, rootfs_date), (0x30, knl_date), (0x50, rfs_date)):
        b = txt.encode("latin1")
        assert len(b) < 32
        pt[off:off + len(b)] = b
    pt[0x70:0x90] = pad * 4
    crc = zlib.crc32(bytes(pt)) & 0xFFFFFFFF
    return crc.to_bytes(4, "little") + NasneBlowfish().chain_encrypt(bytes(pt))


def seg_date(seg: bytes) -> str:
    h = NasneBlowfish().chain_decrypt(seg[:64])
    return h[0x14:0x34].split(b"\0", 1)[0].decode("latin1")


def from_spi(spi: bytes, rootfs_dlm: bytes) -> bytes:
    bf = NasneBlowfish()
    inf2 = spi[0x70000:0x70000 + 0x1000]
    h = bf.chain_decrypt(inf2[:64])
    size = int.from_bytes(h[4:8], "big")
    body = bf.chain_decrypt(inf2[64:min(size, 0x1000)][:(min(size, 0x1000) - 64) // 8 * 8]) if h[14] == 1 else inf2[64:]
    chipid = (h + body)[0xA0:0xA8]
    return build(chipid, seg_date(rootfs_dlm), seg_date(spi[0x100000:0x100040]), seg_date(spi[0x600000:0x600040]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("decode"); p.add_argument("file")
    p = sub.add_parser("selftest"); p.add_argument("file")
    p = sub.add_parser("build")
    p.add_argument("--chipid", required=True); p.add_argument("--rootfs-date", required=True)
    p.add_argument("--knl-date", required=True); p.add_argument("--rfs-date", required=True)
    p.add_argument("--pad", default=DEFAULT_PAD.hex()); p.add_argument("-o", required=True)
    p = sub.add_parser("from-spi")
    p.add_argument("spi"); p.add_argument("rootfs_dlm"); p.add_argument("-o", required=True)
    a = ap.parse_args()

    if a.cmd == "decode":
        r = parse(open(a.file, "rb").read())
        print(f"CRC32 {'OK' if r['crc_ok'] else 'NG'} (file {r['crc']:#010x} / calc {r['crc_calc']:#010x})")
        print(f"chipid      {r['chipid'].hex()}\npad8        {r['pad8'].hex()}\ntail        {r['tail'].hex()}")
        print(f"rootfs date {r['rootfs_date']!r}\nKNL date    {r['knl_date']!r}\nRFS date    {r['rfs_date']!r}")
    elif a.cmd == "selftest":
        orig = open(a.file, "rb").read()
        r = parse(orig)
        new = build(r["chipid"], r["rootfs_date"], r["knl_date"], r["rfs_date"], r["pad8"])
        print("再生成 == 元ファイル:", new == orig)
        sys.exit(0 if new == orig else 1)
    elif a.cmd == "build":
        out = build(bytes.fromhex(a.chipid), a.rootfs_date, a.knl_date, a.rfs_date, bytes.fromhex(a.pad))
        open(a.o, "wb").write(out)
        print(f"{a.o}: {len(out)} バイト")
    elif a.cmd == "from-spi":
        spi = open(a.spi, "rb").read()
        rootfs = open(a.rootfs_dlm, "rb").read(64)
        out = from_spi(spi, rootfs)
        open(a.o, "wb").write(out)
        print(f"{a.o}: {len(out)} バイト")


if __name__ == "__main__":
    main()
