#!/usr/bin/env python3
"""make_stage1_dlm.py — 公式HDDの /disk0/00550066.dlm を、「Debian直起動へ移行する段階1」入りの自作版に作り替える(PC側、root不要)。

公式の rootfs(.dlm の中身の tar.gz)を、展開せずに tar のメンバー単位でコピーし、etc/init.d/rcS の末尾
(/opt/dtvtuner/etc/startdtvtuner の1行)だけを段階1の処理に差し替える。ディスク上に展開しないので、
前回の作業の残骸が混ざる事故(docs/13 の事故)が起きない。ヘッダ(hwtype・日付)は入力 .dlm のものをそのまま使うので、
入力は「そのHDD自身の 00550066.dlm」を使えば、マネージャ(00110022.dlm)の検査にもそのまま通る。

サブコマンド:
  build           --official IN.dlm --out OUT.dlm [--mode p3|embed] [--debian-tree-tar TAR] [--p3-mode image|format] [--image-size 8G] [--format-p3] [--telnet early|fail|off]
  extract-drivers --official IN.dlm --dest DIR      xcode4drv.ko と rc.xcode4 を取り出す
  list            --official IN.dlm                  tar の中身(先頭)と rcS の末尾を表示

--official には公式パッケージ(KRST3101_xxxx_SECURE.dlm、3セグメント)でも、HDDの 00550066.dlm でも渡せる。
モード:
  debug  公式のまま動かし、rcS の先頭で telnetd だけ開く。公式アプリが止まる原因を telnet で中から調べるための版。
  p3     p3 に(PC側で)Debianを用意してある前提。rcS は p3 を mount して chroot し、nasne-stage1.sh を実行する。
  embed  Debian のツリー(prepare_tree.sh の出力を tar にしたもの)をこの .dlm に同梱する。rcS が同梱のDebianをp3に入れ、続けて段階1を実行する。
         --p3-mode image(既定): 公式のp3(XFS。録画データが入っている)は消さず、その中の /.nasne-debian/root.img(ext3のイメージ)にDebianを入れる。
                  録画データは Debian の /data から見える。公式アプリを1回だけ起動して戻ることもできる(nasne-boot-switch official-once)。
         --p3-mode format: p3 を ext3 で初期化して直接Debianを入れる。**p3(録画データ領域)は消える**ので --format-p3 が必須。
"""
import argparse
import gzip
import io
import os
import struct
import sys
import tarfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from dlm_crypto import NasneBlowfish, DLM_MAGIC, decrypt_body, inflate_raw  # noqa: E402
import build_dlm  # noqa: E402

START_LINE = "/opt/dtvtuner/etc/startdtvtuner"
RCS_PATH = "etc/init.d/rcS"
MARK_BEGIN = "# ======== nasne-debian stage1"


def load_dlm_segment(path):
    """path が公式パッケージ(3セグメント)なら DLM セグメントを、単体の .dlm ならそのまま返す。"""
    data = open(path, "rb").read()
    h = NasneBlowfish().chain_decrypt(data[:64])
    magic = bytes(h[:4]).rstrip(b"\0")
    if magic == b"DLM":
        return data
    if magic == b"KNL":                      # 公式パッケージ: KNL ‖ RFS ‖ DLM
        pos, last = 0, None
        while pos < len(data):
            hh = NasneBlowfish().chain_decrypt(data[pos:pos + 64])
            size = int.from_bytes(hh[4:8], "big")
            if bytes(hh[:4]).rstrip(b"\0") == b"DLM":
                last = data[pos:pos + size]
            pos += size
        if last:
            return last
    raise SystemExit(f"{path}: DLM セグメントとして読めない(magic={magic!r})")


def dlm_to_tar_bytes(seg):
    """DLMセグメント → 生の tar バイト列。ボディが暗号化(flag==1)なら復号、gzipならinflateする。"""
    h = NasneBlowfish().chain_decrypt(seg[:64])
    body = seg[64:]
    if h[0x0E] == 1:
        import tempfile
        with tempfile.NamedTemporaryFile(delete=False) as t:
            t.write(seg)
        try:
            body = decrypt_body(t.name, tail_mode="cfb")
        finally:
            os.unlink(t.name)
    if body[:2] == b"\x1f\x8b":
        return inflate_raw(body), h
    return body, h


def iter_members(raw_tar):
    tf = tarfile.open(fileobj=io.BytesIO(raw_tar), mode="r:")
    for m in tf:
        yield tf, m


