#!/usr/bin/env python3
"""地デジ1本の連続録画テスト(間髪入れずに複数セグメント)と、日常レベルの負荷。
使い方: run4h.py <出力ディレクトリ> [--dry]
環境変数: NASNE_IP
セグメント: 3600(クライアント側で切断) → 3600(サーバ側 sec で終了) → 7200(クライアント側で切断)。--dry は 20/20/30 秒。
負荷(録画中に、日常で起き得るレベル):
  - 番組表(EPG)巡回の模擬: 15 秒ごとに別チャンネルを ?si=1 で要求(録画中は 503 が返るはず)
  - 状態の取得: :8301/status?detail を 5 秒ごと、:8302 を 10 秒ごと
  - nasne 内のディスク負荷: 5 分ごとに 200MB の書き込み(fsync)→読み出し→削除
  - ネットワーク+CPU(ssh 暗号化): 10 分ごとに 50MB を scp で取り出す
  - SMART: 5 分ごと
  - 監視: 1 分ごとに load・メモリ・D 状態・eth0・dmesg の異常件数
"""
import os, sys, time, json, subprocess, threading, random, urllib.request, collections, signal

OUT = os.path.abspath(sys.argv[1])
DRY = '--dry' in sys.argv
NAS = os.environ['NASNE_IP']
CH, SID = int(os.environ.get('REC_CH', '27')), os.environ.get('REC_SID', '0xdc28')
SEGS = [(20, 'client'), (20, 'server'), (30, 'client')] if DRY else [(3600, 'client'), (3600, 'server'), (7200, 'client')]
T_DISK = 40 if DRY else 300
T_SCP = 60 if DRY else 600
T_SMART = 60 if DRY else 300
T_MON = 15 if DRY else 60
T_EPG = 5 if DRY else 10
SSHO = ['-o', 'PubkeyAcceptedAlgorithms=+ssh-rsa', '-o', 'HostKeyAlgorithms=+ssh-rsa', '-o', 'ConnectTimeout=8',
        '-o', 'StrictHostKeyChecking=no', '-o', 'LogLevel=ERROR', '-o', 'UserKnownHostsFile=/dev/null']
os.makedirs(OUT, exist_ok=True)
EV = open(os.path.join(OUT, 'events.log'), 'a', buffering=1)
LOCK = threading.Lock()
stop = threading.Event()
state = dict(gap=True, seg_end=0.0, cur_path=None, cur_idx=0)
stats = collections.defaultdict(list)       # 名前 -> 値のリスト
counts = collections.Counter()


def log(tag, msg):
    line = '%s [%s] %s' % (time.strftime('%H:%M:%S'), tag, msg)
    with LOCK:
        EV.write(line + '\n')
        print(line, flush=True)


def ssh(cmd, timeout=60):
    try:
        r = subprocess.run(['ssh'] + SSHO + ['root@' + NAS, cmd], capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout
    except subprocess.TimeoutExpired:
        return 255, ''


def http(url, timeout=6):
    t0 = time.time()
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            body = r.read(200000)
            return r.status, time.time() - t0, body
    except urllib.error.HTTPError as e:
        return e.code, time.time() - t0, b''
    except Exception as e:
        return -1, time.time() - t0, str(e).encode()


# ---------------- 録画 ----------------
def record(idx, dur, mode):
    """1 セグメントを録画する。途切れたら再接続して続ける(途切れは異常として記録)。"""
    t_seg0 = time.time()
    seg_deadline = t_seg0 + dur
    parts = []
    url = 'http://%s:8301/tuner/%d?sid=%s' % (NAS, CH, SID)
    if mode == 'server':
        url += '&sec=%d' % dur
    first_byte_wait = None
    n503 = 0
    alerted = False
    while True:
        remain = seg_deadline - time.time()
        if remain < 3 and mode == 'client':
            break
        path = os.path.join(OUT, 'seg%d_p%d.ts' % (idx, len(parts)))
        t_try = time.time()
        maxt = int(remain) + (150 if mode == 'server' else 0)
        cmd = ['curl', '-sf', '-m', str(max(maxt, 5)), '-o', path, '-w', '%{http_code} %{time_starttransfer} %{size_download}', url]
        state['cur_path'] = path
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True)
        out, _ = p.communicate()
        rc = p.returncode
        t_end = time.time()
        try:
            code, tstart, size = out.split()
            size = int(size)
        except Exception:
            code, tstart, size = '?', '0', 0
        if rc == 22 or (rc not in (0, 28) and size == 0):
            n503 += 1
            counts['rec_503' if rc == 22 else 'rec_connfail'] += 1
            try:
                os.remove(path)
            except OSError:
                pass
            time.sleep(0.05 if rc == 22 else 1.0)
            if first_byte_wait is None and time.time() - t_seg0 > 180 and not alerted:
                alerted = True
                log('ALERT', 'seg%d: 180 秒たっても開けない(rc=%d HTTP %s)' % (idx, rc, code))
            if time.time() - t_seg0 > 900:
                log('ALERT', 'seg%d: 900 秒開けず、あきらめる' % idx)
                break
            continue
        if first_byte_wait is None:
            first_byte_wait = t_try + float(tstart) - t_seg0
            log('REC', 'seg%d 開始: 最初のバイトまで %.2f 秒(503 を %d 回挟んだ) %s' % (idx, first_byte_wait, n503, path))
        parts.append(dict(path=path, rc=rc, size=size, t0=t_try, t1=t_end))
        log('REC', 'seg%d part%d 終了 rc=%d %d バイト %.1f 秒' % (idx, len(parts) - 1, rc, size, t_end - t_try))
        # 想定どおりの終了か
        if mode == 'client' and rc == 28:
            break
        if mode == 'server' and rc == 0 and (t_end - t_try) >= dur - 3:
            break
        if time.time() >= seg_deadline - 3 and mode == 'client':
            break
        log('ALERT', 'seg%d: 想定外の終了(rc=%d, %.1f 秒目)。再接続する' % (idx, rc, t_end - t_try))
        counts['unexpected_end'] += 1
    state['gap'] = True
    t_close = time.time()
    return dict(idx=idx, mode=mode, dur=dur, parts=parts, first_byte_wait=first_byte_wait, n503=n503, t_start=t_seg0, t_end=t_close)


