"""取得した TS(188バイト単位)の簡易検査。使い方: python3 ts_analyze.py <TSファイル>
表示: PAT/PMT(サービスと ES の PID、CA 記述子)、PID ごとの パケット数・CC エラー・スクランブル(transport_scrambling_control)・TEI(transport_error_indicator)。
BS/CS が乱れる、復号されていない、といった切り分けに使う(Issue #8)。ファイルの先頭・末尾は途中で切れるので、ffmpeg のエラーは -ss/-t で両端を落として確認すること。
"""
import sys,collections
d=open(sys.argv[1],"rb").read(); n=len(d)//188
cnt=collections.Counter(); cc={}; ccerr=collections.Counter(); scr=collections.Counter(); tei=collections.Counter()
secs={}  # pid -> 最初のセクション
for i in range(n):
    p=d[i*188:i*188+188]
    if p[0]!=0x47: continue
    pid=((p[1]&0x1f)<<8)|p[2]; cnt[pid]+=1
    if p[1]&0x80: tei[pid]+=1
    if (p[3]>>6)&3: scr[pid]+=1
    afc=(p[3]>>4)&3; c=p[3]&0xf
    if afc&1:
        if pid in cc and c!=(cc[pid]+1)&0xf and not (afc&2 and len(p)>5 and p[5]&0x80): 
            if c!=cc[pid]: ccerr[pid]+=1   # 重複(同じ値)は許容
        cc[pid]=c
# PAT/PMT をパース
def first_section(pid):
    for i in range(n):
        p=d[i*188:i*188+188]
        if p[0]==0x47 and (((p[1]&0x1f)<<8)|p[2])==pid and p[1]&0x40:
            afc=(p[3]>>4)&3; o=4+((1+p[4]) if afc&2 else 0); o+=1+p[o]
            ln=((p[o+1]&0xf)<<8)|p[o+2]; return p[o:o+3+ln]
    return None
pat=first_section(0)
pm={}
if pat:
    ln=((pat[1]&0xf)<<8)|pat[2]; o=8
    while o+4<=3+ln-4:
        pn=(pat[o]<<8)|pat[o+1]; pp=((pat[o+2]&0x1f)<<8)|pat[o+3]; pm[pn]=pp; o+=4
    print("PAT: ",{hex(k):hex(v) for k,v in pm.items()})
for pn,pp in pm.items():
    if pn==0: continue
    s=first_section(pp)
    if not s: print("PMT PID",hex(pp),"なし"); continue
    ln=((s[1]&0xf)<<8)|s[2]; ver=(s[5]>>1)&0x1f; pil=((s[10]&0xf)<<8)|s[11]
    ca=[]; o=12
    while o<12+pil:
        if s[o]==9: ca.append(hex(((s[o+2]<<8)|s[o+3])))
        o+=2+s[o+1]
    es=[]; o=12+pil
    while o+5<=3+ln-4:
        st=s[o]; ep=((s[o+1]&0x1f)<<8)|s[o+2]; il=((s[o+3]&0xf)<<8)|s[o+4]; es.append((hex(st),hex(ep))); o+=5+il
    print("PMT sid=%s pid=%s ver=%d CA記述子=%s ES=%s"%(hex(pn),hex(pp),ver,ca,es))
print("総パケット",n)
for pid,c in sorted(cnt.items()):
    if c>=20 or ccerr[pid] or scr[pid] or tei[pid]: print("PID %#06x 数%6d CCエラー%4d スクランブル%5d TEI%4d"%(pid,c,ccerr[pid],scr[pid],tei[pid]))
