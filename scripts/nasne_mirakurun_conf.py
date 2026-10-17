#!/usr/bin/env python3
"""nasne のサービス一覧から、Mirakurun の tuners.yml / channels.yml を作る。標準ライブラリのみ。

Mirakurun 公式仕様の `commandVars` に完全準拠し、nasne のシングルTS制約を安全に解決します。

使い方:
  nasne_mirakurun_conf.py <nasneのIPまたはホスト名> [-o 出力ディレクトリ] [--scan] [--name チューナー名] [--sat]
    -o     出力先(既定: カレント)。tuners.yml と channels.yml を書く。Mirakurun の設定ディレクトリ(/app-config など)に置く
    --scan 先に /scan を実行して一覧を作る(約80秒。チューナーを使うので、録画中は避ける)
    --name チューナー名(既定 nasne)
    --sat  tuners.yml の types に BS・CS を含め、BS全局(新3局含む全27局)+CS主要局のプリセット定義を channels.yml に統合出力

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

# BS プリセット定義 (局名, channel, serviceId, isDisabled)
# 2024年秋の帯域再編・新局(BS松竹東急/BSJapanext/BSよしもと)に対応した最新定義
BS_PRESETS = [
    ('NHK BS', 'BS15_0', 101, False),
    ('BS日テレ', 'BS13_0', 141, False),
    ('BS朝日', 'BS01_0', 151, False),
    ('BS-TBS', 'BS01_1', 161, False),
    ('BSテレ東', 'BS01_2', 171, False),
    ('BSフジ', 'BS13_1', 181, False),
    ('BS11イレブン', 'BS09_0', 211, False),
    ('BS12トゥエルビ', 'BS09_2', 222, False),
    ('放送大学テレビ', 'BS13_2', 231, False),
    ('WOWOWプライム', 'BS03_0', 191, False),
    ('WOWOWライブ', 'BS05_0', 192, False),
    ('WOWOWシネマ', 'BS05_1', 193, False),
    ('スターチャンネル', 'BS09_1', 200, True),
    ('放送大学ラジオ', 'BS13_2', 531, True),
    ('グリーンチャンネル', 'BS21_2', 234, False),
    ('BSアニマックス', 'BS03_1', 236, False),
    ('BS釣りビジョン', 'BS11_0', 241, True),
    ('J SPORTS 1', 'BS19_1', 242, False),
    ('J SPORTS 2', 'BS19_2', 243, False),
    ('J SPORTS 3', 'BS19_3', 244, False),
    ('J SPORTS 4', 'BS19_0', 245, False),
    ('WOWOWプラス', 'BS21_0', 252, False),
    ('日本映画専門チャンネル', 'BS21_1', 255, False),
    ('ディズニー・チャンネル', 'BS23_0', 256, False),
    ('BS松竹東急', 'BS23_3', 260, False),
    ('BSJapanext', 'BS23_2', 263, True),
    ('BSよしもと', 'BS23_1', 265, False),
]

# CS プリセット定義 (局名, channel, serviceId, isDisabled)
# nasne のシングルTS制約を回避するため、チャンネル名は CS{ND}_{SID} 形式で一意化
CS_PRESETS = [
    ('TBSチャンネル1', 'CS2_296', 296, False),
    ('テレ朝チャンネル1', 'CS2_298', 298, False),
    ('テレ朝チャンネル2', 'CS2_299', 299, False),
    ('ディズニージュニア', 'CS2_339', 339, False),
    ('スカイA', 'CS4_250', 250, False),
    ('時代劇専門チャンネル', 'CS4_292', 292, False),
    ('スカチャン1', 'CS4_301', 301, False),
    ('MTV', 'CS4_323', 323, False),
    ('ホームドラマチャンネル', 'CS6_294', 294, False),
    ('ミュージック・エア', 'CS6_324', 324, False),
    ('歌謡ポップスチャンネル', 'CS6_329', 329, False),
    ('カートゥーン ネットワーク', 'CS6_331', 331, False),
    ('ディスカバリーチャンネル', 'CS6_340', 340, False),
    ('アニマルプラネット', 'CS6_341', 341, False),
    ('CNNj', 'CS6_354', 354, False),
    ('囲碁・将棋チャンネル', 'CS6_363', 363, False),
    ('ショップチャンネル', 'CS8_55', 55, False),
    ('東映チャンネル', 'CS8_218', 218, False),
    ('衛星劇場', 'CS8_318', 318, False),
    ('映画・チャンネルNECO', 'CS8_349', 349, False),
    ('スカチャン2', 'CS10_101', 101, True),
    ('衛星劇場', 'CS10_219', 219, False),
    ('KBS World', 'CS10_317', 317, False),
    ('GAORA SPORTS', 'CS12_254', 254, False),
    ('MUSIC ON! TV', 'CS12_325', 325, False),
    ('キッズステーション', 'CS12_330', 330, False),
    ('ナショナル ジオグラフィック', 'CS12_343', 343, False),
    ('ザ・シネマ', 'CS14_227', 227, False),
    ('ファミリー劇場', 'CS14_293', 293, False),
    ('スーパー!ドラマTV', 'CS14_310', 310, False),
    ('ヒストリーチャンネル', 'CS14_342', 342, False),
    ('アクションチャンネル', 'CS16_311', 311, False),
    ('ミステリーチャンネル', 'CS16_316', 316, False),
    ('ミュージック・グラフィティTV', 'CS16_321', 321, False),
    ('AT-X', 'CS16_333', 333, False),
    ('BBCニュース', 'CS16_353', 353, False),
    ('Dlife', 'CS18_305', 305, False),
    ('FOX', 'CS18_314', 314, False),
    ('フジテレビONE', 'CS20_307', 307, False),
    ('フジテレビTWO', 'CS20_308', 308, False),
    ('フジテレビNEXT', 'CS20_309', 309, False),
    ('スペースシャワーTV', 'CS20_322', 322, False),
    ('映画・チャンネルNECO', 'CS22_297', 297, False),
    ('TBSチャンネル2', 'CS22_312', 312, False),
    ('日テレNEWS24', 'CS22_351', 351, False),
    ('日テレプラス', 'CS24_300', 300, False),
    ('タカラヅカ・スカイ・ステージ', 'CS24_315', 315, False),
]


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
            m_sid = re.search(r"(?m)^\s+serviceId:\s*(\d+)\s*$", it)
            m_ch = re.search(r"(?m)^(\s+channel:\s*)(['\"]?)([^'\"\n\r]+)\2(\s*)$", it)

            if m_sid and m_ch:
                sid_int = int(m_sid.group(1))
                sid_hex = f"0x{sid_int:04x}"
                raw_ch = m_ch.group(3).strip()

                # 1. channel から既存の ?sid= などを綺麗に除去
                clean_ch = raw_ch.split("?")[0].strip()

                # 2. CS の場合、トランスポンダ名のみ(例: CS2)なら CS2_296 のように分離して集約を防ぐ
                if ch_type == "CS" and not re.search(r"_\d+$", clean_ch):
                    clean_ch = f"{clean_ch}_{sid_int}"

                # channel 行を更新
                new_ch_line = f"{m_ch.group(1)}'{clean_ch}'{m_ch.group(4)}"
                it = it[:m_ch.start()] + new_ch_line + it[m_ch.end():]

                # 3. commandVars が無ければ追加、あれば更新
                cmd_vars = (
                    f"  commandVars:\n"
                    f"    sid_query: \"?sid={sid_hex}\"\n"
                    f"    sid: \"{sid_hex}\"\n"
                )
                if "commandVars:" in it:
                    it = re.sub(
                        r"(?m)^\s+commandVars:.*?(?=^\s*\S|\Z)",
                        cmd_vars,
                        it,
                        flags=re.DOTALL
                    )
                else:
                    it = re.sub(
                        r"(?m)^(\s+serviceId:\s*\d+.*?\n)",
                        r"\1" + cmd_vars,
                        it,
                        count=1
                    )
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
    ap.add_argument("--sat", action="store_true", help="BS/CS の定義も生成する (最新BS全局+主要CS局のプリセット定義を統合出力)")
    a = ap.parse_args()

    base = f"http://{a.nasne}:8301"
    if a.scan:
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
        f.write(
            f"- name: {a.name}\n"
            f"  types:\n{types}"
            f"  command: curl -s {base}/tuner/<channel><sid_query>\n"
            f"  isDisabled: false\n"
        )

    with open(os.path.join(a.outdir, "channels.yml"), "w", encoding="utf-8") as f:
        # 実機サービス一覧から書き出し
        for ch, sid, name, ch_type in services:
            sid_hex = f"0x{sid:04x}"
            f.write(
                f"- name: {yq(name)}\n"
                f"  type: {ch_type}\n"
                f"  channel: '{ch}'\n"
                f"  serviceId: {sid}\n"
                f"  commandVars:\n"
                f"    sid_query: \"?sid={sid_hex}\"\n"
                f"    sid: \"{sid_hex}\"\n"
                f"  isDisabled: false\n"
            )

        # 実機スキャンにBS/CSが含まれておらず、--sat が指定された場合はプリセットで補完
        if a.sat and not has_sat_in_services:
            for name, ch, sid, dis in BS_PRESETS:
                sid_hex = f"0x{sid:04x}"
                dis_str = "true" if dis else "false"
                f.write(
                    f"- name: {yq(name)}\n"
                    f"  type: BS\n"
                    f"  channel: '{ch}'\n"
                    f"  serviceId: {sid}\n"
                    f"  commandVars:\n"
                    f"    sid_query: \"?sid={sid_hex}\"\n"
                    f"    sid: \"{sid_hex}\"\n"
                    f"  isDisabled: {dis_str}\n"
                )
            for name, ch, sid, dis in CS_PRESETS:
                sid_hex = f"0x{sid:04x}"
                dis_str = "true" if dis else "false"
                f.write(
                    f"- name: {yq(name)}\n"
                    f"  type: CS\n"
                    f"  channel: '{ch}'\n"
                    f"  serviceId: {sid}\n"
                    f"  commandVars:\n"
                    f"    sid_query: \"?sid={sid_hex}\"\n"
                    f"    sid: \"{sid_hex}\"\n"
                    f"  isDisabled: {dis_str}\n"
                )

    gr_count = sum(1 for s in services if s[3] == "GR")
    bs_count = sum(1 for s in services if s[3] == "BS") + (len(BS_PRESETS) if (a.sat and not has_sat_in_services) else 0)
    cs_count = sum(1 for s in services if s[3] == "CS") + (len(CS_PRESETS) if (a.sat and not has_sat_in_services) else 0)
    total_count = gr_count + bs_count + cs_count
    print(f"{a.outdir}/tuners.yml と channels.yml を書きました(サービス {total_count} 件: GR {gr_count} 件, BS {bs_count} 件, CS {cs_count} 件)")


if __name__ == "__main__":
    main()
