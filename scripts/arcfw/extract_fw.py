#!/usr/bin/env python3
"""SIE 公開の GPL ソース(AVDriver7GSP47)に C の配列として入っているメディアエンジンのファームを、バイナリに戻す。
使い方: python3 -I extract_fw.py <Kernel_Src/kdrv/drv のディレクトリ> <出力ディレクトリ>
  firmware.c → firmware.bin(メインの ARC。平文)、fmsecond.c → fmsecond.bin(2 つ目の ARC。平文)
取り出したファームは Sony のバイナリなので、リポジトリには入れない(gpl_src/ 以下に置く。docs/26)。"""
import re, sys, os
src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
for name, f in (("firmware", "firmware.c"), ("fmsecond", "fmsecond.c")):
    txt = open(os.path.join(src, f)).read()
    a = txt.index("{") + 1; b = txt.index("};", a)      # 最初の配列(NON_ENC_FW_BUILD の側)
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", txt[a:b]))
    open(os.path.join(out, name + ".bin"), "wb").write(data)
    print(name, len(data), hex(len(data)))
