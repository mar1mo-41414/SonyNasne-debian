#!/usr/bin/env python3
"""nasneのKNL/BKNLセグメント(SPIフラッシュ内カーネル)の解析・再構築ツール。

4段目ブート("Fourth Boot", docs/02_boot_chain.md)のローダが行う検証を忠実に再現する:
  1. 先頭64バイトを独自Blowfish-CBC(IV=0、既知鍵)で復号 -> magic("KNL"/"BKNL")、size(BE, +4)
  2. ヘッダの[60:64](BE)をCRCとして取り出し、そこを0にして
        crc32(復号ヘッダ || 暗号化されたままの本体)  ( zlib.crc32、長さ=size )
     と比較(KNL/BKNL/RFS/BRFSの4セグメントで一致確認済み)
  3. [14]==1 なら本体(8の倍数分)をBlowfish-CBC復号
  4. [13]==0x10 なら gunzip して物理0x10000000(KSEG0 0x90000000)へ展開(上限8MB)。署名検証は無い

サブコマンド:
  info   <spi.bin> [--slot KNL|BKNL]               元セグメントの検証結果を表示
  extract<spi.bin> <out_vmlinux.bin> [--slot ..]   復号+gunzipした生カーネルを書き出す
  build  <spi.bin> <out_segment.bin> --slot KNL|BKNL [--kernel vmlinux.bin] [--marker-from A --marker-to B]
                                                   セグメントを再構築(--kernel省略時は元カーネル、
                                                   --marker-*は同一長のバイト列置換でカーネル内文字列を差し替え)
  verify <segment.bin>                             セグメント単体をローダと同じ手順で検証・展開テスト
"""
import argparse
import gzip
import io
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dlm_crypto import NasneBlowfish  # noqa: E402

SLOT_OFFSET = {"KNL": 0x100000, "BKNL": 0x380000}
SLOT_SIZE = 0x280000  # FMAPで確認したパーティション長
MAX_KERNEL = 0x800000  # 4段目のgunzip上限(0x800000)


def gz_header_len(gz: bytes) -> int:
    """gzipヘッダ長(FEXTRA/FNAME/FCOMMENT/FHCRC対応)。"""
    assert gz[:3] == b"\x1f\x8b\x08", "gzipではない"
    flg = gz[3]
    i = 10
    if flg & 4:
        i += 2 + int.from_bytes(gz[i:i + 2], "little")
    if flg & 8:
        i = gz.index(b"\0", i) + 1
    if flg & 16:
        i = gz.index(b"\0", i) + 1
    if flg & 2:
        i += 2
    return i


def gunzip_lenient(gz: bytes, limit: int = MAX_KERNEL + 1) -> bytes:
    """ローダ同様にトレーラ(CRC32/ISIZE)を見ず、deflateの終端までを展開する。"""
    d = zlib.decompressobj(-15)
    return d.decompress(gz[gz_header_len(gz):], limit)


def parse_segment(seg: bytes):
    """ローダ相当の検証。戻り値 dict(ok, reason, magic, size, hdr, gz, ...)"""
    bf = NasneBlowfish()
    hdr = bytearray(bf.chain_decrypt(seg[:64]))
    magic = bytes(hdr[:4]).rstrip(b"\0")
    size = int.from_bytes(hdr[4:8], "big")
    r = {"magic": magic, "size": size, "hdr": bytes(hdr), "ok": False, "reason": ""}
    if size < 64 or size > len(seg):
        r["reason"] = f"size {size:#x} が範囲外(seg長 {len(seg):#x})"
        return r
    stored = bytes(hdr[60:64])
    chk = bytes(hdr[:60]) + b"\0\0\0\0"
    crc = zlib.crc32(chk + seg[64:size]) & 0xFFFFFFFF
    r["crc_stored"] = int.from_bytes(stored, "big")
    r["crc_calc"] = crc
    if crc != r["crc_stored"]:
        r["reason"] = f"CRC32不一致 stored={r['crc_stored']:#010x} calc={crc:#010x}"
        return r
    body = seg[64:size]
    if hdr[14] == 1:
        n = len(body) // 8 * 8
        body = bf.chain_decrypt(body[:n]) + body[n:]
    r["body"] = body
    if hdr[13] == 0x10:
        try:
            k = gunzip_lenient(body)
        except Exception as e:  # noqa: BLE001
            r["reason"] = f"gunzip失敗: {e}"
            return r
        if len(k) > MAX_KERNEL:
            r["reason"] = "展開後が8MB超"
            return r
        r["kernel"] = k
    r["ok"] = True
    return r


