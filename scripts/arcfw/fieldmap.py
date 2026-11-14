#!/usr/bin/env python3
"""ファーム自身のデバッグ表示関数(「*********pcmd_open_stream************」を表示する関数)を読んで、
ストリームを開くコマンド(cmd_open_stream)の欄の名前とオフセットの対応表を作る(docs/26)。
使い方: python3 -I fieldmap.py [開始 終了]   … カレントに firmware.bin と fw.o を置いて実行(dis.py を呼ぶ)。
  既定の範囲は AVDriver7GSP47 のファームでの関数の位置(ファイル内オフセット 0xdc374〜0xdd12c)。
出力: 「オフセット 名前」の一覧。表示関数の中で、r16 がコマンドの先頭、ほかのレジスタは r16 からの足し引きで作られる。"""
import re, subprocess, sys, os
a, b = (sys.argv[1], sys.argv[2]) if len(sys.argv) > 2 else ("0xdc374", "0xdd12c")
here = os.path.dirname(os.path.abspath(__file__))
lines = subprocess.run([sys.executable, "-I", os.path.join(here, "dis.py"), a, b], capture_output=True, text=True).stdout.splitlines()
base = {"r16": 0}           # レジスタ → コマンド先頭からのずれ
fmt = None; args = {}; out = []
def flush():
    global fmt, args
    if fmt is not None and args: out.append((fmt, dict(args)))
    fmt = None; args = {}
for l in lines:
    m = re.search(r'\t(\S+)\s+(.*?)(\s+;.*)?$', l)
    if not m: continue
    op, ops = m.group(1), m.group(2)
    s = re.search(r'; "(.*)"', l)
    if op in ("mov_s", "mov") and s and re.match(r"r(1|18),", ops): flush(); fmt = s.group(1); continue
    if op in ("mov_s", "mov") and ops == "r1,r18": flush(); continue
    mm = re.match(r"(r\d+|fp),(r\d+),0x([0-9a-f]+)$", ops)
    if mm and mm.group(2) in base:
        k = {"add": 1, "sub": -1, "sub2": -4, "sub3": -8}.get(op)
        if k: base[mm.group(1)] = base[mm.group(2)] + k * int(mm.group(3), 16); continue
    if op.startswith("ld"):
        m2 = re.match(r"(r\d+),\[(r\d+),(-?\d+)\]", ops)
        if m2 and m2.group(2) in base:
            args[m2.group(1)] = base[m2.group(2)] + int(m2.group(3)) * (4 if ".as" in op else 1)
flush()
tab = set()
for f, a_ in out:
    names = re.findall(r"([A-Za-z_][A-Za-z0-9_\[\]\.]*)\s*[:=]\s*%", f)
    vals = [a_[r] for r in sorted(a_, key=lambda r: int(r[1:])) if r not in ("r0", "r1")]
    i = 0
    for n, spec in zip(names, re.findall(r"%l?[dxu]", f)):
        if i < len(vals): tab.add((vals[i], n))
        i += 2 if spec == "%ld" else 1          # %ld は 64 ビット(レジスタ 2 本)
for o, n in sorted(tab): print("%#06x %s" % (o, n))
