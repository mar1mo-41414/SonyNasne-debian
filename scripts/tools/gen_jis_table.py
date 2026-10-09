#!/usr/bin/env python3
"""JIS X 0208 → UTF-16 の変換表 jis0208_utf16.inc を生成する(nasne_recpt1.c が #include する。libc不使用でARIB文字列をUTF-8にするため)。
添字 = (区-1)*94 + (点-1)。表にない/BMP外は0。"""
import sys
out = ["/* 自動生成: scripts/userspace_tools/gen_jis_table.py。JIS X 0208 (区1-94 × 点1-94) → UTF-16 */",
       "static const unsigned short jis0208[94 * 94] = {"]
vals = []
for row in range(1, 95):
    for cell in range(1, 95):
        try:
            s = bytes([0xA0 + row, 0xA0 + cell]).decode("euc_jp")
            cp = ord(s) if len(s) == 1 and ord(s) < 0x10000 else 0
        except UnicodeDecodeError:
            cp = 0
        vals.append(cp)
for i in range(0, len(vals), 16):
    out.append("  " + ",".join("0x%04x" % v for v in vals[i:i + 16]) + ",")
out.append("};")
open(sys.argv[1] if len(sys.argv) > 1 else "jis0208_utf16.inc", "w").write("\n".join(out) + "\n")
