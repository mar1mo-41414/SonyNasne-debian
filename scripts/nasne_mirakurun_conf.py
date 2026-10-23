#!/usr/bin/env python3
"""nasne のサービス一覧から、Mirakurun の tuners.yml / channels.yml を作る。標準ライブラリのみ。

Mirakurun 公式仕様の `commandVars` に完全準拠し、nasne のシングルTS制約を安全に解決します。

使い方:
  nasne_mirakurun_conf.py <nasneのIPまたはホスト名> [-o 出力ディレクトリ] [--scan] [--name チューナー名] [--sat]
    -o     出力先(既定: カレント)。tuners.yml と channels.yml を書く。Mirakurun の設定ディレクトリ(/app-config など)に置く
    --scan 先に /scan を実行して一覧を作る(地デジのみ約80秒。--sat と一緒なら /scan?all で BS/CS も、数分。チューナーを使うので録画中は避ける)
    --name チューナー名(既定 nasne)
    --sat  tuners.yml の types に BS・CS を含める (実機スキャンにBS/CSが含まれている場合は自動判別)

  nasne_mirakurun_conf.py --fix-sat <Mirakurun の channels.yml> [--enable]
    Mirakurun のチャンネルスキャンで作られた BS/CS の項目を、nasne 用に直す(ファイルをその場で書き換える。元は .bak に残す)。
    - channel から不正な URL クエリ(?sid=...)を除去し、Mirakurun 公式仕様の commandVars (sid_query) に安全に移し替えます。
      (channel に直接 ?sid= を書くと Mirakurun API や TVTest / BonDriver が 404 や不正フォーマットエラーを起こす問題を解消)
    - CS局のトランスポンダ集約(同一周波数の局が先頭局に潰されるバグ)を防ぐため、channel を CS16_333 のように一意な名前に自動分離します。
    - 同一ディレクトリにある tuners.yml の command に <sid_query> がなければ自動付与します。
    - --enable を付けると、BS/CS の isDisabled を false にします。
"""
import argparse
import os
import re
import shutil
import sys
import urllib.request

def fetch(url, timeout):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def parse_m3u(text):
    """[(チャンネル文字列, sid(int), 局名, 種別(GR/BS/CS))]"""
    out = []
    lines = text.splitlines()
    for i, l in enumerate(lines):
        if not l.startswith("#EXTINF"):
            continue
        name = l.split(",", 1)[1].strip()
        url = lines[i + 1].strip() if i + 1 < len(lines) else ""
        m = re.search(r"/tuner/([^?&]+)\?sid=0x([0-9a-fA-F]+)", url)
        if m:
            ch_str = m.group(1).strip()
            sid = int(m.group(2), 16)
            if ch_str.startswith("BS") or ch_str.startswith("bs"):
                ch_type = "BS"
            elif ch_str.startswith("CS") or ch_str.startswith("cs"):
                ch_type = "CS"
            else:
                ch_type = "GR"
            out.append((ch_str, sid, name, ch_type))
    return out


def yq(s):
    """YAMLのダブルクォート文字列"""
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def unique_channel(ch, sid):
    """Mirakurun のチャンネル名を、サービスごとに一意にする(末尾に _<サービスID(10進)> を足す。`?` は使わない)。
    nasne は1回に1サービスしか出さない。同じ周波数のサービスが同じ名前だと、Mirakurun は「同じチャンネル」とみなして、
    2つ目のサービスの要求を、1つ目のストリームに相当させてしまい、データが来ない(実測)。名前が違えば、チューナーが空くのを待って選局し直す。
    nasne-recpt1 は、`27_56360`(地デジ)・`BS15_0_101`・`CS16_333` の接尾辞を無視して受け付ける。"""
    return ch if re.search(r"_%d$" % sid, ch) else f"{ch}_{sid}"


def fix_tuners_file(tuners_path):
    """tuners.yml の command に <sid_query> が無ければ追加する"""
    if not os.path.exists(tuners_path):
        return False
    with open(tuners_path, "r", encoding="utf-8") as f:
        content = f.read()
    new_content = re.sub(
        r"(command:\s*.*<channel>)(?!<sid_query>)",
        r"\1<sid_query>",
        content
    )
    if new_content != content:
        shutil.copyfile(tuners_path, tuners_path + ".bak")
        with open(tuners_path, "w", encoding="utf-8") as f:
            f.write(new_content)
        return True
    return False


