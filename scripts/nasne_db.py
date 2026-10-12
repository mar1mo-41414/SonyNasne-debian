#!/usr/bin/env python3
"""nasne の録画DB(p3 の 00000015/ 以下の *.dat)の復号・暗号化。標準ライブラリ + scripts/dlm_crypto.py のみ。

形式(2026-10-12 に解読。公式の dtvtuner を LD_PRELOAD で観測して確定):
  ファイル = [a17e4d13 (LE, マジック)] [len (LE32, 以降の本体のバイト数)] [crc32 (LE32)] [暗号文 (len-4 バイト)] [ゼロ詰め(固定サイズまで)]
  暗号文 = nasne独自Blowfish(.dlm と同じ鍵 f855b6e8df0ed0afd8fdcb49751f5c99、CBC風、IV=0、リトルエンディアン)で 8バイト単位に暗号化したもの。
           8の倍数に満たない末尾(1〜7バイト)は、固定の鍵ストリーム bd7d945c36a5e1 の先頭からXORしただけ。
  暗号化前の平文 t = gzip(XML) の「完全な16バイトブロック」の先頭4バイトを 5d5ad209 でXORしたもの
           (末尾の16バイトに満たないブロックにはXORしない)。
  crc32 は t(暗号化直前の平文)の標準CRC32(zlib.crc32)。
  gzip は mtime=0、XFL=0、OS=3(ヘッダ 1f8b0800 00000000 0003)。
  XML は libxml2 の出力そのまま(例: <?xml version="1.0" encoding="UTF-8" standalone="yes"?>\\n<pvr_db_manage format_version="1">...)。

使い方:
  nasne_db.py show <ファイル>             XML を表示(CRCの検算つき)
  nasne_db.py decode <ファイル> -o out.xml
  nasne_db.py encode <xml> -o <ファイル> [--pad 51200]   暗号化して書く(--pad: ゼロ詰めして固定サイズにする)
  nasne_db.py verify <ファイル>...         復号→再暗号化して一致するか確かめる(deflate のレベルが違えば不一致になりうる)
"""
import argparse
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from dlm_crypto import NasneBlowfish  # noqa: E402

MAGIC = bytes.fromhex("a17e4d13")
XOR4 = bytes.fromhex("5d5ad209")
TAIL_KS = bytes.fromhex("bd7d945c36a5e1")
GZ_HEADER = bytes.fromhex("1f8b0800000000000003")


def _xor_blocks(b):
    b = bytearray(b)
    for i in range(0, len(b) - 15, 16):
        for j in range(4):
            b[i + j] ^= XOR4[j]
    return bytes(b)


def decrypt_body(ct, bf=None):
    """暗号文 → t(XOR変換前の gzip ではなく、Blowfish に入る直前の平文)"""
    bf = bf or NasneBlowfish()
    n = len(ct) - len(ct) % 8
    t = bf.chain_decrypt(ct[:n]) + bytes(a ^ b for a, b in zip(ct[n:], TAIL_KS))
    return t


def encrypt_body(t, bf=None):
    bf = bf or NasneBlowfish()
    n = len(t) - len(t) % 8
    return bf.chain_encrypt(t[:n]) + bytes(a ^ b for a, b in zip(t[n:], TAIL_KS))


def parse(data):
    """ファイル全体 → (xml, crc_ok, 本体長)"""
    if data[:4] != MAGIC:
        raise ValueError("マジック(a17e4d13)が違う")
    ln = struct.unpack("<I", data[4:8])[0]
    crc = struct.unpack("<I", data[8:12])[0]
    ct = data[12:8 + ln]
    t = decrypt_body(ct)
    crc_ok = (zlib.crc32(t) & 0xFFFFFFFF) == crc
    gz = _xor_blocks(t)
    if gz[:10] != GZ_HEADER:
        raise ValueError("gzipヘッダが想定と違う: " + gz[:10].hex())
    do = zlib.decompressobj(-15)
    xml = do.decompress(gz[10:])
    return xml, crc_ok, ln


def build(xml, level=6, pad=0):
    co = zlib.compressobj(level, zlib.DEFLATED, -15)
    deflate = co.compress(xml) + co.flush()
    gz = GZ_HEADER + deflate + struct.pack("<II", zlib.crc32(xml) & 0xFFFFFFFF, len(xml))
    t = _xor_blocks(gz)
    ct = encrypt_body(t)
    out = MAGIC + struct.pack("<II", 4 + len(ct), zlib.crc32(t) & 0xFFFFFFFF) + ct
    if pad:
        if len(out) > pad:
            raise ValueError("pad より大きい")
        out += b"\0" * (pad - len(out))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("show"); p.add_argument("file")
    p = sub.add_parser("decode"); p.add_argument("file"); p.add_argument("-o", "--out", required=True)
    p = sub.add_parser("encode"); p.add_argument("xml"); p.add_argument("-o", "--out", required=True); p.add_argument("--pad", type=int, default=0); p.add_argument("--level", type=int, default=6)
    p = sub.add_parser("verify"); p.add_argument("files", nargs="+"); p.add_argument("--level", type=int, default=6)
    a = ap.parse_args()
    if a.cmd == "show":
        xml, ok, ln = parse(open(a.file, "rb").read())
        print(f"本体 {ln} バイト、CRC {'OK' if ok else 'NG'}")
        sys.stdout.write(xml.decode("utf-8"))
    elif a.cmd == "decode":
        xml, ok, ln = parse(open(a.file, "rb").read())
        open(a.out, "wb").write(xml)
        print(f"{a.out}: {len(xml)} バイト(CRC {'OK' if ok else 'NG'})")
    elif a.cmd == "encode":
        out = build(open(a.xml, "rb").read(), a.level, a.pad)
        open(a.out, "wb").write(out)
        print(f"{a.out}: {len(out)} バイト")
    else:
        bad = 0
        for f in a.files:
            raw = open(f, "rb").read()
            xml, ok, ln = parse(raw)
            re = build(xml, a.level)
            same = raw[:8 + ln] == re
            print(f"{f}: CRC {'OK' if ok else 'NG'}、再暗号化 {'一致' if same else '不一致'}")
            bad += (not ok) or (not same)
        sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
