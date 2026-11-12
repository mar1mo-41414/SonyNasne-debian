#!/usr/bin/env python3
"""build_krst_package.py — 公式アップデート経路へ投げる KRST パッケージを組み立てる (PC 側、実機には触れない)。

公式パッケージ (KRST3101_xxxx_SECURE.dlm) は KNL ‖ RFS ‖ DLM の3セグメント連結で、
公式アップデートの適用段はこの連結を宣言サイズで辿って各セグメントを取り込む。
本ツールは、公式 KRST の KNL / RFS セグメント (バイトそのまま) と、自作の DLM
セグメントを連結して、投げられる形のパッケージにする。

自作 DLM セグメントは次のどちらかで用意する:
  --segment  既製の DLM セグメント単体ファイル (make_stage1_dlm.py build の出力など)
  --body     rootfs の tar.gz をボディとして、公式 DLM セグメントのヘッダを
             テンプレートに build_dlm.py でセグメントを組み立てる

どちらの入力でも、連結前に DLM ヘッダへ後加工を行う:
  +0x04 宣言サイズ = 実長 (適用段は宣言サイズで連結を辿るため、テンプレート値の
         ままでは破綻する。ビルダの出力はテンプレートのサイズが残ることがある)
  +0x08 major / +0x0A サブ = 指定した版 (版ゲート: 現行より新しい版でないと適用されない)
  +0x3C CRC32 を再計算する
hwtype (+0x10) と日付文字列 (+0x14) はテンプレート (公式セグメント) の値を継承する。

使い方:
  python3 build_krst_package.py --official KRST3101_0260_SECURE.dlm \
      --segment custom_00550066.dlm --major 3 --sub 61 --out custom_krst.dlm
  python3 build_krst_package.py --official KRST3101_0260_SECURE.dlm \
      --body rootfs.tar.gz --major 3 --sub 61 --out custom_krst.dlm

方式の全体像は docs/25_official_update_push.md を参照。
"""
import argparse
import hashlib
import io
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from dlm_crypto import NasneBlowfish  # noqa: E402
import build_dlm  # noqa: E402


def split_segments(data):
    """宣言サイズで連結を辿ってセグメント列を返す。各要素は (開始オフセット, 復号ヘッダ, 生バイト列)。"""
    out = []
    pos = 0
    while pos < len(data):
        h = NasneBlowfish().chain_decrypt(data[pos:pos + 64])
        size = int.from_bytes(h[4:8], "big")
        if size < 64 or pos + size > len(data):
            raise SystemExit(f"セグメント走査が破綻: offset={pos} 宣言サイズ={size} ファイル長={len(data)}")
        out.append((pos, h, data[pos:pos + size]))
        pos += size
    return out


def crc_ok(seg):
    h = bytearray(NasneBlowfish().chain_decrypt(seg[:64]))
    stored = struct.unpack(">I", bytes(h[0x3C:0x40]))[0]
    h[0x3C:0x40] = b"\0\0\0\0"
    return build_dlm.compute_crc(bytes(h), seg[64:]) == stored


def fixup_header(seg, major, sub):
    """DLM セグメントの +0x04 / +0x08 / +0x0A を設定し、CRC を再計算してヘッダを再暗号化する。"""
    h = bytearray(NasneBlowfish().chain_decrypt(seg[:64]))
    assert bytes(h[:4]) == b"DLM\0", "DLM セグメントではない"
    h[4:8] = len(seg).to_bytes(4, "big")
    h[8:10] = major.to_bytes(2, "big")
    h[10:12] = sub.to_bytes(2, "big")
    h[0x3C:0x40] = b"\0\0\0\0"
    crc = build_dlm.compute_crc(bytes(h), seg[64:])
    h[0x3C:0x40] = struct.pack(">I", crc)
    return NasneBlowfish().chain_encrypt(bytes(h)) + seg[64:]


