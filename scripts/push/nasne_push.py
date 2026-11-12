#!/usr/bin/env python3
"""
nasne_push.py — 公式 FW アップデート経路 (webapi 64210) で .dlm を機体に適用するツール

方式の全体像・前提・エラーコード表は docs/25_official_update_push.md を参照。

できること:
  inspect  .dlm / KRST パッケージのオフライン検査
           (セグメント分割、ヘッダ復号、CRC32 検証、宣言サイズ照合、
            版表示の算出、SHA-256)
  push     検査 → 機体の現行バージョン取得と比較 (版ゲート事前チェック) →
           ファイル配信 (HTTP サーブ) → updateFileDownload 送信 →
           updateProgressGet 監視 → 再起動後の版表示で成否判定

安全弁:
  - --yes を付けない限り送信しない (付けなければドライラン: 検査と計画表示のみ)。
    機体への読み取り (GET) はドライランでも行うことがある。
  - 版ゲート (現行より新しくない .dlm は reason 8 で却下される) に
    引っかかる場合は、送信前に警告して止める。
  - CRC 不一致・サイズ不整合のファイルは送信しない。

終了コード:
  0 成功 (またはドライラン完了) / 1 一般エラー / 2 ファイル検査 NG /
  3 版ゲート事前チェックで停止 / 4 機体に却下された (reason != 0) /
  5 適用後にフォールバックした / 6 判定不能 (タイムアウト等)

Python 標準ライブラリのみ。ヘッダ復号に同リポの scripts/dlm_crypto.py を使う。
"""

import argparse
import base64
import hashlib
import http.server
import importlib.util
import json
import socket
import struct
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib
from pathlib import Path

# ---------------------------------------------------------------- dlm_crypto

def _load_blowfish():
    """scripts/dlm_crypto.py の NasneBlowfish を、配置場所から相対で読み込む。"""
    here = Path(__file__).resolve()
    candidates = [
        here.parent.parent / "dlm_crypto.py",   # scripts/push/ -> scripts/
        here.parent / "dlm_crypto.py",
    ]
    for cand in candidates:
        if cand.exists():
            spec = importlib.util.spec_from_file_location("dlm_crypto", cand)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            return mod.NasneBlowfish
    raise RuntimeError(
        "dlm_crypto.py が見つかりません (scripts/push/ の親に dlm_crypto.py がある構成が必要です)"
    )


# ---------------------------------------------------------------- 検査 (inspect)

KNOWN_MAGICS = {b"KNL\x00": "KNL", b"RFS\x00": "RFS", b"DLM\x00": "DLM"}
HEADER_LEN = 64
CRC_FIELD_OFF = 0x3C


def display_version(major: int, sub: int) -> str:
    """版表示 = major と +0x0A から構成される 4 桁表記 (例: major=2, sub=60 -> '0260')。"""
    return f"0{major}{sub:02d}"


def version_key(major: int, sub: int) -> int:
    """版の新旧比較用キー。表示表記の数値と一致する (0260 -> 260, 0366 -> 366)。"""
    return major * 100 + sub