def size_monitor():
    """録画中のファイルの伸びを 1 秒ごとに見て、止まった秒数を数える。最初のバイトが来たら gap を下ろす(負荷スレッドの開始条件)"""
    last = {}
    stall_start = None
    while not stop.is_set():
        time.sleep(1)
        p = state['cur_path']
        if not p:
            continue
        try:
            sz = os.path.getsize(p)
        except OSError:
            continue
        if state['gap']:
            if sz > 0:
                state['gap'] = False
                log('REC', 'seg%d 最初のバイトを確認(%s)' % (state['cur_idx'], os.path.basename(p)))
            stall_start = None
            last[p] = sz
            continue
        prev = last.get(p)
        last[p] = sz
        if prev is None:
            continue
        if sz == prev:
            if stall_start is None:
                stall_start = time.time()
            elif time.time() - stall_start >= 3 and int(time.time() - stall_start) % 5 == 3:
                log('ALERT', '録画ファイルの伸びが %d 秒止まっている %s' % (time.time() - stall_start, os.path.basename(p)))
        else:
            if stall_start is not None and time.time() - stall_start >= 2:
                stats['stall_s'].append(time.time() - stall_start)
                log('STALL', '伸びが %.1f 秒止まっていた' % (time.time() - stall_start))
            stall_start = None


# ---------------- 負荷 ----------------
def near_boundary():
    return state['gap'] or (state['seg_end'] and abs(state['seg_end'] - time.time()) < 25)


def epg_sim():
    chans = list(range(21, 35))
    i = 0
    while not stop.wait(T_EPG):
        if near_boundary():
            continue
        c = chans[i % len(chans)]
        i += 1
        code, dt, body = http('http://%s:8301/tuner/%d?si=1&sec=3' % (NAS, c), timeout=20)
        counts['epg_%s' % code] += 1
        stats['epg_ms'].append(dt * 1000)
        if code not in (503, 400, 404):
            log('LOAD', 'EPG模擬 ch%d: HTTP %s(録画中に 503 以外)' % (c, code))


def status_poll():
    n = 0
    while not stop.wait(5):
        code, dt, body = http('http://%s:8301/status?detail' % NAS, timeout=5)
        stats['st8301_ms'].append(dt * 1000)
        if code != 200:
            counts['st8301_fail'] += 1
            log('LOAD', '/status 失敗 code=%s %.1fs' % (code, dt))
        elif dt > 2:
            log('LOAD', '/status が遅い %.1fs' % dt)
        n += 1
        if n % 2 == 0:
            code, dt, body = http('http://%s:8302/' % NAS, timeout=8)
            stats['st8302_ms'].append(dt * 1000)
            if code != 200:
                counts['st8302_fail'] += 1
                log('LOAD', '状態ページ(8302) 失敗 code=%s %.1fs' % (code, dt))


def disk_load():
    n = 0
    while not stop.wait(T_DISK):
        t0 = time.time()
        rc, out = ssh('dd if=/dev/zero of=/data/.rt_disk bs=1M count=200 conv=fsync 2>&1 | tail -1; dd if=/data/.rt_disk of=/dev/null bs=1M 2>&1 | tail -1; rm -f /data/.rt_disk', 120)
        stats['disk_s'].append(time.time() - t0)
        n += 1
        log('LOAD', 'disk 200MB 書き込み+読み出し %d 回目 %.1f 秒 rc=%d | %s' % (n, time.time() - t0, rc, ' / '.join(out.strip().splitlines())[:160]))


def scp_load():
    n = 0
    stop.wait(T_SCP / 2)
    while not stop.is_set():
        t0 = time.time()
        dst = os.path.join(OUT, 'scp_tmp')
        r = subprocess.run(['scp'] + SSHO + ['root@%s:/data/.rt_load_50m' % NAS, dst], capture_output=True, text=True, timeout=300)
        try:
            os.remove(dst)
        except OSError:
            pass
        n += 1
        stats['scp_s'].append(time.time() - t0)
        log('LOAD', 'scp 50MB %d 回目 %.1f 秒 rc=%d' % (n, time.time() - t0, r.returncode))
        if stop.wait(T_SCP):
            break


