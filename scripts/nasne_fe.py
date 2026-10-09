#!/usr/bin/env python3
"""nasne のチューナー(復調IC + RFチューナーIC)を、PCからssh経由でI2C操作して選局・状態確認する実験ツール。

nasne側に `i2cx`(scripts/userspace_tools/i2cx.c, -b バッチモード)が /usr/local/sbin にあり、公式ドライバ(rc.xcode4)が
ロード済みでファームが動いていることが前提。手順は dtvtuner の逆コンパイルから起こしたもの(docs/23_tuner_i2c.md)。

ISDB-S 側(バス1、復調IC 8bit 0x22、RFチューナー 8bit 0xC6 をパススルー 0xFE 経由で操作):
  s-init                 初期化(復調ICの設定表、チューナーの初期レジスタ)。電源投入後に1回
  s-tune <BS ch | IF_kHz> [--pol 1|0]   選局して、ロック状態とC/Nを表示
  s-scan                 BS-1〜23(奇数ch)を順に選局して、ロック/C/Nを一覧
  s-sweep [from] [to] [step]   IF周波数[MHz]を掃引して、AGC(信号の強さ)を一覧(既定 950〜2150 を10MHz刻み)。BS/CS-IF(1032〜2150MHz)に
                         信号が来ているかを、TMCCのロックを待たずに調べる。AGC=127(0x7f)は信号なし、小さいほど強い
  s-status               選局せずに現在のステータスだけ表示
  t-init                 ISDB-T(地デジ)側の初期化(復調IC 8bit 0x20、RFチューナー 8bit 0xC0)
  t-tune <UHF ch | MHz>  選局して、ロック状態を表示 (例: t-tune 27 / t-tune 521.143)
  t-scan [from] [to]     UHFチャンネル(既定13〜52)を順に選局して、ロックしたものを一覧(-v で全部)
  t-standby              T側をスタンバイに戻す
  t-capture <ch|MHz> <out.ts> [packets_x64KB]  選局して、TSパススルーでTSを受信しPCに保存(nasne側に tsrecv が必要。既定120回≒17MB)
  t-status               選局せずにステータスだけ表示
  raw                    標準入力のバッチをそのままnasneの i2cx -b に流す

使い方: nasne_fe.py --host <ip> s-init ; nasne_fe.py --host <ip> s-scan
"""
import argparse
import math
import subprocess
import sys

SSH_OPTS = ["-o", "PubkeyAcceptedAlgorithms=+ssh-rsa", "-o", "HostKeyAlgorithms=+ssh-rsa", "-o", "ConnectTimeout=8",
            "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR", "-o", "BatchMode=yes"]

DEMOD_S = "22"
TUNER_S = "c6"
TUNER_S_B = "c8"
LO_KHZ = 10678000          # BS/CS110のLNB局発 10.678GHz
XTAL_HZ = 16000000

DEMOD_T = "20"
TUNER_T = "c0"
# ISDB-T 復調IC(0x20)の初期化表と、チューナー(0xC0)のレジスタ設定列
# (後者は dtvtuner の FUN_0046bbf0 を unicorn(scripts/ghidra/emu_dtvtuner.py)で実行して得た、24MHz水晶・6MHz帯域の初期設定)
DEMOD_T_INIT = [(0x01, 0x40), (0x0e, 0x07), (0x0f, 0x10), (0x23, 0x38), (0x4f, 0xe5)]
TUNER_T_INIT = ("00:02 02:21 05:00 06:14 07:0c 0e:00 0f:00 21:a8 c8:00 af:a2 1d:4a ca:05 2e:7e 2d:44 1b:ac ac:00 2c:01 "
                "ae:03 54:e3 56:47 55:12 09:01 a4:51 a0:0c b0:c2 57:17 6f:79 70:00 6f:78 70:00 6f:7b 70:00 6f:7c 70:00 "
                "6f:7a 70:00 1a:0d 01:01").split()
