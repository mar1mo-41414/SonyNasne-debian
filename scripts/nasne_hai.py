#!/usr/bin/env python3
"""公式ファームの p3(録画領域)直下にある `00000000.hai` / `00000001.hai`(HDD登録情報)を読み書きする。

libsechdd.so / dtvtuner の ssssssc(書込)・ssssssd(読込)の逆アセンブルで判明した形式:
  - ファイルはちょうど 65536 バイト。0 バイトや短いファイルは読込側が弾く
  - 先頭から 4 つの 256 バイト枠に文字列を持つ(NUL終端、255文字まで):
        オフセット  8537  本体の個体ID(16桁の16進、00110022.dlm と同じ chipid)
        オフセット  5649  HDDのベンダ名         例 "ATA"
        オフセット 11375  HDDのモデル名         例 "WDC WD5000LPVX"(先頭16文字)
        オフセット  4259  HDDのシリアル番号
  - 難読化: ファイル全体を、255バイト周期の鍵で XOR する。鍵 = (オフセット 21064 から 255 バイトの「ファイル内鍵」)
    XOR (ファーム内の定数 255 バイト)。ファイル内鍵の領域だけは XOR 後に元の値へ戻して平文で置く
  - 検査: オフセット 144 の 4 バイトに「先頭 32768 バイトを 16 ビット小エンディアン語として足した和」を入れる
          (和を取るときは 144 の 4 バイトを 0 として扱う。和は難読化後のバイト列に対して計算する)
  - 残りの領域は乱数でよい(公式は rand() の出力)
  読込側(nobf0595)が見る照合は「個体IDの先頭一致」と「シリアルの先頭一致」だけ(2つあるファイルのどちらか一方が有効ならよい)。

使い方:
  nasne_hai.py show <.hai>                             復号して 4 つの文字列と検査和を表示する
  nasne_hai.py gen <出力ディレクトリ> --chipid <16桁hex> --device /dev/sdX
                                                       00000000.hai と 00000001.hai を作る。ベンダ・モデル・シリアルは
                                                       /sys/block/sdX/device から取る。--vendor/--model/--serial で上書き可
"""
import argparse
import os
import struct
import sys

# ファーム(dtvtuner / webapi)内の定数 255 バイト。ssssssc/ssssssd が鍵の生成に使う
CONST = bytes.fromhex(
    "4877692a6a2c502a4a6a6e6761726f7134336f6872776e6170392b4c6970716164673a706f69716a3b6e6c6161396a34723b"
    "6d766167667471616c613a30494c6c3b67714866676564614452283461676f456e6c616970653869713466676b6c616f68"
    "67616a6671616972616e7a69402d6168674861713b656c6b6a4f554968676176696f69617267616c6b613a3a7148494f6648"
    "466465626e554f4972696f676128556d6165383468623d61726c6b616172796767726f6938757072616c6b6a40504f494a47"
    "6968617240727266676761763839686739617268716a7265472845546861616938686670733362766437757761613b667655"
    "4972693b392b")
SIZE = 0x10000
KEYOFF = 21064            # ファイル内鍵(255バイト)
SUMOFF = 144              # 検査和(4バイト)
FIELDS = {"chipid": 8537, "vendor": 5649, "model": 11375, "serial": 4259}
NAMES = ("00000000.hai", "00000001.hai")


def _sum(buf):
    b = bytearray(buf[:0x8000])
    b[SUMOFF:SUMOFF + 4] = b"\0\0\0\0"
    return sum(struct.unpack("<%dH" % 0x4000, bytes(b))) & 0xFFFFFFFF


def _key(buf):
    return bytes(a ^ b for a, b in zip(buf[KEYOFF:KEYOFF + 255], CONST))