def inspect_package(path: Path) -> dict:
    """パッケージをセグメント列に分解し、各セグメントのヘッダと CRC を検証する。

    戻り値: {
      'path', 'size', 'sha256', 'segments': [ {...} ], 'ok': bool,
      'errors': [str], 'package_version': {'major','sub','display','key'} or None
    }
    package_version は DLM セグメント (rootfs) のもの。DLM が無ければ最後のセグメント。
    """
    NasneBlowfish = _load_blowfish()
    bf = NasneBlowfish()
    data = path.read_bytes()
    result = {
        "path": str(path),
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "segments": [],
        "errors": [],
    }
    off = 0
    idx = 0
    while off < len(data):
        if off + HEADER_LEN > len(data):
            result["errors"].append(
                f"offset 0x{off:x}: ヘッダ 64 バイトに満たない残り {len(data) - off} バイト"
            )
            break
        hdr = bf.chain_decrypt(data[off:off + HEADER_LEN])
        magic_raw = hdr[0:4]
        magic = KNOWN_MAGICS.get(magic_raw)
        size = struct.unpack(">I", hdr[4:8])[0]
        major = struct.unpack(">H", hdr[8:10])[0]
        sub = struct.unpack(">H", hdr[10:12])[0]
        f0c = struct.unpack(">H", hdr[12:14])[0]
        flag = hdr[14]
        hwtype = hdr[16:20].hex()
        datestr = hdr[20:44].split(b"\x00")[0].decode("ascii", "replace")
        stored_crc = struct.unpack(">I", hdr[CRC_FIELD_OFF:CRC_FIELD_OFF + 4])[0]
        seg = {
            "index": idx,
            "offset": off,
            "magic": magic,
            "magic_raw": magic_raw,
            "size": size,
            "major": major,
            "sub": sub,
            "f0c": f0c,
            "flag": flag,
            "hwtype": hwtype,
            "date": datestr,
            "stored_crc": stored_crc,
            "crc_ok": None,
        }
        if magic is None:
            result["errors"].append(
                f"seg{idx} offset 0x{off:x}: 未知のマジック {magic_raw!r} (ヘッダ復号に失敗した可能性)"
            )
            result["segments"].append(seg)
            break
        if size < HEADER_LEN:
            result["errors"].append(f"seg{idx}: 宣言サイズ {size} がヘッダ長未満")
            result["segments"].append(seg)
            break
        if off + size > len(data):
            result["errors"].append(
                f"seg{idx}: 宣言サイズ {size} がファイル残量 {len(data) - off} を超過 (切り詰めの疑い)"
            )
            result["segments"].append(seg)
            break
        body = data[off + HEADER_LEN:off + size]
        h2 = bytearray(hdr)
        h2[CRC_FIELD_OFF:CRC_FIELD_OFF + 4] = b"\x00\x00\x00\x00"
        calc_crc = zlib.crc32(body, zlib.crc32(bytes(h2))) & 0xFFFFFFFF
        seg["calc_crc"] = calc_crc
        seg["crc_ok"] = (calc_crc == stored_crc)
        if not seg["crc_ok"]:
            result["errors"].append(
                f"seg{idx} ({magic}): CRC32 不一致 stored={stored_crc:08x} calc={calc_crc:08x}"
            )
        result["segments"].append(seg)
        off += size
        idx += 1
    if off != len(data):
        result["errors"].append(
            f"セグメント合計 {off} バイト != ファイルサイズ {len(data)} バイト"
        )
    # パッケージの版 = DLM セグメント (無ければ最後のセグメント)
    version_seg = None
    for seg in result["segments"]:
        if seg["magic"] == "DLM":
            version_seg = seg
    if version_seg is None and result["segments"]:
        version_seg = result["segments"][-1]
    if version_seg is not None and version_seg["magic"] is not None:
        result["package_version"] = {
            "major": version_seg["major"],
            "sub": version_seg["sub"],
            "display": display_version(version_seg["major"], version_seg["sub"]),
            "key": version_key(version_seg["major"], version_seg["sub"]),
        }
    else:
        result["package_version"] = None
    result["ok"] = not result["errors"] and bool(result["segments"])
    return result


def print_inspection(res: dict) -> None:
    print(f"ファイル: {res['path']}")
    print(f"サイズ  : {res['size']} バイト")
    print(f"SHA-256 : {res['sha256']}")
    for seg in res["segments"]:
        name = seg["magic"] or f"不明 {seg['magic_raw']!r}"
        crc = "OK" if seg["crc_ok"] else ("NG" if seg["crc_ok"] is False else "-")
        print(
            f"  seg{seg['index']} @{seg['offset']:>9} {name:<4} size={seg['size']:>9} "
            f"major={seg['major']} +0x0A=0x{seg['sub']:04x} +0x0C=0x{seg['f0c']:04x} "
            f"flag={seg['flag']} hwtype={seg['hwtype']} CRC={crc} date={seg['date']!r}"
        )
    pv = res.get("package_version")
    if pv:
        print(f"版表示 (DLM セグメント基準): {pv['display']} (major={pv['major']}, +0x0A={pv['sub']})")
    for err in res["errors"]:
        print(f"  [NG] {err}")
    print(f"検査結果: {'OK' if res['ok'] else 'NG'}")


# ---------------------------------------------------------------- webapi

