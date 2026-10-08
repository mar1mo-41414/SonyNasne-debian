#!/usr/bin/env python3
"""dtvtuner(公式ファームのMIPS ELF)内の計算関数を、unicorn(MIPSエミュレータ)でそのまま実行する補助。
デコンパイルが読みにくいテーブル駆動の関数(チューナーのレジスタ設定列を作る関数など)の正確な出力を得るために使う。

  python -m pip install unicorn   (venv内で)
  from emu_dtvtuner import Emu
  e = Emu("opt/dtvtuner/bin/dtvtuner")
  ret, out = e.call(0x46af7c, [out_ptr, len_ptr, freq_hz, 6, 1], mem=...)

呼び出す関数は外部関数(PLT)を呼ばない純粋な計算に限ること(呼ぶとマップ外アクセスで例外になる)。
"""
import struct
import sys

from unicorn import Uc, UC_ARCH_MIPS, UC_MODE_MIPS32, UC_MODE_LITTLE_ENDIAN, UC_PROT_ALL
from unicorn.mips_const import (UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2, UC_MIPS_REG_A3, UC_MIPS_REG_SP,
                                UC_MIPS_REG_RA, UC_MIPS_REG_GP, UC_MIPS_REG_V0, UC_MIPS_REG_T9)

GP = 0x91c440           # readelf -A の "canonical GP value"
BUF = 0x10000000
STACK_TOP = 0x7ff00000
SENTINEL = 0x7ffff000


class Emu:
    def __init__(self, path):
        d = open(path, "rb").read()
        phoff = struct.unpack_from("<I", d, 0x1c)[0]
        phentsize, phnum = struct.unpack_from("<HH", d, 0x2a)
        self.mu = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 + UC_MODE_LITTLE_ENDIAN)
        for i in range(phnum):
            t, off, va, pa, fs, ms, fl, al = struct.unpack_from("<8I", d, phoff + i * phentsize)
            if t != 1:
                continue
            start = va & ~0xfff
            end = (va + ms + 0xfff) & ~0xfff
            try:
                self.mu.mem_map(start, end - start, UC_PROT_ALL)
            except Exception:
                pass            # 隣接セグメントで既にマップ済みのページ
            self.mu.mem_write(va, d[off:off + fs])
        self.mu.mem_map(BUF, 0x10000, UC_PROT_ALL)
        self.mu.mem_map(STACK_TOP - 0x10000, 0x20000, UC_PROT_ALL)
        self.mu.mem_map(SENTINEL & ~0xfff, 0x1000, UC_PROT_ALL)

    def call(self, addr, args):
        """O32呼び出し規約(第1〜4引数はa0〜a3、第5引数以降はスタックのsp+16〜)。戻り値v0を返す。"""
        mu = self.mu
        sp = STACK_TOP - 0x200
        for i, v in enumerate(args[4:]):
            mu.mem_write(sp + 16 + 4 * i, struct.pack("<I", v & 0xffffffff))
        regs = [UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2, UC_MIPS_REG_A3]
        for r, v in zip(regs, args[:4]):
            mu.reg_write(r, v & 0xffffffff)
        mu.reg_write(UC_MIPS_REG_SP, sp)
        mu.reg_write(UC_MIPS_REG_RA, SENTINEL)
        mu.reg_write(UC_MIPS_REG_GP, GP)
        mu.reg_write(UC_MIPS_REG_T9, addr)       # PICの関数はt9から$gpを再計算する
        mu.emu_start(addr, SENTINEL, count=2000000)
        return mu.reg_read(UC_MIPS_REG_V0)

    def rd(self, addr, n):
        return bytes(self.mu.mem_read(addr, n))

    def wr(self, addr, data):
        self.mu.mem_write(addr, data)


if __name__ == "__main__":
    path = sys.argv[1]
    e = Emu(path)
    freq = int(sys.argv[2]) if len(sys.argv) > 2 else 473143000
    out, plen = BUF, BUF + 0x400
    e.wr(plen, b"\0\0\0\0")
    ret = e.call(0x46af7c, [out, plen, freq, 6, 1])
    n = struct.unpack("<I", e.rd(plen, 4))[0]
    print("ret=%d len=%d" % (ret, n))
    print(e.rd(out, n).hex())
