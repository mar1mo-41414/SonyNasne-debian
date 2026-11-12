#!/usr/bin/env python3
"""
test_nasne_push.py — nasne_push.py のオフラインテスト (実機には一切触れない)

内容:
  1. inspect: 合成フィクスチャ (公式相当の KRST・自作相当の KRST・単体 .dlm) の
     セグメント解析・CRC 検証・版算出
  2. モック webapi (localhost) に対する push の通し:
     a. 正常系: トークン検証つきで受付 → 進捗遷移 → 版が上がって成功判定、
        かつモックが配信 URL から実ファイルを取得できること
     b. 却下系: 進捗が reason 8 で終了 → 終了コード 4
     c. 版ゲート事前チェック: 現行版 >= 対象版 → 送信せず終了コード 3 (POST が来ないこと)
     d. --yes 無し: ドライランで POST が来ないこと
     e. 終端サンプル取り逃し系: 進捗が 97% の次に送信前と同一の残留値へ戻り、
        status=0 / 100% を観測できないが、版表示は対象版へ到達 → 成功判定

フィクスチャは実ファイルを使わず、このテストが dlm_crypto.py の NasneBlowfish で
ヘッダを組み立てて合成する (実パッケージと同じヘッダ形式・CRC 方式)。
使い方: python3 test_nasne_push.py
  scripts/push/ から実行しても、リポ直下から実行してもよい (相対で解決する)。
"""
import hashlib
import http.server
import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
import threading
import urllib.parse
import urllib.request
import zlib
from pathlib import Path

HERE = Path(__file__).resolve()
TOOL = HERE.parent / "nasne_push.py"


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# scripts/dlm_crypto.py は scripts/push/ の親 (scripts/) にある
_dlmc = _load("dlm_crypto", HERE.parent.parent / "dlm_crypto.py")
NasneBlowfish = _dlmc.NasneBlowfish

PASS = 0
FAIL = 0


