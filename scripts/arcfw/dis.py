# メディアエンジンのファーム(ARC700)を、文字列の注記付きで逆アセンブルする(docs/26)。
# 使い方: python3 -I dis.py <開始(ファイル内オフセット)> <終了>   … カレントに firmware.bin と fw.o(mkelf.sh で作る)を置いて実行。
# 表示のアドレスは実アドレス(ファームは 0x100000 に置かれる)。即値がファーム内の文字列を指すときは、その文字列を注記する。
import sys,subprocess,re
B=0x100000
d=open("firmware.bin","rb").read()
def s_at(off):
    if not (0<=off<len(d)): return None
    e=off
    while e<len(d) and e-off<120 and (32<=d[e]<127 or d[e] in (9,10)): e+=1
    if e-off>=4 and e<len(d) and d[e]==0: return d[off:e].decode("latin1").replace("\n","\\n")
    return None
a,b=int(sys.argv[1],0),int(sys.argv[2],0)
out=subprocess.run(["arc-linux-gnu-objdump","-d","-M","cpu=arc700","--start-address=%d"%a,"--stop-address=%d"%b,"fw.o"],capture_output=True,text=True).stdout
for l in out.splitlines():
    m=re.match(r"\s*([0-9a-f]+):\t(.*)",l)
    if not m: continue
    off=int(m.group(1),16); rest=m.group(2)
    note=""
    for v in re.findall(r"0x([0-9a-f]{5,8})",rest):
        v=int(v,16)
        s=s_at(v-B)
        if s: note+='  ; "%s"'%s[:70]
        elif B<=v<B+len(d) and "bl" in rest.split()[2:3]: pass
    # 分岐先は実アドレスで表示(objdump はオフセットで出すので B を足す)
    rest=re.sub(r";0x([0-9a-f]+)",lambda mm:";%#x"%(int(mm.group(1),16)+B),rest)
    print("%08x: %s%s"%(off+B,rest,note))
