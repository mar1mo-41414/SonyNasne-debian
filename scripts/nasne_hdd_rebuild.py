#!/usr/bin/env python3
"""nasne の HDD(sys1)を公式ファーム(KRST3101_xxxx_SECURE.dlm)からファイル操作だけで作る補助ツール。
SPIの吸い出し等は不要。手順は docs/04_hdd_recovery_guide.md。Python 3.8+ のみ必要(外部ライブラリ不要)。

  phase1 <公式.dlm> <sys1ディレクトリ>        ステップ1用。00550066.dlmを「ヘッダ+8=0」版にして置き、00110022.dlmを消す。
                                                nasneを1回起動すると、本体のinitがこの本体の個体IDで00110022.dlmを自動生成する
  read-id <sys1ディレクトリ または 00110022.dlm> 自動生成された00110022.dlmから個体ID(16桁の16進)を表示する
  final <公式.dlm> <sys1ディレクトリ> --chipid <16桁hex>
                                                ステップ2用。完全なsys1(00550066.dlm / 00110022.dlm / 11002200 / 33004400)を作る
  verify <sys1ディレクトリ>                     sys1の内容をinitと同じ規則で検査する(CRC・日付の整合など)
"""
import argparse
import os
import shutil
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dlm_mng  # noqa: E402
import ofw_tool  # noqa: E402
from dlm_crypto import NasneBlowfish  # noqa: E402


def load_package(path):
    segs = {name: (seg, h) for name, seg, h in ofw_tool.split(open(path, "rb").read())}
    for need in ("KNL", "RFS", "DLM"):
        if need not in segs:
            sys.exit(f"エラー: {path} に {need} セグメントが無い。公式の KRST3101_xxxx_SECURE.dlm か確認してください")
    return segs


def seg_date(h) -> str:
    return bytes(h[0x14:0x34]).split(b"\0", 1)[0].decode("latin1")


def cmd_phase1(a):
    segs = load_package(a.package)
    dlm, _ = segs["DLM"]
    patched = ofw_tool.patch(dlm, 0)
    os.makedirs(a.sys1, exist_ok=True)
    open(os.path.join(a.sys1, "00550066.dlm"), "wb").write(patched)
    mng = os.path.join(a.sys1, "00110022.dlm")
    if os.path.exists(mng):
        os.remove(mng)
    print(f"{a.sys1}/00550066.dlm : ヘッダ+8=0版を書き込みました({len(patched)} バイト)")
    print(f"{a.sys1}/00110022.dlm : 無し(nasneが初回起動で自動生成します)")
    print("次: HDDをnasneに挿して電源を入れ、数分待って電源を切り、HDDをPCに戻して read-id を実行")


def cmd_read_id(a):
    p = a.path
    if os.path.isdir(p):
        p = os.path.join(p, "00110022.dlm")
    if not os.path.exists(p):
        sys.exit("エラー: 00110022.dlm が無い。nasneが起動できていない(initが走っていない)可能性があります")
    data = open(p, "rb").read()
    try:
        r = dlm_mng.parse(data)
    except ValueError as e:
        sys.exit(f"エラー: {e}")
    if not r["crc_ok"]:
        sys.exit("エラー: CRCが合わない。ファイルが壊れています")
    cid = r["chipid"].hex()
    print(f"個体ID: {cid}")
    if cid == "0" * 16:
        print("注意: IDがすべてゼロです。この本体のINF2が空の可能性があります")
    print(f"(rootfs日付 {r['rootfs_date']!r})")


def cmd_final(a):
    chipid = bytes.fromhex(a.chipid)
    if len(chipid) != 8:
        sys.exit("エラー: --chipid は16桁の16進数(8バイト)")
    segs = load_package(a.package)
    knl, hk = segs["KNL"]
    rfs, hr = segs["RFS"]
    dlm, hd = segs["DLM"]
    mng = dlm_mng.build(chipid, seg_date(hd), seg_date(hk), seg_date(hr))
    os.makedirs(a.sys1, exist_ok=True)
    open(os.path.join(a.sys1, "00550066.dlm"), "wb").write(dlm)
    open(os.path.join(a.sys1, "00110022.dlm"), "wb").write(mng)
    for bank in ("11002200", "33004400"):
        d = os.path.join(a.sys1, bank)
        os.makedirs(d, exist_ok=True)
        open(os.path.join(d, "00110022.dlm"), "wb").write(mng)
        open(os.path.join(d, "00220033.dlm"), "wb").write(knl)
        open(os.path.join(d, "00440055.dlm"), "wb").write(rfs)
        open(os.path.join(d, "00550066.dlm"), "wb").write(dlm)
    print(f"{a.sys1} に完全なsys1を作りました(個体ID {a.chipid}、rootfs日付 {seg_date(hd)!r})")
    print("次: HDDをアンマウントしてnasneに挿し、電源を入れる")


def hdr(path):
    return ofw_tool.header_of(open(path, "rb").read(64))


def cmd_verify(a):
    ok = True
    top = os.path.join(a.sys1, "00110022.dlm")
    rootfs = os.path.join(a.sys1, "00550066.dlm")
    for p in (top, rootfs):
        if not os.path.exists(p):
            print("NG: 無い:", p); ok = False
    if not ok:
        sys.exit(1)
    r = dlm_mng.parse(open(top, "rb").read())
    print(f"mng: CRC {'OK' if r['crc_ok'] else 'NG'} / 個体ID {r['chipid'].hex()} / rootfs日付 {r['rootfs_date']!r}")
    ok &= r["crc_ok"]
    data = open(rootfs, "rb").read()
    h = ofw_tool.header_of(data)
    crc_ok = ofw_tool.crc_of(h, data[64:]) == int.from_bytes(h[0x3C:0x40], "big")
    date = seg_date(h)
    print(f"rootfs: magic={bytes(h[:4])!r} CRC {'OK' if crc_ok else 'NG'} / +8={int.from_bytes(h[8:10],'big')} / 日付 {date!r}")
    ok &= crc_ok and bytes(h[:3]) == b"DLM"
    if date != r["rootfs_date"]:
        print("NG: mngのrootfs日付とrootfsヘッダの日付が違う(DLM date ERRORになります)"); ok = False
    if int.from_bytes(h[8:10], "big") == 0:
        print("注意: rootfsのヘッダ+8が0(ステップ1用の版)。ステップ2のfinalで公式のものに置き換えてください")
    for bank in ("11002200", "33004400"):
        d = os.path.join(a.sys1, bank)
        print(f"{bank}: {'あり' if os.path.isdir(d) else 'なし(initがSPIから自動作成するはずだが未検証)'}")
    print("結果:", "OK" if ok else "NG")
    sys.exit(0 if ok else 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("phase1"); p.add_argument("package"); p.add_argument("sys1")
    p = sub.add_parser("read-id"); p.add_argument("path")
    p = sub.add_parser("final"); p.add_argument("package"); p.add_argument("sys1"); p.add_argument("--chipid", required=True)
    p = sub.add_parser("verify"); p.add_argument("sys1")
    a = ap.parse_args()
    {"phase1": cmd_phase1, "read-id": cmd_read_id, "final": cmd_final, "verify": cmd_verify}[a.cmd](a)


if __name__ == "__main__":
    main()