def check(name, cond, extra=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  PASS {name}")
    else:
        FAIL += 1
        print(f"  FAIL {name} {extra}")


def run_tool(*argv, timeout=120):
    return subprocess.run(
        [sys.executable, str(TOOL), *argv],
        capture_output=True, text=True, timeout=timeout,
    )


# ---------------------------------------------------------------- フィクスチャ合成

def make_segment(magic: bytes, major: int, sub: int, flag: int, body: bytes) -> bytes:
    """実パッケージと同じ形式のセグメント (暗号化ヘッダ + ボディ) を合成する。"""
    hdr = bytearray(64)
    hdr[0:4] = magic
    hdr[4:8] = struct.pack(">I", 64 + len(body))
    hdr[8:10] = struct.pack(">H", major)
    hdr[10:12] = struct.pack(">H", sub)
    hdr[14] = flag
    hdr[16:20] = bytes.fromhex("03010000")          # hwtype (公式パッケージの実測値)
    date = b"Mon Jun 17 18:31:10 2013"
    hdr[20:20 + len(date)] = date
    crc = zlib.crc32(body, zlib.crc32(bytes(hdr))) & 0xFFFFFFFF
    hdr[0x3C:0x40] = struct.pack(">I", crc)
    return NasneBlowfish().chain_encrypt(bytes(hdr)) + body


def make_fixtures(dirpath: Path) -> dict:
    """公式相当 KRST (版 0260)、自作相当 KRST (版 0368)、単体 DLM (版 0368) を作る。"""
    body = (b"nasne-push-test\x00" * 300)  # 決定的なダミーボディ
    knl = make_segment(b"KNL\x00", 1, 0x48, 1, body[:2048])
    rfs = make_segment(b"RFS\x00", 1, 0x32, 0, body[:1024])
    dlm_official = make_segment(b"DLM\x00", 2, 60, 1, body)
    dlm_custom = make_segment(b"DLM\x00", 3, 68, 0, body)
    files = {
        "krst_official": dirpath / "krst_official.dlm",
        "krst_custom": dirpath / "krst_custom.dlm",
        "dlm_custom": dirpath / "custom_00550066.dlm",
    }
    files["krst_official"].write_bytes(knl + rfs + dlm_official)
    files["krst_custom"].write_bytes(knl + rfs + dlm_custom)
    files["dlm_custom"].write_bytes(dlm_custom)
    return files


# ---------------------------------------------------------------- モック webapi

# モックが返す MAC はダミー値 (実在の個体情報ではない)
MOCK_MAC = "00:25:dc:aa:bb:cc"
MOCK_PIN = "aabbcc"          # MOCK_MAC の下位 3 オクテット


class MockState:
    def __init__(self, current_version="0260", mode="success", pre_progress=None):
        self.current = current_version
        self.mode = mode            # success / reject / missed_terminal
        self.posts = []             # 受けた POST (path, token, body)
        self.poll_count = 0
        self.downloaded = None      # fileURL から取得した内容の sha256
        self.pre_progress = pre_progress or {
            "errorcode": 0, "status": 0, "completePercent": 100, "reason": 0,
        }
        self.last_progress = None   # 最後に返した updateProgressGet


class MockHandler(http.server.BaseHTTPRequestHandler):
    state: MockState = None

    def log_message(self, *a):
        pass

    def _json(self, obj):
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        st = self.state
        if self.path == "/status/networkIfInfoGet":
            self._json({"errorcode": 0, "macAddr": MOCK_MAC})
        elif self.path == "/status/softwareVersionGet":
            self._json({"errorcode": 0, "softwareVersion": st.current,
                        "backdatedVersion": "0000"})
        elif self.path == "/config/updateProgressGet":
            if not st.posts:
                st.last_progress = dict(st.pre_progress)
                self._json(st.last_progress)
                return
            st.poll_count += 1
            if st.mode == "reject":
                seq = [
                    {"errorcode": 0, "status": 2, "completePercent": 40, "reason": 0},
                    {"errorcode": 0, "status": 1, "completePercent": 100, "reason": 8},
                ]
                prog = seq[min(st.poll_count - 1, len(seq) - 1)]
            elif st.mode == "missed_terminal":
                # 実機で確認済みの事例: 97% の直後に再起動し、終端サンプル
                # (status=0 / 100%) を返さないまま、送信前と同一の残留値
                # (status=1 / 0% / reason=9) に戻る。ただし再起動後の
                # 版表示は対象版へ到達している。
                if st.poll_count == 1:
                    prog = {"errorcode": 0, "status": 2, "completePercent": 0, "reason": 0}
                elif st.poll_count == 2:
                    prog = {"errorcode": 0, "status": 2, "completePercent": 97, "reason": 0}
                else:
                    st.current = "0368"
                    prog = dict(st.pre_progress)
            else:
                seq = [
                    {"errorcode": 0, "status": 2, "completePercent": 40, "reason": 0},
                    {"errorcode": 0, "status": 2, "completePercent": 80, "reason": 0},
                    {"errorcode": 0, "status": 0, "completePercent": 100, "reason": 0},
                ]
                prog = seq[min(st.poll_count - 1, len(seq) - 1)]
                if prog["status"] == 0:
                    st.current = "0368"
            st.last_progress = dict(prog)
            self._json(prog)
        else:
            self.send_error(404)

    def do_POST(self):
        st = self.state
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode()
        st.posts.append((self.path, self.headers.get("X-DTVToken"), body))
        # fileURL を実際に取りに行く (機体の取得段の模擬)
        params = urllib.parse.parse_qs(body)
        url = params["fileURL"][0]
        try:
            with urllib.request.urlopen(url, timeout=10) as resp:
                st.downloaded = hashlib.sha256(resp.read()).hexdigest()
        except Exception as e:
            st.downloaded = f"ERROR {e}"
        self._json({"errorcode": 0})


def start_mock(state):
    handler = type("H", (MockHandler,), {"state": state})
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, httpd.server_address[1]


def expected_token(pin, path, body):
    import base64
    digest = hashlib.sha1((pin + path + body[:256]).encode()).digest()
    return base64.b64encode(digest).decode()


def free_port():
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


# ---------------------------------------------------------------- テスト本体

def main():
    tmp = Path(tempfile.mkdtemp(prefix="nasne_push_test_"))
    fx = make_fixtures(tmp)
    print(f"fixtures: {tmp}")

    print("== 1. inspect ==")
    for key, want_ver, want_nseg in (
        ("krst_official", "0260", 3),
        ("krst_custom", "0368", 3),
        ("dlm_custom", "0368", 1),
    ):
        path = fx[key]
        r = run_tool("inspect", str(path))
        ok = (r.returncode == 0 and f"版表示 (DLM セグメント基準): {want_ver}" in r.stdout
              and "検査結果: OK" in r.stdout and r.stdout.count("CRC=OK") >= want_nseg)
        check(f"inspect {path.name} -> {want_ver}", ok,
              f"rc={r.returncode}\n{r.stdout}\n{r.stderr}")

    print("== 1b. inspect 負系 (ボディ破損で CRC NG) ==")
    bad = tmp / "broken.dlm"
    data = bytearray(fx["dlm_custom"].read_bytes())
    data[-1] ^= 0xFF
    bad.write_bytes(bytes(data))
    r = run_tool("inspect", str(bad))
    check("CRC 不一致を検出して終了コード 2", r.returncode == 2 and "検査結果: NG" in r.stdout,
          f"rc={r.returncode}\n{r.stdout}")

    krst_custom = fx["krst_custom"]

    print("== 2a. push 正常系 (モック) ==")
    st = MockState(current_version="0260", mode="success")
    httpd, api_port = start_mock(st)
    serve_port = free_port()
    try:
        r = run_tool("push", str(krst_custom), "--host", "127.0.0.1",
                     "--api-port", str(api_port), "--serve-ip", "127.0.0.1",
                     "--serve-port", str(serve_port), "--poll-interval", "0.2",
                     "--timeout", "60", "--yes", timeout=120)
        check("終了コード 0", r.returncode == 0, f"rc={r.returncode}\n{r.stdout}\n{r.stderr}")
        check("成功判定の表示", "結果: 成功" in r.stdout)
        check("POST が 1 回", len(st.posts) == 1, f"posts={len(st.posts)}")
        if st.posts:
            path, token, body = st.posts[0]
            check("POST 先", path == "/config/updateFileDownload")
            check("X-DTVToken が式どおり", token == expected_token(MOCK_PIN, path, body),
                  f"token={token}")
            check("body に forcedUpdate=1", "forcedUpdate=1" in body)
        want_sha = hashlib.sha256(krst_custom.read_bytes()).hexdigest()
        check("配信ファイルを取得できた", st.downloaded == want_sha, f"got={st.downloaded}")
    finally:
        httpd.shutdown()

    print("== 2b. push 却下系 (reason 8) ==")
    st = MockState(current_version="0260", mode="reject")
    httpd, api_port = start_mock(st)
    try:
        r = run_tool("push", str(krst_custom), "--host", "127.0.0.1",
                     "--api-port", str(api_port), "--serve-ip", "127.0.0.1",
                     "--serve-port", str(free_port()), "--poll-interval", "0.2",
                     "--timeout", "60", "--yes", timeout=120)
        check("終了コード 4", r.returncode == 4, f"rc={r.returncode}\n{r.stdout}")
        check("版ゲートの説明つき", "版ゲート" in r.stdout)
    finally:
        httpd.shutdown()

    print("== 2c. 版ゲート事前チェック (現行 0368 に 0368 を投げる) ==")
    st = MockState(current_version="0368", mode="success")
    httpd, api_port = start_mock(st)
    try:
        r = run_tool("push", str(krst_custom), "--host", "127.0.0.1",
                     "--api-port", str(api_port), "--serve-ip", "127.0.0.1",
                     "--serve-port", str(free_port()), "--poll-interval", "0.2",
                     "--yes", timeout=60)
        check("終了コード 3", r.returncode == 3, f"rc={r.returncode}\n{r.stdout}")
        check("POST していない", len(st.posts) == 0)
    finally:
        httpd.shutdown()

    print("== 2d. --yes 無しはドライラン ==")
    st = MockState(current_version="0260", mode="success")
    httpd, api_port = start_mock(st)
    try:
        r = run_tool("push", str(krst_custom), "--host", "127.0.0.1",
                     "--api-port", str(api_port), "--serve-ip", "127.0.0.1",
                     "--serve-port", str(free_port()), "--poll-interval", "0.2",
                     timeout=60)
        check("終了コード 0", r.returncode == 0, f"rc={r.returncode}\n{r.stdout}")
        check("ドライラン表示", "ドライラン" in r.stdout)
        check("POST していない", len(st.posts) == 0)
    finally:
        httpd.shutdown()

    print("== 2e. 終端サンプル取り逃し (残留値が送信前と同一でも版到達で成功) ==")
    pre_progress = {"errorcode": 0, "status": 1, "completePercent": 0, "reason": 9}
    st = MockState(current_version="0366", mode="missed_terminal",
                   pre_progress=pre_progress)
    httpd, api_port = start_mock(st)
    try:
        r = run_tool("push", str(krst_custom), "--host", "127.0.0.1",
                     "--api-port", str(api_port), "--serve-ip", "127.0.0.1",
                     "--serve-port", str(free_port()), "--poll-interval", "0.2",
                     "--timeout", "15", "--yes", timeout=60)
        check("終了コード 0", r.returncode == 0, f"rc={r.returncode}\n{r.stdout}\n{r.stderr}")
        check("成功判定の表示", "結果: 成功" in r.stdout)
        check("版表示を終端証拠にした", "version evidence: current=0368" in r.stdout,
              f"\n{r.stdout}")
        check("最後の進捗は送信前と同一の残留値", st.last_progress == pre_progress,
              f"last={st.last_progress}")
    finally:
        httpd.shutdown()

    print(f"== 結果: {PASS} PASS / {FAIL} FAIL ==")
    return 1 if FAIL else 0


if __name__ == "__main__":
    raise SystemExit(main())
