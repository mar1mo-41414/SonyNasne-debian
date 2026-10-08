#!/usr/bin/env python3
"""dtvtuner が OPEN_SECUREDTS(ioctl 0x30)に渡す構造体(0x9ec バイト)を、unicorn で dtvtuner 内の関数を実行して再現する。

流れ(dtvtuner内の関数):
  FUN_0047edfc(streamBit, cfg)        ストリーム既定設定(0x478B)を作る。streamBit = 1/2/4 (3本あるストリームのどれか)
  → cfg を ctx+idx*0x478+0xe8 へ(FUN_00645754 の中でやっているコピー)
  FUN_0063e84c(ctx, idx)              設定から内部状態(ctx+0x24a8 の配列)を作る
  FUN_006420fc(ctx, idx, out)         状態から OPEN_SECUREDTS の ioctl 構造体(本体は out+0xa4〜)を作る
外部関数(libc、ログ)は PLT スタブをフックして最小限だけ実装する。

使い方: python emu_secured_open.py <dtvtuner> [streamBit(1|2|4)]
"""
import struct
import sys

from unicorn import (Uc, UC_ARCH_MIPS, UC_MODE_MIPS32, UC_MODE_LITTLE_ENDIAN, UC_PROT_ALL, UC_HOOK_CODE)
from unicorn.mips_const import *

GP = 0x91c440
STACK_TOP = 0x7ff00000
SENTINEL = 0x7ffff000
CTX = 0x20000000
STATE = 0x20100000
CFG = 0x20200000
OUT = 0x20300000
PASS = 0x20400000
HEAP = 0x30000000
PLT = (0x40d160, 0x40f7c4)
NAMES = {0x40dab0: "memcpy", 0x40db30: "malloc", 0x40db90: "strlen", 0x40dee0: "strcmp", 0x40e860: "free",
         0x40f090: "memset", 0x40f3e0: "memmove", 0x40f580: "strcpy"}
LOGGER = 0x5e569c
STUBS0 = (0x4e4bac,)        # 戻り値0でスキップする内部関数(時刻取得など)


class Emu:
    def __init__(self, path):
        d = open(path, "rb").read()
        phoff = struct.unpack_from("<I", d, 0x1c)[0]
        phentsize, phnum = struct.unpack_from("<HH", d, 0x2a)
        mu = self.mu = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 + UC_MODE_LITTLE_ENDIAN)
        for i in range(phnum):
            t, off, va, pa, fs, ms, fl, al = struct.unpack_from("<8I", d, phoff + i * phentsize)
            if t != 1:
                continue
            s = va & ~0xfff
            e = (va + ms + 0xfff) & ~0xfff
            try:
                mu.mem_map(s, e - s, UC_PROT_ALL)
            except Exception:
                pass
            mu.mem_write(va, d[off:off + fs])
        for base, size in ((CTX, 0x10000), (STATE, 0x10000), (CFG, 0x10000), (OUT, 0x10000), (PASS, 0x10000), (HEAP, 0x100000),
                           (STACK_TOP - 0x20000, 0x40000), (SENTINEL & ~0xfff, 0x1000)):
            mu.mem_map(base, size, UC_PROT_ALL)
        self.heap = HEAP
        mu.hook_add(UC_HOOK_CODE, self.hook)

    def hook(self, mu, addr, size, ud):
        ra = mu.reg_read(UC_MIPS_REG_RA)
        if addr == LOGGER or addr in STUBS0:
            mu.reg_write(UC_MIPS_REG_V0, 0)
            mu.reg_write(UC_MIPS_REG_PC, ra)
        elif PLT[0] <= addr < PLT[1]:
            name = NAMES.get(addr & ~0xf)
            a0, a1, a2 = (mu.reg_read(r) for r in (UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2))
            ret = 0
            if name == "memset":
                mu.mem_write(a0, bytes([a1 & 0xff]) * a2); ret = a0
            elif name in ("memcpy", "memmove"):
                mu.mem_write(a0, bytes(mu.mem_read(a1, a2))); ret = a0
            elif name == "malloc":
                ret = self.heap; self.heap += (a0 + 15) & ~15
            elif name == "strlen":
                n = 0
                while mu.mem_read(a0 + n, 1)[0]:
                    n += 1
                ret = n
            mu.reg_write(UC_MIPS_REG_V0, ret)
            mu.reg_write(UC_MIPS_REG_PC, ra)

    def call(self, addr, args):
        mu = self.mu
        sp = STACK_TOP - 0x400
        for i, v in enumerate(args[4:]):
            mu.mem_write(sp + 16 + 4 * i, struct.pack("<I", v & 0xffffffff))
        for r, v in zip((UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2, UC_MIPS_REG_A3), args[:4]):
            mu.reg_write(r, v & 0xffffffff)
        mu.reg_write(UC_MIPS_REG_SP, sp)
        mu.reg_write(UC_MIPS_REG_RA, SENTINEL)
        mu.reg_write(UC_MIPS_REG_GP, GP)
        mu.reg_write(UC_MIPS_REG_T9, addr)
        try:
            mu.emu_start(addr, SENTINEL, count=5000000)
        except Exception as ex:
            print('emu error at pc=%#x ra=%#x: %s' % (mu.reg_read(UC_MIPS_REG_PC), mu.reg_read(UC_MIPS_REG_RA), ex))
            raise
        return mu.reg_read(UC_MIPS_REG_V0)


def build(path, bit):
    e = Emu(path)
    idx = {1: 0, 2: 1, 4: 2}[bit]
    e.mu.mem_write(CTX + 0x24a8, struct.pack("<I", STATE))
    e.mu.mem_write(CTX + 0x27cc, struct.pack("<I", PASS))
    e.call(0x47edfc, [bit, CFG])
    cfg = bytes(e.mu.mem_read(CFG, 0x470))
    e.mu.mem_write(CTX + idx * 0x478 + 0xe8, cfg)
    r1 = e.call(0x63e84c, [CTX, idx])
    r2 = e.call(0x6420fc, [CTX, idx, OUT])
    return e, idx, r1, r2


if __name__ == "__main__":
    path = sys.argv[1]
    bit = int(sys.argv[2]) if len(sys.argv) > 2 else 2
    e, idx, r1, r2 = build(path, bit)
    out = bytes(e.mu.mem_read(OUT, 0x9ec))
    print("FUN_0063e84c ret=%#x  FUN_006420fc ret=%#x  stream idx=%d" % (r1, r2, idx))
    for o in range(0, 0x9ec, 4):
        v = struct.unpack_from("<I", out, o)[0]
        if v:
            print("  +%04x: %08x" % (o, v))