def decode(buf):
    """(保存された和, 計算した和, 平文, {フィールド: bytes}) を返す。"""
    if len(buf) != SIZE:
        raise ValueError(f"サイズが {len(buf)} バイト(65536 でなければならない)")
    key = _key(buf)
    plain = bytes(buf[i] ^ key[i % 255] for i in range(SIZE))
    f = {}
    for name, off in FIELDS.items():
        e = plain.find(b"\0", off, off + 256)
        f[name] = plain[off:e] if e >= 0 else None
    return struct.unpack_from("<I", buf, SUMOFF)[0], _sum(buf), plain, f


def encode(chipid, vendor, model, serial, rnd=None):
    rnd = rnd or os.urandom
    plain = bytearray(rnd(SIZE))
    for name, val in (("chipid", chipid), ("vendor", vendor), ("model", model), ("serial", serial)):
        v = val.encode() if isinstance(val, str) else val
        if len(v) > 255:
            raise ValueError(f"{name} が長すぎる")
        off = FIELDS[name]
        plain[off:off + 256] = v + b"\0" * (256 - len(v))
    filekey = bytes(plain[KEYOFF:KEYOFF + 255])
    key = bytes(a ^ b for a, b in zip(filekey, CONST))
    out = bytearray(plain[i] ^ key[i % 255] for i in range(SIZE))
    out[KEYOFF:KEYOFF + 255] = filekey                 # 鍵の領域は平文のまま(公式と同じ)
    out[SUMOFF:SUMOFF + 4] = b"\0\0\0\0"
    struct.pack_into("<I", out, SUMOFF, _sum(out))
    return bytes(out)


def device_identity(dev):
    """HDD の (ベンダ, モデル, シリアル)。本体(SATA直結)では "ATA" / モデル先頭16文字 / シリアルに見える。
    PC で USB ブリッジ越しに見ると sysfs はブリッジの名前になるので、lsblk(udev の ID_MODEL)から本体側の見え方に直す。"""
    import subprocess
    lsb = lambda col: subprocess.run(["lsblk", "-dno", col, dev], capture_output=True, text=True).stdout.strip()
    model, serial = lsb("MODEL"), lsb("SERIAL")
    if not model or not serial:
        raise SystemExit("エラー: lsblk からモデル・シリアルが取れない。--model/--serial を指定")
    return "ATA", model[:16], serial


def cmd_show(a):
    for p in a.files:
        try:
            st, calc, _, f = decode(open(p, "rb").read())
        except ValueError as e:
            print(f"{p}: 不正 ({e})")
            continue
        print(f"{p}: 検査和 {'OK' if st == calc else 'NG'} (保存 {st:#010x} / 計算 {calc:#010x})")
        for k in FIELDS:
            print(f"  {k:7s}: {f[k]!r}")


def cmd_gen(a):
    if len(bytes.fromhex(a.chipid)) != 8:
        sys.exit("エラー: --chipid は16桁の16進数(8バイト)")
    vendor, model, serial = device_identity(a.device) if a.device else ("", "", "")
    vendor, model, serial = a.vendor or vendor, a.model or model, a.serial or serial
    if not (vendor and model and serial):
        sys.exit("エラー: ベンダ・モデル・シリアルが取れない。--device か --vendor/--model/--serial を指定")
    os.makedirs(a.outdir, exist_ok=True)
    for n in NAMES:
        open(os.path.join(a.outdir, n), "wb").write(encode(a.chipid.lower(), vendor, model, serial))
    print(f"{a.outdir}: {NAMES[0]} / {NAMES[1]} を作りました(個体ID {a.chipid.lower()}、{vendor} / {model} / {serial})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("show"); p.add_argument("files", nargs="+")
    p = sub.add_parser("gen"); p.add_argument("outdir"); p.add_argument("--chipid", required=True)
    p.add_argument("--device"); p.add_argument("--vendor"); p.add_argument("--model"); p.add_argument("--serial")
    a = ap.parse_args()
    {"show": cmd_show, "gen": cmd_gen}[a.cmd](a)


if __name__ == "__main__":
    main()
