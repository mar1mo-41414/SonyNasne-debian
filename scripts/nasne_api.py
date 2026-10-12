#!/usr/bin/env python3
"""nasne の公式 Web API(ポート 64210)を、公式WebUIと同じ手順で呼ぶ小さなクライアント。標準ライブラリのみ。

公式WebUI(`/var/opt/dtvtuner/nv/00000022/lib/app.js`)から読み取った仕様:
  - GET は認証なし(`/status/...`、`/config/...Get`、`/recorded/...`、LED制御 `/config/LEDControl` など)。
  - POST(`/config/...Set` や初期化など)は、ヘッダ `X-DTVToken` が必要。
        トークン = base64( SHA1( PIN + パス + 本文の先頭256文字 ) )
    PIN は **本体のMACアドレスの下位3バイト**(`/status/networkIfInfoGet` の macAddr からコロンを除いた文字列の7文字目以降。
    APIが返す表記のまま、小文字)。例: macAddr "aa:bb:cc:11:22:33" → PIN "112233"。
    Content-Type は application/x-www-form-urlencoded。トークンが違う・無いと HTTP 400。未知のAPIは 404。
  - LED(`/config/LEDControl?powerGreen=a&powerRed=b&record=c&network=d`): 各値は 0=変更なし、1=点灯、2=消灯。
    WebUIの「nasneを探す」は powerGreen=2, powerRed=1, record=1, network=1(緑を消し、赤・REC・LANを点灯。数秒で元に戻る)。

使い方:
  nasne_api.py <ip> get  <パス> [key=value ...]          例: get /status/HDDListGet
  nasne_api.py <ip> post <パス> <本文>                   例: post /config/boxNameSet name=nasne   (PINは自動で取得)
  nasne_api.py <ip> led  [powerGreen=2 powerRed=1 record=1 network=1]   LED制御(省略するとWebUIの「nasneを探す」と同じ)
注意: POST は設定を変える(初期化 /config/internalHDDInitialize=録画データの消去 など)。内容を理解して使うこと。
"""
import argparse
import base64
import hashlib
import json
import sys
import urllib.error
import urllib.parse
import urllib.request

PORT = 64210


def base(ip):
    return f"http://{ip}:{PORT}"


def http_get(ip, path, query=""):
    url = base(ip) + path + (("?" + query) if query else "")
    try:
        with urllib.request.urlopen(url, timeout=10) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def get_pin(ip):
    st, body = http_get(ip, "/status/networkIfInfoGet")
    if st != 200:
        sys.exit(f"エラー: networkIfInfoGet が HTTP {st}")
    mac = json.loads(body)["macAddr"]
    return mac.replace(":", "")[6:]


def token(pin, path, body):
    return base64.b64encode(hashlib.sha1((pin + path + body[:256]).encode()).digest()).decode()


def http_post(ip, path, body, pin=None):
    pin = pin or get_pin(ip)
    req = urllib.request.Request(base(ip) + path, data=body.encode(), method="POST",
                                 headers={"Content-Type": "application/x-www-form-urlencoded", "X-DTVToken": token(pin, path, body)})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ip")
    ap.add_argument("cmd", choices=["get", "post", "led"])
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    if a.cmd == "get":
        if not a.args:
            sys.exit("パスを指定してください")
        st, body = http_get(a.ip, a.args[0], "&".join(a.args[1:]))
    elif a.cmd == "post":
        if len(a.args) < 2:
            sys.exit("パスと本文を指定してください")
        st, body = http_post(a.ip, a.args[0], a.args[1])
    else:
        kv = dict(powerGreen=2, powerRed=1, record=1, network=1)
        for x in a.args:
            k, _, v = x.partition("=")
            kv[k] = int(v)
        st, body = http_get(a.ip, "/config/LEDControl", urllib.parse.urlencode(kv))
    print(f"HTTP {st}")
    if body:
        print(body)
    sys.exit(0 if st == 200 else 1)


if __name__ == "__main__":
    main()