def read_slot(spi: bytes, slot: str) -> bytes:
    off = SLOT_OFFSET[slot]
    h = NasneBlowfish().chain_decrypt(spi[off:off + 64])
    size = int.from_bytes(h[4:8], "big")
    return spi[off:off + size]


def build_segment(template_seg: bytes, kernel: bytes, name: str) -> bytes:
    """元セグメントのヘッダ雛形(version/flag/hwtype/日付等)を流用し、kernelを格納した新セグメントを作る。"""
    bf = NasneBlowfish()
    hdr = bytearray(bf.chain_decrypt(template_seg[:64]))
    assert hdr[14] == 1 and hdr[13] == 0x10, "雛形がKNL形式(Blowfish+gzip)ではない"
    # 元セグメントのgzipヘッダ(FNAME付き)を流用し、deflate本体とトレーラを自前で組む
    orig = parse_segment(template_seg)
    assert orig["ok"], orig["reason"]
    hlen = gz_header_len(orig["body"])
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    gz = orig["body"][:hlen] + co.compress(kernel) + co.flush()
    gz += struct.pack("<II", zlib.crc32(kernel) & 0xFFFFFFFF, len(kernel) & 0xFFFFFFFF)
    gz += b"\0" * ((-len(gz)) % 8)  # 8の倍数にそろえる(ローダは8の倍数分のみ復号するため)
    body = bf.chain_encrypt(gz)
    size = 64 + len(body)
    if size > SLOT_SIZE:
        raise SystemExit(f"セグメント {size:#x} がパーティション {SLOT_SIZE:#x} を超える")
    hdr[:4] = name.encode().ljust(4, b"\0")
    hdr[4:8] = size.to_bytes(4, "big")
    hdr[60:64] = b"\0\0\0\0"
    crc = zlib.crc32(bytes(hdr) + body) & 0xFFFFFFFF
    hdr[60:64] = crc.to_bytes(4, "big")
    return bf.chain_encrypt(bytes(hdr)) + body


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for c in ("info", "extract", "build"):
        s = sub.add_parser(c)
        s.add_argument("spi")
        if c in ("extract", "build"):
            s.add_argument("out")
        s.add_argument("--slot", default="KNL", choices=["KNL", "BKNL"])
        if c == "build":
            s.add_argument("--kernel")
            s.add_argument("--marker-from")
            s.add_argument("--marker-to")
    v = sub.add_parser("verify")
    v.add_argument("segment")
    a = ap.parse_args()

    if a.cmd == "verify":
        seg = open(a.segment, "rb").read()
        r = parse_segment(seg)
        print(f"magic={r['magic']!r} size={r['size']:#x} ok={r['ok']} {r['reason']}")
        if r["ok"]:
            print(f"kernel展開後 {len(r['kernel']):#x} バイト  先頭={r['kernel'][:8].hex()}")
        sys.exit(0 if r["ok"] else 1)

    spi = open(a.spi, "rb").read()
    seg = read_slot(spi, a.slot)
    r = parse_segment(seg)
    if a.cmd == "info":
        print(f"{a.slot}: magic={r['magic']!r} size={r['size']:#x} ok={r['ok']} {r['reason']}")
        if r["ok"]:
            print(f"  CRC32 {r['crc_calc']:#010x} / 展開後カーネル {len(r['kernel']):#x} バイト")
        return
    if a.cmd == "extract":
        assert r["ok"], r["reason"]
        open(a.out, "wb").write(r["kernel"])
        print(f"{len(r['kernel']):#x} バイトを書き出し")
        return
    # build
    assert r["ok"], r["reason"]
    kernel = open(a.kernel, "rb").read() if a.kernel else r["kernel"]
    if a.marker_from:
        old = a.marker_from.encode()
        new = a.marker_to.encode()
        assert len(old) == len(new), "marker-from/to は同一長のバイト列にすること"
        n = kernel.count(old)
        assert n >= 1, f"{a.marker_from!r} がカーネル内に見つからない"
        kernel = kernel.replace(old, new)
        print(f"マーカー置換: {a.marker_from!r} -> {a.marker_to!r} ({n}箇所)")
    new_seg = build_segment(seg, kernel, a.slot)
    chk = parse_segment(new_seg)
    assert chk["ok"] and chk["kernel"] == kernel, "再構築セグメントの自己検証に失敗: " + chk["reason"]
    open(a.out, "wb").write(new_seg)
    print(f"{a.slot} セグメント {len(new_seg):#x} バイトを書き出し(自己検証OK, CRC32 {chk['crc_calc']:#010x})")


if __name__ == "__main__":
    main()