MON_CMD = 'sh /tmp/rt_mon.sh'


def monitor():
    base_k = None
    while not stop.is_set():
        rc, out = ssh(MON_CMD, 40)
        if rc != 0:
            counts['mon_ssh_fail'] += 1
            log('MON', 'ssh 失敗 rc=%d(nasne の応答なし?)' % rc)
        else:
            d = {l[0]: l[2:] for l in out.strip().splitlines() if len(l) > 2}
            k = int(d.get('K', '0').split()[0] or 0)
            if base_k is None:
                base_k = k
            stats['mon'].append(dict(t=time.time(), load=d.get('L'), mem=d.get('M'), d=d.get('D'), eth=d.get('E'), k=k))
            flag = ' ★dmesg に異常行が増えた(基準 %d)' % base_k if k > base_k else ''
            log('MON', 'load=%s memAvail=%sKB D=%s eth(rx err drop|tx err drop)=%s dmesg異常=%d%s' % (d.get('L'), d.get('M'), d.get('D'), d.get('E'), k, flag))
            if k > base_k:
                base_k = k
                counts['dmesg_anomaly'] += 1
        stop.wait(T_MON)


def smart_poll():
    while not stop.wait(T_SMART):
        rc, out = ssh('smartctl -A /dev/sda | grep -E "Temperature_Celsius|Current_Pending|Reallocated_Sector|G-Sense|UDMA_CRC" | tr -s " " | cut -d" " -f3,11,12', 40)
        log('SMART', ' | '.join(out.strip().splitlines()))


# ---------------- 本体 ----------------
def main():
    subprocess.run(['scp'] + SSHO + [os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rt_mon.sh'), 'root@%s:/tmp/rt_mon.sh' % NAS], capture_output=True)
    rc, out = ssh('dd if=/dev/zero of=/data/.rt_load_50m bs=1M count=50 2>&1 | tail -1; dmesg | grep -cE "Mips Failed|Oops|BUG:|hung task|blocked for more|I/O error"; cat /proc/uptime; md5sum /usr/local/sbin/nasne-recpt1', 60)
    log('START', 'nasne 事前確認: %s' % ' / '.join(out.strip().splitlines()))
    log('START', 'dry=%s セグメント=%s ch=%d sid=%s 出力=%s' % (DRY, SEGS, CH, SID, OUT))
    for fn in (size_monitor, epg_sim, status_poll, disk_load, scp_load, monitor, smart_poll):
        threading.Thread(target=fn, daemon=True).start()
    results = []
    t_prev_close = None
    for i, (dur, mode) in enumerate(SEGS, 1):
        state['cur_idx'] = i
        state['seg_end'] = time.time() + dur + (0 if mode == 'client' else 5)
        state['gap'] = True
        t_open = time.time()
        r = record(i, dur, mode)
        if t_prev_close is not None:
            r['gap_s'] = (r['t_start'] + (r['first_byte_wait'] or 0)) - t_prev_close
            log('GAP', 'seg%d の最初のバイトは、前のセグメントの終了から %.2f 秒後' % (i, r['gap_s']))
        t_prev_close = r['t_end']
        results.append(r)
        log('REC', 'seg%d 完了: 合計 %d バイト' % (i, sum(p['size'] for p in r['parts'])))
        # 解析は、次の録画を邪魔しないように、nice で別プロセスに
        for k, p in enumerate(r['parts']):
            subprocess.Popen(['nice', '-n', '19', sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tsq.py'),
                              p['path'], p['path'].replace('.ts', '.json')], stdout=open(p['path'].replace('.ts', '.txt'), 'w'), stderr=subprocess.STDOUT)
    stop.set()
    time.sleep(3)
    rc, out = ssh('rm -f /data/.rt_load_50m /data/.rt_disk; cat /proc/uptime; dmesg | grep -cE "Mips Failed|Oops|BUG:|hung task|blocked for more|I/O error"; ip -s link show eth0 | tail -8', 60)
    log('END', 'nasne 事後: %s' % ' / '.join(out.strip().splitlines()))
    def pct(a, q):
        a = sorted(a)
        return a[min(len(a) - 1, int(len(a) * q))] if a else None
    summ = dict(segs=results, counts=dict(counts),
                latency={k: dict(n=len(v), p50=pct(v, .5), p99=pct(v, .99), max=max(v) if v else None) for k, v in stats.items() if k.endswith('_ms') or k.endswith('_s')},
                stalls=stats['stall_s'], mon=stats['mon'])
    json.dump(summ, open(os.path.join(OUT, 'summary.json'), 'w'), ensure_ascii=False, indent=1, default=str)
    log('END', '完了。summary.json を書いた。counts=%s' % dict(counts))


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, lambda *a: (stop.set(), sys.exit(1)))
    main()
