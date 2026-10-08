#!/usr/bin/env python3
"""SPIフラッシュ(S25FL128P 16MB)内の名前付きセグメントを、.dlmと同じ64バイト
Blowfishヘッダ(既知鍵)で列挙・抽出する。

先頭1MBは「一枚岩の暗号文」ではなく、FMAP(パーティションテーブル)が指す複数の
セグメントで構成されていた。64KB境界ごとにヘッダを復号して
マジック(BFWF/INFO/INF2/FMAP/KNL)を確認し、flag=1のボディはBlowfish-CBCで復号する。

使い方: spi_segments.py <spi.bin> [出力ディレクトリ]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dlm_crypto import NasneBlowfish  # noqa: E402

# FMAPに載っている名前付きセグメント(ヘッダ有り)の既知オフセット
SEGMENTS = [
    (0x50000, "BFWF"), (0x60000, "INFO"), (0x70000, "INF2"),
    (0x80000, "FMAP"), (0x100000, "KNL"), (0x380000, "BKNL"),
]


def main():
    path = sys.argv[1]
    outdir = sys.argv[2] if len(sys.argv) > 2 else None
    data = open(path, "rb").read()
    bf = NasneBlowfish()
    for off, name in SEGMENTS:
        h = bf.chain_decrypt(data[off:off + 64])
        magic = h[:4].rstrip(b"\0").decode("ascii", "replace")
        size = int.from_bytes(h[4:8], "big")
        flag = h[14]
        date = h[20:52].rstrip(b"\0").decode("ascii", "replace")
        print(f"{name:5} @0x{off:06x} magic={magic!r} size=0x{size:x} flag={flag} date={date!r}")
        if outdir and magic == name[:len(magic)] and size >= 64:
            body = data[off + 64:off + size]
            if flag == 1:
                n = len(body) // 8 * 8
                body = bf.chain_decrypt(body[:n]) + body[n:]
            os.makedirs(outdir, exist_ok=True)
            open(os.path.join(outdir, f"{name}.bin"), "wb").write(body)


if __name__ == "__main__":
    main()
