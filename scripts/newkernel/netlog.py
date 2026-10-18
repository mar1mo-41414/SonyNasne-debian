#!/usr/bin/env python3
"""新カーネルの早期コンソール(UDP ブロードキャスト、ポート6666)を受けて、時刻つきで標準出力とファイルへ書く。
使い方: netlog.py [出力ファイル(既定 newkernel/out/netlog.txt)]   (Ctrl-C で終了)"""
import os, socket, sys, time
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "newkernel", "out", "netlog.txt")
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
s.bind(("", 6666))
t0 = time.time()
with open(out, "ab", buffering=0) as f:
    while True:
        d, a = s.recvfrom(4096)
        line = f"[{time.time() - t0:8.2f}] ".encode() + d
        f.write(line if line.endswith(b"\n") else line + b"\n")
        sys.stdout.buffer.write(line if line.endswith(b"\n") else line + b"\n"); sys.stdout.flush()
