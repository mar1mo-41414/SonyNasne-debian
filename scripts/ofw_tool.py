#!/usr/bin/env python3
"""公式FW(KRST3101_xxxx_SECURE.dlm)の分解と、rootfs `.dlm`のヘッダ書き換え(CRC再計算)ツール。

公式パッケージは3つのセグメントの連結: KNL(SPIのKNLに書く) ‖ RFS(miniroot) ‖ DLM(rootfs、HDD sys1の00550066.dlm)。
各セグメントは先頭64Bが独自Blowfishで暗号化されたヘッダ(`[4:8]`=セグメント総長BE)。HDDのバンクdir(11002200等)の
00220033.dlm=KNL、00440055.dlm=RFS、00550066.dlm=DLM に1対1で対応し、md5まで一致する。

  ofw_tool.py split <package.dlm> <outdir>             KNL.bin / RFS.bin / DLM.bin に分解して情報表示
  ofw_tool.py patch <rootfs.dlm> <out.dlm> [--major N] ヘッダ+8(BE16、メジャーバージョン)を書き換え、CRC32を再計算
                                                       (--major省略時は変更なし=再計算のみ。元と一致すれば自己検証になる)
"""
import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dlm_crypto import NasneBlowfish  # noqa: E402

CRC_OFF = 0x3C


def header_of(seg: bytes) -> bytearray:
    return bytearray(NasneBlowfish().chain_decrypt(seg[:64]))


def crc_of(hdr: bytes, body: bytes) -> int:
    h = bytearray(hdr)
    h[CRC_OFF:CRC_OFF + 4] = b"\0\0\0\0"
    return zlib.crc32(body, zlib.crc32(bytes(h))) & 0xFFFFFFFF


def split(data: bytes):
    pos, out = 0, []
    while pos < len(data):
        h = header_of(data[pos:pos + 64])
        size = int.from_bytes(h[4:8], "big")
        out.append((bytes(h[:4]).rstrip(b"\0").decode(), data[pos:pos + size], h))
        pos += size
    return out


def patch(seg: bytes, major=None) -> bytes:
    h = header_of(seg)
    size = int.from_bytes(h[4:8], "big")
    assert size == len(seg), f"セグメント長が合わない {size:#x} != {len(seg):#x}"
    body = seg[64:]
    if major is not None:
        h[8:10] = struct.pack(">H", major)
    h[CRC_OFF:CRC_OFF + 4] = struct.pack(">I", crc_of(h, body))
    return NasneBlowfish().chain_encrypt(bytes(h)) + body


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("split"); s.add_argument("pkg"); s.add_argument("outdir")
    p = sub.add_parser("patch"); p.add_argument("src"); p.add_argument("dst"); p.add_argument("--major", type=int)
    a = ap.parse_args()
    if a.cmd == "split":
        os.makedirs(a.outdir, exist_ok=True)
        for name, seg, h in split(open(a.pkg, "rb").read()):
            stored = struct.unpack(">I", bytes(h[CRC_OFF:CRC_OFF + 4]))[0]
            ok = crc_of(h, seg[64:]) == stored
            open(os.path.join(a.outdir, name + ".bin"), "wb").write(seg)
            print(f"{name:4s} size={len(seg):#x} +8(major)={int.from_bytes(h[8:10],'big')} flag={h[14]} "
                  f"hwtype={bytes(h[16:20]).hex()} date={bytes(h[20:52]).rstrip(b'\0').decode()!r} crc={'OK' if ok else 'NG'}")
    else:
        seg = open(a.src, "rb").read()
        out = patch(seg, a.major)
        open(a.dst, "wb").write(out)
        h = header_of(out)
        print(f"{a.dst}: size={len(out):#x} +8(major)={int.from_bytes(h[8:10],'big')} "
              f"identical-to-src={out == seg}")


if __name__ == "__main__":
    main()
