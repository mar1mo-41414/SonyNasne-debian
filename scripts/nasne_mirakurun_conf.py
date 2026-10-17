#!/usr/bin/env python3
"""nasne のサービス一覧から、Mirakurun の tuners.yml / channels.yml を作る。標準ライブラリのみ。

nasne-recpt1 の HTTP サーバー(8301番)の /playlist.m3u8(先に `curl http://<nasne>:8301/scan` を1回実行して保存しておく)を読み、
テレビ(tv)のサービスごとに 1 エントリのチャンネル設定を書き出します(ワンセグ・データ放送は含めない)。
nasne は1回に1サービスしか出力しないので、Mirakurun のチャンネルは「サービスごと」に 1 つ必要です(docs/11)。

使い方:
  nasne_mirakurun_conf.py <nasneのIPまたはホスト名> [-o 出力ディレクトリ] [--scan] [--sat]
    -o     出力先(既定: カレント)。tuners.yml と channels.yml を書く。Mirakurun の設定ディレクトリ(/app-config など)に置く
    --scan 先に /scan を実行して一覧を作る(約80秒。チューナーを使うので、録画中は避ける)
    --name チューナー名(既定 nasne)
    --sat  tuners.yml の types に BS・CS も入れる(BS/CS は Mirakurun のチャンネルスキャンで探す。下記)

  nasne_mirakurun_conf.py --fix-sat <Mirakurun の channels.yml> [--enable]
    Mirakurun のチャンネルスキャンで作られた BS/CS の項目を、nasne 用に直す(ファイルをその場で書き換える。元は .bak に残す)。
    - channel に `?sid=0x<serviceId>` を足す(nasne は1回に1サービスしか出さないので、サービスの指定が要る。足さないと、同じ周波数の
      全サービスが「最初のサービス」の映像になる)。地デジ(GR)の項目は変えない。すでに `?` があるものも変えない。
    - --enable を付けると、BS/CS の isDisabled を false にする(Mirakurun のスキャンは、衛星の項目を無効で追加する)。

BS/CS の手順(Mirakurun のスキャンで探して、直す):
  1. tuners.yml の types に BS・CS が入っていること(--sat)。Mirakurun を再起動。
  2. BS: PUT /api/config/channels/scan?type=BS&useSubCh=1   (既定の名前は 101〜256 で、nasne は受けられない。useSubCh=1 で BS01_0 形式になる)
     CS: PUT /api/config/channels/scan?type=CS               (CS2〜CS24 になる)
     (Mirakurun は `useSubCh=false` を「真」と読むので、値は 1 にするか、付けない)
  3. --fix-sat で channels.yml を直し、Mirakurun を再起動。
"""
import argparse
import os
import re
import sys
import urllib.request


def fetch(url, timeout):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def parse_m3u(text):
    """[(物理チャンネル, sid(int), 局名)]"""
    out = []
    lines = text.splitlines()
    for i, l in enumerate(lines):
        if not l.startswith("#EXTINF"):
            continue
        name = l.split(",", 1)[1].strip()
        url = lines[i + 1].strip() if i + 1 < len(lines) else ""
        m = re.search(r"/tuner/(\d+)\?sid=0x([0-9a-fA-F]+)", url)
        if m:
            out.append((int(m.group(1)), int(m.group(2), 16), name))
    return out


def yq(s):
    """YAMLのダブルクォート文字列"""
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def fix_sat(path, enable):
    """Mirakurun の channels.yml の BS/CS 項目の channel に ?sid=0x<serviceId> を足す。返り値: (直した件数, 有効にした件数)"""
    import shutil
    with open(path, encoding="utf-8") as f:
        text = f.read()
    items = re.split(r"(?m)^(?=- )", text)          # 先頭が "- " の行で項目に分ける(Mirakurun が書く形式)
    fixed = enabled = 0
    out = []
    for it in items:
        m_type = re.search(r"(?m)^\s+type:\s*['\"]?(\w+)['\"]?\s*$", it)
        if not m_type or m_type.group(1) not in ("BS", "CS"):
            out.append(it); continue
        m_ch = re.search(r"(?m)^(\s+channel:\s*)(['\"]?)([^'\"\n]+)\2\s*$", it)
        m_sid = re.search(r"(?m)^\s+serviceId:\s*(\d+)\s*$", it)
        if m_ch and m_sid and "?" not in m_ch.group(3):
            new = f"{m_ch.group(1)}'{m_ch.group(3).strip()}?sid=0x{int(m_sid.group(1)):04x}'"
            it = it[:m_ch.start()] + new + it[m_ch.end():]
            fixed += 1
        if enable and re.search(r"(?m)^\s+isDisabled:\s*true\s*$", it):
            it = re.sub(r"(?m)^(\s+isDisabled:\s*)true(\s*)$", r"\1false\2", it); enabled += 1
        out.append(it)
    if fixed or enabled:                              # 何も変えないときは、ファイルにも .bak にも触らない(元の .bak を残す)
        shutil.copyfile(path, path + ".bak")
        with open(path, "w", encoding="utf-8") as f:
            f.write("".join(out))
    return fixed, enabled


def main():
    if "--fix-sat" in sys.argv:
        i = sys.argv.index("--fix-sat")
        if i + 1 >= len(sys.argv):
            sys.exit("使い方: nasne_mirakurun_conf.py --fix-sat <channels.yml> [--enable]")
        path = sys.argv[i + 1]
        fixed, enabled = fix_sat(path, "--enable" in sys.argv)
        print(f"{path}: BS/CS の channel に ?sid= を足した {fixed} 件、有効にした {enabled} 件(元は {path}.bak)")
        return
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("nasne")
    ap.add_argument("-o", "--outdir", default=".")
    ap.add_argument("--scan", action="store_true")
    ap.add_argument("--name", default="nasne")
    ap.add_argument("--sat", action="store_true")
    a = ap.parse_args()
    base = f"http://{a.nasne}:8301"
    if a.scan:
        print("スキャン中(約80秒)...", file=sys.stderr)
        fetch(base + "/scan", 300)
    services = parse_m3u(fetch(base + "/playlist.m3u8", 30))
    if not services:
        sys.exit("エラー: サービス一覧が空です。先に `curl %s/scan` を実行するか、--scan を付けてください" % base)
    os.makedirs(a.outdir, exist_ok=True)
    with open(os.path.join(a.outdir, "tuners.yml"), "w", encoding="utf-8") as f:
        types = "    - GR\n    - BS\n    - CS\n" if a.sat else "    - GR\n"
        f.write(f"- name: {a.name}\n  types:\n{types}  command: curl -s {base}/tuner/<channel>\n  isDisabled: false\n")
    with open(os.path.join(a.outdir, "channels.yml"), "w", encoding="utf-8") as f:
        for ch, sid, name in services:
            f.write(f"- name: {yq(name)}\n  type: GR\n  channel: '{ch}?sid=0x{sid:04x}'\n  serviceId: {sid}\n  isDisabled: false\n")
    print(f"{a.outdir}/tuners.yml と channels.yml を書きました(サービス {len(services)} 件)")


if __name__ == "__main__":
    main()
