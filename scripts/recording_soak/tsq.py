#!/usr/bin/env python3
"""TS のストリーム解析(大きなファイル向け、numpy)。
使い方: tsq.py <file.ts> [<out.json>]
  - 同期バイト(0x47)の喪失と再同期、PID ごとの CC エラー・重複・TEI・スクランブル
  - PCR の連続性(PMT から PCR_PID を探す)、PCR 間隔の最大値、PCR から求めた長さとビットレートの下限/上限
  - エラーの位置(パケット番号と、PCR から求めた先頭からの秒数の目安)
"""
import sys, json
import numpy as np

CHUNK = 1 << 16          # パケット数/回(約12MB)
POS_MAX = 20


def find_sync(buf, start=0):
    n = len(buf)
    o = start
    while o + 188 * 3 < n:
        i = buf.find(b'\x47', o)
        if i < 0 or i + 188 * 3 >= n:
            return -1
        if buf[i + 188] == 0x47 and buf[i + 376] == 0x47:
            return i
        o = i + 1
    return -1


def parse_pat_pmt(pk_list):
    """先頭付近のパケットから PAT → PMT を引いて PCR_PID を返す"""
    pmt_pid = None
    pcr_pid = None
    for p in pk_list:
        pid = ((p[1] & 0x1f) << 8) | p[2]
        if pid == 0 and p[1] & 0x40 and pmt_pid is None:
            o = 4 + ((1 + p[4]) if (p[3] >> 4) & 2 else 0)
            o += 1 + p[o]
            ln = ((p[o + 1] & 0xf) << 8) | p[o + 2]
            q = o + 8
            while q + 4 <= o + 3 + ln - 4:
                pn = (p[q] << 8) | p[q + 1]
                pp = ((p[q + 2] & 0x1f) << 8) | p[q + 3]
                if pn != 0:
                    pmt_pid = pp
                    break
                q += 4
        if pmt_pid is not None and pid == pmt_pid and p[1] & 0x40:
            o = 4 + ((1 + p[4]) if (p[3] >> 4) & 2 else 0)
            o += 1 + p[o]
            pcr_pid = ((p[o + 8] & 0x1f) << 8) | p[o + 9]
            break
    return pmt_pid, pcr_pid


