#!/usr/bin/env python3
"""SPI のA/B冗長スロット(KNL↔BKNL、RFS↔BRFS)のヘッダと中身を読んで、A と B を比べる。読むだけ(何も書かない)。
使い方: spi_ab_info.py <SPIダンプ(mtd0、13,631,488バイト)> [--extract <出力ディレクトリ>]
表示: 各セグメントの 検証結果(ローダと同じ CRC32)・ビルド番号・ビルド日時(復号ヘッダの文字列)・大きさ・本体のmd5。
      A と B で「先頭64バイトのヘッダ以外が同一か」。--extract で、復号した本体(KNL は展開後カーネル、RFS は gzip の cpio)を書き出す。
ヘッダ(復号後): [0:4]=名前 [4:8]=総長(BE) [8:12]=版(下位2バイトがビルド番号) [13]=0x10 なら gunzip、[14]=1 なら Blowfish、[20:60]=ビルド日時、[60:64]=CRC32
"""
import os, sys, hashlib, importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("build_knl", os.path.join(HERE, "build_knl.py"))
bk = importlib.util.module_from_spec(spec); spec.loader.exec_module(bk)

SEGS = [("KNL", 0x100000, 0x280000), ("BKNL", 0x380000, 0x280000), ("RFS", 0x600000, 0x380000), ("BRFS", 0x980000, 0x380000)]

def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__); return 0
    out = None
    if "--extract" in args:
        i = args.index("--extract"); out = args[i + 1]; del args[i:i + 2]
    spi = open(args[0], "rb").read()
    if len(spi) < 0xd00000:
        sys.exit("SPIダンプ(mtd0)が短い: %d バイト" % len(spi))
    info = {}
    for name, off, n in SEGS:
        seg = spi[off:off + n]
        r = bk.parse_segment(seg)
        h = r["hdr"]
        date = bytes(h[20:60]).split(b"\0")[0].decode("latin1")
        body = r.get("kernel") or r.get("body") or b""
        info[name] = dict(seg=seg, r=r, body=body)
        print(f"{name:5} 検証={'OK' if r['ok'] else 'NG ' + r['reason']}  総長={r['size']:#x}  ビルド番号={int.from_bytes(h[10:12], 'big')}  日時=\"{date}\"  CRC={r.get('crc_stored', 0):#010x}")
        print(f"      本体md5={hashlib.md5(body).hexdigest()}  ({'展開後カーネル' if 'kernel' in r else '本体'} {len(body)} バイト)")
        if out:
            os.makedirs(out, exist_ok=True)
            ext = "vmlinux" if "kernel" in r else "cpio.gz"
            open(os.path.join(out, f"{name}.{ext}"), "wb").write(body)
    print()
    for a, b in (("KNL", "BKNL"), ("RFS", "BRFS")):
        sa, sb = info[a]["seg"], info[b]["seg"]
        n = min(info[a]["r"]["size"], info[b]["r"]["size"])
        same_body = info[a]["body"] == info[b]["body"]
        diff_hdr = sum(1 for x, y in zip(sa[:64], sb[:64]) if x != y)
        diff_rest = sum(1 for x, y in zip(sa[64:n], sb[64:n]) if x != y)
        print(f"{a} と {b}: 本体{'は同一' if same_body else 'は違う'}、ヘッダの違うバイト {diff_hdr}/64、ヘッダ以降の違うバイト {diff_rest}"
              + ("(ヘッダ(名前・CRC)以外は同一)" if diff_rest == 0 and info[a]['r']['size'] == info[b]['r']['size'] else ""))
    return 0

if __name__ == "__main__":
    sys.exit(main())