def build_segment_from_body(official_krst_path, body_path, workdir):
    """公式 KRST の DLM セグメントをテンプレートに、tar.gz ボディから DLM セグメントを組み立てる。"""
    krst = open(official_krst_path, "rb").read()
    segs = split_segments(krst)
    dlm = [raw for _, h, raw in segs if bytes(h[:4]) == b"DLM\0"]
    if len(dlm) != 1:
        raise SystemExit("公式 KRST に DLM セグメントが1つ見つからない")
    template_path = os.path.join(workdir, "template_00550066.dlm")
    with open(template_path, "wb") as f:
        f.write(dlm[0])
    body = open(body_path, "rb").read()
    seg = build_dlm.build_custom_dlm(template_path, body, encrypt_body=False)
    ok, details = build_dlm.validate_dlm_bytes(seg)
    if not ok:
        raise SystemExit(f"組み立てた DLM セグメントの自己検証 NG: {details}")
    return seg


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--official", required=True, help="公式 KRST パッケージ (KNL / RFS とヘッダのテンプレート源)")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--segment", help="既製の DLM セグメント単体ファイル")
    src.add_argument("--body", help="rootfs の tar.gz (公式 DLM セグメントをテンプレートにセグメント化する)")
    ap.add_argument("--major", type=int, required=True, help="狙う版の major (+0x08)")
    ap.add_argument("--sub", type=int, required=True, help="狙う版のサブ (+0x0A、10進)")
    ap.add_argument("--out", required=True, help="出力する KRST パッケージ")
    ap.add_argument("--segment-out", help="後加工後の DLM セグメント単体も書き出す場合のパス")
    ap.add_argument("--workdir", default=None, help="--body 使用時の一時置き場 (既定は一時ディレクトリ)")
    a = ap.parse_args()

    krst = open(a.official, "rb").read()
    ksegs = split_segments(krst)
    assert [bytes(h[:4]) for _, h, _ in ksegs] == [b"KNL\0", b"RFS\0", b"DLM\0"], "公式 KRST の構成が想定と違う"
    print(f"公式 KRST: {len(krst)} バイト、セグメント " +
          " / ".join(f"{bytes(h[:4]).rstrip(bytes(1)).decode()}@{pos}+{len(raw)}" for pos, h, raw in ksegs))

    # 1. DLM セグメントを用意する
    if a.segment:
        seg = open(a.segment, "rb").read()
    else:
        import tempfile
        workdir = a.workdir or tempfile.mkdtemp(prefix="krst_pkg_")
        os.makedirs(workdir, exist_ok=True)
        seg = build_segment_from_body(a.official, a.body, workdir)
        print(f"ボディから DLM セグメントを組み立てた: {len(seg)} バイト")

    # 2. ヘッダ後加工 (サイズ・版・CRC)
    seg = fixup_header(seg, a.major, a.sub)
    if a.segment_out:
        open(a.segment_out, "wb").write(seg)
        print(f"DLM セグメント: {a.segment_out} ({len(seg)} バイト)")

    # 3. パッケージ化 (KNL / RFS は公式バイト列そのまま)
    pkg = krst[:ksegs[2][0]] + seg
    open(a.out, "wb").write(pkg)
    print(f"パッケージ: {a.out} ({len(pkg)} バイト、SHA-256 {hashlib.sha256(pkg).hexdigest()})")

    # 4. 検証
    print("--- 検証 ---")
    psegs = split_segments(pkg)
    assert len(psegs) == 3, "パッケージのセグメント数が3でない"
    for i, (pos, h, raw) in enumerate(psegs):
        name = bytes(h[:4]).rstrip(b"\0").decode()
        ok = crc_ok(raw)
        print(f"  [{i}] {name} @{pos} サイズ={len(raw)} 宣言={int.from_bytes(h[4:8], 'big')} "
              f"major={int.from_bytes(h[8:10], 'big')} サブ={int.from_bytes(h[10:12], 'big')} flag={h[0x0E]} CRC={'OK' if ok else 'NG'}")
        assert ok, f"セグメント {i} の CRC が不一致"
        assert int.from_bytes(h[4:8], "big") == len(raw), f"セグメント {i} の宣言サイズ != 実長"
    assert psegs[0][2] == ksegs[0][2], "KNL が公式とバイト一致しない"
    assert psegs[1][2] == ksegs[1][2], "RFS が公式とバイト一致しない"
    print("  KNL / RFS: 公式とバイト完全一致")
    dh = psegs[2][1]
    gh = ksegs[2][1]
    assert int.from_bytes(dh[8:10], "big") == a.major and int.from_bytes(dh[10:12], "big") == a.sub
    assert dh[0x10:0x14] == gh[0x10:0x14], "hwtype が公式と違う"
    assert dh[0x14:0x34] == gh[0x14:0x34], "日付文字列が公式と違う"
    print(f"  DLM ヘッダ: major={a.major} サブ={a.sub} (版表示 \"0{a.major}{a.sub:02d}\")、"
          f"hwtype・日付は公式と同一、flag={dh[0x0E]}")
    print("検証OK")


if __name__ == "__main__":
    main()
