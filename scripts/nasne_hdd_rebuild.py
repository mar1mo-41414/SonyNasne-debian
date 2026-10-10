#!/usr/bin/env python3
"""nasne の HDD(sys1)を公式ファーム(KRST3101_xxxx_SECURE.dlm)からファイル操作だけで作る補助ツール。
SPIの吸い出し等は不要。手順は docs/18_hdd_rebuild_guide.md。Python 3.8+ のみ必要(外部ライブラリ不要)。

  phase1 <公式.dlm> <sys1ディレクトリ>        ステップ1用。00550066.dlmを「ヘッダ+8=0」版にして置き、00110022.dlmを消す。
                                                nasneを1回起動すると、本体のinitがこの本体の個体IDで00110022.dlmを自動生成する
  read-id <sys1ディレクトリ または 00110022.dlm> 自動生成された00110022.dlmから個体ID(16桁の16進)を表示する
  final <公式.dlm> <sys1ディレクトリ> --chipid <16桁hex>
                                                ステップ2用。完全なsys1(00550066.dlm / 00110022.dlm / 11002200 / 33004400)を作る
  verify <sys1ディレクトリ>                     sys1の内容をinitと同じ規則で検査する(CRC・日付の整合など)

p3(録画領域)の初期構造について(docs/04): v2.60 は、空のXFSのp3を自分で初期化できず停止する(PWR/REC赤点灯)。
v1.00 は初期化できる。そこで「v1.00で1回起動してp3を初期化させる」方式を推奨する(下の mkdisk / v100):
  mkdisk <デバイス> --serial <シリアル> [--p3 xfs|none]
                                                HDDを丸ごと作り直す(区画3つ + ext3×2 + 記事と同じオプションのXFS)。**全消去**。
                                                誤爆防止のため、lsblkで確認したシリアル番号を --serial で渡す(シリアルが無いデバイスは none)
  p3init <p3のマウント先> --chipid <16桁hex> --device /dev/sdX
                                                v1.00 を使わずに p3(空のXFS)へ公式の初期構造を作る。HDD登録情報(.hai、nasne_hai.py)と
                                                録画DBの雛形(00000015/10000000.dat・10000001.dat)と空ディレクトリ群。v2.60 がこれで起動する(docs/15)
  hai <p3のマウント先> --chipid <16桁hex> --device /dev/sdX
                                                .hai の2ファイルだけを作る(p3init の一部)
  v100 <v1.00の00550066.dlm> <sys1ディレクトリ>  記事の方式でsys1を作る(00550066.dlm、55006600/00550066.dlm、148バイトのゼロの00110022.dlm)。
                                                nasneで2回起動(1回目はREC/LAN高速点滅で電源を抜く)すると、v1.00が起動してp3も初期化される。
                                                そのあと final(v2.60)に置き換えれば、v1.00のまま使わず v2.60 に戻せる
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


V100_MD5 = "1c921378f2a7846a6492982c98c82fcd"       # 公式 v1.00 の 00550066.dlm(21,287,161バイト)。記事の値と一致


def cmd_v100(a):
    import hashlib
    data = open(a.dlm, "rb").read()
    md5 = hashlib.md5(data).hexdigest()
    h = ofw_tool.header_of(data)
    if bytes(h[:3]) != b"DLM" or ofw_tool.crc_of(h, data[64:]) != int.from_bytes(h[0x3C:0x40], "big"):
        sys.exit("エラー: v1.00の00550066.dlmとして不正(マジック/CRC)")
    if md5 != V100_MD5:
        print(f"注意: md5が既知のv1.00と違う({md5})。v1.00の00550066.dlm({len(data)}バイト、md5 {V100_MD5})ですか?")
    os.makedirs(os.path.join(a.sys1, "55006600"), exist_ok=True)
    for dst in (os.path.join(a.sys1, "00550066.dlm"), os.path.join(a.sys1, "55006600", "00550066.dlm")):
        open(dst, "wb").write(data)
    open(os.path.join(a.sys1, "00110022.dlm"), "wb").write(b"\0" * 148)
    print(f"{a.sys1}: 00550066.dlm、55006600/00550066.dlm、00110022.dlm(148バイトのゼロ)を置きました")
    print("次: nasneに挿して電源を入れ、約2分でREC/LANが高速点滅したら電源を抜き、入れ直す(v1.00が起動)")


# 録画DBの雛形(00000015/10000000.dat と 10000001.dat、同一内容)。実データは先頭121バイトで残り(合計100KB)はゼロ。
# 形式: マジック a17e4d13 / 長さ0x71 / 乱数様4B / 固定8B / 暗号化された本体。暗号は未解読だが、別のHDD2台
# (機種・シリアル・容量が違う)で v1.00 が作ったものが完全に一致したので、HDDには依存しない固定の雛形として扱う
DB_ROOT = bytes.fromhex(
    "a17e4d1371000000bfeac5955b51bbf1583287d5f8ed56806cb930c2853f44371351450db79b7319849c3c9d2a5de5fc"
    "309d12c6c33659643ce34eabe3b32f3a2345c014aa0674d16f90468864d9e48b9671d3719351152480d1304502816c76"
    "a56974bffb1d2119a76c0ea2b0a19b0a479402aea610945c36")
DB_SIZE = 102400


def cmd_p3init(a):
    root = a.p3
    if not os.path.isdir(root):
        sys.exit(f"エラー: {root} が無い")
    if [n for n in os.listdir(root) if n != "lost+found"]:
        sys.exit(f"エラー: {root} が空ではない。p3 が空(mkdisk 直後)のときだけ使えます(録画データを守るため)")
    def mk(path, mode=0o755):
        os.makedirs(os.path.join(root, path), exist_ok=True)
        os.chmod(os.path.join(root, path), mode)
    mk("00000015")
    for i in range(96):
        mk(f"00000015/{i:02d}", 0o341)
    for n in ("10000000.dat", "10000001.dat"):
        open(os.path.join(root, "00000015", n), "wb").write(DB_ROOT + b"\0" * (DB_SIZE - len(DB_ROOT)))
    for d in ("00000021/00000001", "00000021/00000002", "00000021/00000003", "opt/CMA/SCE", "opt/CMA/tmp"):
        mk(d)
    open(os.path.join(root, "00000021", "00000005"), "wb").write(os.urandom(8))
    for d in ("share", "setup"):
        mk(d, 0o775)
    for d in ("VIDEO", "MUSIC", "PHOTO"):
        mk("share/" + d)
    open(os.path.join(root, "setup", "index.html"), "w").write(
        '<html>\n<meta http-equiv="refresh" content="0;url=/nasne_home/index.html">\n</html>\n')
    cmd_hai(a)


def cmd_hai(a):
    import nasne_hai
    nasne_hai.cmd_gen(argparse.Namespace(outdir=a.p3, chipid=a.chipid, device=a.device, vendor=None, model=None, serial=None))


def _run(cmd, **kw):
    import subprocess
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.exit(f"エラー: {' '.join(cmd)}\n{r.stderr or r.stdout}")
    return r.stdout


def cmd_mkdisk(a):
    import subprocess
    dev = a.device
    if not os.path.exists(dev):
        sys.exit(f"エラー: {dev} が無い")
    info = subprocess.run(["lsblk", "-dno", "TYPE,SIZE,MODEL,SERIAL", dev], capture_output=True, text=True).stdout.split()
    if not info or info[0] not in ("disk", "loop"):
        sys.exit(f"エラー: {dev} はディスク全体ではない(区画ではなく /dev/sdX を指定)")
    serial = _run(["lsblk", "-dno", "SERIAL", dev]).strip() or "none"
    if serial != a.serial:
        sys.exit(f"エラー: シリアルが違う。{dev} は『{' '.join(info[1:])}』、シリアル『{serial}』。`--serial {serial}` で確認済みと示してください")
    mounted = [l for l in _run(["lsblk", "-no", "MOUNTPOINT", dev]).split() if l]
    if mounted:
        sys.exit(f"エラー: マウント中の区画がある: {mounted}")
    root_src = _run(["findmnt", "-no", "SOURCE", "/"]).strip()
    if root_src.startswith(dev):
        sys.exit("エラー: システムのディスクです")
    size = int(_run(["lsblk", "-bdno", "SIZE", dev]).strip())
    print(f"{dev}: {' '.join(info[1:])} シリアル {serial} ({size / 1e9:.0f}GB) を全消去して nasne 用に作り直します")
    _run(["wipefs", "-a", dev])
    open(dev, "r+b").write(b"\0" * (8 << 20))                       # 先頭8MB(区画表・旧署名)を消す
    layout = "label: dos\nunit: sectors\nstart=2048, size=524288, type=83, bootable\nstart=526336, size=2097152, type=83\n"
    if a.p3 == "xfs":
        layout += "start=2623488, type=83\n"
    subprocess.run(["sfdisk", "--force", dev], input=layout, text=True, capture_output=True, check=True)
    _run(["partprobe", dev])
    import time
    time.sleep(1)
    p = lambda n: dev + ("p" if dev[-1].isdigit() else "") + str(n)
    _run(["mkfs.ext3", "-q", "-F", "-L", "sys1", p(1)])
    _run(["mkfs.ext3", "-q", "-F", "-L", "sys2", p(2)])
    if a.p3 == "xfs":
        # 記事のオプション。agcount は容量で変える(1TBまで4、2TBまで8、それ以上は16)。物理セクタに関係なく -s size=512
        agc = 4 if size <= 1.2e12 else (8 if size <= 2.5e12 else 16)
        _run(["mkfs.xfs", "-q", "-f", "-m", "crc=0", "-d", f"agcount={agc}", "-i", "size=256,attr=2,projid32bit=0",
              "-L", "user", "-n", "ftype=0", "-s", "size=512", p(3)])
    print("完了: sys1(ext3)・sys2(ext3)" + ("・user(XFS)" if a.p3 == "xfs" else "") + "。次: sys1をマウントして v100 または phase1/final")


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
    p = sub.add_parser("v100"); p.add_argument("dlm"); p.add_argument("sys1")
    p = sub.add_parser("p3init"); p.add_argument("p3"); p.add_argument("--chipid", required=True); p.add_argument("--device", required=True)
    p = sub.add_parser("hai"); p.add_argument("p3"); p.add_argument("--chipid", required=True); p.add_argument("--device", required=True)
    p = sub.add_parser("mkdisk"); p.add_argument("device"); p.add_argument("--serial", required=True); p.add_argument("--p3", choices=["xfs", "none"], default="xfs")
    a = ap.parse_args()
    {"phase1": cmd_phase1, "read-id": cmd_read_id, "final": cmd_final, "verify": cmd_verify, "v100": cmd_v100, "mkdisk": cmd_mkdisk, "hai": cmd_hai, "p3init": cmd_p3init}[a.cmd](a)


if __name__ == "__main__":
    main()