def fix_sat(path, enable):
    """Mirakurun の channels.yml の BS/CS 項目を commandVars 形式に直し、CSを集約回避する。
    返り値: (直した件数, 有効にした件数)"""
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()

    items = re.split(r"(?m)^(?=- )", text)
    fixed = 0
    enabled = 0
    out = []

    for it in items:
        if not it.strip():
            continue
        m_type = re.search(r"(?m)^\s+type:\s*['\"]?(\w+)['\"]?\s*$", it)
        ch_type = m_type.group(1) if m_type else None

        # BS または CS の項目を修正
        if ch_type in ("BS", "CS"):
            orig_it = it
            m_sid = re.search(r"(?m)^\s+serviceId:\s*(\d+)\s*$", it)
            m_ch = re.search(r"(?m)^(\s+channel:\s*)(['\"]?)([^'\"\n\r]+)\2(\s*)$", it)

            if m_sid and m_ch:
                sid_int = int(m_sid.group(1))
                sid_hex = f"0x{sid_int:04x}"
                raw_ch = m_ch.group(3).strip()

                # 1. channel から既存の ?sid= などを綺麗に除去
                clean_ch = raw_ch.split("?")[0].strip()

                # 2. サービスごとに一意な名前にする(例: CS2 → CS2_296、BS01_0 → BS01_0_101)。同じ名前だと Mirakurun が1つのチャンネルに集約してしまう
                clean_ch = unique_channel(clean_ch, sid_int)

                # channel 行を更新
                new_ch_line = f"{m_ch.group(1)}'{clean_ch}'{m_ch.group(4)}"
                it = it[:m_ch.start()] + new_ch_line + it[m_ch.end():]

                # 3. commandVars が無ければ追加、あれば更新
                cmd_vars = (
                    f"  commandVars:\n"
                    f"    sid_query: \"?sid={sid_hex}\"\n"
                    f"    sid: \"{sid_hex}\"\n"
                )
                # 既存の commandVars ブロック(見出し行と、それより深くインデントされた行)を消してから入れ直す(2回実行しても二重にならない)
                it = re.sub(r"(?m)^[ \t]+commandVars:[^\n]*\n(?:[ \t]{3,}\S[^\n]*\n)*", "", it)
                it = re.sub(
                    r"(?m)^(\s+serviceId:\s*\d+.*?\n)",
                    lambda m: m.group(1) + cmd_vars,
                    it,
                    count=1
                )
                if it != orig_it:                      # 実際に変わったものだけ数える(すでに直っていれば、ファイルにも .bak にも触らない)
                    fixed += 1

            if enable and re.search(r"(?m)^\s+isDisabled:\s*true\s*$", it):
                it = re.sub(r"(?m)^(\s+isDisabled:\s*)true(\s*)$", r"\1false\2", it)
                enabled += 1

        out.append(it)

    if fixed or enabled:
        shutil.copyfile(path, path + ".bak")
        with open(path, "w", encoding="utf-8") as f:
            f.write("".join(out))

    # 同一ディレクトリに tuners.yml があれば検査・補正
    tuners_candidate = os.path.join(os.path.dirname(path) or ".", "tuners.yml")
    if os.path.exists(tuners_candidate):
        fix_tuners_file(tuners_candidate)

    return fixed, enabled