def rcs_new(rcs_text, mode, telnet, pre_snippet=None, official_pre=None):
    lines = rcs_text.split("\n")
    idx = [i for i, l in enumerate(lines) if l.strip() == START_LINE]
    if len(idx) != 1:
        raise SystemExit(f"rcS に {START_LINE} の行が{len(idx)}個ある(1個のはず)。このファームのバージョンは想定外。中身を確認すること")
    if any(MARK_BEGIN in l for l in lines):
        raise SystemExit("rcS にすでに stage1 のブロックがある(入力が自作版)。公式の .dlm を入力にすること")
    last_nonblank = max(i for i, l in enumerate(lines) if l.strip())
    if idx[0] != last_nonblank:
        print(f"警告: startdtvtuner の行が rcS の最後ではない(行 {idx[0] + 1}/{last_nonblank + 1})。後ろの行は残す", file=sys.stderr)
    if mode == "debug":                              # 公式のまま動かし、先にtelnetだけ開く(止まった原因を中から調べる用)
        block = [MARK_BEGIN + " (debug: telnetのみ) ========",
                 "ifconfig eth0 up",
                 "udhcpc -i eth0 -t 5 -T 3 -A 3 -b -p /var/run/udhcpc.eth0.pid",
                 "telnetd -l /bin/sh &",
                 "# 公式アプリが halt/reboot を呼んでも機体を止めない(呼ばれた事実だけ /tmp/halt.log に残す)",
                 "for c in halt reboot poweroff; do rm -f /sbin/$c; printf '#!/bin/sh\\necho \"'$c' called $(date) $*\" >> /tmp/halt.log\\n' > /sbin/$c; chmod 755 /sbin/$c; done",
                 "# 公式アプリのログを出す(既定は0)",
                 "sed -i 's/SHARED_LOGLEVEL=0/SHARED_LOGLEVEL=5/' /opt/dtvtuner/etc/startdtvtuner",
                 "# 状態を 2 秒ごとに sys1(sda1)へ書き続ける(落ちても最後の様子がHDDに残る。PCで dbglog.txt を読む)",
                 "mkdir -p /tmp/s1; mount -t ext3 /dev/sda1 /tmp/s1 && ( while :; do { date; cut -d' ' -f1 /proc/uptime; cat /proc/mounts; ps; echo '--- halt.log'; cat /tmp/halt.log; echo '--- recent files'; find /opt/dtvtuner/tmp /var/opt/dtvtuner /tmp /disk0 -xdev -type f -mmin -10 2>/dev/null | grep -v s1/ | head -80; echo '--- logs'; for f in $(find /opt/dtvtuner/tmp /var/opt/dtvtuner /tmp -xdev -type f \\( -name '*.err' -o -name '*.trc' -o -name '*.log' \\) -size +0 2>/dev/null | grep -v s1/ | head -20); do echo \"## $f\"; tail -n 30 $f; done; echo '--- dmesg'; dmesg; } > /tmp/s1/dbglog.tmp; mv /tmp/s1/dbglog.tmp /tmp/s1/dbglog.txt; sync; sleep 2; done ) &",
                 "sleep 2", "    " + START_LINE, "# ======== end ========"]
        return "\n".join(lines[:idx[0]] + block + lines[idx[0] + 1:])
    tpl = open(os.path.join(HERE, "rcS_block_p3.sh"), encoding="utf-8").read()
    install = open(os.path.join(HERE, "rcS_install_embed.sh"), encoding="utf-8").read() if mode == "embed" else ""
    if pre_snippet:                                  # テスト用: インストール処理の前に差し込む任意のシェル(例: p3をXFSにして疑似録画を置く)
        install = open(pre_snippet, encoding="utf-8").read().rstrip("\n") + "\n" + install
    telnet_cmd = "    telnetd -l /bin/sh &"
    block = (tpl.replace("@INSTALL_BLOCK@", install.rstrip("\n"))
                .replace("@OFFICIAL_PRE@", open(official_pre, encoding="utf-8").read().rstrip("\n") if official_pre else "    :")
                .replace("@TELNET_EARLY@", telnet_cmd if telnet == "early" else "    :")
                .replace("@TELNET_LATE@", "telnetd -l /bin/sh &" if telnet == "fail" else ":"))
    new = lines[:idx[0]] + block.split("\n") + lines[idx[0] + 1:]
    return "\n".join(new)