def api_get(base: str, path: str, timeout: float = 10.0) -> dict:
    with urllib.request.urlopen(base + path, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def dtv_token(pin: str, path: str, body: str) -> str:
    """X-DTVToken = base64(SHA1(PIN + path + body[:256]))"""
    digest = hashlib.sha1((pin + path + body[:256]).encode("utf-8")).digest()
    return base64.b64encode(digest).decode("ascii")


def api_post_with_token(base: str, path: str, pin: str, body: str, timeout: float = 15.0) -> dict:
    req = urllib.request.Request(
        base + path,
        data=body.encode("utf-8"),
        method="POST",
        headers={
            "Content-Type": "application/x-www-form-urlencoded",
            "X-DTVToken": dtv_token(pin, path, body),
        },
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def get_current_version(base: str, timeout: float = 10.0) -> dict:
    """softwareVersionGet を引き、現行の版表示と backdatedVersion を返す。"""
    info = api_get(base, "/status/softwareVersionGet", timeout=timeout)
    if info.get("errorcode") != 0 or "softwareVersion" not in info:
        raise RuntimeError(f"softwareVersionGet の応答が想定外: {info}")
    cur = str(info["softwareVersion"])
    return {
        "display": cur,
        "key": int(cur),
        "backdated": str(info.get("backdatedVersion", "0000")),
        "raw": info,
    }


def get_pin(base: str, timeout: float = 10.0) -> tuple[str, str]:
    """networkIfInfoGet の macAddr から PIN (下位 3 オクテット、コロン無し) を導出する。"""
    info = api_get(base, "/status/networkIfInfoGet", timeout=timeout)
    mac = info.get("macAddr")
    if not mac:
        raise RuntimeError(f"networkIfInfoGet の応答に macAddr が無い: {info}")
    return mac.replace(":", "")[6:], mac


# ---------------------------------------------------------------- 配信サーバー

class _SingleFileHandler(http.server.BaseHTTPRequestHandler):
    serve_path: Path = None      # 実ファイル
    url_name: str = ""           # 配信する URL パス ("/<basename>")
    on_get = None                # GET 完了時のコールバック

    def _serve(self, head_only: bool):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path != self.url_name:
            self.send_error(404)
            return
        data_len = self.serve_path.stat().st_size
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(data_len))
        self.end_headers()
        if not head_only:
            with open(self.serve_path, "rb") as f:
                while True:
                    chunk = f.read(1024 * 1024)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
            if self.on_get:
                self.on_get()

    def do_GET(self):
        self._serve(False)

    def do_HEAD(self):
        self._serve(True)

    def log_message(self, fmt, *args):  # 既定のアクセスログを抑える
        pass


def start_server(file_path: Path, bind_ip: str, port: int) -> tuple[http.server.ThreadingHTTPServer, threading.Thread, dict]:
    state = {"downloads": 0}

    def _count():
        state["downloads"] += 1

    handler = type(
        "Handler", (_SingleFileHandler,),
        {
            "serve_path": file_path,
            "url_name": "/" + urllib.parse.quote(file_path.name),
            "on_get": staticmethod(_count),
        },
    )
    httpd = http.server.ThreadingHTTPServer((bind_ip, port), handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    return httpd, thread, state


def detect_serve_ip(target_ip: str) -> str:
    """対象機へ向ける経路の自側 IP を UDP ソケットの getsockname で求める。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((target_ip, 64210))
        return s.getsockname()[0]
    finally:
        s.close()


# ---------------------------------------------------------------- 監視

REASON_HINTS = {
    0: "正常",
    1: "適用段: ヘッダ検査 (areaheadCheck) の失敗",
    2: "適用段: データ部の CRC32 不一致 (ファイル破損・転送異常の可能性)",
    3: "適用段: ファイル終端処理の異常",
    4: "適用段: sys1 への書出し失敗 (主因は空き容量不足。退避コピー + 新規書込みのピークが空きを超えた)",
    5: "ダウンロード段: ネットワークエラー (bad address / No route to host / Connection timed out)",
    6: "ダウンロード段: 機体の状態変化による中断",
    7: "ダウンロード段: 時間超過で取得を打ち切った",
    8: "版ゲート (現行より新しくない .dlm は同版・旧版とも却下される。forcedUpdate でも回避できない)。"
       "ただし他の適用段・ダウンロード段の失敗も reason 8 に合流する",
}


def watch_and_judge(base: str, target_display: str, pre_progress: dict,
                    timeout_s: float, interval: float, log=print) -> tuple[str, dict]:
    """updateProgressGet を監視し、適用段の終端判定を返す。

    戻り値: (verdict, detail)
      verdict: 'applied' (適用段が正常終了。版確認は呼び出し側) /
               'rejected' (reason != 0 で終了) /
               'fallback' (backdatedVersion が対象版 = 適用後に前版へ戻った) /
               'timeout'

    判定の根拠は進捗 API だけにしない。送信前の updateProgressGet には
    前回試行の最終状態が残留し得るうえ、再起動直前はポーリング間隔の間で
    status=0 / 100% の終端サンプルを取り逃すことがある (実機で確認済みの事例)。
    そこで、処理中を見た後・高進捗率・API 無応答からの復帰といった終盤の
    兆候では softwareVersionGet も併せて確認し、現行版が対象版に到達して
    いて backdatedVersion が対象版でなければ、それを適用成功の終端証拠
    として扱う。逆に backdatedVersion が対象版ならフォールバックである。
    送信前と同一の残留値だけでは却下と判定しない。
    """
    deadline = time.time() + timeout_s
    pre_state = (
        pre_progress.get("status"),
        pre_progress.get("completePercent"),
        pre_progress.get("reason"),
    )
    saw_progress = False
    api_was_unavailable = False
    last_state = None
    last_prog = None
    last_version_state = None
    last_version_check_at = 0.0
    # 版確認は終盤だけに絞るが、既定 5 秒間隔では遅すぎるため最大でも
    # 1 秒に 1 回まで許可する (テスト用の短い間隔は 0.5 秒を下限にする)。
    version_check_interval = max(0.5, min(interval, 1.0))

    def version_evidence() -> tuple[str, dict] | None:
        """現時点の版表示が終端証拠になれば (verdict, detail) を返す。"""
        nonlocal last_version_state
        try:
            cur = get_current_version(base, timeout=8.0)
        except Exception:
            return None
        version_state = (cur["display"], cur["backdated"])
        if version_state != last_version_state:
            log(
                "    version evidence: "
                f"current={cur['display']} backdated={cur['backdated']}"
            )
            last_version_state = version_state
        if cur["display"] == target_display and cur["backdated"] != target_display:
            return "applied", {
                "progress": last_prog,
                "version": cur,
                "evidence": "softwareVersionGet",
            }
        if cur["backdated"] == target_display and cur["display"] != target_display:
            return "fallback", {
                "progress": last_prog,
                "version": cur,
                "evidence": "softwareVersionGet",
            }
        return None

    while time.time() < deadline:
        try:
            prog = api_get(base, "/config/updateProgressGet", timeout=8.0)
        except Exception as e:  # 再起動中など
            api_was_unavailable = True
            log(f"    progress 取得不可 ({type(e).__name__}) — 再起動中の可能性、そのまま監視継続")
            # 再起動兆候の後は終端サンプルを取り逃さないよう間隔を詰める。
            time.sleep(min(interval, 1.0) if saw_progress else interval)
            continue

        last_prog = prog
        state = (prog.get("status"), prog.get("completePercent"), prog.get("reason"))
        if state != last_state:
            log(f"    progress: status={state[0]} percent={state[1]} reason={state[2]}")
            last_state = state
        status, percent, reason = state
        if status == 2:
            saw_progress = True
        changed_from_pre = state != pre_state

        # 進捗 API 自身が示す終端を先に判定する。送信前から変化していない
        # 残留値は、ここでは終端として扱わない。
        if (saw_progress or changed_from_pre) and percent == 100:
            if reason not in (0, None):
                return "rejected", {"progress": prog}
            if status == 0:
                return "applied", {"progress": prog}

        # 終端サンプルを取り逃した場合の保険: 終盤の兆候があるときだけ
        # 版表示を確認する。対象版への到達は、進捗の残留値が送信前と
        # 同一でも成功の証拠になる。一方 backdatedVersion が対象版なら
        # フォールバックであり、成功とは区別する。
        percent_num = percent if isinstance(percent, (int, float)) else None
        evidence_due = (
            (saw_progress or changed_from_pre or api_was_unavailable)
            and (
                api_was_unavailable
                or state == pre_state
                or (percent_num is not None and percent_num >= 90)
            )
        )
        now = time.time()
        if evidence_due and (
            api_was_unavailable or now - last_version_check_at >= version_check_interval
        ):
            checked = version_evidence()
            last_version_check_at = now
            api_was_unavailable = False
            if checked is not None:
                return checked

        # 終盤 (高進捗率、または再起動から復帰した直後) はポーリングを詰める。
        if percent_num is not None and percent_num >= 90:
            time.sleep(min(interval, 1.0))
        else:
            time.sleep(interval)
    return "timeout", {"last_state": last_state}


def confirm_version(base: str, target_display: str, timeout_s: float,
                    interval: float, log=print) -> tuple[str, dict]:
    """再起動後の softwareVersionGet で最終判定する。

    'success'  : 現行版が対象版になった
    'fallback' : backdatedVersion が対象版 (適用はされたが起動に失敗し前版へ戻った)
    'other'    : 応答はあるが版が想定と違う
    'timeout'  : 期限内に版が取得できなかった
    """
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            cur = get_current_version(base, timeout=8.0)
        except Exception:
            time.sleep(interval)
            continue
        log(f"    version: current={cur['display']} backdated={cur['backdated']}")
        if cur["display"] == target_display:
            return "success", cur
        if cur["backdated"] == target_display:
            return "fallback", cur
        return "other", cur
    return "timeout", {}


# ---------------------------------------------------------------- CLI

def cmd_inspect(args) -> int:
    res = inspect_package(Path(args.file))
    print_inspection(res)
    return 0 if res["ok"] else 2


def cmd_push(args) -> int:
    file_path = Path(args.file)
    if not file_path.exists():
        print(f"ファイルがありません: {file_path}", file=sys.stderr)
        return 1

    # 1. 事前検査 (オフライン)
    print("== 事前検査 ==")
    res = inspect_package(file_path)
    print_inspection(res)
    if not res["ok"]:
        print("検査 NG のため送信しません。", file=sys.stderr)
        return 2
    pv = res["package_version"]

    base = f"http://{args.host}:{args.api_port}"

    # 2. 機体の現行バージョンと PIN
    print("== 機体の状態 ==")
    try:
        current = get_current_version(base)
        if args.pin:
            pin = args.pin
            print(f"現行バージョン: {current['display']} (backdated: {current['backdated']})")
            print(f"PIN: --pin で指定された値を使用")
        else:
            pin, mac = get_pin(base)
            print(f"現行バージョン: {current['display']} (backdated: {current['backdated']})")
            print(f"MAC: {mac} -> PIN (下位3オクテット): {pin}")
        pre_progress = api_get(base, "/config/updateProgressGet")
        print(f"送信前の updateProgressGet: {pre_progress}")
    except Exception as e:
        print(f"機体への問い合わせに失敗: {e}", file=sys.stderr)
        return 1

    # 3. 版ゲート事前チェック
    print("== 版ゲート事前チェック ==")
    print(f"対象 .dlm の版: {pv['display']} / 機体の現行版: {current['display']}")
    if pv["key"] <= current["key"]:
        print(
            "警告: 対象の版が現行より新しくありません。公式の版ゲートにより "
            "reason 8 で却下されることが確定しているため、送信しません。",
            file=sys.stderr,
        )
        return 3
    print("対象の方が新しいため、版ゲートは通過見込み。")

    # 4. 配信サーバーの準備
    serve_ip = args.serve_ip or detect_serve_ip(args.host)
    file_url = f"http://{serve_ip}:{args.serve_port}/" + urllib.parse.quote(file_path.name)
    body = f"fileURL={urllib.parse.quote(file_url, safe='')}&forcedUpdate=1"
    print("== 送信計画 ==")
    print(f"配信 URL : {file_url}")
    print(f"POST 先  : {base}/config/updateFileDownload")
    print(f"POST body: {body}")
    print(f"SHA-256  : {res['sha256']}")
    print("注意: 送信すると機体の版フロアが消費されます (同じ版・旧版は二度と適用できません)。")

    if args.dry_run or not args.yes:
        print("ドライラン: 送信はしません (--yes を付けると送信します)。")
        return 0

    httpd, _thread, dl_state = start_server(file_path, "0.0.0.0", args.serve_port)
    try:
        # 5. 送信
        print("== 送信 ==")
        try:
            resp = api_post_with_token(base, "/config/updateFileDownload", pin, body)
        except Exception as e:
            print(f"updateFileDownload の送信に失敗: {e}", file=sys.stderr)
            return 1
        print(f"updateFileDownload 応答: {resp}")
        if resp.get("errorcode") != 0:
            print("errorcode != 0 のため中止します (認証不一致・URL 拒否など)。", file=sys.stderr)
            return 1

        # 6. 監視と判定
        print("== 進捗監視 ==")
        verdict, detail = watch_and_judge(
            base, pv["display"], pre_progress,
            timeout_s=args.timeout, interval=args.poll_interval,
        )
        if verdict == "rejected":
            prog = detail["progress"]
            reason = prog.get("reason")
            hint = REASON_HINTS.get(reason, "未知の reason。docs/25_official_update_push.md のエラーコード表と突き合わせること")
            print(f"機体に却下されました: status={prog.get('status')} reason={reason} ({hint})")
            return 4
        if verdict == "fallback":
            ver = detail["version"]
            print(
                "== 結果: フォールバック == 適用はされましたが起動に失敗し、"
                f"前版 {ver['display']} に戻りました (backdatedVersion={ver['backdated']})。"
                " sys1 のログ回収と切り分けを行ってください。反射的に次版を投げないこと。"
            )
            return 5
        if verdict == "timeout":
            print("期限内に適用段の終了を確認できませんでした。機体の状態を別途確認してください。")
            return 6

        print("適用段は正常終了。再起動後の版を確認します (フォールバック検出を含む)。")
        vres, vdetail = confirm_version(
            base, pv["display"],
            timeout_s=max(args.timeout, 600.0), interval=args.poll_interval,
        )
        print(f"機体からの .dlm 取得回数: {dl_state['downloads']}")
        if vres == "success":
            print(f"== 結果: 成功 == 機体は版 {pv['display']} で動作しています。")
            return 0
        if vres == "fallback":
            print(
                f"== 結果: フォールバック == 適用はされましたが起動に失敗し、"
                f"前版 {vdetail['display']} に戻りました (backdatedVersion={vdetail['backdated']})。"
                " sys1 のログ回収と切り分けを行ってください。反射的に次版を投げないこと。"
            )
            return 5
        if vres == "other":
            print(f"== 結果: 想定外の版 == current={vdetail.get('display')} backdated={vdetail.get('backdated')}")
            return 6
        print("== 結果: 判定不能 == 再起動後の版を取得できませんでした。時間を置いて version だけ再確認してください。")
        return 6
    finally:
        httpd.shutdown()


def main() -> int:
    ap = argparse.ArgumentParser(description="nasne 公式アップデート経路 push ツール")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_insp = sub.add_parser("inspect", help=".dlm / KRST のオフライン検査")
    p_insp.add_argument("file")
    p_insp.set_defaults(func=cmd_inspect)

    p_push = sub.add_parser("push", help="検査→版チェック→配信→送信→監視→判定")
    p_push.add_argument("file")
    p_push.add_argument("--host", required=True, help="nasne の IP / ホスト名")
    p_push.add_argument("--api-port", type=int, default=64210, help="webapi のポート (既定 64210)")
    p_push.add_argument("--serve-ip", default=None, help="配信サーバーの自側 IP (既定: 経路から自動検出)")
    p_push.add_argument("--serve-port", type=int, default=18080, help="配信サーバーのポート (既定 18080)")
    p_push.add_argument("--pin", default=None, help="PIN を直接指定 (既定: networkIfInfoGet の MAC から導出)")
    p_push.add_argument("--poll-interval", type=float, default=5.0, help="進捗ポーリング間隔 秒 (既定 5)")
    p_push.add_argument("--timeout", type=float, default=900.0, help="適用監視の期限 秒 (既定 900)")
    p_push.add_argument("--dry-run", action="store_true", help="送信せず検査と計画表示のみ")
    p_push.add_argument("--yes", action="store_true", help="実際に送信する (この明示フラグが無い限り送信しない)")
    p_push.set_defaults(func=cmd_push)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