def main():
    if "--fix-sat" in sys.argv:
        i = sys.argv.index("--fix-sat")
        if i + 1 >= len(sys.argv):
            sys.exit("使い方: nasne_mirakurun_conf.py --fix-sat <channels.yml> [--enable]")
        path = sys.argv[i + 1]
        fixed, enabled = fix_sat(path, "--enable" in sys.argv)
        print(f"{path}: BS/CS を commandVars 形式に修正 {fixed} 件、有効化 {enabled} 件 (元は {path}.bak)")
        tuners_path = os.path.join(os.path.dirname(path) or ".", "tuners.yml")
        if os.path.exists(tuners_path):
            print(f"{tuners_path}: command 設定を点検・同期しました")
        return

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("nasne", help="nasne の IP アドレスまたはホスト名")
    ap.add_argument("-o", "--outdir", default=".", help="設定ファイルの出力先ディレクトリ (既定: カレント)")
    ap.add_argument("--scan", action="store_true", help="実行前に nasne で /scan を実行する (約80秒)")
    ap.add_argument("--name", default="nasne", help="チューナー名 (既定: nasne)")
    ap.add_argument("--port", type=int, default=8301, help="nasne-recpt1 のHTTPポート(既定 8301)")
    ap.add_argument("--command", choices=("node", "curl"), default="node",
                    help="tuners.yml のコマンド。node(既定): Mirakurun のイメージに必ず入っている node で取る。curl: curl を足したイメージ用")
    ap.add_argument("--dual", action="store_true",
                    help="tuners.yml を地デジ用(GR)と衛星用(BS・CS)の2つのチューナーにする(試験的)。nasne 側で NASNE_RECPT1_DUAL=1(--dual-tuner)が必要。地デジと BS/CS を同時に使える")
    ap.add_argument("--sat", action="store_true", help="tuners.yml の types に BS・CS を含める (実機スキャンにBS/CSが含まれている場合は自動判別)")
    a = ap.parse_args()

    base = f"http://{a.nasne}:{a.port}"
    if a.scan:
        if a.sat:
            print("スキャン中(地デジ+BS+CS。数分かかる)...", file=sys.stderr)
            fetch(base + "/scan?all", 1200)
        else:
            print("スキャン中(約80秒)...", file=sys.stderr)
            fetch(base + "/scan", 300)

    services = parse_m3u(fetch(base + "/playlist.m3u8", 30))
    if not services:
        sys.exit("エラー: サービス一覧が空です。先に `curl %s/scan` を実行するか、--scan を付けてください" % base)

    os.makedirs(a.outdir, exist_ok=True)
    has_sat_in_services = any(s[3] in ("BS", "CS") for s in services)
    use_sat = a.sat or has_sat_in_services

    with open(os.path.join(a.outdir, "tuners.yml"), "w", encoding="utf-8") as f:
        types = "    - GR\n    - BS\n    - CS\n" if use_sat else "    - GR\n"
        if a.command == "node":    # 公式の Mirakurun イメージには curl が無い(node はある)
            cmd = f"node -e \"require('http').get('{base}/tuner/<channel><sid_query>', res => res.pipe(process.stdout))\""
        else:
            cmd = f"curl -s {base}/tuner/<channel><sid_query>"
        if a.dual:
            # 地デジ用と衛星用の2つ。同じコマンド(nasne-recpt1 が、チャンネル名から地デジか衛星かを判断する)
            f.write(
                f"- name: {a.name}-gr\n"
                f"  types:\n    - GR\n"
                f"  command: {cmd}\n"
                f"  isDisabled: false\n"
                f"- name: {a.name}-sat\n"
                f"  types:\n    - BS\n    - CS\n"
                f"  command: {cmd}\n"
                f"  isDisabled: false\n"
            )
        else:
            f.write(
                f"- name: {a.name}\n"
                f"  types:\n{types}"
                f"  command: {cmd}\n"
                f"  isDisabled: false\n"
            )

    with open(os.path.join(a.outdir, "channels.yml"), "w", encoding="utf-8") as f:
        # 実機サービス一覧から書き出し
        for ch, sid, name, ch_type in services:
            sid_hex = f"0x{sid:04x}"
            f.write(
                f"- name: {yq(name)}\n"
                f"  type: {ch_type}\n"
                f"  channel: '{unique_channel(ch, sid)}'\n"
                f"  serviceId: {sid}\n"
                f"  commandVars:\n"
                f"    sid_query: \"?sid={sid_hex}\"\n"
                f"    sid: \"{sid_hex}\"\n"
                f"  isDisabled: false\n"
            )

    gr_count = sum(1 for s in services if s[3] == "GR")
    bs_count = sum(1 for s in services if s[3] == "BS")
    cs_count = sum(1 for s in services if s[3] == "CS")
    total_count = len(services)
    print(f"{a.outdir}/tuners.yml と channels.yml を書きました(サービス {total_count} 件: GR {gr_count} 件, BS {bs_count} 件, CS {cs_count} 件)")


if __name__ == "__main__":
    main()
