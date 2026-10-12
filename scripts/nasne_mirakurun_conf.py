#!/usr/bin/env python3
"""nasne のサービス一覧から、Mirakurun の tuners.yml / channels.yml を作る。標準ライブラリのみ。

nasne-recpt1 の HTTP サーバー(8301番)の /playlist.m3u8(先に `curl http://<nasne>:8301/scan` を1回実行して保存しておく)を読み、
テレビ(tv)のサービスごとに 1 エントリのチャンネル設定を書き出します(ワンセグ・データ放送は含めない)。
nasne は1回に1サービスしか出力しないので、Mirakurun のチャンネルは「サービスごと」に 1 つ必要です(docs/11)。

使い方:
  nasne_mirakurun_conf.py <nasneのIPまたはホスト名> [-o 出力ディレクトリ] [--scan]
    -o     出力先(既定: カレント)。tuners.yml と channels.yml を書く。Mirakurun の設定ディレクトリ(/app-config など)に置く
    --scan 先に /scan を実行して一覧を作る(約80秒。チューナーを使うので、録画中は避ける)
    --name チューナー名(既定 nasne)
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("nasne")
    ap.add_argument("-o", "--outdir", default=".")
    ap.add_argument("--scan", action="store_true")
    ap.add_argument("--name", default="nasne")
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
        f.write(f"- name: {a.name}\n  types:\n    - GR\n  command: curl -s {base}/tuner/<channel>\n  isDisabled: false\n")
    with open(os.path.join(a.outdir, "channels.yml"), "w", encoding="utf-8") as f:
        for ch, sid, name in services:
            f.write(f"- name: {yq(name)}\n  type: GR\n  channel: '{ch}?sid=0x{sid:04x}'\n  serviceId: {sid}\n  isDisabled: false\n")
    print(f"{a.outdir}/tuners.yml と channels.yml を書きました(サービス {len(services)} 件)")


if __name__ == "__main__":
    main()