# 初期化表(dtvtunerのデータから): 復調IC(S)のレジスタ設定、チューナーの初期レジスタ0〜7
DEMOD_S_INIT = [(0x03, 0x01), (0x06, 0x40), (0x07, 0x01), (0x08, 0x00), (0x10, 0xb1),
                (0x11, 0x40), (0x85, 0x7a), (0x8e, 0x05), (0xa3, 0x33), (0xa5, 0xc0)]
TUNER_S_INIT = [0x03, 0x13, 0xdc, 0x85, 0x12, 0x01, 0xe6, 0x1e]


def bs_if_khz(ch):
    """BS放送のチャンネル(奇数1〜23)の中心周波数からIF(kHz)。BS-1 = 11727.48MHz、2ch(=1トランスポンダ)で38.36MHz間隔。"""
    rf_khz = 11727480 + int(round((ch - 1) / 2 * 38360))
    return rf_khz - LO_KHZ


class Fe:
    def __init__(self, host, verbose=False):
        self.host = host
        self.verbose = verbose
        self.shadow = list(TUNER_S_INIT)       # チューナーのレジスタ影(dtvtunerと同じく、ビット更新の元にする)

    def run(self, lines):
        """lines を nasne の i2cx -b に流し、"> cmd" ごとの結果を返す: [(cmd, 結果行)...]"""
        data = "\n".join(lines) + "\n"
        r = subprocess.run(["ssh"] + SSH_OPTS + ["root@" + self.host, "/usr/local/sbin/i2cx -b"],
                           input=data.encode(), capture_output=True, timeout=120)
        out = r.stdout.decode(errors="replace").splitlines()
        res = []
        for ln in out:
            if ln.startswith("> "):
                res.append([ln[2:], ""])
            elif res:
                res[-1][1] += ln
        if self.verbose:
            for c, o in res:
                print("  ", c, "->", o)
        return res

    # --- 低レベル(dtvtunerの FUN_00461864 / FUN_004611c4 / FUN_004683c4 相当) ---
    @staticmethod
    def dw(reg, *vals):
        return "1 %s w %02x %s" % (DEMOD_S, reg, " ".join("%02x" % v for v in vals))

    @staticmethod
    def dr(reg, n=1):
        return "1 %s h %02x %d" % (DEMOD_S, reg, n)

    def tw(self, reg, mask, val):
        """チューナーのビット更新書き込み(FUN_004683c4)。影を更新して [0xfe,0xc6,reg,new] を返す"""
        new = (~mask & self.shadow[reg] | (val & mask)) & 0xff
        self.shadow[reg] = new
        return "1 %s w fe %s %02x %02x" % (DEMOD_S, TUNER_S, reg, new)

    @staticmethod
    def tr(reg):
        return "1 %s p %s 1 %02x" % (DEMOD_S, TUNER_S, reg)

    @staticmethod
    def parse_data(res):
        if "data=" not in res:
            return None
        return [int(x, 16) for x in res.split("data=")[1].split()]

    # --- 初期化 ---
    def s_init(self):
        L = []
        # チューナー: レジスタ0〜7に初期値、reg1の上位2bit(用途別設定)= 0、reg1下位4bit = 3
        L.append("1 %s w fe %s 00 %s" % (DEMOD_S, TUNER_S, " ".join("%02x" % v for v in TUNER_S_INIT)))
        self.shadow = list(TUNER_S_INIT)
        L.append(self.tw(1, 0xc0, 0x00))
        L.append(self.tw(1, 0x0f, 0x03))
        for reg, val in DEMOD_S_INIT:
            L.append(self.dw(reg, val))
        L.append("1 %s w fe %s 02 53" % (DEMOD_S, TUNER_S_B))       # 第2のIC(0xC8)
        # スタンバイ状態にして終わる(dtvtunerの初期化の終わり)
        L.append(self.tw(0, 0x07, 0x00))
        L.append(self.dw(0x13, 0x80))
        L.append(self.dw(0x17, 0xff))
        return self.run(L)

    # --- 選局(FUN_0046037c) ---
    @staticmethod
    def pll(if_khz):
        if if_khz < 1022001:
            l28, l24, base = 1, 0, 2
        elif if_khz < 1324577:
            l28, l24, base = 1, 1, 2
        elif if_khz < 2046001:
            l28, l24, base = 0, 0, 1
        else:
            l28, l24, base = 0, 1, 1
        p = 2 ** base
        target = 10 * p
        s1, s2 = 1000, 0
        for s0 in range(1, 5):
            v = int(160 / 2 ** s0)
            if abs(v - target) < abs(s1 - target):
                s2 = s0 - 1
            s1 = int(160 / 2 ** (s2 + 1))
        n = int((2 ** (s2 + 1)) * if_khz * p * 10 / (XTAL_HZ // 1000))
        t0 = n + 5
        reg2 = (t0 // 10) & 0xff
        reg3 = ((l24 << 5) | (l28 << 4) | (s2 << 6) | (t0 // 2560)) & 0xff
        return reg2, reg3

    def s_tune_lines(self, if_khz, pol=1):
        reg2, reg3 = self.pll(if_khz)
        L = []
        L.append(self.tw(0, 0x07, 0x03))                # チューナー電源ON
        L.append(self.dw(0x13, 0x00))                  # 復調IC スタンバイ解除
        L.append(self.dw(0x17, 0x00))
        L.append(self.dw(0xa5, 0x80 if pol == 1 else 0xc0))
        L.append(self.dw(0x07, 0x01))
        L.append(self.dw(0xa6, 0x04))
        L.append(self.dw(0x03, 0x01))
        L.append(self.dw(0x0a, 0x00))
        L.append(self.dw(0x11, 0x00))
        L.append(self.dw(0x03, 0x01))
        L.append(self.tw(0, 0xf8, 0x00))               # xtal 16MHz
        L.append(self.tw(3, 0xff, reg3))
        L.append(self.tw(2, 0xff, reg2))
        L.append(self.tw(5, 0x04, 0x04))               # PLL更新
        for _ in range(4):
            L.append(self.tr(5))                       # bit2が落ちたら完了
            L.append("sleep 2")
        L.append(self.dw(0x0a, 0xff))
        L.append(self.dw(0x11, 0x40))
        L.append(self.dw(0x03, 0x01))
        return L

    def s_status_lines(self):
        return [self.dr(0xc3), self.dr(0xea), self.dr(0xba), self.dr(0xbc, 2), self.dr(0xce, 2), self.tr(5)]

    def s_status(self, res):
        d = {}
        for cmd, out in res:
            t = cmd.split()
            if len(t) >= 4 and t[2] == "h":
                d[t[3]] = self.parse_data(out)
        c3 = (d.get("c3") or [None])[0]
        ea = (d.get("ea") or [None])[0]
        b0 = d.get("ba")
        bc = d.get("bc")
        cn_raw = (bc[0] << 8 | bc[1]) if bc else None
        return {"c3": c3, "ea": ea, "b0": b0, "cn_raw": cn_raw, "ce": d.get("ce")}

    def s_tune(self, if_khz, pol=1, wait_ms=300):
        L = self.s_tune_lines(if_khz, pol)
        # TMCC取得待ち(最大 約250ms): reg3=1 後に reg c3 bit4 が落ちるのを待つ
        for _ in range(6):
            L.append(self.dr(0xc3))
            L.append("sleep 50")
        L.append("sleep %d" % wait_ms)
        L += self.s_status_lines()
        res = self.run(L)
        return self.s_status(res[-6:]), res



# ---------------- ISDB-T ----------------
def t_freq_hz(ch_or_mhz):
    """UHFチャンネル番号(13〜62)またはMHzの小数から周波数[Hz]。UHF chN の中心 = 473.142857MHz + 6MHz×(N-13)"""
    v = float(ch_or_mhz)
    if v < 100:
        return int(round(473142857 + 6000000 * (int(v) - 13)))
    return int(round(v * 1e6))


def t_tuner_pll_pairs(freq_hz):
    """6MHz帯域・variant 1 の周波数設定列(dtvtunerの FUN_0046af7c の出力と一致する式)。
    reg13=0, reg3b=c0, reg3b=80, reg10=bw, reg1a=05, reg11/12=周波数×64[MHz]の丸め, reg13=1"""
    n = freq_hz // 1000000
    rem = freq_hz % 1000000
    div = 1000000
    for _ in range(6):
        div >>= 1
        n *= 2
        if div < rem:
            rem -= div
            n += 1
    n += 1 if rem > 0x1e84 else 0
    return [(0x13, 0x00), (0x3b, 0xc0), (0x3b, 0x80), (0x10, 0x95), (0x1a, 0x05), (0x11, n & 0xff), (0x12, (n >> 8) & 0xff), (0x13, 0x01)]


def tw_t(pairs):
    """チューナー(0xC0)へのレジスタ(reg,val)列を、dtvtunerと同じく1回の書き込み [0xfe,0xc0,reg,val,reg,val,...] にする"""
    flat = []
    for r, v in pairs:
        flat += ["%02x" % r, "%02x" % v]
    return "1 %s w fe %s %s" % (DEMOD_T, TUNER_T, " ".join(flat))


def dw_t(reg, *vals):
    return "1 %s w %02x %s" % (DEMOD_T, reg, " ".join("%02x" % v for v in vals))


def dr_t(reg, n=1):
    return "1 %s h %02x %d" % (DEMOD_T, reg, n)


def tr_t(reg):
    """チューナー(0xC0)のレジスタ読み出し: [fe c0 fb reg] を書いて、[fe c1] で読む"""
    return "1 %s p %s 1 fb %02x" % (DEMOD_T, TUNER_T, reg)


def t_init_lines():
    L = ["1 %s w fe %s ff" % (DEMOD_T, TUNER_T), "sleep 10"]            # チューナーのリセット
    pairs = [(int(x.split(":")[0], 16), int(x.split(":")[1], 16)) for x in TUNER_T_INIT]
    L.append(tw_t(pairs))
    L.append("1 %s w fe %s 01 01 13 01" % (DEMOD_T, TUNER_T))
    for reg, val in DEMOD_T_INIT:
        L.append(dw_t(reg, val))
    # スタンバイ
    L.append("1 %s w fe %s 13 01" % (DEMOD_T, TUNER_T))
    L.append("sleep 30")
    L.append("1 %s w fe %s 01 00 13 00" % (DEMOD_T, TUNER_T))
    L.append(dw_t(0x03, 0xf0))
    L.append(tr_t(0xcc))                                                 # チップID
    return L


def t_tune_lines(freq_hz):
    L = []
    L.append("1 %s w fe %s 01 01" % (DEMOD_T, TUNER_T))                 # チューナー起動
    L.append(dw_t(0x03, 0x00))                                           # 復調IC スタンバイ解除
    L.append(dw_t(0x77, 0x03))
    L.append(dw_t(0x71, 0x30))
    L.append(dw_t(0x20, 0x00))
    L.append(dw_t(0x25, 0x00))
    L.append(dw_t(0x23, 0x39))
    L.append(tw_t(t_tuner_pll_pairs(freq_hz)))
    L.append("sleep 31")
    L.append("1 %s w fe %s 1a 0d" % (DEMOD_T, TUNER_T))
    L.append(dw_t(0x23, 0x38))
    L.append(dw_t(0x47, 0x40))
    L.append(dw_t(0x01, 0x40))
    return L


def t_status_lines():
    return [dr_t(0x80), dr_t(0x81), dr_t(0x82), dr_t(0xb0, 10), dr_t(0x8b, 3)]


def t_status(res):
    d = {}
    for cmd, out in res:
        t = cmd.split()
        if len(t) >= 4 and t[2] == "h":
            d[t[3]] = Fe.parse_data(out)
    return d


def t_cn_db(raw):
    """C/N[dB]。dtvtuner の FUN_0046a664 と同じ換算(reg 0x8b〜0x8d の24bit値 → log10 → 4次多項式)"""
    if raw == 0:
        return 0.0
    f = math.log10(5505024.0 / raw) * 10.0
    return ((((f * 2.4e-05 - 0.0016) * f + 0.0398) * f + 0.5491) * f) + 3.0965


def t_info(d):
    """ステータスを辞書にまとめる。locked は reg 0x80 の bit3(同期)とbit7(エラー)が共に0"""
    r80 = (d.get("80") or [None])[0]
    cn = d.get("8b")
    raw = (cn[0] << 16 | cn[1] << 8 | cn[2]) if cn else None
    return {"reg80": r80, "locked": r80 is not None and (r80 & 0x88) == 0,
            "agc": (d.get("82") or [None])[0], "tmcc": d.get("b0"), "cn_raw": raw,
            "cn_db": t_cn_db(raw) if raw is not None else None}


def t_fmt(d):
    i = t_info(d)
    r = i["reg80"]
    return "%s reg80=%s AGC=%s CN=%s TMCC(b0..)=%s" % (
        "LOCK  " if i["locked"] else "unlock", "%02x" % r if r is not None else "-",
        "%02x" % i["agc"] if i["agc"] is not None else "-",
        "%.1fdB" % i["cn_db"] if i["locked"] and i["cn_db"] is not None else "-",
        " ".join("%02x" % x for x in i["tmcc"][:10]) if i["tmcc"] else "-")


def t_standby_lines():
    return ["1 %s w fe %s 13 01" % (DEMOD_T, TUNER_T), "sleep 30", "1 %s w fe %s 01 00 13 00" % (DEMOD_T, TUNER_T), dw_t(0x03, 0xf0)]


def fmt(st):
    if not st or st["c3"] is None:
        return "読み出し失敗"
    b0 = st["b0"]
    return "c3=%02x(bit4=0でTMCC有効) ea=%02x AGC(ba&7f)=%s  CN_raw=%s ce/cf=%s" % (
        st["c3"], st["ea"] if st["ea"] is not None else 0, ("%02x" % (b0[0] & 0x7f)) if b0 else "-",
        st["cn_raw"], " ".join("%02x" % x for x in st["ce"]) if st["ce"] else "-")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("-v", "--verbose", action="store_true")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("s-init")
    p = sub.add_parser("s-tune"); p.add_argument("target"); p.add_argument("--pol", type=int, default=1)
    sub.add_parser("s-scan")
    p = sub.add_parser("s-sweep"); p.add_argument("first", nargs="?", type=int, default=950); p.add_argument("last", nargs="?", type=int, default=2150)
    p.add_argument("step", nargs="?", type=int, default=10); p.add_argument("--pol", type=int, default=1)
    sub.add_parser("s-status")
    sub.add_parser("t-init")
    p = sub.add_parser("t-tune"); p.add_argument("target")
    p = sub.add_parser("t-scan"); p.add_argument("first", nargs="?", type=int, default=13); p.add_argument("last", nargs="?", type=int, default=52)
    sub.add_parser("t-status")
    sub.add_parser("t-standby")
    p = sub.add_parser("t-capture"); p.add_argument("target"); p.add_argument("out"); p.add_argument("count", nargs="?", type=int, default=120)
    sub.add_parser("raw")
    a = ap.parse_args()
    fe = Fe(a.host, a.verbose)
    if a.cmd == "s-init":
        for c, o in fe.s_init():
            if "rv=01" not in o and o:
                print("!!", c, o)
        print("S側初期化: 完了")
    elif a.cmd == "s-tune":
        if_khz = int(a.target) if int(a.target) > 100 else bs_if_khz(int(a.target))
        pll = fe.pll(if_khz)
        print("IF=%d kHz  reg2=%02x reg3=%02x" % (if_khz, pll[0], pll[1]))
        st, _ = fe.s_tune(if_khz, a.pol)
        print(fmt(st))
    elif a.cmd == "s-scan":
        for ch in range(1, 24, 2):
            ifk = bs_if_khz(ch)
            st, _ = fe.s_tune(ifk)
            print("BS-%-2d IF=%7d  %s" % (ch, ifk, fmt(st)))
    elif a.cmd == "s-sweep":
        fe.s_init()
        print("IF[MHz]  AGC(127=信号なし。小さいほど強い)")
        for mhz in range(a.first, a.last + 1, a.step):
            st, _ = fe.s_tune(mhz * 1000, a.pol, wait_ms=60)
            agc = (st["b0"][0] & 0x7f) if st["b0"] else None
            print("%6d   %3s  %s" % (mhz, agc if agc is not None else "-", "#" * ((127 - agc) // 4) if agc is not None else ""))
        fe.s_init()                                   # スタンバイに戻す
    elif a.cmd == "s-status":
        print(fmt(fe.s_status(fe.run(fe.s_status_lines()))))
    elif a.cmd == "t-init":
        res = fe.run(t_init_lines())
        for c, o in res:
            if o and "rv=01" not in o and "read rv=01" not in o:
                print("!!", c, o)
        idb = Fe.parse_data(res[-1][1])
        print("T側初期化: 完了  チューナーID(reg 0xcc)=%s" % ("%02x" % idb[0] if idb else "?"))
    elif a.cmd == "t-tune":
        f = t_freq_hz(a.target)
        L = t_tune_lines(f)
        for _ in range(8):
            L += ["sleep 100"] + [dr_t(0x80)]
        L += t_status_lines()
        res = fe.run(L)
        print("freq=%d Hz  PLL=%s" % (f, " ".join("%02x:%02x" % x for x in t_tuner_pll_pairs(f))))
        for c, o in res[-(5 + 8):-5]:
            d = Fe.parse_data(o)
            print("  reg80 =", ("%02x" % d[0]) if d else o)
        print(t_fmt(t_status(res[-5:])))
    elif a.cmd == "t-scan":
        for ch in range(a.first, a.last + 1):
            f = t_freq_hz(ch)
            L = t_tune_lines(f) + ["sleep 400"] + t_status_lines()
            res = fe.run(L)
            line = "UHF %2d %9.3fMHz  %s" % (ch, f / 1e6, t_fmt(t_status(res[-5:])))
            if a.verbose or t_info(t_status(res[-5:]))["locked"]:
                print(line)
        fe.run(t_standby_lines())
    elif a.cmd == "t-capture":
        f = t_freq_hz(a.target)
        res = fe.run(t_tune_lines(f) + ["sleep 500"] + t_status_lines())
        info = t_info(t_status(res[-5:]))
        print("選局 %.3fMHz: %s" % (f / 1e6, t_fmt(t_status(res[-5:]))))
        if not info["locked"]:
            sys.exit("ロックしていないので中止")
        remote = "/tmp/nasne_fe_capture.ts"
        r = subprocess.run(["ssh"] + SSH_OPTS + ["root@" + a.host, "cd /usr/local/sbin && timeout 120 ./tsrecv 0 20000 0 0 %x %s" % (a.count, remote)],
                           capture_output=True, timeout=180)
        txt = r.stdout.decode(errors="replace")
        for ln in txt.splitlines():
            if "OPEN_TS" in ln or "TSパケット" in ln:
                print(ln)
        subprocess.run(["scp"] + SSH_OPTS + ["root@%s:%s" % (a.host, remote), a.out], check=True)
        print("保存:", a.out)
    elif a.cmd == "t-standby":
        fe.run(t_standby_lines())
        print("T側をスタンバイにしました")
    elif a.cmd == "t-status":
        print(t_fmt(t_status(fe.run(t_status_lines()))))
    elif a.cmd == "raw":
        r = subprocess.run(["ssh"] + SSH_OPTS + ["root@" + a.host, "/usr/local/sbin/i2cx -b"], input=sys.stdin.read().encode())
        sys.exit(r.returncode)


if __name__ == "__main__":
    main()