def make_ti(name, data=None, mode=0o755, typ=tarfile.REGTYPE):
    ti = tarfile.TarInfo(name)
    ti.mode = mode; ti.uid = ti.gid = 0; ti.uname = ti.gname = "root"
    ti.mtime = int(__import__("time").time())
    ti.type = typ
    if data is not None:
        ti.size = len(data)
    return ti


def build(args):
    seg = load_dlm_segment(args.official)
    raw, hdr = dlm_to_tar_bytes(seg)
    print(f"公式rootfs: tar {len(raw)} bytes、ヘッダ date={bytes(hdr[0x14:0x34]).rstrip(b'\\0').decode()!r} +8(major)={int.from_bytes(hdr[8:10], 'big')}")
    if args.mode == "embed" and args.p3_mode == "format" and not args.format_p3:
        raise SystemExit("--p3-mode format は p3 を初期化する(録画データが消える)。同意するなら --format-p3 を付けること(消したくなければ --p3-mode image)")
    if args.mode == "embed" and not args.debian_tree_tar:
        raise SystemExit("--mode embed には --debian-tree-tar (prepare_tree.sh の出力ツリーを tar にしたもの)が必要")

    # 公式の tar には同名エントリが複数ある(後のものが有効)。rcS は最後のものだけを差し替える
    rcs_idx = [i for i, (_, m) in enumerate(iter_members(raw)) if m.name.lstrip("./") == RCS_PATH]
    if not rcs_idx:
        raise SystemExit(f"tar に {RCS_PATH} が無い")
    last_rcs = rcs_idx[-1]
    print(f"rcS のエントリ: {len(rcs_idx)}個(最後の #{last_rcs} を差し替える)")

    out_tar = io.BytesIO()
    gz = gzip.GzipFile(fileobj=out_tar, mode="wb", compresslevel=args.level, mtime=0, filename="tmpdlm.tar")
    tw = tarfile.open(fileobj=gz, mode="w|", format=tarfile.GNU_FORMAT)
    rcs_done = False
    n = 0
    for i, (tf, m) in enumerate(iter_members(raw)):
        if i == last_rcs:
            old = tf.extractfile(m).read().decode("utf-8")
            new = rcs_new(old, args.mode, args.telnet, args.pre_snippet, args.official_pre_snippet).encode("utf-8")
            import copy
            ti = copy.copy(m); ti.size = len(new); ti.type = tarfile.REGTYPE
            tw.addfile(ti, io.BytesIO(new)); rcs_done = True
        elif m.isreg():
            tw.addfile(m, tf.extractfile(m))
        else:
            tw.addfile(m)
        n += 1
    if not rcs_done:
        raise SystemExit(f"tar に {RCS_PATH} が無い")
    extra = 0
    if args.mode == "embed":
        deb = tarfile.open(args.debian_tree_tar, "r:*")
        for m in deb:
            nm = m.name[2:] if m.name.startswith("./") else m.name
            if nm in ("", "."):
                continue
            if nm.startswith("dev/") and (m.ischr() or m.isblk()):
                continue                                  # デバイスノードは同梱しない(nasne_init が自前で作る)
            m.name = "debian/" + nm
            if m.islnk():
                ln = m.linkname[2:] if m.linkname.startswith("./") else m.linkname
                m.linkname = "debian/" + ln
            if m.isreg():
                tw.addfile(m, deb.extractfile(m))
            else:
                tw.addfile(m)
            extra += 1
        d = (b"format\n" if args.p3_mode == "format" else ("image %s\n" % args.image_size).encode())
        tw.addfile(make_ti("debian/etc/nasne-install-mode", d, 0o644), io.BytesIO(d))
        print(f"同梱したDebianのエントリ数: {extra}")
    tw.close(); gz.close()
    body = out_tar.getvalue()
    print(f"新しいボディ: {len(body)} bytes ({len(body) / 1e6:.1f}MB)  公式のエントリ数 {n}")
    if len(body) > 100 * 1024 * 1024:
        print("警告: 100MB超。miniroot は /tmp(tmpfs, RAM)に展開前のファイルを置くので、メモリ不足になりうる", file=sys.stderr)

    # テンプレートヘッダ(入力の .dlm 自身)を使って組み立てる
    import tempfile
    with tempfile.NamedTemporaryFile(delete=False) as t:
        t.write(seg)
    try:
        out = build_dlm.build_custom_dlm(t.name, body, encrypt_body=False)
    finally:
        os.unlink(t.name)
    open(args.out, "wb").write(out)
    ok, details = build_dlm.validate_dlm_bytes(out)
    print(f"{args.out}: {len(out)} bytes  自己検証={'OK' if ok else 'NG'} flag={details.get('flag')} crc={details.get('crc_ok')}")
    if not ok:
        raise SystemExit("自己検証NG")
    # 読み戻し検証: 作った .dlm を再度開き、rcS と全エントリが期待どおりかを確認
    check(args.out, args.mode, raw)