def analyze(path):
    f = open(path, 'rb')
    st = dict(packets=0, bytes=0, sync_loss=0, sync_loss_pos=[], resync_skipped_bytes=0)
    last_cc = {}
    per = {}      # pid -> dict
    pcr_pid = None
    pmt_pid = None
    pcr_pkts = []
    pcr_vals = []
    head_pk = []
    carry = b''
    base = 0                     # 処理済みパケット数
    synced = False
    eof = False
    while not eof:
        data = f.read(CHUNK * 188)
        if not data:
            eof = True
        buf = carry + data
        carry = b''
        if not buf:
            break
        if not synced:
            o = find_sync(buf)
            if o < 0:
                if eof:
                    break
                carry = buf[-188 * 3:]
                st['resync_skipped_bytes'] += max(0, len(buf) - len(carry))
                continue
            if o:
                st['resync_skipped_bytes'] += o
            buf = buf[o:]
            synced = True
        n = len(buf) // 188
        if n == 0:
            carry = buf
            continue
        a = np.frombuffer(buf[:n * 188], dtype=np.uint8).reshape(n, 188)
        bad = np.nonzero(a[:, 0] != 0x47)[0]
        if len(bad):
            # 最初の壊れた位置までを処理して、そこから再同期
            n = int(bad[0])
            st['sync_loss'] += 1
            if len(st['sync_loss_pos']) < POS_MAX:
                st['sync_loss_pos'].append(base + n)
            rest = buf[n * 188:]
            synced = False
            a = a[:n]
            carry = rest
            if eof:
                eof = False       # 残りを再同期のために回す
                # carry を一度は処理させる(無限ループ防止のため、同期できなければ捨てる)
                o = find_sync(carry)
                if o < 0:
                    st['resync_skipped_bytes'] += len(carry)
                    carry = b''
                    eof = True
        else:
            carry = buf[n * 188:]
        if n == 0:
            continue
        if not head_pk or pcr_pid is None:
            for i in range(min(n, 4000)):
                if len(head_pk) < 6000:
                    head_pk.append(bytes(a[i]))
            if pcr_pid is None and len(head_pk) >= 2000:
                pmt_pid, pcr_pid = parse_pat_pmt(head_pk)
        pid = ((a[:, 1].astype(np.uint16) & 0x1f) << 8) | a[:, 2]
        tei = (a[:, 1] >> 7) & 1
        scr = (a[:, 3] >> 6) & 3
        afc = (a[:, 3] >> 4) & 3
        cc = a[:, 3] & 15
        st['packets'] += n
        st['bytes'] += n * 188
        for p in np.unique(pid):
            p = int(p)
            m = pid == p
            d = per.setdefault(p, dict(count=0, cc_err=0, cc_dup=0, scr=0, tei=0, err_pos=[]))
            d['count'] += int(m.sum())
            d['tei'] += int((m & (tei == 1)).sum())
            if p == 0x1fff:
                continue
            d['scr'] += int((m & (scr != 0) & ((afc & 1) == 1)).sum())
            pm = m & ((afc & 1) == 1)
            idx = np.nonzero(pm)[0]
            if not len(idx):
                continue
            ccs = cc[idx].astype(np.int16)
            seq = np.concatenate(([last_cc[p]], ccs)) if p in last_cc else ccs
            last_cc[p] = int(ccs[-1])
            if len(seq) < 2:
                continue
            diff = (seq[1:] - seq[:-1]) & 15
            off = 0 if p in last_cc and len(seq) == len(ccs) + 1 else 1
            # diff[k] は idx[k - (1 if 前回値あり else 0) + 1] ... 位置の対応を計算
            has_prev = len(seq) == len(ccs) + 1
            pos_idx = idx if has_prev else idx[1:]
            dup = diff == 0
            # discontinuity_indicator が立っている重複/ジャンプは許容
            disc = np.zeros(len(pos_idx), dtype=bool)
            if len(pos_idx):
                aa = a[pos_idx]
                disc = (((aa[:, 3] >> 4) & 2) != 0) & (aa[:, 4] > 0) & ((aa[:, 5] & 0x80) != 0)
            err = (diff != 0) & (diff != 1) & ~disc
            d['cc_dup'] += int((dup & ~disc).sum())
            ne = int(err.sum())
            if ne:
                d['cc_err'] += ne
                for k in np.nonzero(err)[0][: POS_MAX - len(d['err_pos'])]:
                    d['err_pos'].append(base + int(pos_idx[k]))
        if pcr_pid is not None:
            m = (pid == pcr_pid) & ((afc & 2) != 0) & (a[:, 4] >= 7) & ((a[:, 5] & 0x10) != 0)
            idx = np.nonzero(m)[0]
            if len(idx):
                aa = a[idx].astype(np.uint64)
                v = (aa[:, 6] << 25) | (aa[:, 7] << 17) | (aa[:, 8] << 9) | (aa[:, 9] << 1) | (aa[:, 10] >> 7)
                pcr_pkts.append(idx.astype(np.int64) + base)
                pcr_vals.append(v)
        base += n
    st['pmt_pid'] = pmt_pid
    st['pcr_pid'] = pcr_pid
    st['pids'] = {hex(k): v for k, v in sorted(per.items())}
    # PCR
    pcr = {}
    if pcr_vals:
        pk = np.concatenate(pcr_pkts)
        v = np.concatenate(pcr_vals).astype(np.int64)
        dt = ((v[1:] - v[:-1]) & ((1 << 33) - 1)) / 90000.0
        dp = (pk[1:] - pk[:-1]) * 188 * 8
        bad = (dt > 0.15) | (dt <= 0)
        pcr['n'] = int(len(v))
        pcr['dur_s'] = float(dt[~bad].sum()) if len(dt) else 0.0
        pcr['max_dt_s'] = float(dt.max()) if len(dt) else 0.0
        pcr['disc'] = int(bad.sum())
        pcr['disc_pos'] = [(int(pk[k + 1]), float(dt[k])) for k in np.nonzero(bad)[0][:POS_MAX]]
        # 1 秒ごとのビットレート
        t = np.cumsum(np.where(bad, 0, dt))
        pcr['map'] = [(int(pk[k + 1]), float(t[k])) for k in range(0, len(t), 100)]   # パケット番号 → PCR 時間(秒)の粗い対応
        bits = np.cumsum(np.where(bad, 0, dp))
        if len(t) > 10 and t[-1] > 3:
            ts = np.arange(1, int(t[-1]), 1.0)
            bi = np.interp(ts, t, bits)
            br = np.diff(bi, prepend=0) / 1e6
            pcr['mbps_1s_min'] = float(br[1:].min())
            pcr['mbps_1s_max'] = float(br[1:].max())
            pcr['mbps_avg'] = float(bits[-1] / t[-1] / 1e6)
            # 5 秒窓の最小
            if len(br) > 6:
                w = np.convolve(br, np.ones(5) / 5, mode='valid')
                pcr['mbps_5s_min'] = float(w.min())
    st['pcr'] = pcr
    # 要約
    tot_cc = sum(d['cc_err'] for d in per.values())
    tot_tei = sum(d['tei'] for d in per.values())
    tot_scr = sum(d['scr'] for k, d in per.items())
    st['total_cc_err'] = tot_cc
    st['total_tei'] = tot_tei
    st['total_scrambled'] = tot_scr
    return st


if __name__ == '__main__':
    r = analyze(sys.argv[1])
    if len(sys.argv) > 2:
        json.dump(r, open(sys.argv[2], 'w'), ensure_ascii=False, indent=1)
    print(json.dumps({k: v for k, v in r.items() if k != 'pids'}, ensure_ascii=False))
    for k, d in r['pids'].items():
        if d['count'] >= 20 or d['cc_err'] or d['tei'] or d['scr']:
            print("PID %-7s 数%9d CCエラー%4d 重複%5d スクランブル%6d TEI%4d %s" % (k, d['count'], d['cc_err'], d['cc_dup'], d['scr'], d['tei'], d['err_pos'][:5] if d['cc_err'] else ''))