def check(path, mode, orig_raw):
    seg = open(path, "rb").read()
    h = NasneBlowfish().chain_decrypt(seg[:64])
    assert bytes(h[:4]) == DLM_MAGIC and h[0x0E] != 1
    body = seg[64:]
    tar = gzip.decompress(body)
    names = []
    with tarfile.open(fileobj=io.BytesIO(tar), mode="r:") as tf:
        for m in tf:
            names.append(m.name)
            if m.name.lstrip("./") == RCS_PATH:
                rcs = tf.extractfile(m).read().decode()      # 最後のものが有効
    orig_names = [m.name for _, m in iter_members(orig_raw)]
    missing = [x for x in orig_names if x not in names]
    assert not missing, f"元のエントリが欠けている: {missing[:5]}"
    assert MARK_BEGIN in rcs and not any(l == START_LINE for l in rcs.split("\n")), "rcS の差し替えが反映されていない"
    assert "@" not in "".join(l for l in rcs.split("\n") if "@TELNET" in l or "@INSTALL" in l), "プレースホルダが残っている"
    print(f"読み戻し検証OK: エントリ {len(names)}(元 {len(orig_names)})、rcS 差し替え済み、mode={mode}")


def extract_drivers(args):
    seg = load_dlm_segment(args.official)
    raw, _ = dlm_to_tar_bytes(seg)
    want = {"opt/dtvtuner/lib/modules/xcode4drv.ko": "xcode4drv.ko", "opt/dtvtuner/lib/modules/rc.xcode4": "rc.xcode4"}
    os.makedirs(args.dest, exist_ok=True)
    got = set()
    for tf, m in iter_members(raw):
        nm = m.name.lstrip("./")
        if nm in want and m.isreg():
            open(os.path.join(args.dest, want[nm]), "wb").write(tf.extractfile(m).read())
            got.add(nm)
    if len(got) != len(want):
        raise SystemExit(f"ドライバが見つからない: {set(want) - got}")
    print("取り出した:", ", ".join(sorted(want.values())))


def list_cmd(args):
    seg = load_dlm_segment(args.official)
    raw, h = dlm_to_tar_bytes(seg)
    k = 0
    rcs = []
    for tf, m in iter_members(raw):
        if k < 5:
            print(m.name, m.size)
        if m.name.lstrip("./") == RCS_PATH:
            rcs.append(tf.extractfile(m).read().decode())
        k += 1
    print(f"entries: {k}、rcS エントリ: {len(rcs)}個(最後のものが有効)")
    print("--- 最後の rcS の末尾 ---")
    print("\n".join(rcs[-1].split("\n")[-14:]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build"); b.add_argument("--official", required=True); b.add_argument("--out", required=True)
    b.add_argument("--mode", choices=["p3", "embed", "debug"], default="p3"); b.add_argument("--debian-tree-tar")
    b.add_argument("--format-p3", action="store_true"); b.add_argument("--p3-mode", choices=["image", "format"], default="image"); b.add_argument("--image-size", default="8G"); b.add_argument("--telnet", choices=["early", "fail", "off"], default="early")
    b.add_argument("--level", type=int, default=6); b.add_argument("--official-pre-snippet", help="テスト用: 公式アプリ(official-once)を起動する直前に実行するシェルのファイル"); b.add_argument("--pre-snippet", help="テスト用: インストール処理の前に rcS へ差し込むシェルのファイル")
    e = sub.add_parser("extract-drivers"); e.add_argument("--official", required=True); e.add_argument("--dest", required=True)
    l = sub.add_parser("list"); l.add_argument("--official", required=True)
    a = ap.parse_args()
    {"build": build, "extract-drivers": extract_drivers, "list": list_cmd}[a.cmd](a)


if __name__ == "__main__":
    main()
